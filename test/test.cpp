#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <fmt/chrono.h>

#include "searcher.hpp"

auto s() -> corpus_search::searcher& {
    static auto s = corpus_search::searcher(
        "/home/park/devel/mk-tokenizer/tokenized_sentences.msgpack",
        "/home/park/devel/mk-tokenizer/bpe_tokenizer/tokenizer.json");
    return s;
}

auto measure_time(corpus_search::searcher const &s, std::string search_term) -> std::vector<int>
{
    using namespace std::chrono;
    auto start_time = high_resolution_clock::now();

    auto result = s.search(search_term);

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
    EXPECT_EQ(measure_time(s(), "z").size(), 20'621);
    EXPECT_EQ(measure_time(s(), "o").size(), 1'286'797);
    EXPECT_EQ(measure_time(s(), "ho").size(), 811'047);
    EXPECT_EQ(measure_time(s(), "TT").size(), 0);
}

TEST(Searcher, SearchStringHard)
{
    EXPECT_EQ(measure_time(s(), "hoxni").size(), 94'307);
    EXPECT_EQ(measure_time(s(), "sixtaxsoxngixta").size(), 14);
    EXPECT_EQ(measure_time(s(), "ngixta").size(), 2'472);
    EXPECT_EQ(measure_time(s(), "kaxnanxho").size(), 719);
    EXPECT_EQ(measure_time(s(), "oxnon").size(), 74'953);
    EXPECT_EQ(measure_time(s(), "國家").size(), 296);
}
