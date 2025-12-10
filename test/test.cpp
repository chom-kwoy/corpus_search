#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <fmt/chrono.h>

#include "regex_dfa.hpp"
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
    // auto const regex = "abc[a-zA-Z]+?(?<name>st|uv)(?:pid)*\\b\\d*\\?\\p{Script=Han}$";
    auto const regex = "abc[^a-zA-Z]+?(?<name>st|uv)(?:pid)*\\?";
    // auto const regex = "a(a|ba)*|c*a";
    // auto const regex = "(k[aeiou]\\.){3}k";

    auto cst = corpus_search::regex::parse(regex);
    fmt::println("CST: {}", corpus_search::regex::print_cst(cst));

    auto ast = corpus_search::regex::cst_to_ast(cst);
    fmt::println("AST: {}", corpus_search::regex::print_ast(ast));

    auto dfa = corpus_search::regex::ast_to_dfa(ast);
    fmt::println("DFA: start_state={}, accept_states=[{}], num_states={}",
                 dfa.start_state,
                 fmt::join(dfa.accept_states, ", "),
                 dfa.num_states);

    auto printch = [](char ch) {
        if (std::isprint(ch)) {
            return fmt::format("'{}'", ch);
        }
        return fmt::format("\\{:x}", ch);
    };

    for (auto&& [state, edges] : dfa.edges) {
        fmt::println("State {} {}", state, dfa.accept_states.contains(state) ? "(accept)" : "");
        for (auto&& edge : edges) {
            fmt::println("  [{}-{}] --> {}",
                         printch(edge.range.min),
                         printch(edge.range.max),
                         edge.target_state);
        }
    };
}

TEST(Searcher, SearchStringSimple)
{
    get_tok(), get_index();

    EXPECT_EQ(measure_time("z").size(), 20'621);
    EXPECT_EQ(measure_time("o").size(), 1'286'817);
    EXPECT_EQ(measure_time("ho").size(), 811'085);
    EXPECT_EQ(measure_time("TT").size(), 0);
}

TEST(Searcher, SearchStringHard)
{
    get_tok(), get_index();

    EXPECT_EQ(measure_time("ho\\.ni").size(), 94'307);
    EXPECT_EQ(measure_time("si\\.ta\\.so\\.ngi\\.ta").size(), 14);
    EXPECT_EQ(measure_time("ngi\\.ta").size(), 2'472);
    EXPECT_EQ(measure_time("ka\\.nan\\.ho").size(), 719);
    EXPECT_EQ(measure_time("o\\.non").size(), 74'953);
    EXPECT_EQ(measure_time("國家").size(), 296);
}

TEST(Searcher, SearchRegexEasy)
{
    get_tok(), get_index();

    EXPECT_EQ(measure_time("cho\\.c[ou]\\.ni").size(), 168);
    EXPECT_EQ(measure_time("w[ou]\\.toy").size(), 44'782);
}

#define HANJA_RE "[\u4E00-\u9FCC\u3400-\u4DB5]"

TEST(Searcher, SearchRegexHard)
{
    get_tok(), get_index();

    EXPECT_EQ(measure_time("(k[aeiou]\\.){3}k").size(), 0);
    // EXPECT_EQ(measure_time(HANJA_RE "`i").size(), 61'261);
}
