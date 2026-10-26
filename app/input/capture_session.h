#ifndef OCR_INPUT_CAPTURE_SESSION_H
#define OCR_INPUT_CAPTURE_SESSION_H
/**
 * @file capture_session.h
 * @brief 按键拍照：登记请求，由实时 OCR 线程把下一次识别完成的帧连同译文存档
 *
 * 按键线程只调用 request 登记一次拍照请求，不推理也不读图像。实时 OCR 线程每识别完
 * 一帧就调用 take_request；有请求时调用 save，把这一帧叠加译文后编码为 image.jpg，
 * 并写入 result.txt / result.json。模型因此只由 OCR 线程使用，不需要跨线程加锁。
 */

#include "buffer.h"
#include "ocr_postproc.h"
#include "ocr_rec.h"
#include "archiver.h"
#include "key_event.h"
#include "text_renderer.h"
#include <stdatomic.h>

/** 合成到照片上的一条文字，与屏幕文字层的内容和位置一一对应。 */
typedef struct {
    const char *text; /**< 借用的 UTF-8 文字，只在 save 调用期间使用。 */
    int         x; /**< 文字左上角横坐标，帧像素，与屏幕文字层相同。 */
    int         y; /**< 文字左上角纵坐标，帧像素，与屏幕文字层相同。 */
} ocr_capture_label_t;

/** 拍照会话上下文 */
typedef struct {
    ocr_archiver_t     *archiver; /**< 借用的归档器，save 时建立目录并写入结果；会话不负责销毁。 */
    ocr_text_renderer_t text; /**< 本会话独占的文字渲染器，只由调用 save 的 OCR 线程使用。 */
    atomic_int          pending; /**< 未处理的拍照请求：按键线程置 1，OCR 线程用原子交换取走。 */
    char                src_lang[16]; /**< 写入归档元数据的源语言代码，含 NUL；configure 设置。 */
    char                tgt_lang[16]; /**< 写入归档元数据的目标语言代码，含 NUL；不自动更换已加载模型。 */
    int                 jpeg_quality; /**< JPEG 质量参数，1..100，默认 90；传给 libjpeg，非百分比压缩率。 */
    int                 initialized; /**< init 成功的标志；destroy 后为 0。 */
} ocr_capture_session_t;

/**
 * @brief 初始化拍照会话并加载合成译文用的字体
 * @param[out] sess 待初始化的会话对象
 * @param[in] arch 已初始化的归档器
 * @param[in] font_path,font_size 与屏幕文字层相同的字体和像素字号
 * @return 0=成功，-1=参数无效
 * @note 字体加载失败只告警，之后保存的照片不带译文，其余归档内容不受影响。
 */
int ocr_capture_session_init(ocr_capture_session_t *sess, ocr_archiver_t *arch,
                             const char *font_path, int font_size);

/**
 * @brief 设置归档语言标签和 JPEG 质量，不切换翻译模型
 * @param[in,out] sess 已初始化的会话，须在 OCR 线程启动前调用
 * @param[in] src_lang 非空源语言代码，最多保存 15 字节，超长截断
 * @param[in] tgt_lang 非空目标语言代码，最多保存 15 字节，超长截断
 * @param[in] jpeg_quality JPEG 质量，1..100
 * @return 0=成功，-1=参数无效
 */
int ocr_capture_session_configure(ocr_capture_session_t *sess,
                                  const char *src_lang, const char *tgt_lang,
                                  int jpeg_quality);

/**
 * @brief 登记一次拍照请求，立即返回
 * @note 可由任意线程调用；上一次请求还未被取走时合并为一次。
 */
void ocr_capture_session_request(ocr_capture_session_t *sess);

/**
 * @brief 取走未处理的拍照请求
 * @return 1=有请求且已清除，调用方应随后调用 save；0=没有请求
 * @note 只应在一次识别成功完成后调用，识别失败时保留请求留给下一帧。
 */
int ocr_capture_session_take_request(ocr_capture_session_t *sess);

/**
 * @brief 把一帧画面叠加译文后编码为 image.jpg，并写入 result.txt / result.json
 * @param[in] frame 已 mmap 的帧；调用方负责在调用前后完成 CPU 读同步
 * @param[in] boxes 本帧检测框，坐标为帧像素
 * @param[in] texts,translated 与 boxes 同序的原文和译文；译文为空串表示翻译失败
 * @param[in] labels,label_count 要画到照片上的文字，与屏幕文字层相同
 * @param[in] color straight 0xAARRGGBB 文字颜色，与屏幕文字层相同
 * @return 0=成功；-1=参数无效；-2=建立归档目录失败；-3=JPEG 编码失败；
 *         -4=内存不足；-5=写入文本或 JSON 失败
 * @note 在调用线程同步完成。失败时已建立的归档目录和已写出的文件不回滚。
 */
int ocr_capture_session_save(ocr_capture_session_t *sess, const ocr_buffer_t *frame,
                             const ocr_text_box_list_t *boxes,
                             char (*texts)[OCR_MAX_TEXT_LEN],
                             char (*translated)[OCR_MAX_TEXT_LEN],
                             const ocr_capture_label_t *labels, int label_count,
                             uint32_t color);

/** @brief 释放会话的字体资源；须在按键线程和 OCR 线程都停止后调用。 */
void ocr_capture_session_destroy(ocr_capture_session_t *sess);

/**
 * @brief 按键事件回调（注册到 key_event）
 * @param[in] event 仅 KEY_EVENT_CAMERA 会登记拍照
 * @param[in] state 仅 KEY_STATE_PRESSED 生效，松开事件不触发
 * @param[in,out] user 指向 ocr_capture_session_t；回调只登记请求，立即返回
 */
void ocr_capture_session_on_key(ocr_key_event_t event, ocr_key_state_t state, void *user);

#endif /* OCR_INPUT_CAPTURE_SESSION_H */
