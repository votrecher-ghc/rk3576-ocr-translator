# input 模块 API

| 版本 | 日期 | 作者 | 变更说明 |
|---|---|---|---|
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |
| v2.0 | 2026-07-16 | 项目组 | 删除不存在的 input_create API，对齐 KEY_CAMERA worker 与拍照会话 |

---

## 1. 文件与职责

| 文件 | 职责 |
|---|---|
| `app/input/key_event.h/.c` | 发现 evdev、读取 `KEY_CAMERA`、管理工作线程 |
| `app/input/capture_session.h/.c` | 按键触发抓拍、JPEG、OCR、翻译和归档 |

内核和 overlay 上报 `KEY_CAMERA`。当前没有模式切换键、长按事件分类或通用
`input_event_t` 分发层。

## 2. 事件类型

```c
typedef enum {
    KEY_EVENT_NONE = 0,
    KEY_EVENT_CAMERA,
    KEY_EVENT_MODE,
    KEY_EVENT_POWER,
} ocr_key_event_t;

typedef enum {
    KEY_STATE_RELEASED = 0,
    KEY_STATE_PRESSED,
} ocr_key_state_t;
```

当前 evdev 映射只使用 `KEY_EVENT_CAMERA`。其他枚举值是扩展位，不代表对应硬件
已经实现。

## 3. 设备发现与监听 API

```c
int ocr_key_event_find_device(char *dev_path,
                              size_t dev_path_size);

int ocr_key_event_init(
    ocr_key_event_t_ctx *ke,
    const char *dev_path,
    void (*cb)(ocr_key_event_t event,
               ocr_key_state_t state,
               void *user),
    void *user);

int ocr_key_event_start(ocr_key_event_t_ctx *ke);
int ocr_key_event_stop(ocr_key_event_t_ctx *ke);
int ocr_key_event_get_worker_error(
    const ocr_key_event_t_ctx *ke);
int ocr_key_event_is_running(
    const ocr_key_event_t_ctx *ke);
void ocr_key_event_destroy(ocr_key_event_t_ctx *ke);
```

`ocr_key_event_find_device()` 枚举 `/dev/input/event*` 并检查：

- `EV_KEY` capability；
- `KEY_CAMERA` capability。

只有唯一匹配才成功；不依赖 `event0`。

worker 读取完整 `struct input_event`，仅在 `EV_KEY/KEY_CAMERA` 时回调。设备被关闭
或读取失败后，`worker_error` 保存负 errno，主程序可检测工作线程死亡。

## 4. 拍照会话 API

```c
int ocr_capture_session_init(
    ocr_capture_session_t *sess,
    ocr_v4l2_capture_t *cap,
    ocr_det_t *det,
    ocr_rec_t *rec,
    ocr_translator_t *trs,
    ocr_archiver_t *arch);

int ocr_capture_session_configure(
    ocr_capture_session_t *sess,
    const char *src_lang,
    const char *tgt_lang,
    int jpeg_quality);

int ocr_capture_session_trigger(
    ocr_capture_session_t *sess);

void ocr_capture_session_on_key(
    ocr_key_event_t event,
    ocr_key_state_t state,
    void *user);
```

`trigger()` 返回：

- `0`：本次会话已执行；
- `1`：已有会话忙碌，本次触发被忽略；
- 负值：参数、抓拍、编码、OCR、翻译或归档失败。

回调只在 `KEY_EVENT_CAMERA + KEY_STATE_PRESSED` 时触发会话。

## 5. 线程安全

- `ocr_key_event_t_ctx` 自己拥有 evdev worker。
- `init/start/stop/destroy` 不应并发调用。
- 回调运行在 key worker 线程中，不能执行会永久阻塞 stop/join 的操作。
- `ocr_capture_session_t.busy` 使用原子状态阻止重入。
- live OCR 与 photo 会话共享 RKNN 上下文时，必须使用主程序现有互斥策略。
- `capture_session` 不是通用多线程任务队列；同一对象仅允许一个活动会话。

## 6. 示例

```c
char key_path[64];
ocr_key_event_t_ctx key;
ocr_capture_session_t session;

if (ocr_key_event_find_device(key_path, sizeof(key_path)) != 0)
    return -1;

if (ocr_capture_session_init(&session,
                             &capture,
                             &det,
                             &rec,
                             &translator,
                             &archiver) != 0)
    return -1;

ocr_capture_session_configure(&session, "en", "zh", 90);

if (ocr_key_event_init(&key,
                       key_path,
                       ocr_capture_session_on_key,
                       &session) != 0)
    return -1;

if (ocr_key_event_start(&key) != 0)
    return -1;

/* 主循环周期检查 worker_error。 */
if (ocr_key_event_get_worker_error(&key) < 0)
    request_application_exit();

ocr_key_event_stop(&key);
ocr_key_event_destroy(&key);
```

---

> 相关文档：
>
> - [按键拍照与归档](../02-模块详细设计/07-按键拍照与归档.md)
> - [按键驱动开发详解](../03-驱动开发/03-按键驱动开发详解.md)
