#ifndef OCR_AI_TOKENIZER_H
#define OCR_AI_TOKENIZER_H
/**
 * @file tokenizer.h
 * @brief 分词器：BPE/WordPiece，词表加载
 *
 * 用于翻译模型的输入/输出 token 编码与解码。
 */

#include <stdint.h>

#define TOKENIZER_MAX_VOCAB 32000
#define TOKENIZER_MAX_TOKEN_LEN 32
#define TOKENIZER_MAX_TEXT 4096

/** 分词器类型 */
typedef enum {
    TOKENIZER_BPE = 0,
    TOKENIZER_WORDPIECE,
} tokenizer_type_t;

/** 分词器上下文 */
typedef struct {
    tokenizer_type_t type;         /* 分词类型 */
    char    (*vocab)[TOKENIZER_MAX_TOKEN_LEN]; /* 词表 */
    int      vocab_size;           /* 词表大小 */
    int      pad_id;               /* PAD token id */
    int      bos_id;               /* BOS token id */
    int      eos_id;               /* EOS token id */
    int      unk_id;               /* UNK token id */
    int      max_len;              /* 最大长度 */
} ocr_tokenizer_t;

/**
 * @brief 初始化分词器并加载词表
 * @param[in] tok       分词器
 * @param[in] vocab_path 词表文件路径
 * @param[in] type      分词类型
 * @return 0=成功，负数=错误
 */
int tokenizer_init(ocr_tokenizer_t *tok, const char *vocab_path, tokenizer_type_t type);

/**
 * @brief 将文本编码为 token id 序列
 * @param[in]  tok    分词器
 * @param[in]  text   UTF-8 文本
 * @param[out] ids    输出 token id 数组
 * @param[in]  max_ids 数组容量
 * @return token 数量，负数=错误
 */
int tokenizer_encode(ocr_tokenizer_t *tok, const char *text, int32_t *ids, int max_ids);

/**
 * @brief 将 token id 序列解码为文本
 * @param[in]  tok    分词器
 * @param[in]  ids    token id 数组
 * @param[in]  count  token 数量
 * @param[out] text   输出文本缓冲
 * @param[in]  text_size 文本缓冲大小
 * @return 文本长度，负数=错误
 */
int tokenizer_decode(ocr_tokenizer_t *tok, const int32_t *ids, int count,
                     char *text, int text_size);

/**
 * @brief 销毁分词器
 */
void tokenizer_destroy(ocr_tokenizer_t *tok);

#endif /* OCR_AI_TOKENIZER_H */
