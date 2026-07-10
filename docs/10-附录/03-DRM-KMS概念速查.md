# DRM-KMS概念速查

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. DRM/KMS 概念](#1-drmkms-概念)
- [2. 核心 API](#2-核心-api)
- [3. 对象关系](#3-对象关系)
- [4. 显示流程](#4-显示流程)

---

## 1. DRM/KMS 概念

| 概念       | 说明                                   |
| ---------- | -------------------------------------- |
| DRM        | 直接渲染管理器，显示子系统             |
| KMS        | 内核模式设置，DRM 的显示控制部分       |
| CRTC       | 显示扫描控制器，生成时序               |
| Plane      | 图层，叠加到 CRTC                      |
| Connector  | 物理输出（HDMI/DSI）                   |
| Encoder    | 编码器，CRTC→Connector 桥接            |
| Framebuffer| 帧缓冲，DMA-BUF 的显示封装            |

## 2. 核心 API

```c
// 设备
int drm_fd = open("/dev/dri/card0", O_RDWR);
drmModeRes *res = drmModeGetResources(drm_fd);

// CRTC
drmModeCrtc *crtc = drmModeGetCrtc(drm_fd, res->crtcs[0]);

// Connector
drmModeConnector *conn = drmModeGetConnector(drm_fd, res->connectors[0]);

// Plane
drmModePlaneRes *planes = drmModeGetPlaneResources(drm_fd);
drmModePlane *plane = drmModeGetPlane(drm_fd, planes->planes[0]);

// Framebuffer（导入 DMA-BUF）
drmModeAddFB2(drm_fd, w, h, DRM_FORMAT_NV12, handles, pitches, offsets, &fb_id, 0);

// 显示
drmModeSetPlane(drm_fd, plane_id, crtc_id, fb_id, 0, ...);

// PageFlip（VSync 同步）
drmModePageFlip(drm_fd, crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL);
drmHandleEvent(drm_fd, &evctx);
```

## 3. 对象关系

```
Framebuffer ─→ Plane ─┐
                       ├→ CRTC ─→ Encoder ─→ Connector ─→ 屏幕
Framebuffer ─→ Plane ─┘
```

## 4. 显示流程

```
1. 打开 /dev/dri/card0
2. 获取 resources（CRTC/Connector/Plane）
3. 设置 Connector 模式
4. 创建 Framebuffer（AddFB2，导入 DMA-BUF）
5. SetPlane 显示
6. PageFlip + VSync 事件循环
```

---

> 相关文档：[01-架构设计/06-零拷贝管线设计.md](../01-架构设计/06-零拷贝管线设计.md)
