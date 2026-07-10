/**
 * @file key_event.c
 * @brief Input 子系统事件读取实现
 */
#include "key_event.h"
#include "log.h"

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <linux/input.h>

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
    LOG_I("按键监听线程启动: %s", ke->dev_path);

    struct pollfd pfd;
    pfd.fd = ke->dev_fd;
    pfd.events = POLLIN;

    while (atomic_load(&ke->running)) {
        int ret = poll(&pfd, 1, 100);
        if (ret <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        struct input_event ev;
        ssize_t n = read(ke->dev_fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) continue;

        /* 仅处理按键事件 */
        if (ev.type == EV_KEY) {
            ocr_key_event_t event = code_to_event(ev.code);
            if (event == KEY_EVENT_NONE) continue;
            ocr_key_state_t state = (ev.value > 0) ? KEY_STATE_PRESSED : KEY_STATE_RELEASED;
            if (ke->on_key) {
                ke->on_key(event, state, ke->user_ctx);
            }
        }
    }

    LOG_I("按键监听线程退出");
    return NULL;
}

int ocr_key_event_init(ocr_key_event_t_ctx *ke, const char *dev_path,
                       void (*cb)(ocr_key_event_t, ocr_key_state_t, void *),
                       void *user)
{
    if (!ke || !dev_path) return -1;
    memset(ke, 0, sizeof(*ke));

    ke->dev_fd = open(dev_path, O_RDONLY | O_CLOEXEC);
    if (ke->dev_fd < 0) {
        LOG_E("打开 input 设备 %s 失败: %s", dev_path, strerror(errno));
        return -2;
    }
    strncpy(ke->dev_path, dev_path, sizeof(ke->dev_path) - 1);
    ke->on_key = cb;
    ke->user_ctx = user;
    atomic_init(&ke->running, 0);
    return 0;
}

int ocr_key_event_start(ocr_key_event_t_ctx *ke)
{
    if (!ke || ke->dev_fd < 0) return -1;
    atomic_store(&ke->running, 1);
    if (ocr_thread_create(&ke->thread, key_thread, ke) != 0) {
        LOG_E("按键监听线程创建失败");
        return -2;
    }
    return 0;
}

int ocr_key_event_stop(ocr_key_event_t_ctx *ke)
{
    if (!ke) return -1;
    atomic_store(&ke->running, 0);
    ocr_thread_join(ke->thread, NULL);
    return 0;
}

void ocr_key_event_destroy(ocr_key_event_t_ctx *ke)
{
    if (!ke) return;
    ocr_key_event_stop(ke);
    if (ke->dev_fd >= 0) {
        close(ke->dev_fd);
        ke->dev_fd = -1;
    }
}
