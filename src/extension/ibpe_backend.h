#ifndef IBPE_BACKEND_H
#define IBPE_BACKEND_H

#include <stddef.h>

#include "sizes.h"

#ifdef __cplusplus
extern "C" {
namespace corpus_search::backend {
#else
#define noexcept
#endif

// index builder
typedef struct index_builder_data *index_builder;

typedef struct __attribute__((__may_alias__))
{
    enum {
        POS_BITS = CORPUS_SEARCH_POSITION_BITS,
        SENTID_BITS = CORPUS_SEARCH_SENTID_BITS,
    };
    sentid_t sent_id : SENTID_BITS;
    tokpos_t pos : POS_BITS;
} index_entry;

typedef void (*index_builder_iterate_function)(void *user_data,
                                               int token,
                                               index_entry const *p_entries,
                                               int n_entries);

index_builder create_index_builder(void) noexcept;
void destroy_index_builder(index_builder builder) noexcept;
void index_builder_add_sentence(index_builder builder,
                                sentid_t sent_id,
                                int *p_tokens,
                                int n_tokens) noexcept;
void index_builder_finalize(index_builder builder) noexcept;
void index_builder_iterate(index_builder builder,
                           index_builder_iterate_function callback,
                           void *user_data) noexcept;

// tokenizer
typedef struct tokenizer_data *tokenizer;

tokenizer create_tokenizer(char const *tokenizer_path,
                           char normalize_mappings[][2],
                           int n_normalize_mappings,
                           char *err_msg,
                           int err_len) noexcept;
void destroy_tokenizer(tokenizer tok) noexcept;
int tokenizer_tokenize(tokenizer tok, char const *string, int *out_tokens, size_t maxlen) noexcept;
int tokenizer_get_vocab_size(tokenizer tok) noexcept;

// searcher
typedef struct sentid_vec_data *sentid_vec;

typedef int (*index_accessor)(void *user_data, int token, index_entry *data, int num_entries);
typedef struct
{
    void *user_data;
    index_accessor func;
} index_accessor_cb;

typedef struct
{
    sentid_vec candidates;
    bool needs_recheck;
} search_result;

search_result search_corpus(tokenizer tok,
                            index_accessor_cb callback,
                            char const *search_term) noexcept;
sentid_t const *sentid_vec_get_data(sentid_vec vec) noexcept;
size_t sentid_vec_get_size(sentid_vec vec) noexcept;
void destroy_sentid_vec(sentid_vec vec) noexcept;

// json parser
int parse_normalize_mappings(char const *json_str, char mappings[][2], int max_mappings) noexcept;

#ifdef __cplusplus
} // namespace corpus_search::backend
}
#endif

#endif // IBPE_BACKEND_H
