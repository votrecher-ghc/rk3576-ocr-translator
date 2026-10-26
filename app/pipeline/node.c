/**
 * @file node.c
 * @brief Pipeline worker node implementation.
 *
 * 每个节点拥有工作线程和有界输入队列；入队传递一个引用，队列满时丢弃并释放最旧帧。
 * 回调借用输入；返回 0 转发原图，正数表示已自行输出，负数表示丢帧。分发每个下游都独立持有引用。
 */
#include "node.h"
#include "log.h"

#include <string.h>

/** @brief 给每个接收成功的下游各加一份引用；入队失败则撤销该份引用。 */
static int node_forward_borrowed(ocr_pipeline_node_t *node,
                                 ocr_buffer_t *buf)
{
    int accepted = 0;

    for (int i = 0; i < node->downstream_count; ++i) {
        /* 先加引用再让下游看见指针；下游线程可能立即处理并 unref。
         * 若先入队后加引用，快速消费者可能提前把共享缓冲归还池。 */
        if (ocr_buffer_ref(buf) < 0) {
            break;
        }
        if (ocr_node_push_input(node->downstream[i], buf) == 0) {
            ++accepted;
        } else {
            /* push 失败未接管引用，撤销刚才为这个下游预留的一份。
             * 其他成功下游的引用不受影响，仍可正常读取同一幅只读图像。 */
            (void)ocr_buffer_unref(buf);
        }
    }
    return accepted;
}

static int node_stop_locked(ocr_pipeline_node_t *node)
{
    int ret;

    atomic_store_explicit(&node->running, 0, memory_order_release);
    if (!atomic_load_explicit(&node->thread_started, memory_order_acquire)) {
        return 0;
    }

    /* 工作线程可能正在空队列上睡眠；仅清 running 不会唤醒 cond_wait，
     * 所以也要广播通知，使它重新检查退出条件。 */
    ocr_mutex_lock(&node->in_lock);
    ocr_cond_broadcast(&node->in_cond);
    ocr_mutex_unlock(&node->in_lock);

    /* 回调运行在该工作线程本身，允许它请求停机，但不能 join 自己。
     * 返回 -2 后保留 thread_started，让其他管理线程稍后完成 join。 */
    if (pthread_equal(node->thread, pthread_self())) {
        return -2;
    }

    ret = ocr_thread_join(node->thread, NULL);
    if (ret != 0) {
        return -2;
    }
    atomic_store_explicit(&node->thread_started, 0, memory_order_release);
    memset(&node->thread, 0, sizeof(node->thread));
    return 0;
}

int ocr_node_init(ocr_pipeline_node_t *node, const char *name,
                  ocr_node_process_fn process, void *ctx)
{
    if (!node || !name || name[0] == '\0') {
        return -1;
    }

    memset(node, 0, sizeof(*node));
    if (ocr_mutex_init(&node->lifecycle_lock) != 0) {
        return -2;
    }
    if (ocr_mutex_init(&node->in_lock) != 0) {
        ocr_mutex_destroy(&node->lifecycle_lock);
        return -2;
    }
    if (ocr_cond_init(&node->in_cond) != 0) {
        ocr_mutex_destroy(&node->in_lock);
        ocr_mutex_destroy(&node->lifecycle_lock);
        return -2;
    }

    strncpy(node->name, name, OCR_NODE_NAME_LEN - 1);
    node->name[OCR_NODE_NAME_LEN - 1] = '\0';
    node->process = process;
    node->user_ctx = ctx;
    atomic_init(&node->in_count, 0);
    atomic_init(&node->thread_started, 0);
    atomic_init(&node->running, 0);
    atomic_init(&node->processed, 0);
    atomic_init(&node->dropped, 0);
    atomic_init(&node->busy, 0);
    atomic_init(&node->skipped, 0);
    atomic_init(&node->initialized, 1);
    return 0;
}

int ocr_node_link(ocr_pipeline_node_t *node,
                  ocr_pipeline_node_t *downstream)
{
    int ret = 0;

    if (!node || !downstream || node == downstream ||
        !atomic_load_explicit(&node->initialized, memory_order_acquire) ||
        !atomic_load_explicit(&downstream->initialized,
                              memory_order_acquire)) {
        return -1;
    }

    /* 拓扑在工作线程启动前固定；分发遍历 downstream 时不再逐次加拓扑锁。
     * 这里只禁止自己连自己及重复边，整张图是否无环仍由装配者保证。 */
    ocr_mutex_lock(&node->lifecycle_lock);
    if (!atomic_load_explicit(&node->initialized, memory_order_acquire) ||
        atomic_load_explicit(&node->thread_started, memory_order_acquire)) {
        ret = -2;
        goto out;
    }
    for (int i = 0; i < node->downstream_count; ++i) {
        if (node->downstream[i] == downstream) {
            goto out;
        }
    }
    if (node->downstream_count >= OCR_NODE_MAX_DOWNSTREAM) {
        ret = -3;
        goto out;
    }
    node->downstream[node->downstream_count++] = downstream;

out:
    ocr_mutex_unlock(&node->lifecycle_lock);
    return ret;
}

int ocr_node_set_idle_only(ocr_pipeline_node_t *node, int enable)
{
    int ret = 0;

    if (!node ||
        !atomic_load_explicit(&node->initialized, memory_order_acquire)) {
        return -1;
    }

    /* 与拓扑一样在启动前固定：线程创建建立先后关系，运行时入队方无需再加锁读取。 */
    ocr_mutex_lock(&node->lifecycle_lock);
    if (atomic_load_explicit(&node->thread_started, memory_order_acquire)) {
        ret = -2;
    } else {
        node->idle_only = enable ? 1 : 0;
    }
    ocr_mutex_unlock(&node->lifecycle_lock);
    return ret;
}

/** @brief 成功入队时接管调用者的一个引用；失败不接管，满队列淘汰最旧帧。 */
int ocr_node_push_input(ocr_pipeline_node_t *node, ocr_buffer_t *buf)
{
    ocr_buffer_t *evicted = NULL;

    if (!node || !buf ||
        !atomic_load_explicit(&node->initialized, memory_order_acquire) ||
        atomic_load_explicit(&buf->refcount, memory_order_acquire) <= 0) {
        return -1;
    }

    ocr_mutex_lock(&node->in_lock);
    if (!atomic_load_explicit(&node->initialized, memory_order_acquire)) {
        ocr_mutex_unlock(&node->in_lock);
        return -1;
    }

    /* 仅空闲接收：处理中或已有一帧待处理时直接拒收，既不排队也不淘汰。
     * 慢分支因此最多占住一个缓冲，不会把与其他分支共用的输出池耗尽。
     * busy 与出队在同一把锁内更新，不存在“帧已取走、尚未置忙”的空档。 */
    if (node->idle_only &&
        (atomic_load_explicit(&node->busy, memory_order_acquire) ||
         atomic_load_explicit(&node->in_count, memory_order_relaxed) > 0)) {
        ocr_mutex_unlock(&node->in_lock);
        atomic_fetch_add_explicit(&node->skipped, 1, memory_order_relaxed);
        return -2;
    }

    /* 实时图像优先保留新帧：队列满时丢最旧项而非阻塞采集/RGA。
     * 只移除队列自己的引用，别的分支若仍持有此图像，内存不会立即回收。 */
    if (atomic_load_explicit(&node->in_count, memory_order_relaxed) >=
        OCR_NODE_QUEUE_DEPTH) {
        evicted = node->in_queue[node->in_head];
        node->in_queue[node->in_head] = NULL;
        node->in_head = (node->in_head + 1) % OCR_NODE_QUEUE_DEPTH;
        atomic_fetch_sub_explicit(&node->in_count, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&node->dropped, 1, memory_order_relaxed);
    }

    /* 这个赋值就是一份引用的所有权交接，不额外 ref，也不 memcpy 像素。
     * 成功返回后调用者不能再释放这份引用，失败返回时才仍由调用者负责。 */
    node->in_queue[node->in_tail] = buf;
    node->in_tail = (node->in_tail + 1) % OCR_NODE_QUEUE_DEPTH;
    atomic_fetch_add_explicit(&node->in_count, 1, memory_order_release);
    ocr_cond_signal(&node->in_cond);
    ocr_mutex_unlock(&node->in_lock);

    /* 锁外释放被淘汰项，因为 unref 可能进入缓冲池甚至执行 V4L2 QBUF。
     * 队列锁只保护头尾及元素，不应覆盖跨模块回收或设备 ioctl。 */
    if (evicted) {
        (void)ocr_buffer_unref(evicted);
        LOG_W("node %s input queue full; oldest frame dropped", node->name);
    }
    return 0;
}

/** @brief 分发回调新生成的输出并消费其原引用；无下游时也要释放，避免耗尽输出池。 */
int ocr_node_emit(ocr_pipeline_node_t *node, ocr_buffer_t *buf)
{
    int accepted;

    if (!node || !buf ||
        !atomic_load_explicit(&node->initialized, memory_order_acquire) ||
        atomic_load_explicit(&buf->refcount, memory_order_acquire) <= 0) {
        return -1;
    }

    /* 示例：输出初始 ref=1，成功给 display 和 OCR 各 ref 一次，临时变为 3；
     * 再释放生产者原有的 1 份，只留下两个下游的引用（实际计数会受并发影响）。
     * accepted=0 也会消费原引用；只有前面的参数错误返回才不接管。 */
    accepted = node_forward_borrowed(node, buf);
    (void)ocr_buffer_unref(buf);
    return accepted;
}

/** @brief 等输入、执行借用式回调、按返回值转发，最后统一释放输入引用。 */
void *ocr_node_thread_entry(void *arg)
{
    ocr_pipeline_node_t *node = (ocr_pipeline_node_t *)arg;

    if (!node) {
        return NULL;
    }
    LOG_I("node %s worker started", node->name);

    for (;;) {
        ocr_buffer_t *buf;
        int process_ret;

        ocr_mutex_lock(&node->in_lock);
        /* 用 while 而不是 if：条件变量可能虚假唤醒；stop 也会唤醒但没有新帧。
         * cond_wait 原子解 in_lock 并睡眠，返回时重新持锁，避免丢失入队通知。 */
        while (atomic_load_explicit(&node->in_count, memory_order_acquire) == 0 &&
               atomic_load_explicit(&node->running, memory_order_acquire)) {
            (void)ocr_cond_wait(&node->in_cond, &node->in_lock);
        }
        if (!atomic_load_explicit(&node->running, memory_order_acquire)) {
            ocr_mutex_unlock(&node->in_lock);
            break;
        }

        /* 将队列这一份引用移到局部变量，随后解锁执行耗时处理。
         * 因而处理当前帧期间，上游仍可继续入队其他缓冲。 */
        buf = node->in_queue[node->in_head];
        node->in_queue[node->in_head] = NULL;
        node->in_head = (node->in_head + 1) % OCR_NODE_QUEUE_DEPTH;
        atomic_fetch_sub_explicit(&node->in_count, 1, memory_order_release);
        /* 与出队同一临界区置忙，idle_only 入队判断看到的队列和忙状态总是一致。 */
        atomic_store_explicit(&node->busy, buf != NULL, memory_order_relaxed);
        ocr_mutex_unlock(&node->in_lock);

        if (!buf) {
            atomic_fetch_add_explicit(&node->dropped, 1,
                                      memory_order_relaxed);
            continue;
        }

        /* 队列的引用现在由工作线程持有，回调仅借用，避免各回调重复 unref。
         * 同步 RGA 回调返回前已用完输入；未来若异步提交，必须额外保留引用。 */
        process_ret = node->process ? node->process(node, buf) : 0;
        if (process_ret < 0) {
            atomic_fetch_add_explicit(&node->dropped, 1,
                                      memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&node->processed, 1,
                                      memory_order_relaxed);
            /* 返回 0 复用原输入；RGA 生成新输出后返回 1，不能把原始采集图再
             * 发给显示/OCR，否则这些分支会收到未经处理且尺寸不同的图像。 */
            if (process_ret == 0) {
                int accepted = node_forward_borrowed(node, buf);
                if (node->downstream_count > 0 && accepted == 0) {
                    atomic_fetch_add_explicit(&node->dropped, 1,
                                              memory_order_relaxed);
                }
            }
        }
        /* 成功、失败、是否有下游最终都到这里，统一结束本节点对输入的使用。
         * RGA 输入若同时是 latest_frame，这次释放不会归零；等最新帧缓存也换掉
         * 后才触发 QBUF。输出图则等显示与 OCR 的引用都释放后才归还输出池。 */
        (void)ocr_buffer_unref(buf);
        /* 先归还输入再清忙：idle_only 节点接收下一帧时，上一帧已不再占用缓冲。 */
        atomic_store_explicit(&node->busy, 0, memory_order_release);
    }

    LOG_I("node %s worker stopped", node->name);
    return NULL;
}

int ocr_node_start(ocr_pipeline_node_t *node)
{
    if (!node ||
        !atomic_load_explicit(&node->initialized, memory_order_acquire)) {
        return -1;
    }

    ocr_mutex_lock(&node->lifecycle_lock);
    if (!atomic_load_explicit(&node->initialized, memory_order_acquire)) {
        ocr_mutex_unlock(&node->lifecycle_lock);
        return -1;
    }
    if (atomic_load_explicit(&node->thread_started, memory_order_acquire)) {
        ocr_mutex_unlock(&node->lifecycle_lock);
        return 0;
    }

    atomic_store_explicit(&node->running, 1, memory_order_release);
    if (ocr_thread_create(&node->thread, ocr_node_thread_entry, node) != 0) {
        atomic_store_explicit(&node->running, 0, memory_order_release);
        memset(&node->thread, 0, sizeof(node->thread));
        ocr_mutex_unlock(&node->lifecycle_lock);
        LOG_E("node %s worker creation failed", node->name);
        return -2;
    }
    atomic_store_explicit(&node->thread_started, 1, memory_order_release);
    ocr_mutex_unlock(&node->lifecycle_lock);
    return 0;
}

int ocr_node_stop(ocr_pipeline_node_t *node)
{
    int ret;

    if (!node) {
        return -1;
    }
    if (!atomic_load_explicit(&node->initialized, memory_order_acquire)) {
        return 0;
    }

    ocr_mutex_lock(&node->lifecycle_lock);
    if (!atomic_load_explicit(&node->initialized, memory_order_acquire)) {
        ocr_mutex_unlock(&node->lifecycle_lock);
        return 0;
    }
    ret = node_stop_locked(node);
    ocr_mutex_unlock(&node->lifecycle_lock);
    return ret;
}

/** @brief 停止线程并释放队列剩余帧，再销毁锁；不允许在线程自己的回调中完成销毁。 */
void ocr_node_destroy(ocr_pipeline_node_t *node)
{
    ocr_buffer_t *queued[OCR_NODE_QUEUE_DEPTH];
    int queued_count = 0;

    if (!node ||
        !atomic_load_explicit(&node->initialized, memory_order_acquire)) {
        return;
    }

    ocr_mutex_lock(&node->lifecycle_lock);
    if (!atomic_load_explicit(&node->initialized, memory_order_acquire)) {
        ocr_mutex_unlock(&node->lifecycle_lock);
        return;
    }
    if (node_stop_locked(node) != 0) {
        ocr_mutex_unlock(&node->lifecycle_lock);
        LOG_E("node %s cannot be destroyed from its worker", node->name);
        return;
    }

    atomic_store_explicit(&node->initialized, 0, memory_order_release);
    ocr_mutex_lock(&node->in_lock);
    while (atomic_load_explicit(&node->in_count, memory_order_relaxed) > 0) {
        ocr_buffer_t *buf = node->in_queue[node->in_head];
        node->in_queue[node->in_head] = NULL;
        node->in_head = (node->in_head + 1) % OCR_NODE_QUEUE_DEPTH;
        atomic_fetch_sub_explicit(&node->in_count, 1, memory_order_relaxed);
        if (buf && queued_count < OCR_NODE_QUEUE_DEPTH) {
            queued[queued_count++] = buf;
        }
    }
    node->in_head = 0;
    node->in_tail = 0;
    ocr_mutex_unlock(&node->in_lock);

    for (int i = 0; i < queued_count; ++i) {
        (void)ocr_buffer_unref(queued[i]);
    }

    ocr_cond_destroy(&node->in_cond);
    ocr_mutex_destroy(&node->in_lock);
    ocr_mutex_unlock(&node->lifecycle_lock);
    ocr_mutex_destroy(&node->lifecycle_lock);
}
