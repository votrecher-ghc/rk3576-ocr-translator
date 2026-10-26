# display模块API

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |
| v2.0 | 2026-10-26 | 项目组 | 按实际实现重写：DRM 设备/plane/framebuffer/sink/文字层；新增 text_renderer |

---

## 目录

- [1. 概述](#1-概述)
- [2. 文件列表](#2-文件列表)
- [3. 函数API](#3-函数api)
- [4. 显示流程](#4-显示流程)
- [5. 线程安全](#5-线程安全)
- [6. 示例](#6-示例)

---

## 1. 概述

display 模块基于 legacy DRM/KMS 接口：primary plane 显示 NV12 视频帧，可选的 overlay plane 显示 ARGB8888 文字层，二者由显示控制器按 premultiplied alpha 合成。文字用 FreeType 在 CPU 上渲染。

## 2. 文件列表

| 文件 | 说明 |
| ---- | ---- |
| `drm_device.h/.c` | 打开 DRM 设备，选择已连接的 connector、CRTC 和与配置完全一致的 mode；退出时恢复原 CRTC |
| `drm_plane.h/.c` | 选出 NV12 primary plane 和空闲的 ARGB8888 overlay plane，设置 zpos 与预乘混合 |
| `drm_fb.h/.c` | 从 DMA-BUF fd 导入 framebuffer（`drmPrimeFDToHandle` + `drmModeAddFB2`），同步翻页 |
| `drm_sink.h/.c` | 视频送显：缓存最多 32 个 framebuffer，持有正在扫描的帧引用 |
| `drm_overlay.h/.c` | 文字层：两块 ARGB8888 dumb buffer 双缓冲，提交后等待 vblank |
| `text_renderer.h/.c` | FreeType 文字渲染到 32 位 premultiplied ARGB 画布；文字层和拍照合成共用 |

## 3. 函数API

```c
/* 设备 */
int  ocr_drm_open(ocr_drm_device_t *dev, const char *dev_path);
int  ocr_drm_setup_crtc_mode(ocr_drm_device_t *dev, uint32_t width, uint32_t height);
void ocr_drm_close(ocr_drm_device_t *dev);

/* plane */
int  ocr_drm_planes_init(ocr_drm_planes_t *planes, ocr_drm_device_t *dev);
int  ocr_drm_plane_set_fb_ex(ocr_drm_planes_t *planes, ocr_drm_plane_t *plane, uint32_t fb_id,
                             uint32_t src_x, uint32_t src_y, uint32_t src_w, uint32_t src_h,
                             uint32_t dst_x, uint32_t dst_y, uint32_t dst_w, uint32_t dst_h);
int  ocr_drm_plane_disable(ocr_drm_planes_t *planes, ocr_drm_plane_t *plane);
void ocr_drm_planes_destroy(ocr_drm_planes_t *planes);

/* framebuffer */
int  ocr_drm_fb_create_with_layout(ocr_drm_device_t *dev, const ocr_buffer_t *buf,
                                   const uint32_t pitches[4], const uint32_t offsets[4],
                                   ocr_drm_fb_t *fb);
int  ocr_drm_fb_destroy(ocr_drm_device_t *dev, ocr_drm_fb_t *fb);
int  ocr_drm_fb_pageflip(ocr_drm_device_t *dev, ocr_drm_fb_t *fb,
                         void (*vblank_cb)(int, uint32_t, uint32_t, void *), void *user);

/* 视频送显 */
int  ocr_drm_sink_init(ocr_drm_sink_t *sink, ocr_drm_device_t *dev, ocr_drm_planes_t *planes);
int  ocr_drm_sink_present(ocr_drm_sink_t *sink, ocr_buffer_t *buffer);
void ocr_drm_sink_destroy(ocr_drm_sink_t *sink);

/* 文字层 */
int  ocr_overlay_init(ocr_drm_overlay_t *overlay, ocr_drm_device_t *dev, ocr_drm_planes_t *planes,
                      const char *font_path, int font_size, uint32_t width, uint32_t height);
int  ocr_overlay_clear(ocr_drm_overlay_t *overlay);
int  ocr_overlay_render_text(ocr_drm_overlay_t *overlay, const char *text,
                             int x, int y, uint32_t color);
int  ocr_overlay_commit(ocr_drm_overlay_t *overlay);
void ocr_overlay_destroy(ocr_drm_overlay_t *overlay);

/* 文字渲染 */
int  ocr_text_renderer_init(ocr_text_renderer_t *r, const char *font_path, int font_size);
int  ocr_text_renderer_draw(ocr_text_renderer_t *r, void *canvas,
                            uint32_t width, uint32_t height, uint32_t pitch,
                            const char *text, int x, int y, uint32_t color);
void ocr_text_renderer_destroy(ocr_text_renderer_t *r);
```

另有 `ocr_drm_setup_crtc`、`ocr_drm_plane_set_fb`、`ocr_drm_fb_create`、`ocr_drm_sink_process` 等简化封装，主程序未使用。

### 3.1 text_renderer

- 画布每像素是本机字节序的 premultiplied `0xAARRGGBB`，与 DRM `ARGB8888` 和 Pre-multiplied 混合模式一致；`color` 传 straight `0xAARRGGBB`。
- `(x, y)` 是文字左上角，第一行基线在 `y + font_size`；支持 `\n` 换行（行高 `font_size × 1.25`）和 `\t`，不自动折行；非法 UTF-8 字节画成 U+FFFD；超出画布的部分裁掉。
- `init` 返回：`0` 成功，`-1` 参数无效，`-2` FreeType 初始化失败，`-3` 字体加载失败，`-4` 字号设置失败。

## 4. 显示流程

- **视频**：`ocr_drm_sink_present` 先为该帧额外加一份引用。首帧走 `drmModeSetCrtc`，要求帧与 mode 尺寸 1:1，legacy modeset 不支持缩放。之后用 `drmModePageFlip` 并 `poll` 等待翻页事件，等待没有超时。切换成功后才释放上一帧，失败时撤销新加的引用。
- **文字层**：显示线程按版本号判断是否有新结果；有则 `clear` → `render_text` × N → `commit`。`commit` 提交后台缓冲并等待 vblank，再交换双缓冲；vblank 等待失败时文字层停用，直到重启。
- **退出顺序**：先销毁文字层，再销毁 sink（关闭 primary、释放 current），然后释放输出池，最后 `ocr_drm_planes_destroy`（恢复属性）和 `ocr_drm_close`（恢复开机时的 CRTC）。

## 5. 线程安全

- 本模块内部不加锁，也不创建线程。
- `sink_present` 与文字层函数只由显示节点线程调用；初始化和销毁在主线程，且在显示线程退出后进行。
- 每个 `ocr_text_renderer_t` 拥有独立的 FreeType 库和 face，同一实例只能由一个线程使用。文字层的实例属于显示线程，拍照合成另持一个实例，在 OCR 线程使用。

## 6. 示例

```c
ocr_drm_device_t drm;
ocr_drm_planes_t planes;
ocr_drm_sink_t sink;
ocr_drm_overlay_t overlay;

ocr_drm_open(&drm, "/dev/dri/card0");
ocr_drm_setup_crtc_mode(&drm, 1280, 720);
ocr_drm_planes_init(&planes, &drm);
ocr_drm_sink_init(&sink, &drm, &planes);
if (ocr_overlay_init(&overlay, &drm, &planes, font_path, 32,
                     drm.width, drm.height) != 0)
    /* 没有 overlay plane 或字体：只显示视频 */;

/* 显示线程，每帧 */
ocr_drm_sink_present(&sink, frame);           /* frame 尺寸须等于 mode 尺寸 */
ocr_overlay_clear(&overlay);
ocr_overlay_render_text(&overlay, "翻译结果", 100, 100, 0xEFFFFFFFU);
ocr_overlay_commit(&overlay);

/* 离屏画布，例如拍照合成 */
ocr_text_renderer_t text;
uint32_t *layer = calloc((size_t)w * h, 4);
ocr_text_renderer_init(&text, font_path, 32);
ocr_text_renderer_draw(&text, layer, w, h, w * 4, "已保存", 16, 16, 0xEFFFFFFFU);
ocr_text_renderer_destroy(&text);
```

---

> 相关文档：[01-应用层API总览.md](01-应用层API总览.md)
