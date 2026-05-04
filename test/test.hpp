#ifndef TEST_HPP
#define TEST_HPP

#include "index_builder.hpp"
#include "tokenizer.hpp"

inline auto get_tok_path() -> const char*
{
    const char* tokenizer_file = std::getenv("IBPE_TEST_TOKENIZER_FILE");
    if (tokenizer_file == nullptr) {
        throw std::runtime_error(
            "Set IBPE_TEST_TOKENIZER_FILE to the appropriate tokenizer.json file path");
    }
    return tokenizer_file;
}

inline auto get_tok() -> corpus_search::tokenizer &
{
    static auto t = corpus_search::tokenizer{
        get_tok_path(),
        {{'.', 'x'}, {'/', 'Z'}, {'\\', 'X'}, {'`', 'C'}},
        true,
    };
    return t;
}

inline auto get_index() -> corpus_search::index_builder &
{
    const char* corpus_file = std::getenv("IBPE_TEST_CORPUS_FILE");
    if (corpus_file == nullptr) {
        throw std::runtime_error(
            "Set IBPE_TEST_CORPUS_FILE to the appropriate tokenized_sentences.msgpack file path");
    }
    static auto idx = corpus_search::index_builder::from_file(corpus_file);
    return idx;
}

#define HANJA_RE "[\u4E00-\u9FCC\u3400-\u4DB5]"

#endif // TEST_HPP
