/**
 * @file imu_reader.c
 * @brief IMU reader backed by a discoverable IIO triggered-buffer layout.
 */
#include "imu_reader.h"
#include "log.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#define OCR_IMU_POLL_TIMEOUT_MS 100
#define OCR_IMU_MAX_SCAN_BYTES 4096
#define OCR_IMU_REQUIRED_MASK   0x7fU
#define OCR_IMU_LOCK_PATH_LEN   256

static int path_join(char *out, size_t out_size,
                     const char *dir, const char *name)
{
    int written;

    if (!out || out_size == 0 || !dir || !name) return -1;
    written = snprintf(out, out_size, "%s/%s", dir, name);
    return written >= 0 && (size_t)written < out_size ? 0 : -1;
}

static int read_text_file(const char *path, char *out, size_t out_size)
{
    FILE *fp;
    char *start;
    size_t length;

    if (!path || !out || out_size < 2) return -1;
    fp = fopen(path, "r");
    if (!fp) return -1;
    if (!fgets(out, (int)out_size, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    length = strlen(out);
    while (length > 0 && isspace((unsigned char)out[length - 1])) {
        out[--length] = '\0';
    }
    start = out;
    while (*start && isspace((unsigned char)*start)) ++start;
    if (start != out) memmove(out, start, strlen(start) + 1);
    return out[0] != '\0' ? 0 : -1;
}

static int read_long_file(const char *path, long *value)
{
    char text[64];
    char *end;
    long parsed;

    if (!value || read_text_file(path, text, sizeof(text)) != 0) return -1;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return -1;
    *value = parsed;
    return 0;
}

static int read_double_file(const char *path, double *value)
{
    char text[96];
    char *end;
    double parsed;

    if (!value || read_text_file(path, text, sizeof(text)) != 0) return -1;
    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed)) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int write_text_file(const char *path, const char *value)
{
    size_t length;
    size_t written = 0;
    int fd;

    if (!path || !value) return -1;
    length = strlen(value);
    if (length == 0) return -1;
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    while (written < length) {
        ssize_t count = write(fd, value + written, length - written);
        if (count > 0) {
            written += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        close(fd);
        return -1;
    }
    return close(fd) == 0 ? 0 : -1;
}

static int write_long_file(const char *path, long value)
{
    char text[32];
    int length = snprintf(text, sizeof(text), "%ld", value);

    if (length < 0 || (size_t)length >= sizeof(text)) return -1;
    return write_text_file(path, text);
}

static int acquire_device_lock(ocr_imu_reader_t *reader, const char *dev_path)
{
    const char *basename;
    char lock_path[OCR_IMU_LOCK_PATH_LEN];
    int length;
    int fd;

    if (!reader || !dev_path || reader->lock_fd >= 0) return -1;
    basename = strrchr(dev_path, '/');
    basename = basename ? basename + 1 : dev_path;
    if (*basename == '\0') return -1;
    length = snprintf(lock_path, sizeof(lock_path),
                      "/run/ocr-translator-imu-%s.lock", basename);
    if (length < 0 || (size_t)length >= sizeof(lock_path)) return -1;

    fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        LOG_E("无法打开 IMU 设备锁 %s: %s", lock_path, strerror(errno));
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int lock_errno = errno;

        close(fd);
        if (lock_errno == EWOULDBLOCK || lock_errno == EAGAIN) {
            LOG_E("IMU 设备 %s 已由另一个合规实例占用", dev_path);
        } else {
            LOG_E("无法锁定 IMU 设备 %s: %s", dev_path,
                  strerror(lock_errno));
        }
        return -1;
    }
    reader->lock_fd = fd;
    return 0;
}

static void release_device_lock(ocr_imu_reader_t *reader)
{
    if (!reader || reader->lock_fd < 0) return;
    (void)flock(reader->lock_fd, LOCK_UN);
    (void)close(reader->lock_fd);
    reader->lock_fd = -1;
}

static void release_reader_control(ocr_imu_reader_t *reader)
{
    if (!reader) return;
    release_device_lock(reader);
    if (reader->buffer_lock_initialized) {
        (void)ocr_mutex_destroy(&reader->buffer_lock);
        reader->buffer_lock_initialized = 0;
    }
}

static unsigned int required_scan_bit(const char *name)
{
    static const char *const names[] = {
        "in_accel_x", "in_accel_y", "in_accel_z",
        "in_anglvel_x", "in_anglvel_y", "in_anglvel_z",
        "in_temp", "in_timestamp",
    };

    for (unsigned int i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(name, names[i]) == 0) return 1U << i;
    }
    if (strcmp(name, "timestamp") == 0) return 1U << 7;
    return 0;
}

static int configure_scan_elements(ocr_imu_reader_t *reader)
{
    char scan_dir[OCR_IMU_SYSFS_PATH_LEN + 32];
    DIR *directory;
    struct dirent *entry;
    unsigned int found = 0;

    if (path_join(scan_dir, sizeof(scan_dir), reader->sysfs_path,
                  "scan_elements") != 0) return -1;
    directory = opendir(scan_dir);
    if (!directory) return -1;
    while ((entry = readdir(directory)) != NULL) {
        size_t name_length = strlen(entry->d_name);
        size_t base_length;
        char base[OCR_IMU_SCAN_NAME_LEN];
        char path[OCR_IMU_SYSFS_PATH_LEN + OCR_IMU_SCAN_NAME_LEN + 48];
        unsigned int bit;

        if (name_length <= 3 ||
            strcmp(entry->d_name + name_length - 3, "_en") != 0) continue;
        base_length = name_length - 3;
        if (base_length == 0 || base_length >= sizeof(base)) {
            closedir(directory);
            return -1;
        }
        memcpy(base, entry->d_name, base_length);
        base[base_length] = '\0';
        bit = required_scan_bit(base);
        if (path_join(path, sizeof(path), scan_dir, entry->d_name) != 0 ||
            write_long_file(path, bit != 0 ? 1 : 0) != 0) {
            closedir(directory);
            LOG_E("无法配置 IIO scan element: %s", entry->d_name);
            return -1;
        }
        found |= bit;
    }
    closedir(directory);
    if (found != 0xffU) {
        LOG_E("ICM42688 缺少必需 scan elements (mask=0x%x)", found);
        return -1;
    }
    return 0;
}

static int set_iio_buffer_enabled(ocr_imu_reader_t *reader, int enabled);

static int prepare_iio_buffer(ocr_imu_reader_t *reader, int depth)
{
    char path[OCR_IMU_SYSFS_PATH_LEN + 48];
    char trigger[128];
    long enabled;

    if (path_join(path, sizeof(path), reader->sysfs_path, "buffer/enable") != 0 ||
        read_long_file(path, &enabled) != 0 || (enabled != 0 && enabled != 1)) {
        LOG_E("IMU IIO buffer 状态不可读或非法");
        return -1;
    }
    reader->buffer_enabled = (int)enabled;
    if (enabled == 1) {
        LOG_W("检测到 IMU IIO buffer 的崩溃遗留启用状态，正在恢复");
        if (set_iio_buffer_enabled(reader, 0) != 0) {
            LOG_E("无法清理 IMU IIO buffer 的遗留启用状态");
            return -1;
        }
    }
    if (path_join(path, sizeof(path), reader->sysfs_path,
                  "trigger/current_trigger") != 0 ||
        read_text_file(path, trigger, sizeof(trigger)) != 0 ||
        strncmp(trigger, "icm42688-dev",
                sizeof("icm42688-dev") - 1U) != 0) {
        LOG_E("ICM42688 immutable trigger 未正确绑定");
        return -1;
    }
    if (configure_scan_elements(reader) != 0) return -1;
    if (path_join(path, sizeof(path), reader->sysfs_path, "buffer/length") != 0 ||
        write_long_file(path, depth) != 0) {
        LOG_E("无法配置 IMU IIO buffer length=%d", depth);
        return -1;
    }
    return 0;
}

static int set_iio_buffer_enabled(ocr_imu_reader_t *reader, int enabled)
{
    char path[OCR_IMU_SYSFS_PATH_LEN + 48];
    int desired = enabled ? 1 : 0;
    int result = -1;

    if (!reader || !reader->buffer_lock_initialized ||
        ocr_mutex_lock(&reader->buffer_lock) != 0) {
        return -1;
    }
    if (reader->buffer_enabled == desired) {
        result = 0;
        goto unlock;
    }
    if (path_join(path, sizeof(path), reader->sysfs_path, "buffer/enable") == 0 &&
        write_long_file(path, desired) == 0) {
        reader->buffer_enabled = desired;
        result = 0;
    }

unlock:
    if (ocr_mutex_unlock(&reader->buffer_lock) != 0) return -1;
    return result;
}

static int parse_unsigned(const char **cursor, unsigned long *value)
{
    char *end;
    unsigned long parsed;

    if (!cursor || !*cursor || !value || !isdigit((unsigned char)**cursor)) {
        return -1;
    }
    errno = 0;
    parsed = strtoul(*cursor, &end, 10);
    if (errno != 0 || end == *cursor) return -1;
    *cursor = end;
    *value = parsed;
    return 0;
}

static int parse_scan_type(const char *text, ocr_imu_scan_channel_t *channel)
{
    const char *cursor;
    unsigned long real_bits;
    unsigned long storage_bits;
    unsigned long repeat = 1;
    unsigned long shift;
    int big_endian;

    if (!text || !channel) return -1;
    cursor = text;
    if (strncmp(cursor, "le:", 3) == 0) {
        big_endian = 0;
        cursor += 3;
    } else if (strncmp(cursor, "be:", 3) == 0) {
        big_endian = 1;
        cursor += 3;
    } else if (strncmp(cursor, "cpu:", 4) == 0) {
        const uint16_t endian_probe = 0x0100;
        big_endian = *(const uint8_t *)&endian_probe == 0x01;
        cursor += 4;
    } else {
        return -1;
    }

    if (*cursor != 's' && *cursor != 'u') return -1;
    channel->is_signed = (uint8_t)(*cursor++ == 's');
    if (parse_unsigned(&cursor, &real_bits) != 0 || *cursor != '/') return -1;
    ++cursor;
    if (parse_unsigned(&cursor, &storage_bits) != 0) return -1;
    if (*cursor == 'X') {
        ++cursor;
        if (parse_unsigned(&cursor, &repeat) != 0) return -1;
    }
    if (strncmp(cursor, ">>", 2) != 0) return -1;
    cursor += 2;
    if (parse_unsigned(&cursor, &shift) != 0 || *cursor != '\0') return -1;

    if (repeat != 1 || real_bits == 0 || real_bits > 64 ||
        storage_bits < 8 || storage_bits > 64 || storage_bits % 8 != 0 ||
        shift >= storage_bits || real_bits + shift > storage_bits) {
        return -1;
    }
    channel->storage_bytes = (uint8_t)(storage_bits / 8);
    if ((channel->storage_bytes & (channel->storage_bytes - 1)) != 0) {
        return -1;
    }
    channel->real_bits = (uint8_t)real_bits;
    channel->shift = (uint8_t)shift;
    channel->is_big_endian = (uint8_t)big_endian;
    return 0;
}

static int axis_from_name(const char *name, const char *prefix)
{
    size_t prefix_len;

    if (!name || !prefix) return -1;
    prefix_len = strlen(prefix);
    if (strlen(name) != prefix_len + 2 ||
        strncmp(name, prefix, prefix_len) != 0 || name[prefix_len] != '_') {
        return -1;
    }
    switch (name[prefix_len + 1]) {
    case 'x': return 0;
    case 'y': return 1;
    case 'z': return 2;
    default:  return -1;
    }
}

static void classify_channel(ocr_imu_scan_channel_t *channel)
{
    int axis;

    channel->kind = OCR_IMU_SCAN_OTHER;
    channel->axis = -1;
    axis = axis_from_name(channel->name, "in_accel");
    if (axis >= 0) {
        channel->kind = OCR_IMU_SCAN_ACCEL;
        channel->axis = (int8_t)axis;
        return;
    }
    axis = axis_from_name(channel->name, "in_anglvel");
    if (axis >= 0) {
        channel->kind = OCR_IMU_SCAN_GYRO;
        channel->axis = (int8_t)axis;
        return;
    }
    if (strcmp(channel->name, "in_timestamp") == 0 ||
        strcmp(channel->name, "timestamp") == 0) {
        channel->kind = OCR_IMU_SCAN_TIMESTAMP;
    }
}

static int load_channel_scale(const ocr_imu_reader_t *reader,
                              ocr_imu_scan_channel_t *channel)
{
    char filename[OCR_IMU_SCAN_NAME_LEN + 16];
    char path[OCR_IMU_SYSFS_PATH_LEN + OCR_IMU_SCAN_NAME_LEN + 24];
    const char *shared_name;
    double scale;

    if (channel->kind != OCR_IMU_SCAN_ACCEL &&
        channel->kind != OCR_IMU_SCAN_GYRO) {
        channel->scale = 1.0;
        return 0;
    }

    if (snprintf(filename, sizeof(filename), "%s_scale", channel->name) < 0 ||
        path_join(path, sizeof(path), reader->sysfs_path, filename) != 0) {
        return -1;
    }
    if (read_double_file(path, &scale) != 0) {
        shared_name = channel->kind == OCR_IMU_SCAN_ACCEL ?
                      "in_accel_scale" : "in_anglvel_scale";
        if (path_join(path, sizeof(path), reader->sysfs_path, shared_name) != 0 ||
            read_double_file(path, &scale) != 0) {
            LOG_E("IMU 通道 %s 缺少可解析的 scale", channel->name);
            return -1;
        }
    }
    if (!(scale > 0.0) || !isfinite(scale)) {
        LOG_E("IMU 通道 %s scale 非法: %.12g", channel->name, scale);
        return -1;
    }
    channel->scale = scale;
    return 0;
}

static int scan_channel_compare(const void *lhs, const void *rhs)
{
    const ocr_imu_scan_channel_t *a = lhs;
    const ocr_imu_scan_channel_t *b = rhs;
    return (a->scan_index > b->scan_index) - (a->scan_index < b->scan_index);
}

static int align_up(size_t value, size_t alignment, size_t *aligned)
{
    size_t mask;

    if (!aligned || alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return -1;
    }
    mask = alignment - 1;
    if (value > SIZE_MAX - mask) return -1;
    *aligned = (value + mask) & ~mask;
    return 0;
}

static int scale_to_micro(double scale)
{
    double value = scale * 1000000.0;
    if (value >= (double)INT_MAX) return INT_MAX;
    if (value <= (double)INT_MIN) return INT_MIN;
    return (int)llround(value);
}

static int discover_scan_layout(ocr_imu_reader_t *reader)
{
    char scan_dir[OCR_IMU_SYSFS_PATH_LEN + 32];
    DIR *dir;
    struct dirent *entry;
    unsigned int required = 0;
    size_t cursor = 0;
    size_t largest_alignment = 1;

    if (path_join(scan_dir, sizeof(scan_dir), reader->sysfs_path,
                  "scan_elements") != 0) {
        return -1;
    }
    dir = opendir(scan_dir);
    if (!dir) {
        LOG_E("无法发现 IMU IIO 布局 %s: %s", scan_dir, strerror(errno));
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        size_t base_len;
        char base[OCR_IMU_SCAN_NAME_LEN];
        char filename[OCR_IMU_SCAN_NAME_LEN + 16];
        char path[OCR_IMU_SYSFS_PATH_LEN + OCR_IMU_SCAN_NAME_LEN + 48];
        char type_text[96];
        long enabled;
        long index;
        ocr_imu_scan_channel_t *channel;

        if (name_len <= 3 || strcmp(entry->d_name + name_len - 3, "_en") != 0) {
            continue;
        }
        if (path_join(path, sizeof(path), scan_dir, entry->d_name) != 0 ||
            read_long_file(path, &enabled) != 0) {
            closedir(dir);
            LOG_E("无法读取 IIO scan enable: %s", entry->d_name);
            return -1;
        }
        if (enabled == 0) continue;
        if (enabled != 1 || reader->scan_channel_count >= OCR_IMU_MAX_SCAN_CHANNELS) {
            closedir(dir);
            LOG_E("IIO scan 通道数量或 enable 值非法: %s", entry->d_name);
            return -1;
        }

        base_len = name_len - 3;
        if (base_len == 0 || base_len >= sizeof(base)) {
            closedir(dir);
            LOG_E("IIO scan 通道名过长: %s", entry->d_name);
            return -1;
        }
        memcpy(base, entry->d_name, base_len);
        base[base_len] = '\0';

        channel = &reader->scan_channels[reader->scan_channel_count];
        memset(channel, 0, sizeof(*channel));
        memcpy(channel->name, base, base_len + 1);
        channel->axis = -1;
        channel->scale = 1.0;

        if (snprintf(filename, sizeof(filename), "%s_index", base) < 0 ||
            path_join(path, sizeof(path), scan_dir, filename) != 0 ||
            read_long_file(path, &index) != 0 || index < 0 || index > INT_MAX) {
            closedir(dir);
            LOG_E("无法解析 IIO scan index: %s", base);
            return -1;
        }
        channel->scan_index = (int)index;

        if (snprintf(filename, sizeof(filename), "%s_type", base) < 0 ||
            path_join(path, sizeof(path), scan_dir, filename) != 0 ||
            read_text_file(path, type_text, sizeof(type_text)) != 0 ||
            parse_scan_type(type_text, channel) != 0) {
            closedir(dir);
            LOG_E("无法解析 IIO scan type: %s", base);
            return -1;
        }
        classify_channel(channel);
        if ((channel->kind == OCR_IMU_SCAN_ACCEL ||
             channel->kind == OCR_IMU_SCAN_GYRO) && !channel->is_signed) {
            closedir(dir);
            LOG_E("不支持无符号且无 offset 的 IMU 通道: %s", base);
            return -1;
        }
        if (load_channel_scale(reader, channel) != 0) {
            closedir(dir);
            return -1;
        }

        if (channel->kind == OCR_IMU_SCAN_ACCEL) {
            unsigned int bit = 1U << (unsigned int)channel->axis;
            if (required & bit) {
                closedir(dir);
                LOG_E("重复的 IIO accel 轴: %s", base);
                return -1;
            }
            required |= bit;
            reader->accel_scale_si[channel->axis] = channel->scale;
        } else if (channel->kind == OCR_IMU_SCAN_GYRO) {
            unsigned int bit = 1U << (unsigned int)(channel->axis + 3);
            if (required & bit) {
                closedir(dir);
                LOG_E("重复的 IIO gyro 轴: %s", base);
                return -1;
            }
            required |= bit;
            reader->gyro_scale_si[channel->axis] = channel->scale;
        } else if (channel->kind == OCR_IMU_SCAN_TIMESTAMP) {
            if (required & (1U << 6)) {
                closedir(dir);
                LOG_E("重复的 IIO timestamp 通道");
                return -1;
            }
            required |= 1U << 6;
        }
        ++reader->scan_channel_count;
    }
    closedir(dir);

    if (required != OCR_IMU_REQUIRED_MASK) {
        LOG_E("IMU IIO 布局缺少必需 accel/gyro/timestamp 通道 (mask=0x%x)",
              required);
        return -1;
    }

    qsort(reader->scan_channels, (size_t)reader->scan_channel_count,
          sizeof(reader->scan_channels[0]), scan_channel_compare);
    for (int i = 0; i < reader->scan_channel_count; ++i) {
        ocr_imu_scan_channel_t *channel = &reader->scan_channels[i];
        if (i > 0 && channel->scan_index ==
                     reader->scan_channels[i - 1].scan_index) {
            LOG_E("IIO scan_index 重复: %d", channel->scan_index);
            return -1;
        }
        if (align_up(cursor, channel->storage_bytes, &cursor) != 0) return -1;
        channel->offset = cursor;
        if (cursor > SIZE_MAX - channel->storage_bytes) return -1;
        cursor += channel->storage_bytes;
        if (channel->storage_bytes > largest_alignment) {
            largest_alignment = channel->storage_bytes;
        }
    }
    if (align_up(cursor, largest_alignment, &reader->scan_bytes) != 0 ||
        reader->scan_bytes == 0 || reader->scan_bytes > OCR_IMU_MAX_SCAN_BYTES) {
        LOG_E("IIO scan frame 大小非法: %zu", reader->scan_bytes);
        return -1;
    }

    reader->accel_scale = scale_to_micro(reader->accel_scale_si[0]);
    reader->gyro_scale = scale_to_micro(reader->gyro_scale_si[0]);
    LOG_I("IMU IIO 布局: channels=%d frame=%zu bytes", reader->scan_channel_count,
          reader->scan_bytes);
    return 0;
}

static int derive_sysfs_path(ocr_imu_reader_t *reader, const char *dev_path)
{
    const char *name = strrchr(dev_path, '/');
    const char *suffix;
    int written;

    name = name ? name + 1 : dev_path;
    if (strncmp(name, "iio:device", 10) != 0) return -1;
    suffix = name + 10;
    if (*suffix == '\0') return -1;
    while (*suffix) {
        if (!isdigit((unsigned char)*suffix++)) return -1;
    }
    written = snprintf(reader->sysfs_path, sizeof(reader->sysfs_path),
                       "/sys/bus/iio/devices/%s", name);
    return written >= 0 && (size_t)written < sizeof(reader->sysfs_path) ? 0 : -1;
}

static int decode_scan_value(const uint8_t *frame, size_t frame_size,
                             const ocr_imu_scan_channel_t *channel,
                             int64_t *decoded)
{
    const uint8_t *data;
    uint64_t value = 0;
    uint64_t mask;

    if (!frame || !channel || !decoded ||
        channel->offset > frame_size ||
        channel->storage_bytes > frame_size - channel->offset) {
        return -1;
    }
    data = frame + channel->offset;
    if (channel->is_big_endian) {
        for (uint8_t i = 0; i < channel->storage_bytes; ++i) {
            value = (value << 8) | data[i];
        }
    } else {
        for (uint8_t i = 0; i < channel->storage_bytes; ++i) {
            value |= (uint64_t)data[i] << (8U * i);
        }
    }
    value >>= channel->shift;
    mask = channel->real_bits == 64 ? UINT64_MAX :
           ((UINT64_C(1) << channel->real_bits) - 1U);
    value &= mask;

    if (channel->is_signed &&
        (value & (UINT64_C(1) << (channel->real_bits - 1U)))) {
        int64_t signed_value;
        value |= ~mask;
        memcpy(&signed_value, &value, sizeof(signed_value));
        *decoded = signed_value;
    } else {
        if (value > (uint64_t)INT64_MAX) return -1;
        *decoded = (int64_t)value;
    }
    return 0;
}

static int16_t clamp_i16(int64_t value)
{
    if (value > INT16_MAX) return INT16_MAX;
    if (value < INT16_MIN) return INT16_MIN;
    return (int16_t)value;
}

static int parse_scan_frame(const ocr_imu_reader_t *reader,
                            const uint8_t *frame, imu_sample_t *sample)
{
    memset(sample, 0, sizeof(*sample));
    for (int i = 0; i < reader->scan_channel_count; ++i) {
        const ocr_imu_scan_channel_t *channel = &reader->scan_channels[i];
        int64_t raw;
        double scaled;

        if (channel->kind == OCR_IMU_SCAN_OTHER) continue;
        if (decode_scan_value(frame, reader->scan_bytes, channel, &raw) != 0) {
            return -1;
        }
        if (channel->kind == OCR_IMU_SCAN_ACCEL) {
            scaled = (double)raw * channel->scale;
            if (!isfinite(scaled) || fabs(scaled) > FLT_MAX) return -1;
            sample->accel_raw[channel->axis] = raw;
            sample->accel[channel->axis] = clamp_i16(raw);
            sample->accel_si[channel->axis] = (float)scaled;
        } else if (channel->kind == OCR_IMU_SCAN_GYRO) {
            scaled = (double)raw * channel->scale;
            if (!isfinite(scaled) || fabs(scaled) > FLT_MAX) return -1;
            sample->gyro_raw[channel->axis] = raw;
            sample->gyro[channel->axis] = clamp_i16(raw);
            sample->gyro_si[channel->axis] = (float)scaled;
        } else if (channel->kind == OCR_IMU_SCAN_TIMESTAMP) {
            sample->timestamp = raw;
        }
    }
    return 0;
}

int ocr_imu_reader_init(ocr_imu_reader_t *reader, const char *dev_path,
                        int buf_depth)
{
    size_t dev_len;

    if (!reader) return -1;
    memset(reader, 0, sizeof(*reader));
    reader->dev_fd = -1;
    reader->lock_fd = -1;
    atomic_init(&reader->running, 0);
    if (!dev_path) return -1;

    dev_len = strlen(dev_path);
    if (dev_len == 0 || dev_len >= sizeof(reader->dev_name)) return -1;
    memcpy(reader->dev_name, dev_path, dev_len + 1);
    if (derive_sysfs_path(reader, dev_path) != 0) {
        LOG_E("无法从 IMU 设备路径发现 IIO sysfs 布局: %s", dev_path);
        return -2;
    }
    if (ocr_mutex_init(&reader->buffer_lock) != 0) {
        LOG_E("无法初始化 IMU buffer 状态锁");
        return -3;
    }
    reader->buffer_lock_initialized = 1;
    if (acquire_device_lock(reader, dev_path) != 0) {
        release_reader_control(reader);
        return -3;
    }

    if (buf_depth <= 0) buf_depth = 256;
    if (prepare_iio_buffer(reader, buf_depth) != 0) {
        LOG_E("IMU IIO buffer 配置失败: %s", reader->sysfs_path);
        release_reader_control(reader);
        return -4;
    }

    reader->dev_fd = open(dev_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (reader->dev_fd < 0) {
        LOG_E("打开 IIO 设备 %s 失败: %s", dev_path, strerror(errno));
        release_reader_control(reader);
        return -5;
    }
    if (discover_scan_layout(reader) != 0) {
        LOG_E("IMU IIO 布局不可发现或不完整: %s", reader->sysfs_path);
        close(reader->dev_fd);
        reader->dev_fd = -1;
        release_reader_control(reader);
        return -6;
    }

    if (ocr_ringbuffer_init(&reader->ring, sizeof(imu_sample_t),
                            (size_t)buf_depth) != 0) {
        close(reader->dev_fd);
        reader->dev_fd = -1;
        release_reader_control(reader);
        return -7;
    }
    reader->initialized = 1;
    LOG_I("IMU 读取初始化: %s depth=%d frame=%zu", dev_path, buf_depth,
          reader->scan_bytes);
    return 0;
}

static void *imu_thread(void *arg)
{
    ocr_imu_reader_t *reader = arg;
    uint8_t *frame = malloc(reader->scan_bytes);
    size_t have = 0;
    int fatal_error = 0;

    if (!frame) {
        LOG_E("IMU 读取缓冲分配失败");
        atomic_store_explicit(&reader->running, 0, memory_order_release);
        if (set_iio_buffer_enabled(reader, 0) != 0)
            LOG_E("IMU 读取线程异常退出后无法禁用 IIO buffer");
        return NULL;
    }
    LOG_I("IMU 读取线程启动");

    while (atomic_load_explicit(&reader->running, memory_order_acquire)) {
        struct pollfd pfd = {
            .fd = reader->dev_fd,
            .events = POLLIN,
        };
        int poll_result;

        do {
            poll_result = poll(&pfd, 1, OCR_IMU_POLL_TIMEOUT_MS);
        } while (poll_result < 0 && errno == EINTR &&
                 atomic_load_explicit(&reader->running, memory_order_acquire));
        if (!atomic_load_explicit(&reader->running, memory_order_acquire)) break;
        if (poll_result == 0) continue;
        if (poll_result < 0) {
            LOG_E("IMU poll 失败: %s", strerror(errno));
            fatal_error = 1;
            break;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            LOG_E("IMU poll 返回设备错误: revents=0x%x",
                  (unsigned int)(unsigned short)pfd.revents);
            fatal_error = 1;
            break;
        }
        if (!(pfd.revents & POLLIN)) continue;

        while (have < reader->scan_bytes &&
               atomic_load_explicit(&reader->running, memory_order_acquire)) {
            ssize_t count = read(reader->dev_fd, frame + have,
                                 reader->scan_bytes - have);
            if (count > 0) {
                have += (size_t)count;
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            LOG_E("IMU read 失败: %s",
                  count == 0 ? "设备返回 EOF" : strerror(errno));
            fatal_error = 1;
            break;
        }
        if (fatal_error) break;
        if (have == reader->scan_bytes) {
            imu_sample_t sample;
            int push_result;

            if (parse_scan_frame(reader, frame, &sample) != 0) {
                ++reader->parse_error_count;
                LOG_E("IMU scan frame 解析失败");
                fatal_error = 1;
                break;
            }
            ++reader->sample_count;
            push_result = ocr_ringbuffer_push(&reader->ring, &sample);
            if (push_result == 1) {
                ++reader->dropped_count;
            } else if (push_result != 0) {
                LOG_E("IMU 环形缓冲写入失败: %d", push_result);
                fatal_error = 1;
                break;
            }
            have = 0;
        }
    }

    free(frame);
    atomic_store_explicit(&reader->running, 0, memory_order_release);
    if (set_iio_buffer_enabled(reader, 0) != 0)
        LOG_E("IMU 读取线程退出后无法禁用 IIO buffer");
    LOG_I("IMU 读取线程退出 (samples=%llu dropped=%llu errors=%llu)",
          (unsigned long long)reader->sample_count,
          (unsigned long long)reader->dropped_count,
          (unsigned long long)reader->parse_error_count);
    return NULL;
}

int ocr_imu_reader_start(ocr_imu_reader_t *reader)
{
    if (!reader || !reader->initialized || reader->dev_fd < 0 ||
        reader->scan_bytes == 0) {
        return -1;
    }
    if (atomic_load_explicit(&reader->running, memory_order_acquire)) return 0;

    if (reader->thread_started) {
        if (ocr_thread_join(reader->thread, NULL) != 0) return -2;
        reader->thread_started = 0;
        if (set_iio_buffer_enabled(reader, 0) != 0)
            return -3;
    }
    if (set_iio_buffer_enabled(reader, 1) != 0) {
        LOG_E("无法启用 IMU IIO buffer");
        return -3;
    }
    atomic_store_explicit(&reader->running, 1, memory_order_release);
    if (ocr_thread_create(&reader->thread, imu_thread, reader) != 0) {
        atomic_store_explicit(&reader->running, 0, memory_order_release);
        (void)set_iio_buffer_enabled(reader, 0);
        LOG_E("IMU 读取线程创建失败");
        return -4;
    }
    reader->thread_started = 1;
    return 0;
}

int ocr_imu_reader_stop(ocr_imu_reader_t *reader)
{
    int result = 0;
    int disable_result = 0;

    if (!reader) return -1;
    atomic_store_explicit(&reader->running, 0, memory_order_release);
    if (reader->buffer_lock_initialized) {
        disable_result = set_iio_buffer_enabled(reader, 0);
        if (disable_result != 0) LOG_E("无法禁用 IMU IIO buffer");
    }
    if (reader->thread_started) {
        result = ocr_thread_join(reader->thread, NULL);
        if (result == 0) reader->thread_started = 0;
    }
    return result == 0 && disable_result == 0 ? 0 : -2;
}

int ocr_imu_reader_get(ocr_imu_reader_t *reader, imu_sample_t *sample)
{
    if (!reader || !sample || !reader->initialized) return -1;
    return ocr_ringbuffer_pop(&reader->ring, sample);
}

void ocr_imu_reader_destroy(ocr_imu_reader_t *reader)
{
    int was_initialized;

    if (!reader) return;
    was_initialized = reader->initialized;
    if (ocr_imu_reader_stop(reader) != 0) {
        LOG_E("IMU 线程无法 join，保留资源以避免并发释放");
        return;
    }
    if (reader->initialized) {
        ocr_ringbuffer_destroy(&reader->ring);
        reader->initialized = 0;
    }
    if (was_initialized && reader->dev_fd >= 0) {
        close(reader->dev_fd);
    }
    reader->dev_fd = -1;
    release_reader_control(reader);
    reader->buffer_enabled = 0;
    reader->scan_bytes = 0;
    reader->scan_channel_count = 0;
}
