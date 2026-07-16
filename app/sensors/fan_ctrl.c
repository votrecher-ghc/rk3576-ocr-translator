/**
 * @file fan_ctrl.c
 * @brief 风扇调速实现
 */
#include "fan_ctrl.h"
#include "log.h"

#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#define PWM_SYSFS_ROOT "/sys/class/pwm"
#define FAN_LOCK_PATH "/run/ocr-translator-fan.lock"

static void reset_controller(ocr_fan_ctrl_t *ctrl)
{
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->lock_fd = -1;
}

static int acquire_fan_lock(ocr_fan_ctrl_t *ctrl)
{
    int fd;

    if (!ctrl || ctrl->lock_held) return -1;
    fd = open(FAN_LOCK_PATH,
              O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        LOG_E("cannot open fan ownership lock %s: %s", FAN_LOCK_PATH,
              strerror(errno));
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int lock_errno = errno;

        (void)close(fd);
        if (lock_errno == EWOULDBLOCK || lock_errno == EAGAIN) {
            LOG_E("fan PWM is already owned by another service instance");
        } else {
            LOG_E("cannot lock fan PWM ownership: %s", strerror(lock_errno));
        }
        return -1;
    }
    ctrl->lock_fd = fd;
    ctrl->lock_held = 1;
    return 0;
}

static void release_fan_lock(ocr_fan_ctrl_t *ctrl)
{
    int fd;

    if (!ctrl || !ctrl->lock_held) return;
    fd = ctrl->lock_fd;
    ctrl->lock_fd = -1;
    ctrl->lock_held = 0;
    if (flock(fd, LOCK_UN) != 0)
        LOG_W("cannot explicitly unlock fan PWM ownership: %s",
              strerror(errno));
    if (close(fd) != 0)
        LOG_W("cannot close fan PWM ownership lock: %s", strerror(errno));
}

static int finish_init_failure(ocr_fan_ctrl_t *ctrl, int error)
{
    release_fan_lock(ctrl);
    reset_controller(ctrl);
    return error;
}

static int is_pwmchip_name(const char *name)
{
    static const char prefix[] = "pwmchip";
    const char *cursor;

    if (!name || strncmp(name, prefix, sizeof(prefix) - 1U) != 0) return 0;
    cursor = name + sizeof(prefix) - 1U;
    if (*cursor == '\0') return 0;
    while (*cursor != '\0') {
        if (!isdigit((unsigned char)*cursor)) return 0;
        ++cursor;
    }
    return 1;
}

static int find_marked_fan_pwm(char *path, size_t path_size)
{
    DIR *directory;
    struct dirent *entry;
    char selected[128] = "";
    int matches = 0;
    int scan_error = 0;

    if (!path || path_size == 0) return -1;
    path[0] = '\0';
    directory = opendir(PWM_SYSFS_ROOT);
    if (!directory) {
        LOG_E("无法枚举 PWM 控制器: %s", strerror(errno));
        return -1;
    }
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        char marker[256];
        char candidate[128];
        int length;

        if (!is_pwmchip_name(entry->d_name)) continue;
        length = snprintf(marker, sizeof(marker),
                          PWM_SYSFS_ROOT "/%s/device/of_node/ocr,fan-pwm",
                          entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(marker) ||
            access(marker, F_OK) != 0) {
            errno = 0;
            continue;
        }
        length = snprintf(candidate, sizeof(candidate),
                          PWM_SYSFS_ROOT "/%s/pwm0", entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(candidate)) {
            scan_error = 1;
            break;
        }
        ++matches;
        if (matches == 1) memcpy(selected, candidate, (size_t)length + 1U);
        if (matches > 1) break;
        errno = 0;
    }
    if (!entry && errno != 0) scan_error = 1;
    if (closedir(directory) != 0) scan_error = 1;
    if (scan_error) {
        LOG_E("枚举 PWM 控制器失败");
        return -1;
    }
    if (matches == 0) {
        LOG_E("未发现带 ocr,fan-pwm 标记的 PWM 控制器");
        return -1;
    }
    if (matches != 1) {
        LOG_E("发现多个带 ocr,fan-pwm 标记的 PWM 控制器，拒绝自动选择");
        return -1;
    }
    int length = snprintf(path, path_size, "%s", selected);
    if (length < 0 || (size_t)length >= path_size) {
        path[0] = '\0';
        return -1;
    }
    LOG_I("自动发现风扇 PWM: %s", path);
    return 0;
}

/* 写 sysfs 字符串值 */
static int write_sysfs_str(const char *path, const char *val)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    int ret = fputs(val, fp) == EOF ? -1 : 0;
    if (fclose(fp) != 0) ret = -1;
    return ret;
}

static int write_sysfs_int(const char *path, int val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    return write_sysfs_str(path, buf);
}

static int read_sysfs_int(const char *path, int *value)
{
    long parsed;
    FILE *fp;

    if (!path || !value) return -1;
    fp = fopen(path, "r");
    if (!fp) return -1;
    int ret = fscanf(fp, "%ld", &parsed) == 1 &&
              parsed >= INT_MIN && parsed <= INT_MAX ? 0 : -1;
    if (fclose(fp) != 0) ret = -1;
    if (ret == 0) *value = (int)parsed;
    return ret;
}

static int attribute_path(const ocr_fan_ctrl_t *ctrl, const char *attribute,
                          char *path, size_t path_size)
{
    int length = snprintf(path, path_size, "%s/%s", ctrl->pwm_path, attribute);
    return length >= 0 && (size_t)length < path_size ? 0 : -1;
}

static int read_attribute(const ocr_fan_ctrl_t *ctrl, const char *attribute,
                          int *value)
{
    char path[256];
    return attribute_path(ctrl, attribute, path, sizeof(path)) == 0
         ? read_sysfs_int(path, value) : -1;
}

static int write_attribute(const ocr_fan_ctrl_t *ctrl, const char *attribute,
                           int value)
{
    char path[256];
    return attribute_path(ctrl, attribute, path, sizeof(path)) == 0
         ? write_sysfs_int(path, value) : -1;
}

static int parse_pwm_location(ocr_fan_ctrl_t *ctrl)
{
    const char *component = strrchr(ctrl->pwm_path, '/');
    char *end = NULL;
    long channel;
    size_t chip_length;

    if (!component || strncmp(component + 1, "pwm", 3) != 0) return -1;
    errno = 0;
    channel = strtol(component + 4, &end, 10);
    if (errno || end == component + 4 || *end != '\0' ||
        channel < 0 || channel > 65535) {
        return -1;
    }
    chip_length = (size_t)(component - ctrl->pwm_path);
    if (chip_length == 0 || chip_length >= sizeof(ctrl->pwm_chip_path)) return -1;
    memcpy(ctrl->pwm_chip_path, ctrl->pwm_path, chip_length);
    ctrl->pwm_chip_path[chip_length] = '\0';
    ctrl->pwm_channel = (int)channel;
    return 0;
}

static int ensure_pwm_exported(ocr_fan_ctrl_t *ctrl)
{
    char path[256];
    int length;
    const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };

    if (access(ctrl->pwm_path, F_OK) == 0) {
        if (ctrl->auto_mode) {
            /* The marked channel is dedicated to this service.  Since the
             * flock proves no live instance owns it, an existing pwm0 is a
             * crash-left export and must not be restored as external state. */
            ctrl->exported_by_us = 1;
            ctrl->auto_takeover = 1;
            LOG_W("taking over crash-left dedicated fan PWM: %s",
                  ctrl->pwm_path);
        }
        return 0;
    }
    length = snprintf(path, sizeof(path), "%s/export", ctrl->pwm_chip_path);
    if (length < 0 || (size_t)length >= sizeof(path)) return -1;
    if (write_sysfs_int(path, ctrl->pwm_channel) != 0) return -1;
    ctrl->exported_by_us = 1;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (access(ctrl->pwm_path, F_OK) == 0) return 0;
        (void)nanosleep(&delay, NULL);
    }
    return -1;
}

static int unexport_owned_pwm(ocr_fan_ctrl_t *ctrl)
{
    char path[256];
    int length;

    if (!ctrl->exported_by_us) return 0;
    length = snprintf(path, sizeof(path), "%s/unexport", ctrl->pwm_chip_path);
    if (length < 0 || (size_t)length >= sizeof(path) ||
        write_sysfs_int(path, ctrl->pwm_channel) != 0) {
        return -1;
    }
    ctrl->exported_by_us = 0;
    return 0;
}

static int disable_pwm(const ocr_fan_ctrl_t *ctrl)
{
    int ret = 0;
    if (write_attribute(ctrl, "enable", 0) != 0) ret = -1;
    if (write_attribute(ctrl, "duty_cycle", 0) != 0) ret = -1;
    return ret;
}

static int capture_external_state(ocr_fan_ctrl_t *ctrl)
{
    int enable, period, duty;

    if (read_attribute(ctrl, "enable", &enable) != 0 ||
        read_attribute(ctrl, "period", &period) != 0 ||
        read_attribute(ctrl, "duty_cycle", &duty) != 0 ||
        (enable != 0 && enable != 1) || period <= 0 || duty < 0 ||
        duty > period) {
        return -1;
    }
    ctrl->original_enable = enable;
    ctrl->original_period_ns = period;
    ctrl->original_duty_ns = duty;
    ctrl->original_state_valid = 1;
    return 0;
}

static int restore_external_state(ocr_fan_ctrl_t *ctrl)
{
    int ret = 0;

    if (!ctrl->original_state_valid) return 0;
    /* Disable first, then use a zero duty while restoring the old period. */
    if (write_attribute(ctrl, "enable", 0) != 0) ret = -1;
    if (write_attribute(ctrl, "duty_cycle", 0) != 0) ret = -1;
    if (write_attribute(ctrl, "period", ctrl->original_period_ns) != 0)
        ret = -1;
    if (write_attribute(ctrl, "duty_cycle", ctrl->original_duty_ns) != 0)
        ret = -1;
    if (write_attribute(ctrl, "enable", ctrl->original_enable) != 0)
        ret = -1;
    return ret;
}

static int release_owned_pwm(ocr_fan_ctrl_t *ctrl)
{
    if (!ctrl->exported_by_us) return 0;
    if (disable_pwm(ctrl) != 0) {
        /* Still attempt unexport: once that succeeds the channel can no
         * longer drive the fan, so there is no hardware state left to retry. */
        LOG_W("could not fully disable owned fan PWM before unexport: %s",
              ctrl->pwm_path);
    }
    return unexport_owned_pwm(ctrl);
}

/* 等级 → 占空比（纳秒），等级 0=关闭 */
static int level_to_duty(int level, int period)
{
    if (level <= 0) return 0;
    if (level >= FAN_MAX_LEVEL) return period;
    /* 线性映射 */
    return period * level / FAN_MAX_LEVEL;
}

int ocr_fan_ctrl_init(ocr_fan_ctrl_t *ctrl, const char *pwm_path)
{
    char discovered_path[128];
    const char *selected_path = pwm_path;
    int is_auto;

    if (!ctrl || !pwm_path || !*pwm_path) return -1;
    is_auto = strcasecmp(pwm_path, "auto") == 0;
    reset_controller(ctrl);
    ctrl->auto_mode = is_auto;

    /* No PWM sysfs discovery or mutation is allowed before this lock. */
    if (acquire_fan_lock(ctrl) != 0)
        return finish_init_failure(ctrl, -2);

    if (is_auto) {
        if (find_marked_fan_pwm(discovered_path, sizeof(discovered_path)) != 0)
            return finish_init_failure(ctrl, -2);
        selected_path = discovered_path;
    }
    size_t len = strlen(selected_path);
    const char suffix[] = "/duty_cycle";
    if (len >= sizeof(suffix) - 1 &&
        strcmp(selected_path + len - (sizeof(suffix) - 1), suffix) == 0) {
        len -= sizeof(suffix) - 1;
    }
    if (len == 0 || len >= sizeof(ctrl->pwm_path))
        return finish_init_failure(ctrl, -2);
    memcpy(ctrl->pwm_path, selected_path, len);
    ctrl->pwm_path[len] = '\0';
    if (parse_pwm_location(ctrl) != 0)
        return finish_init_failure(ctrl, -3);
    if (ensure_pwm_exported(ctrl) != 0) {
        if (release_owned_pwm(ctrl) != 0) {
            LOG_E("fan PWM export failed and rollback is incomplete; "
                  "retaining ownership lock for cleanup retry: %s",
                  ctrl->pwm_path);
            return -3;
        }
        return finish_init_failure(ctrl, -3);
    }
    if (!ctrl->exported_by_us && capture_external_state(ctrl) != 0) {
        LOG_E("无法保存既有 PWM 状态，拒绝接管: %s", ctrl->pwm_path);
        return finish_init_failure(ctrl, -3);
    }
    ctrl->period_ns = 40000; /* 40us = 25kHz */
    ctrl->cur_level = 0;
    ctrl->hysteresis_mdeg = 3000; /* 3°C 回差 */

    /* 温度阈值（毫摄氏度）
     * 等级0→1: 45°C, 1→2: 55°C, 2→3: 65°C, 3→4: 75°C
     * 降级阈值加回差 */
    ctrl->threshold_up[0]   = 45000;
    ctrl->threshold_up[1]   = 55000;
    ctrl->threshold_up[2]   = 65000;
    ctrl->threshold_up[3]   = 75000;
    ctrl->threshold_down[0] = 42000;
    ctrl->threshold_down[1] = 52000;
    ctrl->threshold_down[2] = 62000;
    ctrl->threshold_down[3] = 72000;

    /* 启用 PWM 输出 */
    if (write_attribute(ctrl, "enable", 0) != 0 ||
        write_attribute(ctrl, "duty_cycle", 0) != 0 ||
        write_attribute(ctrl, "period", ctrl->period_ns) != 0 ||
        write_attribute(ctrl, "duty_cycle", 0) != 0 ||
        write_attribute(ctrl, "enable", 1) != 0) {
        int rollback = ctrl->original_state_valid
                     ? restore_external_state(ctrl) : release_owned_pwm(ctrl);
        if (rollback != 0)
            LOG_E("PWM 初始化失败且原状态回滚不完整: %s", ctrl->pwm_path);
        if (rollback != 0) return -4;
        return finish_init_failure(ctrl, -4);
    }

    ctrl->initialized = 1;

    LOG_I("fan PWM ownership mode: %s (lock held until destroy)",
          ctrl->original_state_valid ? "external-restore" :
          (ctrl->auto_takeover ? "auto-crash-takeover" : "owned-export"));

    LOG_I("风扇控制初始化: pwm=%s period=%dns", ctrl->pwm_path,
          ctrl->period_ns);
    return 0;
}

int ocr_fan_ctrl_update(ocr_fan_ctrl_t *ctrl, int temp_mdeg)
{
    if (!ctrl || !ctrl->initialized || !ctrl->lock_held) return -1;

    int new_level = ctrl->cur_level;

    /* 状态机：根据温度升降等级 */
    while (new_level < FAN_MAX_LEVEL && temp_mdeg >= ctrl->threshold_up[new_level])
        new_level++;
    while (new_level > 0 && temp_mdeg < ctrl->threshold_down[new_level - 1])
        new_level--;

    if (new_level != ctrl->cur_level) {
        LOG_I("风扇等级变更: %d → %d (temp=%dm°C)", ctrl->cur_level, new_level, temp_mdeg);
        return ocr_fan_ctrl_set_level(ctrl, new_level);
    }
    return 0;
}

int ocr_fan_ctrl_set_level(ocr_fan_ctrl_t *ctrl, int level)
{
    int duty;
    if (!ctrl || !ctrl->initialized || !ctrl->lock_held) return -1;
    if (level < 0) level = 0;
    if (level > FAN_MAX_LEVEL) level = FAN_MAX_LEVEL;

    duty = level_to_duty(level, ctrl->period_ns);

    if (write_attribute(ctrl, "duty_cycle", duty) != 0) {
        LOG_W("写入风扇占空比失败: %s/duty_cycle", ctrl->pwm_path);
        return -2;
    }
    ctrl->cur_level = level;
    ctrl->cur_duty_ns = duty;
    return 0;
}

int ocr_fan_ctrl_destroy(ocr_fan_ctrl_t *ctrl)
{
    if (!ctrl) return -1;
    if ((ctrl->original_state_valid || ctrl->exported_by_us) &&
        !ctrl->lock_held) {
        LOG_E("refusing fan PWM cleanup without ownership lock");
        return -1;
    }
    int ret = 0;
    if (ctrl->original_state_valid) {
        ret = restore_external_state(ctrl);
    } else if (ctrl->exported_by_us) {
        ret = release_owned_pwm(ctrl);
    }
    if (ret != 0) {
        LOG_E("风扇 PWM 所有权状态恢复失败，保留锁以便重试: %s",
              ctrl->pwm_path);
        return -1;
    }
    release_fan_lock(ctrl);
    reset_controller(ctrl);
    return 0;
}
