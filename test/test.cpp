#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <fmt/chrono.h>

#include "regex_ast.hpp"
#include "searcher.hpp"

static auto get_tok() -> corpus_search::tokenizer &
{
    static auto t = corpus_search::tokenizer{
        "/home/park/devel/mk-tokenizer/bpe_tokenizer-12/tokenizer.json",
        {{'.', 'x'}, {'/', 'Z'}, {'\\', 'X'}, {'`', 'C'}},
        true,
    };
    return t;
}

static auto get_index() -> corpus_search::index_builder &
{
    static auto idx = corpus_search::index_builder::from_file(
        "/home/park/devel/mk-tokenizer/tokenized_sentences12.msgpack");
    return idx;
}

static auto measure_time(std::string search_term) -> std::vector<sentid_t>
{
    using namespace std::chrono;
    auto start_time = high_resolution_clock::now();

    auto result = search(
        get_tok(),
        [](int token) -> std::vector<corpus_search::index_entry> {
            auto& index = get_index().get_index();
            if (index.count(token) == 0) {
                return {};
            }
            return index.at(token);
        },
        search_term);

    auto end_time = high_resolution_clock::now();

    if (result.size() < 200) {
        fmt::println("Result for '{}' = Array[{}]{{{}}}",
                     search_term,
                     result.size(),
                     fmt::join(result, ", "));
    } else {
        fmt::println("Result for '{}' = Array[{}]{{...}}", search_term, result.size());
    }

    auto elapsed = end_time - start_time;
    fmt::println("Took {}.", duration_cast<duration<float>>(elapsed));

    return result;
}

TEST(Regex, Regex)
{
    // corpus_search::regex::parse("[a-z]+");
    // corpus_search::regex::parse("[a-zA-Z]+");
    corpus_search::regex::parse(
        "abc[a-zA-Z]+?(?<name>st|uv)(?:pid)*\\b\\d*\\?\\p{Script=Han}$"); // \\B\\d*\\?\\p{Script=Han}
}

// TEST(Searcher, SearchStringSimple)
// {
//     get_tok(), get_index();

//     EXPECT_EQ(measure_time("z").size(), 20'621);
//     EXPECT_EQ(measure_time("o").size(), 1'286'817);
//     EXPECT_EQ(measure_time("ho").size(), 811'085);
//     EXPECT_EQ(measure_time("TT").size(), 0);
// }

// TEST(Searcher, SearchStringHard)
// {
//     get_tok(), get_index();

//     EXPECT_EQ(measure_time("ho\\.ni").size(), 94'307);
//     EXPECT_EQ(measure_time("si\\.ta\\.so\\.ngi\\.ta").size(), 14);
//     EXPECT_EQ(measure_time("ngi\\.ta").size(), 2'472);
//     EXPECT_EQ(measure_time("ka\\.nan\\.ho").size(), 719);
//     EXPECT_EQ(measure_time("o\\.non").size(), 74'953);
//     EXPECT_EQ(measure_time("國家").size(), 296);
// }

// TEST(Searcher, SearchRegexEasy)
// {
//     get_tok(), get_index();

//     EXPECT_EQ(measure_time("cho\\.c[ou]\\.ni").size(), 168);
//     EXPECT_EQ(measure_time("w[ou]\\.toy").size(), 44'782);
// }

// #define HANJA_RE "[\u4E00-\u9FCC\u3400-\u4DB5]"

// TEST(Searcher, SearchRegexHard)
// {
//     get_tok(), get_index();

//     EXPECT_EQ(measure_time("(k[aeiou]\\.){3}k").size(), 61'261);
//     // EXPECT_EQ(measure_time(HANJA_RE "`i").size(), 61'261);
// }
