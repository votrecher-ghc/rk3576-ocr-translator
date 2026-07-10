/**
 * @file tokenizer.c
 * @brief 分词器实现（简化版 BPE/WordPiece）
 */
#include "tokenizer.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int tokenizer_init(ocr_tokenizer_t *tok, const char *vocab_path, tokenizer_type_t type)
{
    if (!tok || !vocab_path) return -1;
    memset(tok, 0, sizeof(*tok));
    tok->type = type;
    tok->pad_id = 0;
    tok->bos_id = 1;
    tok->eos_id = 2;
    tok->unk_id = 3;
    tok->max_len = 256;

    /* 读取词表文件 */
    FILE *fp = fopen(vocab_path, "r");
    if (!fp) {
        LOG_E("打开词表失败: %s", vocab_path);
        return -2;
    }

    /* 先统计行数 */
    int count = 0;
    char line[TOKENIZER_MAX_TOKEN_LEN];
    while (fgets(line, sizeof(line), fp)) count++;
    rewind(fp);
    if (count > TOKENIZER_MAX_VOCAB) count = TOKENIZER_MAX_VOCAB;

    tok->vocab = (char (*)[TOKENIZER_MAX_TOKEN_LEN])
                 malloc((size_t)count * TOKENIZER_MAX_TOKEN_LEN);
    if (!tok->vocab) { fclose(fp); return -3; }

    int idx = 0;
    while (idx < count && fgets(tok->vocab[idx], TOKENIZER_MAX_TOKEN_LEN, fp)) {
        /* 去除换行 */
        size_t len = strlen(tok->vocab[idx]);
        while (len > 0 && (tok->vocab[idx][len-1] == '\n' || tok->vocab[idx][len-1] == '\r'))
            tok->vocab[idx][--len] = '\0';
        idx++;
    }
    tok->vocab_size = idx;
    fclose(fp);

    LOG_I("分词器初始化: vocab_size=%d type=%d", tok->vocab_size, tok->type);
    return 0;
}

/* 查找词表中的 token，返回 id，未找到返回 unk_id */
static int vocab_lookup(ocr_tokenizer_t *tok, const char *token)
{
    for (int i = 0; i < tok->vocab_size; i++) {
        if (strcmp(tok->vocab[i], token) == 0) return i;
    }
    return tok->unk_id;
}

int tokenizer_encode(ocr_tokenizer_t *tok, const char *text, int32_t *ids, int max_ids)
{
    if (!tok || !text || !ids) return -1;
    int count = 0;

    /* BOS */
    if (count < max_ids) ids[count++] = tok->bos_id;

    /* TODO: 完整 BPE/WordPiece 分词算法
     * 简化版：按字符分割（适用于中文等无空格分隔语言） */
    const unsigned char *p = (const unsigned char *)text;
    while (*p && count < max_ids - 1) {
        char token[8] = {0};
        if (*p < 0x80) {
            /* ASCII：尝试连续字母数字作为一个 token */
            int len = 0;
            while (p[len] < 0x80 && (isalnum(p[len]) || p[len] == '\'') && len < 7) {
                token[len] = p[len];
                len++;
            }
            if (len == 0) { p++; continue; }
            token[len] = '\0';
            ids[count++] = vocab_lookup(tok, token);
            p += len;
        } else {
            /* 多字节 UTF-8：取一个字符 */
            int len = 0;
            if ((*p & 0xE0) == 0xC0) len = 2;
            else if ((*p & 0xF0) == 0xE0) len = 3;
            else if ((*p & 0xF8) == 0xF0) len = 4;
            else len = 1;
            for (int i = 0; i < len && i < 7; i++) token[i] = p[i];
            token[len] = '\0';
            ids[count++] = vocab_lookup(tok, token);
            p += len;
        }

        if (count >= tok->max_len) break;
    }

    /* EOS */
    if (count < max_ids) ids[count++] = tok->eos_id;

    /* PAD 填充 */
    while (count < tok->max_len && count < max_ids) {
        ids[count++] = tok->pad_id;
    }
    return count;
}

int tokenizer_decode(ocr_tokenizer_t *tok, const int32_t *ids, int count,
                     char *text, int text_size)
{
    if (!tok || !ids || !text) return -1;
    int pos = 0;
    for (int i = 0; i < count && pos < text_size - 1; i++) {
        int id = ids[i];
        if (id == tok->pad_id || id == tok->bos_id || id == tok->eos_id) continue;
        if (id < 0 || id >= tok->vocab_size) continue;
        const char *token = tok->vocab[id];
        size_t len = strlen(token);
        /* 跳过 BPE 特殊前缀（如 ##） */
        if (len >= 2 && token[0] == '#' && token[1] == '#') token += 2, len -= 2;
        if (pos + (int)len >= text_size) break;
        strcpy(text + pos, token);
        pos += (int)len;
    }
    text[pos] = '\0';
    return pos;
}

void tokenizer_destroy(ocr_tokenizer_t *tok)
{
    if (!tok) return;
    free(tok->vocab);
    tok->vocab = NULL;
    tok->vocab_size = 0;
}
