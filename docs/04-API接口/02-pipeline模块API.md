# pipeline模块API

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |
| v2.0 | 2026-10-26 | 项目组 | 按实际实现重写：缓冲引用计数、缓冲池、DMA-HEAP、节点与管线；新增节点“仅空闲接收” |

---

## 目录

- [1. 概述](#1-概述)
- [2. 文件列表](#2-文件列表)
- [3. 缓冲与缓冲池](#3-缓冲与缓冲池)
- [4. 节点与管线](#4-节点与管线)
- [5. 线程安全](#5-线程安全)
- [6. 示例](#6-示例)

---

## 1. 概述

pipeline 模块提供零拷贝管线的基础设施：

- 带引用计数的 DMA-BUF 描述 `ocr_buffer_t`；最后一个引用释放时自动归还缓冲池。
- 固定容量的缓冲池；采集池在归还时触发 V4L2 `QBUF`，RGA 输出池从 `/dev/dma_heap` 分配。
- 管线节点：每个节点一个工作线程和一个有界输入队列，处理结果按引用分发给下游，不复制像素。

主程序装配的拓扑是：采集线程 → `rga` 节点 → `display` 节点 和 `ocr` 节点。

## 2. 文件列表

| 文件 | 说明 |
| ---- | ---- |
| `buffer.h/.c` | `ocr_buffer_t`：fd、mmap、平面布局、原子引用计数、释放回调 |
| `buffer_pool.h/.c` | 最多 32 个槽位的缓冲池，空闲栈与槽位状态 |
| `dma_heap.h/.c` | 从 `/dev/dma_heap/*` 分配 DMA-BUF，建立同布局输出池 |
| `zerocopy.h/.c` | `DMA_BUF_IOCTL_SYNC` 封装；fd 复制与 SCM_RIGHTS 传递（应用未使用） |
| `node.h/.c` | 管线节点：工作线程、输入队列、下游分发 |
| `pipeline.h/.c` | 节点登记、按序启动（失败回滚）、逆序停止与销毁、统计 |

## 3. 缓冲与缓冲池

```c
int  ocr_buffer_init(ocr_buffer_t *buf);
int  ocr_buffer_set_release_callback(ocr_buffer_t *buf,
                                     ocr_buffer_release_fn cb, void *user);
int  ocr_buffer_set_layout(ocr_buffer_t *buf, uint32_t plane_count,
                           const uint32_t *strides, const uint32_t *offsets);
int  ocr_buffer_ref(ocr_buffer_t *buf);     /* 返回新计数；计数为 0 的缓冲不能复活 */
int  ocr_buffer_unref(ocr_buffer_t *buf);   /* 返回剩余计数；归零时在当前线程调用释放回调 */
int  ocr_buffer_mmap(ocr_buffer_t *buf);
void ocr_buffer_munmap(ocr_buffer_t *buf);

int  ocr_pool_init(ocr_buffer_pool_t *pool, int count);
int  ocr_pool_register(ocr_buffer_pool_t *pool, int index, int fd, size_t size,
                       uint32_t width, uint32_t height, ocr_pixel_format_t format);
int  ocr_pool_set_recycle_callback(ocr_buffer_pool_t *pool,
                                   ocr_pool_recycle_fn cb, void *user);
ocr_buffer_t *ocr_pool_acquire(ocr_buffer_pool_t *pool);              /* 取任一空闲块，耗尽返回 NULL，不阻塞 */
ocr_buffer_t *ocr_pool_acquire_index(ocr_buffer_pool_t *pool, int i); /* 取指定空闲块（V4L2 DQBUF 的 index） */
int  ocr_pool_release(ocr_buffer_pool_t *pool, ocr_buffer_t *buf);
int  ocr_pool_is_idle(ocr_buffer_pool_t *pool);   /* 1=全部空闲 */
int  ocr_pool_destroy(ocr_buffer_pool_t *pool);   /* 仍有引用时返回 -2 并保留，可重试 */

int  ocr_dma_heap_alloc(const char *heap_path, size_t size);
int  ocr_dma_heap_alloc_pool(ocr_buffer_pool_t *pool, int count,
                             uint32_t width, uint32_t height,
                             ocr_pixel_format_t format,
                             const char *heap_path, int map_cpu);
int  ocr_zerocopy_sync(int fd, int sync);  /* 0=READ|START 1=READ|END 2=WRITE|START 3=WRITE|END */
```

- 槽位状态：`UNUSED` →（register）→ `FREE` →（acquire，计数置 1）→ `IN_USE` →（最后一次 unref）→ `FREE`。
- 归还链：`ocr_buffer_unref` 归零 → `pool_recycle_buffer` → 可选的池回收回调。采集模块把回收回调设为 `capture_recycle`，在其中 `QBUF`。回收回调在池锁内执行，不能再调用池函数。
- `ocr_dma_heap_alloc_pool` 的行宽按 16 像素对齐；`heap_path` 为 NULL 时依次尝试 `system`、`system-uncached`、`system-allocated`；`map_cpu=1` 时为 CPU 建立映射。

## 4. 节点与管线

```c
typedef int (*ocr_node_process_fn)(ocr_pipeline_node_t *node, ocr_buffer_t *buf);

int  ocr_node_init(ocr_pipeline_node_t *node, const char *name,
                   ocr_node_process_fn process, void *ctx);
int  ocr_node_link(ocr_pipeline_node_t *node, ocr_pipeline_node_t *downstream);
int  ocr_node_set_idle_only(ocr_pipeline_node_t *node, int enable);
int  ocr_node_push_input(ocr_pipeline_node_t *node, ocr_buffer_t *buf);
int  ocr_node_emit(ocr_pipeline_node_t *node, ocr_buffer_t *buf);
int  ocr_node_start(ocr_pipeline_node_t *node);
int  ocr_node_stop(ocr_pipeline_node_t *node);
void ocr_node_destroy(ocr_pipeline_node_t *node);

int  ocr_pipeline_init(ocr_pipeline_t *pl);
int  ocr_pipeline_add(ocr_pipeline_t *pl, ocr_pipeline_node_t *node);
int  ocr_pipeline_start(ocr_pipeline_t *pl);
int  ocr_pipeline_stop(ocr_pipeline_t *pl);
int  ocr_pipeline_destroy(ocr_pipeline_t *pl);
void ocr_pipeline_dump_stats(const ocr_pipeline_t *pl);
```

### 4.1 输入队列

- 每个节点有深度 8 的输入队列，每个队列项持有一份引用。`push_input` 成功即接管调用方的那份引用。
- 默认策略：队列满时淘汰最旧的一项并接收新项，不阻塞生产者。
- **仅空闲接收**（`ocr_node_set_idle_only(node, 1)`，须在启动前设置）：工作线程正在处理、或已有一帧待处理时，`push_input` 直接返回 `-2`，不接管引用，由调用方释放；计入 `skipped`，不计入 `dropped`。这样节点同一时刻最多占一块缓冲。主程序给 OCR 节点开启此选项：OCR 一帧要几百毫秒，若按队列排队，排着的每一帧都占住一块输出缓冲，会把只有 4 块的输出池占满，连带屏幕停顿。

`push_input` 返回：`0` 接管成功；`-1` 参数无效或节点不可用；`-2` 仅空闲接收的节点正忙。非 0 时都未接管。

### 4.2 处理回调与分发

- 工作线程先在锁内把队首帧取出（同时置 `busy`），解锁后调用 `process`；回调只借用输入，返回后由工作线程统一 unref，之后才清除 `busy`。
- 回调返回值：负数表示丢帧；`0` 表示把原输入转发给所有下游；正数表示回调已自行 `emit` 了新输出，不再转发原输入（RGA 节点返回 1）。
- `ocr_node_emit` 给每个下游先 ref 再 `push_input`，最后释放调用方那份引用；返回非负的接收下游数（为 0 也已消费引用），负数表示参数错误、引用未消费。

### 4.3 生命周期

- `link` 和 `set_idle_only` 只能在节点启动前调用。
- `ocr_pipeline_start` 按登记顺序启动节点，任一失败则逆序停止已启动的节点并返回 `-2`。
- `ocr_pipeline_stop` 逆序停止；某个节点无法 join（例如在自己的回调里请求停止）时返回 `-2`，保留状态供重试。
- `ocr_pipeline_destroy` 先 stop，再逆序 `ocr_node_destroy`，释放队列里剩余帧的引用。
- `ocr_pipeline_dump_stats` 为每个节点打印 `processed`、`dropped`、`skipped`。

## 5. 线程安全

- 引用计数是原子 CAS；对共享指针加引用时，须持有保护该指针的锁（例如采集模块的 `latest_lock`）。
- 池状态受 `pool->lock` 保护；`ocr_pool_release` 先在锁内校验、解锁后再 unref，避免回收路径自锁。
- 节点：`lifecycle_lock` 串行化 link/start/stop/destroy；`in_lock` + `in_cond` 保护队列；锁顺序为 lifecycle_lock → in_lock。被淘汰的帧在锁外 unref。
- `ocr_pipeline_*` 本身不加锁，应由一个管理线程调用。

## 6. 示例

```c
ocr_pipeline_t pl;
ocr_pipeline_node_t rga, display, ocr;

ocr_dma_heap_alloc_pool(&out_pool, 4, 1280, 720, OCR_FMT_NV12, NULL, 1);

ocr_pipeline_init(&pl);
ocr_node_init(&rga, "rga", rga_process, &rga_ctx);
ocr_node_init(&display, "display", display_process, &sink);
ocr_node_init(&ocr, "ocr", ocr_process, NULL);
ocr_node_set_idle_only(&ocr, 1);          /* 慢分支只处理最新一帧 */
ocr_pipeline_add(&pl, &rga);
ocr_pipeline_add(&pl, &display);
ocr_pipeline_add(&pl, &ocr);
ocr_node_link(&rga, &display);
ocr_node_link(&rga, &ocr);
ocr_pipeline_start(&pl);

/* 采集线程：成功即移交引用，失败由采集线程 unref */
if (ocr_node_push_input(&rga, frame) != 0)
    ocr_buffer_unref(frame);

ocr_pipeline_dump_stats(&pl);
ocr_pipeline_destroy(&pl);
```

---

> 相关文档：[01-应用层API总览.md](01-应用层API总览.md)、[../01-架构设计/04-线程与并发模型.md](../01-架构设计/04-线程与并发模型.md)
