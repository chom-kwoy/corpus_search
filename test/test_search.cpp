#include "test.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <fmt/chrono.h>
#include <fmt/os.h>

#include "searcher.hpp"

static FILE* output_file = nullptr;

static auto run_match_test(std::string search_term, int expected) -> std::vector<sentid_t>
{
    using namespace std::chrono;
    auto start_time = high_resolution_clock::now();

    auto index_accessor = [](int token) -> std::vector<corpus_search::token_match> {
        auto result = std::vector<corpus_search::token_match>{};
        auto& index = get_index().get_index();
        if (index.count(token) == 0) {
            return result;
        }
        auto const& hit = index.at(token);
        result.reserve(hit.entries.size());
        for (auto const& entry : hit.entries) {
            result.push_back({
                entry.sent_id,
                entry.pos,
#if CORPUS_SEARCH_NEXT_TOKEN_BITS > 0
                entry.next_tok,
#endif
            });
        }
        return result;
    };

    auto filter_accessor = [](int token) -> corpus_search::default_residue_filter {
        auto& index = get_index().get_index();
        if (index.count(token) == 0) {
            return {};
        }
        return index.at(token).filter;
    };

    auto result = search(get_tok(), index_accessor, filter_accessor, search_term);

    auto end_time = high_resolution_clock::now();

    if (result.candidates.size() < 200) {
        fmt::println(stderr,
                     "Result for '{}' = Array[{}]{{{}}}",
                     search_term,
                     result.candidates.size(),
                     fmt::join(result.candidates, ", "));
    } else {
        fmt::println(stderr,
                     "Result for '{}' = Array[{}]{{...}}",
                     search_term,
                     result.candidates.size());
        fmt::println(stderr, "Needs recheck = {}", result.needs_recheck);
    }

    auto elapsed_sec = duration_cast<duration<float, std::ratio<1, 1000>>>(end_time - start_time);
    fmt::println("Search for '{}': Took {}.", search_term, elapsed_sec);
    std::fflush(stdout);

    EXPECT_EQ(result.candidates.size(), expected);
    bool passed = result.candidates.size() == expected;

    auto test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    if (test_info != nullptr && output_file) {
        // write results to output file
        fmt::println(output_file,
                     "{}\tpass={}\telapsed={}\t{}",
                     test_info->test_case_name(),
                     passed,
                     elapsed_sec,
                     search_term);
    }

    return result.candidates;
}

class Searcher : public ::testing::Test
{
protected:
    // Called once before the first test in this test suite
    static void SetUpTestSuite()
    {
        get_tok(), get_index();

        const auto outfile = std::getenv("IBPE_TEST_WRITE_SEARCH_RESULTS");
        if (outfile) {
            output_file = std::fopen(fmt::format("test_search_{}.txt", outfile).c_str(), "w");
        }
    }

    // Called once after the last test in this test suite
    static void TearDownTestSuite() { std::fclose(output_file); }
};

TEST_F(Searcher, SearchStringSimple1)
{
    run_match_test("ho", 811034);
}
TEST_F(Searcher, SearchStringSimple2)
{
    run_match_test("o", 1286676);
}
TEST_F(Searcher, SearchStringSimple3)
{
    run_match_test("z", 20621);
}
TEST_F(Searcher, SearchStringSimple4)
{
    run_match_test("TT", 0);
}

TEST_F(Searcher, SearchStringHard1)
{
    run_match_test("si\\.ta\\.so\\.ngi\\.ta", 14);
}
TEST_F(Searcher, SearchStringHard2)
{
    run_match_test("ho\\.ni", 94299);
    run_match_test("ngi\\.ta", 2472);
    run_match_test("ka\\.nan\\.ho", 719);
    run_match_test("o\\.non", 74946);
    run_match_test("國家", 296);
    run_match_test("家non", 59);
}

TEST_F(Searcher, SearchRegexEasy1)
{
    run_match_test("cho\\.c[ou]\\.ni", 168);
}
TEST_F(Searcher, SearchRegexEasy2)
{
    run_match_test("cho\\.cw?[ou]\\.n", 231);
}
TEST_F(Searcher, SearchRegexEasy3)
{
    run_match_test("w[ou]\\.toy", 44781);
}

TEST_F(Searcher, SearchRegexHard1)
{
    run_match_test("(k[aeiou]\\.){3}k", 0);
}
TEST_F(Searcher, SearchRegexHard2)
{
    run_match_test(HANJA_RE "`i", 61261);
}
TEST_F(Searcher, SearchRegexHard3)
{
    run_match_test("pskuy....", 1763);
}
TEST_F(Searcher, SearchRegexHard4)
{
    run_match_test("[a-zA-Z. ]{4}pskuy", 771);
}

TEST_F(Searcher, SearchRegexUnicode)
{
    run_match_test("\\p{Script=Han}`i", 61263);
}

TEST_F(Searcher, SearchRegexInfinite)
{
    run_match_test(".*abc", 0);
}

TEST_F(Searcher, SearchRegexMatchAll)
{
    run_match_test(".*", 1733874);
}
