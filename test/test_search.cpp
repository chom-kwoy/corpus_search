#include "test.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <fmt/chrono.h>

#include "searcher.hpp"

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

class Searcher : public ::testing::Test
{
protected:
    // Called once before the first test in this test suite
    static void SetUpTestSuite() { get_tok(), get_index(); }

    // Called once after the last test in this test suite
    static void TearDownTestSuite() {}
};

TEST_F(Searcher, SearchStringSimple1)
{
    EXPECT_EQ(measure_time("ho").size(), 811'085);
}

TEST_F(Searcher, SearchStringSimple2)
{
    EXPECT_EQ(measure_time("z").size(), 20'621);
    EXPECT_EQ(measure_time("o").size(), 1'286'817);
    EXPECT_EQ(measure_time("TT").size(), 0);
}

TEST_F(Searcher, SearchRegexMatchAll)
{
    EXPECT_EQ(measure_time(".*").size(), 1'734'021);
}

TEST_F(Searcher, SearchRegexInfinite)
{
    EXPECT_EQ(measure_time(".*abc").size(), 1'734'021);
}

TEST_F(Searcher, SearchStringHard)
{
    get_tok(), get_index();

    EXPECT_EQ(measure_time("ho\\.ni").size(), 94'307);
    EXPECT_EQ(measure_time("si\\.ta\\.so\\.ngi\\.ta").size(), 14);
    EXPECT_EQ(measure_time("ngi\\.ta").size(), 2'472);
    EXPECT_EQ(measure_time("ka\\.nan\\.ho").size(), 719);
    EXPECT_EQ(measure_time("o\\.non").size(), 74'953);
    EXPECT_EQ(measure_time("國家").size(), 296);
    EXPECT_EQ(measure_time("家non").size(), 59);
}

TEST_F(Searcher, SearchRegexEasy)
{
    get_tok(), get_index();

    EXPECT_EQ(measure_time("cho\\.c[ou]\\.ni").size(), 168);
    EXPECT_EQ(measure_time("cho\\.cw?[ou]\\.n").size(), 231);
    EXPECT_EQ(measure_time("w[ou]\\.toy").size(), 44'782);
}

TEST_F(Searcher, SearchRegexHard1)
{
    get_tok(), get_index();

    EXPECT_EQ(measure_time("(k[aeiou]\\.){3}k").size(), 0);
}

TEST_F(Searcher, SearchRegexHard2)
{
    get_tok(), get_index();

    EXPECT_EQ(measure_time(HANJA_RE "`i").size(), 61'261);
}

TEST_F(Searcher, SearchRegexHard3)
{
    get_tok(), get_index();

    EXPECT_EQ(measure_time("....pskuy").size(), 776);
}
