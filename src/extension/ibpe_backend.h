#ifndef IBPE_BACKEND_H
#define IBPE_BACKEND_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#else
#define noexcept
#endif

struct index_builder_data;
typedef struct index_builder_data *index_builder;

struct index_entry
{
    enum {
        POS_BITS = 11,
        SENTID_BITS = 32 - POS_BITS,
    };
    unsigned int sent_id : SENTID_BITS;
    unsigned int pos : POS_BITS;
};

typedef void (*index_builder_iterate_function)(void *user_data,
                                               int token,
                                               struct index_entry const *p_sentids,
                                               int n_sentids);

index_builder create_index_builder(void) noexcept;
void destroy_index_builder(index_builder builder) noexcept;
void index_builder_add_sentence(index_builder builder,
                                int sent_id,
                                int *p_tokens,
                                int n_tokens) noexcept;
void index_builder_finalize(index_builder builder) noexcept;
void index_builder_iterate(index_builder builder,
                           index_builder_iterate_function callback,
                           void *user_data) noexcept;

struct tokenizer_data;
typedef struct tokenizer_data *tokenizer;

tokenizer create_tokenizer(char const *tokenizer_path, char *err_msg, int err_len) noexcept;
void destroy_tokenizer(tokenizer tok) noexcept;
int tokenizer_tokenize(tokenizer tok, char const *string, int *out_tokens, size_t maxlen) noexcept;

#ifdef __cplusplus
}
#endif

#endif // IBPE_BACKEND_H
