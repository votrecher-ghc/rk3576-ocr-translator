# input 模块 API

| 版本 | 日期 | 作者 | 变更说明 |
|---|---|---|---|
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |
| v2.0 | 2026-07-16 | 项目组 | 删除不存在的 input_create API，对齐 KEY_CAMERA worker 与拍照会话 |
| v2.1 | 2026-10-26 | 项目组 | 拍照会话改为“按键登记请求、OCR 线程存档”：删除 trigger，新增 request/take_request/save |

---

## 1. 文件与职责

| 文件 | 职责 |
|---|---|
| `app/input/key_event.h/.c` | 发现 evdev、读取 `KEY_CAMERA`、管理工作线程 |
| `app/input/capture_session.h/.c` | 登记拍照请求；把实时识别完成的帧叠加译文编码为 JPEG，并写入 txt/json 归档 |

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
typedef struct {
    const char *text;   /* 借用的 UTF-8 文字，只在 save 期间使用 */
    int x;              /* 文字左上角，帧像素坐标，与屏幕文字层相同 */
    int y;
} ocr_capture_label_t;

int ocr_capture_session_init(
    ocr_capture_session_t *sess,
    ocr_archiver_t *arch,
    const char *font_path,
    int font_size);

int ocr_capture_session_configure(
    ocr_capture_session_t *sess,
    const char *src_lang,
    const char *tgt_lang,
    int jpeg_quality);

void ocr_capture_session_request(ocr_capture_session_t *sess);
int  ocr_capture_session_take_request(ocr_capture_session_t *sess);

int ocr_capture_session_save(
    ocr_capture_session_t *sess,
    const ocr_buffer_t *frame,
    const ocr_text_box_list_t *boxes,
    char (*texts)[OCR_MAX_TEXT_LEN],
    char (*translated)[OCR_MAX_TEXT_LEN],
    const ocr_capture_label_t *labels,
    int label_count,
    uint32_t color);

void ocr_capture_session_destroy(ocr_capture_session_t *sess);

void ocr_capture_session_on_key(
    ocr_key_event_t event,
    ocr_key_state_t state,
    void *user);
```

会话不推理、不取帧，只做两件事：记录拍照请求，以及把调用方给出的一帧和识别结果存档。

| 函数 | 调用线程 | 说明 |
|---|---|---|
| `init` | 主线程 | 借用归档器，加载合成译文用的字体；字体加载失败只告警，之后照片不带译文 |
| `configure` | 主线程 | 设置归档语言标签和 JPEG 质量（1..100） |
| `on_key` / `request` | 按键线程 | 仅 `KEY_EVENT_CAMERA + KEY_STATE_PRESSED` 登记请求；只置原子标记，立即返回；未取走前的多次请求合并为一次 |
| `take_request` | OCR 线程 | 返回 1 并清除请求，或返回 0；应在一次识别成功完成后调用，识别失败时保留请求 |
| `save` | OCR 线程 | 见下文 |
| `destroy` | 主线程 | 释放字体；须在按键线程和 OCR 线程都停止后调用 |

`save()` 的步骤：

1. `ocr_archiver_create()` 建立 `<root>/YYYYMMDD/HHMMSS_mmm_<pid>_<序号>/`；
2. 在与帧同尺寸的透明 premultiplied ARGB 图层上用 `text_renderer` 画出 `labels`；
3. 逐行把帧转成 RGB、叠加图层（`out = 文字 + 画面 × (1 − α)`），编码为 `image.jpg`；
4. 写 `result.txt`（原文 + 译文）和 `result.json`（框坐标、置信度、原文、译文）。

`frame` 必须已 mmap，调用方负责在调用前后完成 DMA-BUF CPU 读同步。

`save()` 返回：

- `0`：成功；
- `-1`：参数无效；
- `-2`：建立归档目录失败；
- `-3`：JPEG 编码失败；
- `-4`：内存不足；
- `-5`：写入 txt 或 json 失败。

失败时已建立的目录和已写出的文件不回滚。

## 5. 线程安全

- `ocr_key_event_t_ctx` 自己拥有 evdev worker。
- `init/start/stop/destroy` 不应并发调用。
- 回调运行在 key worker 线程中，只登记请求，不会阻塞 stop/join。
- 请求标记是 `atomic_int`：按键线程写，OCR 线程用原子交换取走，二者无需加锁。
- `save()` 只能由一个线程调用（实时 OCR 线程）；会话自带的 FreeType 实例不与显示线程共用。
- RKNN 模型只由 OCR 线程使用，拍照不另做推理。

## 6. 示例

```c
/* 主线程装配 */
char key_path[64];
ocr_key_event_t_ctx key;
ocr_capture_session_t session;

if (ocr_capture_session_init(&session, &archiver,
                             font_path, font_size) != 0)
    return -1;
ocr_capture_session_configure(&session, "en", "zh", 90);

if (ocr_key_event_find_device(key_path, sizeof(key_path)) != 0 ||
    ocr_key_event_init(&key, key_path,
                       ocr_capture_session_on_key, &session) != 0)
    disable_key_capture();          /* 只禁用拍照，实时预览继续 */
else
    ocr_key_event_start(&key);

/* OCR 线程：每识别完一帧 */
publish_overlay(&layout);
if (ocr_capture_session_take_request(&session))
    ocr_capture_session_save(&session, frame, &boxes, texts, translated,
                             labels, label_count, 0xEFFFFFFFU);

/* 退出：先停按键，再停 OCR 线程，最后销毁会话 */
ocr_key_event_stop(&key);
ocr_key_event_destroy(&key);
ocr_capture_session_destroy(&session);
```

---

> 相关文档：
>
> - [按键拍照与归档](../02-模块详细设计/07-按键拍照与归档.md)
> - [按键驱动开发详解](../03-驱动开发/03-按键驱动开发详解.md)
