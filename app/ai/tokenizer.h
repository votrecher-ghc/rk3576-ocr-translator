#ifndef OCR_AI_TOKENIZER_H
#define OCR_AI_TOKENIZER_H

/** @file tokenizer.h @brief UTF-8 vocabulary tokenizer. */

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
    tokenizer_type_t type;
    char    (*vocab)[TOKENIZER_MAX_TOKEN_LEN];
    int      vocab_size;
    int      pad_id;
    int      bos_id;
    int      eos_id;
    int      unk_id;
    int      max_len;
    int      initialized;
} ocr_tokenizer_t;

int tokenizer_init(ocr_tokenizer_t *tok, const char *vocab_path,
                   tokenizer_type_t type);

/** Encode UTF-8 text. The returned length is not padded. */
int tokenizer_encode(ocr_tokenizer_t *tok, const char *text,
                     int32_t *ids, int max_ids);

int tokenizer_decode(ocr_tokenizer_t *tok, const int32_t *ids, int count,
                     char *text, int text_size);

void tokenizer_destroy(ocr_tokenizer_t *tok);

#endif /* OCR_AI_TOKENIZER_H */
