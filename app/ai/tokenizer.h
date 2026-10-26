#ifndef OCR_AI_TOKENIZER_H
#define OCR_AI_TOKENIZER_H

/** @file tokenizer.h @brief UTF-8 vocabulary tokenizer.
 *
 * 词表分词与还原：校验 UTF-8、查找特殊 token，并按所选模式切分或拼接文本。
 * 编码结果和解码字符串均受调用方容量限制；模型必须与词表、分词模式及特殊 token 编号一致。
 */

#include <stdint.h>

#define TOKENIZER_MAX_VOCAB 32000
#define TOKENIZER_MAX_TOKEN_LEN 256
#define TOKENIZER_MAX_TEXT 4096

typedef enum {
    TOKENIZER_GREEDY = 0, /* UTF-8 vocabulary longest-match contract */
    TOKENIZER_WORDPIECE = 1,
    TOKENIZER_BPE = TOKENIZER_GREEDY, /* source compatibility; not generic BPE */
} tokenizer_type_t;

typedef struct {
    tokenizer_type_t type; /**< 分词模式：WordPiece 或词表贪心匹配；决定 ##、词首标记的处理规则。 */
    char    (*vocab)[TOKENIZER_MAX_TOKEN_LEN]; /**< 本实例拥有的 UTF-8 词表数组，行号就是 token ID；init 分配、destroy 释放。 */
    int      vocab_size; /**< 已载入的有效词项数；有效 token ID 范围为 0..vocab_size-1。 */
    int      pad_id; /**< 定长输入补齐 token 的编号；-1 表示未找到，翻译契约要求必须存在。 */
    int      bos_id; /**< 序列开始 token 的编号；-1 表示未找到，编码时存在则放在开头。 */
    int      eos_id; /**< 序列结束 token 的编号；-1 表示未找到，解码遇到它终止而不输出文字。 */
    int      unk_id; /**< 无法匹配词项时使用的未知 token 编号；由词表中的特殊词项确定。 */
    int      max_len; /**< 单次编码允许的最大 token 数，含特殊 token；不是 UTF-8 字节数或字符数。 */
    int      initialized; /**< 词表及分词配置准备完成标志；destroy 清零后不可继续 encode/decode。 */
} ocr_tokenizer_t;

/**
 * @brief 加载逐行 UTF-8 词表，行号作为 token ID，并定位特殊 token
 * @param[out] tok 待初始化的实例；已有实例须先 destroy，避免覆盖已分配的词表
 * @param[in] vocab_path 词表路径；单项最多 TOKENIZER_MAX_TOKEN_LEN-1 字节
 * @param[in] type 词表贪心匹配或 WordPiece；TOKENIZER_BPE 只是贪心模式的兼容别名
 * @return 0=成功，负数=文件、编码、容量或内存错误
 * @note UNK 必须存在；PAD/BOS/EOS 可缺省，翻译器会进一步要求三者齐全。
 */
int tokenizer_init(ocr_tokenizer_t *tok, const char *vocab_path,
                   tokenizer_type_t type);

/**
 * @brief 将 UTF-8 文本编码为 token 序列，不补 PAD
 * @param[in] tok 已初始化的分词器
 * @param[in] text 以 NUL 结尾的 UTF-8 原文
 * @param[out] ids 调用者分配的 token 数组，仅返回长度内的元素有效
 * @param[in] max_ids 数组容量，以 int32_t 元素数计
 * @return 非负数=实际 token 数，-EINVAL=参数无效，-EILSEQ=非法 UTF-8
 * @note 有效容量取 max_ids 与 tok->max_len 的较小值，包含存在的 BOS/EOS。
 *       容量不足会截断正文并返回已写入长度，不另报错；调用方自行决定截断策略。
 */
int tokenizer_encode(ocr_tokenizer_t *tok, const char *text,
                     int32_t *ids, int max_ids);

/**
 * @brief 跳过 PAD/BOS，遇 EOS 停止，按分词模式拼接 UTF-8 文本
 * @param[in] tok 已初始化的分词器
 * @param[in] ids 至少 count 项的 token 数组，count 为 0 时也须提供非 NULL 指针
 * @param[in] count 待解码 token 数，不是文本字节数
 * @param[out] text 调用者分配的输出缓冲
 * @param[in] text_size 输出容量，以字节计，包含结尾 NUL
 * @return 非负数=输出字节数（不含 NUL），-ENOSPC=空间不足，-ERANGE=ID 越界，
 *         -EINVAL=参数无效
 * @note 解码开始后始终保留 NUL 结尾；中途出错时可能留下部分结果，不能视为完整文本。
 */
int tokenizer_decode(ocr_tokenizer_t *tok, const int32_t *ids, int count,
                     char *text, int text_size);

/** @brief 释放词表并清空状态；允许 NULL，成功初始化后可重复销毁。 */
void tokenizer_destroy(ocr_tokenizer_t *tok);

#endif /* OCR_AI_TOKENIZER_H */
