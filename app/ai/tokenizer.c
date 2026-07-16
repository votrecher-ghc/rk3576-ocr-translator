/**
 * @file tokenizer.c
 * @brief Reliable UTF-8 vocabulary loading and vocabulary-only tokenization.
 *
 * WordPiece uses the standard greedy longest-match rule with ## continuation.
 * TOKENIZER_GREEDY is a project-specific vocabulary-only contract; it is not a
 * generic BPE/SentencePiece implementation because no merge ranks or normalizer
 * metadata are available at runtime.
 */
#include "tokenizer.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int utf8_decode(const unsigned char *s, size_t remaining,
                       uint32_t *codepoint)
{
    uint32_t cp;
    int len;

    if (!s || remaining == 0) return -1;
    if (s[0] < 0x80) {
        if (codepoint) *codepoint = s[0];
        return 1;
    }
    if (s[0] >= 0xC2 && s[0] <= 0xDF) {
        len = 2;
        cp = s[0] & 0x1Fu;
    } else if (s[0] >= 0xE0 && s[0] <= 0xEF) {
        len = 3;
        cp = s[0] & 0x0Fu;
    } else if (s[0] >= 0xF0 && s[0] <= 0xF4) {
        len = 4;
        cp = s[0] & 0x07u;
    } else {
        return -1;
    }
    if (remaining < (size_t)len) return -1;
    for (int i = 1; i < len; ++i) {
        if ((s[i] & 0xC0u) != 0x80u) return -1;
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    if ((len == 3 && cp < 0x800u) || (len == 4 && cp < 0x10000u) ||
        (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) {
        return -1;
    }
    if (codepoint) *codepoint = cp;
    return len;
}

static int valid_utf8_n(const char *text, size_t length)
{
    size_t pos = 0;
    while (pos < length) {
        int len = utf8_decode((const unsigned char *)text + pos,
                              length - pos, NULL);
        if (len < 0) return 0;
        pos += (size_t)len;
    }
    return 1;
}

static int is_utf8_boundary(const unsigned char *text, size_t length,
                            size_t pos)
{
    return pos == 0 || pos == length || (text[pos] & 0xC0u) != 0x80u;
}

static int vocab_lookup(const ocr_tokenizer_t *tok, const char *token)
{
    if (!tok || !token) return -1;
    for (int i = 0; i < tok->vocab_size; ++i) {
        if (strcmp(tok->vocab[i], token) == 0) return i;
    }
    return -1;
}

static int lookup_special(const ocr_tokenizer_t *tok,
                          const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        int id = vocab_lookup(tok, names[i]);
        if (id >= 0) return id;
    }
    return -1;
}

int tokenizer_init(ocr_tokenizer_t *tok, const char *vocab_path,
                   tokenizer_type_t type)
{
    static const char *const pad_names[] = {"<pad>", "[PAD]", "<PAD>"};
    static const char *const bos_names[] = {
        "<s>", "<bos>", "[BOS]", "<BOS>", "[CLS]"
    };
    static const char *const eos_names[] = {
        "</s>", "<eos>", "[EOS]", "<EOS>", "[SEP]"
    };
    static const char *const unk_names[] = {"<unk>", "[UNK]", "<UNK>"};
    char line[TOKENIZER_MAX_TOKEN_LEN + 2];
    char (*vocab)[TOKENIZER_MAX_TOKEN_LEN] = NULL;
    size_t capacity = 0;
    int count = 0;
    FILE *fp;
    int ret = 0;

    if (!tok || !vocab_path || vocab_path[0] == '\0' ||
        (type != TOKENIZER_BPE && type != TOKENIZER_WORDPIECE)) {
        return -EINVAL;
    }
    memset(tok, 0, sizeof(*tok));
    tok->pad_id = tok->bos_id = tok->eos_id = tok->unk_id = -1;

    fp = fopen(vocab_path, "rb");
    if (!fp) {
        LOG_E("failed to open vocabulary %s: %s", vocab_path, strerror(errno));
        return errno ? -errno : -ENOENT;
    }

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        int has_newline = len > 0 && line[len - 1] == '\n';

        if (!has_newline && !feof(fp)) {
            int ch;
            while ((ch = fgetc(fp)) != '\n' && ch != EOF) {}
            ret = -E2BIG;
            break;
        }
        if (has_newline) line[--len] = '\0';
        if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';
        if (count == 0 && len >= 3 &&
            (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF) {
            memmove(line, line + 3, len - 2);
            len -= 3;
        }
        if (len >= TOKENIZER_MAX_TOKEN_LEN || !valid_utf8_n(line, len)) {
            ret = -EILSEQ;
            break;
        }
        if (count >= TOKENIZER_MAX_VOCAB) {
            ret = -E2BIG;
            break;
        }
        if ((size_t)count == capacity) {
            size_t new_capacity = capacity == 0 ? 1024 : capacity * 2;
            if (new_capacity > TOKENIZER_MAX_VOCAB) {
                new_capacity = TOKENIZER_MAX_VOCAB;
            }
            void *new_vocab = realloc(vocab, new_capacity * sizeof(*vocab));
            if (!new_vocab) {
                ret = -ENOMEM;
                break;
            }
            vocab = new_vocab;
            capacity = new_capacity;
        }
        memset(vocab[count], 0, sizeof(vocab[count]));
        memcpy(vocab[count], line, len + 1);
        ++count;
    }
    if (ferror(fp) && ret == 0) ret = -EIO;
    if (fclose(fp) != 0 && ret == 0) ret = -EIO;
    if (ret != 0 || count == 0) {
        free(vocab);
        return ret != 0 ? ret : -ENODATA;
    }

    tok->type = type;
    tok->vocab = vocab;
    tok->vocab_size = count;
    tok->max_len = 256;
    tok->pad_id = lookup_special(tok, pad_names,
                                 sizeof(pad_names) / sizeof(pad_names[0]));
    tok->bos_id = lookup_special(tok, bos_names,
                                 sizeof(bos_names) / sizeof(bos_names[0]));
    tok->eos_id = lookup_special(tok, eos_names,
                                 sizeof(eos_names) / sizeof(eos_names[0]));
    tok->unk_id = lookup_special(tok, unk_names,
                                 sizeof(unk_names) / sizeof(unk_names[0]));
    if (tok->unk_id < 0) {
        LOG_E("vocabulary has no recognized UNK token");
        tokenizer_destroy(tok);
        return -EINVAL;
    }
    tok->initialized = 1;

    LOG_I("tokenizer initialized: vocab=%d type=%d pad=%d bos=%d eos=%d unk=%d",
          tok->vocab_size, tok->type, tok->pad_id, tok->bos_id,
          tok->eos_id, tok->unk_id);
    return 0;
}

static int is_special_id(const ocr_tokenizer_t *tok, int id)
{
    return id == tok->pad_id || id == tok->bos_id || id == tok->eos_id;
}

static int is_cjk(uint32_t cp)
{
    return (cp >= 0x3400u && cp <= 0x4DBFu) ||
           (cp >= 0x4E00u && cp <= 0x9FFFu) ||
           (cp >= 0xF900u && cp <= 0xFAFFu) ||
           (cp >= 0x20000u && cp <= 0x2FA1Fu);
}

static int is_unicode_space(uint32_t cp)
{
    return cp == 0x20u || (cp >= 0x09u && cp <= 0x0Du) || cp == 0x85u ||
           cp == 0xA0u || cp == 0x1680u ||
           (cp >= 0x2000u && cp <= 0x200Au) || cp == 0x2028u ||
           cp == 0x2029u || cp == 0x202Fu || cp == 0x205Fu || cp == 0x3000u;
}

static int append_id(int32_t *ids, int *count, int capacity, int id)
{
    if (*count >= capacity) return 0;
    ids[(*count)++] = id;
    return 1;
}

static int encode_wordpiece_span(const ocr_tokenizer_t *tok,
                                 const unsigned char *span, size_t span_len,
                                 int32_t *ids, int *count, int capacity)
{
    size_t pos = 0;
    int initial_count = *count;

    while (pos < span_len) {
        int best_id = -1;
        size_t best_len = 0;

        for (int i = 0; i < tok->vocab_size; ++i) {
            const char *piece = tok->vocab[i];
            size_t prefix = 0;
            size_t piece_len;

            if (is_special_id(tok, i) || i == tok->unk_id) continue;
            if (piece[0] == '#' && piece[1] == '#') prefix = 2;
            if ((pos == 0 && prefix != 0) || (pos != 0 && prefix == 0)) continue;
            piece_len = strlen(piece + prefix);
            if (piece_len == 0 || piece_len <= best_len ||
                piece_len > span_len - pos ||
                !is_utf8_boundary(span, span_len, pos + piece_len)) {
                continue;
            }
            if (memcmp(span + pos, piece + prefix, piece_len) == 0) {
                best_id = i;
                best_len = piece_len;
            }
        }

        if (best_id < 0) {
            *count = initial_count;
            return append_id(ids, count, capacity, tok->unk_id) ? 0 : 1;
        }
        if (!append_id(ids, count, capacity, best_id)) return 1;
        pos += best_len;
    }
    return 0;
}

static int encode_wordpiece(const ocr_tokenizer_t *tok, const char *text,
                            int32_t *ids, int *count, int capacity)
{
    const unsigned char *bytes = (const unsigned char *)text;
    size_t length = strlen(text);
    size_t pos = 0;

    while (pos < length && *count < capacity) {
        uint32_t cp;
        int char_len = utf8_decode(bytes + pos, length - pos, &cp);
        size_t span_start;
        size_t span_len;

        if (char_len < 0) return -EILSEQ;
        if (is_unicode_space(cp)) {
            pos += (size_t)char_len;
            continue;
        }

        if (is_cjk(cp) || (cp < 0x80u && ispunct((unsigned char)cp))) {
            span_start = pos;
            span_len = (size_t)char_len;
            pos += (size_t)char_len;
        } else {
            span_start = pos;
            pos += (size_t)char_len;
            while (pos < length) {
                char_len = utf8_decode(bytes + pos, length - pos, &cp);
                if (char_len < 0) return -EILSEQ;
                if (is_unicode_space(cp) || is_cjk(cp) ||
                    (cp < 0x80u && ispunct((unsigned char)cp))) {
                    break;
                }
                pos += (size_t)char_len;
            }
            span_len = pos - span_start;
        }

        if (encode_wordpiece_span(tok, bytes + span_start, span_len,
                                  ids, count, capacity) != 0) {
            break;
        }
    }
    return 0;
}

static int encode_bpe(const ocr_tokenizer_t *tok, const char *text,
                      int32_t *ids, int *count, int capacity)
{
    static const unsigned char sp_marker[] = {0xE2, 0x96, 0x81}; /* U+2581 */
    static const unsigned char gpt_marker[] = {0xC4, 0xA0};      /* U+0120 */
    const unsigned char *bytes = (const unsigned char *)text;
    size_t length = strlen(text);
    size_t pos = 0;
    int at_word_start = 1;

    while (pos < length && *count < capacity) {
        uint32_t cp;
        int char_len = utf8_decode(bytes + pos, length - pos, &cp);
        int best_id = -1;
        size_t best_consumed = 0;
        int best_marked = 0;

        if (char_len < 0) return -EILSEQ;
        if (is_unicode_space(cp)) {
            at_word_start = 1;
            pos += (size_t)char_len;
            continue;
        }

        for (int i = 0; i < tok->vocab_size; ++i) {
            const unsigned char *piece = (const unsigned char *)tok->vocab[i];
            size_t piece_len = strlen((const char *)piece);
            size_t prefix = 0;

            if (is_special_id(tok, i) || i == tok->unk_id || piece_len == 0) continue;
            if (piece_len >= sizeof(sp_marker) &&
                memcmp(piece, sp_marker, sizeof(sp_marker)) == 0) {
                prefix = sizeof(sp_marker);
            } else if (piece_len >= sizeof(gpt_marker) &&
                       memcmp(piece, gpt_marker, sizeof(gpt_marker)) == 0) {
                prefix = sizeof(gpt_marker);
            }
            if (prefix != 0 && !at_word_start) continue;
            piece_len -= prefix;
            if (piece_len == 0 || piece_len > length - pos ||
                !is_utf8_boundary(bytes, length, pos + piece_len)) {
                continue;
            }
            if (memcmp(bytes + pos, piece + prefix, piece_len) == 0 &&
                (piece_len > best_consumed ||
                 (piece_len == best_consumed && prefix != 0 && !best_marked))) {
                best_id = i;
                best_consumed = piece_len;
                best_marked = prefix != 0;
            }
        }

        if (best_id < 0) {
            if (!append_id(ids, count, capacity, tok->unk_id)) break;
            pos += (size_t)char_len;
        } else {
            if (!append_id(ids, count, capacity, best_id)) break;
            pos += best_consumed;
        }
        at_word_start = 0;
    }
    return 0;
}

int tokenizer_encode(ocr_tokenizer_t *tok, const char *text,
                     int32_t *ids, int max_ids)
{
    int count = 0;
    int capacity;
    int payload_capacity;
    int ret;

    if (!tok || !tok->initialized || !text || !ids || max_ids <= 0) {
        return -EINVAL;
    }
    if (!valid_utf8_n(text, strlen(text))) return -EILSEQ;

    capacity = max_ids < tok->max_len ? max_ids : tok->max_len;
    if (tok->bos_id >= 0 && !append_id(ids, &count, capacity, tok->bos_id)) {
        return count;
    }
    payload_capacity = capacity - (tok->eos_id >= 0 ? 1 : 0);
    if (payload_capacity < count) payload_capacity = count;

    if (tok->type == TOKENIZER_WORDPIECE) {
        ret = encode_wordpiece(tok, text, ids, &count, payload_capacity);
    } else {
        ret = encode_bpe(tok, text, ids, &count, payload_capacity);
    }
    if (ret != 0) return ret;
    if (tok->eos_id >= 0 && count < capacity) ids[count++] = tok->eos_id;
    return count;
}

static int append_text(char *text, int text_size, int *pos,
                       const char *piece, size_t length)
{
    if (length == 0) return 0;
    if (*pos < 0 || *pos >= text_size || length > (size_t)(text_size - 1 - *pos)) {
        return -ENOSPC;
    }
    memcpy(text + *pos, piece, length);
    *pos += (int)length;
    text[*pos] = '\0';
    return 0;
}

static int token_is_ascii_punct(const char *token)
{
    return token[0] != '\0' && token[1] == '\0' &&
           ispunct((unsigned char)token[0]);
}

static int token_is_single_cjk(const char *token)
{
    uint32_t cp;
    size_t len = strlen(token);
    int char_len = utf8_decode((const unsigned char *)token, len, &cp);
    return char_len > 0 && (size_t)char_len == len && is_cjk(cp);
}

int tokenizer_decode(ocr_tokenizer_t *tok, const int32_t *ids, int count,
                     char *text, int text_size)
{
    static const char sp_marker[] = "\xE2\x96\x81";
    static const char gpt_marker[] = "\xC4\xA0";
    int pos = 0;

    if (!tok || !tok->initialized || !ids || count < 0 ||
        !text || text_size <= 0) {
        return -EINVAL;
    }
    text[0] = '\0';

    for (int i = 0; i < count; ++i) {
        int id = ids[i];
        const char *piece;
        size_t len;
        int ret;

        if (id == tok->eos_id && tok->eos_id >= 0) break;
        if (id == tok->pad_id || id == tok->bos_id) continue;
        if (id < 0 || id >= tok->vocab_size) return -ERANGE;
        piece = tok->vocab[id];
        len = strlen(piece);

        if (tok->type == TOKENIZER_WORDPIECE) {
            int continuation = len >= 2 && piece[0] == '#' && piece[1] == '#';
            if (continuation) {
                piece += 2;
                len -= 2;
            } else if (pos > 0 && !token_is_ascii_punct(piece) &&
                       !token_is_single_cjk(piece)) {
                ret = append_text(text, text_size, &pos, " ", 1);
                if (ret != 0) return ret;
            }
        } else {
            size_t marker_len = 0;
            if (len >= 3 && memcmp(piece, sp_marker, 3) == 0) marker_len = 3;
            else if (len >= 2 && memcmp(piece, gpt_marker, 2) == 0) marker_len = 2;
            if (marker_len != 0) {
                if (pos > 0) {
                    ret = append_text(text, text_size, &pos, " ", 1);
                    if (ret != 0) return ret;
                }
                piece += marker_len;
                len -= marker_len;
            }
        }

        int end_word = len >= 4 && memcmp(piece + len - 4, "</w>", 4) == 0;
        if (end_word) len -= 4;
        ret = append_text(text, text_size, &pos, piece, len);
        if (ret != 0) return ret;
        if (end_word && i + 1 < count) {
            ret = append_text(text, text_size, &pos, " ", 1);
            if (ret != 0) return ret;
        }
    }
    return pos;
}

void tokenizer_destroy(ocr_tokenizer_t *tok)
{
    if (!tok) return;
    free(tok->vocab);
    memset(tok, 0, sizeof(*tok));
    tok->pad_id = tok->bos_id = tok->eos_id = tok->unk_id = -1;
}
