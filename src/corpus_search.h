#ifndef CORPUS_SEARCH_H
#define CORPUS_SEARCH_H

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
    int sent_id;
    int pos;
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

#endif // CORPUS_SEARCH_H
