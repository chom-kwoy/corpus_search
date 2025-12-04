#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <fmt/chrono.h>

#include "searcher.hpp"

static auto get_tok() -> corpus_search::tokenizer &
{
    static auto t = corpus_search::tokenizer{
        "/home/park/devel/mk-tokenizer/bpe_tokenizer/tokenizer.json",
        true,
    };
    return t;
}

static auto get_index() -> corpus_search::index_builder &
{
    static auto idx = corpus_search::index_builder::from_file(
        "/home/park/devel/mk-tokenizer/tokenized_sentences.msgpack");
    return idx;
}

static auto measure_time(std::string search_term) -> std::vector<int>
{
    using namespace std::chrono;
    auto start_time = high_resolution_clock::now();

    auto result = search(get_tok(), get_index(), search_term);

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

TEST(Searcher, SearchStringSimple)
{
    EXPECT_EQ(measure_time("z").size(), 20'621);
    EXPECT_EQ(measure_time("o").size(), 1'286'797);
    EXPECT_EQ(measure_time("ho").size(), 811'047);
    EXPECT_EQ(measure_time("TT").size(), 0);
}

TEST(Searcher, SearchStringHard)
{
    EXPECT_EQ(measure_time("hoxni").size(), 94'307);
    EXPECT_EQ(measure_time("sixtaxsoxngixta").size(), 14);
    EXPECT_EQ(measure_time("ngixta").size(), 2'472);
    EXPECT_EQ(measure_time("kaxnanxho").size(), 719);
    EXPECT_EQ(measure_time("oxnon").size(), 74'953);
    EXPECT_EQ(measure_time("國家").size(), 296);
}
