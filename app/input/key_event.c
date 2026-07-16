/**
 * @file key_event.c
 * @brief Input 子系统事件读取实现
 */
#include "key_event.h"
#include "log.h"

#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#define BITS_PER_LONG_VALUE (sizeof(unsigned long) * 8U)
#define BIT_WORD_COUNT(maximum) (((maximum) + BITS_PER_LONG_VALUE) / BITS_PER_LONG_VALUE)

static int has_key_camera(int fd)
{
    unsigned long bits[BIT_WORD_COUNT(KEY_MAX)];
    size_t word = KEY_CAMERA / BITS_PER_LONG_VALUE;
    size_t bit = KEY_CAMERA % BITS_PER_LONG_VALUE;

    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return 0;
    return (bits[word] & (1UL << bit)) != 0;
}

static int is_event_name(const char *name)
{
    static const char prefix[] = "event";
    const char *cursor;

    if (!name || strncmp(name, prefix, sizeof(prefix) - 1) != 0) return 0;
    cursor = name + sizeof(prefix) - 1;
    if (*cursor == '\0') return 0;
    while (*cursor != '\0') {
        if (!isdigit((unsigned char)*cursor)) return 0;
        ++cursor;
    }
    return 1;
}

int ocr_key_event_find_device(char *dev_path, size_t dev_path_size)
{
    DIR *directory;
    struct dirent *entry;
    char selected[128] = "";
    char selected_name[128] = "unknown";
    int matches = 0;
    int scan_error = 0;

    if (!dev_path || dev_path_size == 0) return -1;
    dev_path[0] = '\0';
    directory = opendir("/dev/input");
    if (!directory) return -2;

    for (;;) {
        char candidate[128];
        char device_name[128] = "unknown";
        int length;
        int fd;

        errno = 0;
        entry = readdir(directory);
        if (!entry) {
            if (errno != 0) scan_error = 1;
            break;
        }
        if (!is_event_name(entry->d_name)) continue;
        length = snprintf(candidate, sizeof(candidate), "/dev/input/%s",
                          entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(candidate)) continue;
        fd = open(candidate, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (!has_key_camera(fd)) {
            close(fd);
            continue;
        }
        (void)ioctl(fd, EVIOCGNAME(sizeof(device_name)), device_name);
        device_name[sizeof(device_name) - 1] = '\0';
        close(fd);
        ++matches;
        if (matches == 1) {
            memcpy(selected, candidate, (size_t)length + 1U);
            length = snprintf(selected_name, sizeof(selected_name), "%s",
                              device_name);
            if (length < 0 || (size_t)length >= sizeof(selected_name)) {
                scan_error = 1;
                break;
            }
        } else {
            break;
        }
    }

    if (closedir(directory) != 0) scan_error = 1;
    if (scan_error) return -2;
    if (matches == 0) return -4;
    if (matches != 1) {
        LOG_E("发现多个支持 KEY_CAMERA 的 input 设备，请显式配置 key_device");
        return -5;
    }
    int length = snprintf(dev_path, dev_path_size, "%s", selected);
    if (length < 0 || (size_t)length >= dev_path_size) {
        dev_path[0] = '\0';
        return -3;
    }
    LOG_I("发现 KEY_CAMERA 输入设备: %s (%s)", dev_path, selected_name);
    return 0;
}

/* input 事件 code → 内部按键事件映射 */
static ocr_key_event_t code_to_event(uint16_t code)
{
    switch (code) {
        case KEY_CAMERA: return KEY_EVENT_CAMERA;
        case KEY_MODE:   return KEY_EVENT_MODE;
        case KEY_POWER:  return KEY_EVENT_POWER;
        default:         return KEY_EVENT_NONE;
    }
}

static void *key_thread(void *arg)
{
    ocr_key_event_t_ctx *ke = (ocr_key_event_t_ctx *)arg;
    int worker_error = 0;
    LOG_I("按键监听线程启动: %s", ke->dev_path);

    struct pollfd pfd;
    pfd.fd = ke->dev_fd;
    pfd.events = POLLIN;

    while (atomic_load(&ke->running)) {
        int ret;
        do {
            ret = poll(&pfd, 1, 100);
        } while (ret < 0 && errno == EINTR);
        if (ret < 0) {
            LOG_W("input poll 失败: %s", strerror(errno));
            worker_error = errno ? -errno : -EIO;
            break;
        }
        if (ret == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            worker_error = -ENODEV;
            break;
        }
        if (!(pfd.revents & POLLIN)) continue;

        struct input_event ev;
        ssize_t n = read(ke->dev_fd, &ev, sizeof(ev));
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n != (ssize_t)sizeof(ev)) {
            worker_error = n < 0 && errno ? -errno : -EIO;
            break;
        }

        /* 仅处理按键事件 */
        if (ev.type == EV_KEY) {
            if (ev.value == 2) continue; /* 忽略按键自动重复 */
            ocr_key_event_t event = code_to_event(ev.code);
            if (event == KEY_EVENT_NONE) continue;
            ocr_key_state_t state = ev.value == 1 ? KEY_STATE_PRESSED : KEY_STATE_RELEASED;
            if (ke->on_key) {
                ke->on_key(event, state, ke->user_ctx);
            }
        }
    }

    atomic_store(&ke->worker_error, worker_error);
    atomic_store(&ke->running, 0);
    LOG_I("按键监听线程退出");
    return NULL;
}

int ocr_key_event_init(ocr_key_event_t_ctx *ke, const char *dev_path,
                       void (*cb)(ocr_key_event_t, ocr_key_state_t, void *),
                       void *user)
{
    size_t path_length;

    if (!ke || !dev_path) return -1;
    path_length = strlen(dev_path);
    if (path_length == 0 || path_length >= sizeof(ke->dev_path)) return -1;
    memset(ke, 0, sizeof(*ke));
    ke->dev_fd = -1;

    ke->dev_fd = open(dev_path, O_RDONLY | O_CLOEXEC);
    if (ke->dev_fd < 0) {
        LOG_E("打开 input 设备 %s 失败: %s", dev_path, strerror(errno));
        return -2;
    }
    if (!has_key_camera(ke->dev_fd)) {
        LOG_E("input 设备不支持 KEY_CAMERA: %s", dev_path);
        close(ke->dev_fd);
        ke->dev_fd = -1;
        return -3;
    }
    memcpy(ke->dev_path, dev_path, path_length + 1U);
    ke->on_key = cb;
    ke->user_ctx = user;
    atomic_init(&ke->running, 0);
    atomic_init(&ke->thread_started, 0);
    atomic_init(&ke->worker_error, 0);
    ke->initialized = 1;
    return 0;
}

int ocr_key_event_start(ocr_key_event_t_ctx *ke)
{
    if (!ke || !ke->initialized || ke->dev_fd < 0) return -1;
    if (atomic_load(&ke->thread_started)) {
        if (atomic_load(&ke->running)) return 0;
        if (pthread_equal(ke->thread, pthread_self())) return -2;
        if (ocr_thread_join(ke->thread, NULL) != 0) return -2;
        atomic_store(&ke->thread_started, 0);
    }

    atomic_store(&ke->worker_error, 0);
    atomic_store(&ke->running, 1);
    if (ocr_thread_create(&ke->thread, key_thread, ke) != 0) {
        atomic_store(&ke->running, 0);
        LOG_E("按键监听线程创建失败");
        return -2;
    }
    atomic_store(&ke->thread_started, 1);
    return 0;
}

int ocr_key_event_stop(ocr_key_event_t_ctx *ke)
{
    if (!ke) return -1;
    if (!ke->initialized) return 0;
    atomic_store(&ke->running, 0);
    if (atomic_load(&ke->thread_started) &&
        pthread_equal(ke->thread, pthread_self())) {
        return -2;
    }
    if (atomic_exchange(&ke->thread_started, 0)) {
        if (ocr_thread_join(ke->thread, NULL) != 0) {
            atomic_store(&ke->thread_started, 1);
            return -2;
        }
    }
    return 0;
}

int ocr_key_event_get_worker_error(const ocr_key_event_t_ctx *ke)
{
    if (!ke || !ke->initialized) return -EINVAL;
    return atomic_load(&ke->worker_error);
}

int ocr_key_event_is_running(const ocr_key_event_t_ctx *ke)
{
    return ke && ke->initialized && atomic_load(&ke->running) ? 1 : 0;
}

void ocr_key_event_destroy(ocr_key_event_t_ctx *ke)
{
    if (!ke || !ke->initialized) return;
    if (ocr_key_event_stop(ke) != 0) {
        LOG_E("按键监听线程尚未回收，保留 input 资源");
        return;
    }
    if (ke->dev_fd >= 0) {
        close(ke->dev_fd);
        ke->dev_fd = -1;
    }
    ke->dev_path[0] = '\0';
    ke->on_key = NULL;
    ke->user_ctx = NULL;
    ke->initialized = 0;
    atomic_store(&ke->worker_error, 0);
}
