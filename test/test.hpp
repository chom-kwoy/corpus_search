#ifndef TEST_HPP
#define TEST_HPP

#include "index_builder.hpp"
#include "tokenizer.hpp"

constexpr auto TOKENIZER_FILE = "/home/park/devel/mk-tokenizer/bpe_tokenizer-12/tokenizer.json";
constexpr auto CORPUS_FILE = "/home/park/devel/mk-tokenizer/tokenized_sentences12.msgpack";

inline auto get_tok() -> corpus_search::tokenizer &
{
    static auto t = corpus_search::tokenizer{
        TOKENIZER_FILE,
        {{'.', 'x'}, {'/', 'Z'}, {'\\', 'X'}, {'`', 'C'}},
        true,
    };
    return t;
}

inline auto get_index() -> corpus_search::index_builder &
{
    static auto idx = corpus_search::index_builder::from_file(CORPUS_FILE);
    return idx;
}

#define HANJA_RE "[\u4E00-\u9FCC\u3400-\u4DB5]"

#endif // TEST_HPP
