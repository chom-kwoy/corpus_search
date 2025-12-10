#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <fmt/chrono.h>
#include <llguidance.h>

#include "dfa_trie.hpp"
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

auto test_parse(std::string regex) -> corpus_search::regex::sm::graph
{
    fmt::println("Regex: {}", regex);

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
            fmt::println("  [{}-{}] --> State {}",
                         printch(edge.range.min),
                         printch(edge.range.max),
                         edge.target_state);
        }
    };

    fmt::println("");

    return dfa;
}

#define HANJA_RE "[\u4E00-\u9FCC\u3400-\u4DB5]"
TEST(Regex, Regex)
{
    test_parse(HANJA_RE "`i");
    test_parse("(k[aeiou]\\.){3}k");
    test_parse("a(a|ba)*|c*a");
    test_parse("abc[^a-zA-Z]+?(?<name>st|uv)(?:pid)*\\?");
    // test_parse("abc[a-zA-Z]+?(?<name>st|uv)(?:pid)*\\b\\d*\\?\\p{Script=Han}$");
}

static auto to_bitset(std::uint32_t const* mask, int len) -> roaring::Roaring
{
    auto result = roaring::Roaring{};
    for (int i = 0; i < (len + 31) / 32; ++i) {
        if (mask[i] == 0) {
            continue;
        }
        for (int b = 0; b < 32; ++b) {
            if ((mask[i] >> b) & 0b1) {
                result.add(i * 32 + b);
            }
        }
    }
    return result;
}

static auto nonzero_pos(roaring::Roaring const& bitmap)
{
    std::vector<uint32_t> result(bitmap.cardinality());
    bitmap.toUint32Array(result.data());
    return result;
}

TEST(Regex, RegexTrie)
{
    auto const regex = "[^\u4FCD-\u9FCC\u3400-\u4DB5]`i";

    // llg implementation
    LlgConstraintInit init;
    llg_constraint_init_set_defaults(&init, get_tok().get_ll_tokenizer());
    struct LlgMatcherDeleter
    {
        void operator()(LlgMatcher* p) const { llg_free_matcher(p); }
    };
    auto m = std::unique_ptr<LlgMatcher, LlgMatcherDeleter>(llg_new_matcher(&init, "regex", regex));
    if (llg_matcher_get_error(m.get())) {
        throw std::runtime_error("Error constructing constraint");
    }
    if (llg_matcher_compute_mask(m.get())) {
        throw std::runtime_error("Error computing mask");
    }
    auto llg_bitmap = to_bitset(llg_matcher_get_mask(m.get()), get_tok().vocab_size());
    auto llg_next_tokens = nonzero_pos(llg_bitmap);

    // my implementation
    auto dfa = test_parse(regex);
    auto trie = corpus_search::dfa_trie(get_tok());
    auto my_bitmap = trie.get_next_tids(dfa, dfa.start_state);
    auto my_next_tokens = nonzero_pos(my_bitmap);

    // test if they are the same
    std::vector<int> symmetric_difference;
    std::set_symmetric_difference(llg_next_tokens.begin(),
                                  llg_next_tokens.end(),
                                  my_next_tokens.begin(),
                                  my_next_tokens.end(),
                                  std::back_inserter(symmetric_difference));
    fmt::println("symmetric difference = [{}]", fmt::join(symmetric_difference, ", "));
    EXPECT_EQ(symmetric_difference.size(), 0);

    std::vector<int> difference;
    std::set_difference(llg_next_tokens.begin(),
                        llg_next_tokens.end(),
                        my_next_tokens.begin(),
                        my_next_tokens.end(),
                        std::back_inserter(difference));
    auto diff_tokens = std::vector<std::string>{};
    for (int tid : difference) {
        diff_tokens.push_back(get_tok().get_tid_to_token().at(tid));
    }
    fmt::println("difference = [{}] (tokens = [{}])",
                 fmt::join(difference, ", "),
                 fmt::join(diff_tokens, ", "));
    EXPECT_EQ(difference.size(), 0);
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

TEST(Searcher, SearchRegexHard)
{
    get_tok(), get_index();

    EXPECT_EQ(measure_time("(k[aeiou]\\.){3}k").size(), 0);
    // EXPECT_EQ(measure_time(HANJA_RE "`i").size(), 61'261);
}
