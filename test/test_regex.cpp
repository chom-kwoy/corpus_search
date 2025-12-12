#include "test.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fmt/chrono.h>
#include <llguidance.h>
#include <nlohmann/json.hpp>

#include "dfa_trie.hpp"

static auto test_parse(std::string regex) -> corpus_search::regex::sm::graph
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
    std::fflush(stdout);

    return dfa;
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

static auto replace_chars(std::string_view string,
                          std::unordered_map<char, char> const& mapping,
                          bool* was_replaced = nullptr) -> std::string
{
    bool is_replaced = false;
    auto new_string = std::string(string);
    for (int i = 0; i < string.size(); ++i) {
        if (mapping.contains(string[i])) {
            new_string[i] = mapping.at(string[i]);
            is_replaced = true;
        }
    }
    if (was_replaced) {
        *was_replaced = is_replaced;
    }
    return new_string;
}

static auto call_tokenize(const void* user_data,
                          const uint8_t* bytes,
                          size_t bytes_len,
                          uint32_t* output_tokens,
                          size_t output_tokens_len) noexcept -> std::size_t
{
    auto t = static_cast<corpus_search::tokenizer const*>(user_data);

    auto input = std::string(bytes, bytes + bytes_len);
    auto result = t->tokenize(input);

    for (int i = 0; i < std::min(result.size(), output_tokens_len); ++i) {
        output_tokens[i] = result[i];
    }

    return result.size();
}

static auto load_llg_tokenizer(corpus_search::tokenizer const& tok,
                               nlohmann::json json) -> LlgTokenizer*
{
    // replace vocab with unnormalized tokens
    auto& vocab = json["model"]["vocab"];
    std::unordered_map<std::string, int> new_vocab;
    for (auto& [tok_str, tok_id] : vocab.items()) {
        bool is_replaced;
        auto new_tok_str = replace_chars(tok_str, tok.inv_normalize_mapping(), &is_replaced);
        if (!new_vocab.contains(new_tok_str) || is_replaced) {
            new_vocab[new_tok_str] = tok_id;
        }
    }
    vocab = new_vocab;

    // replace merges
    auto& merges = json["model"]["merges"];
    std::vector<std::vector<std::string>> new_merges = merges;
    for (auto& merge : new_merges) {
        merge.at(0) = replace_chars(merge.at(0), tok.inv_normalize_mapping());
        merge.at(1) = replace_chars(merge.at(1), tok.inv_normalize_mapping());
    }
    merges = new_merges;

    auto const modified_json = json.dump();

    LlgTokenizerInit tok_init = {};
    tok_init.tok_eos = corpus_search::tokenizer::EOS_TOKEN_ID;
    tok_init.use_approximate_greedy_tokenize_fn = false;
    tok_init.tokenize_user_data = &tok;
    tok_init.tokenize_fn = call_tokenize;
    tok_init.tokenizer_json = modified_json.c_str();

    char error_buf[256];
    LlgTokenizer* ll_tokenizer = llg_new_tokenizer(&tok_init, error_buf, sizeof(error_buf));
    if (ll_tokenizer == nullptr) {
        throw std::runtime_error(fmt::format("Error creating tokenizer: {}", error_buf));
    }

    return ll_tokenizer;
}

TEST(Regex, RegexOptional)
{
    test_parse("cho\\.cw?o\\.ni");
}

TEST(Regex, Regex)
{
    test_parse(HANJA_RE "`i");
    test_parse("(k[aeiou]\\.){3}k");
    test_parse("a(a|ba)*|c*a");
    test_parse("abc[^a-zA-Z]+?(?<name>st|uv)(?:pid)*\\?");
}

TEST(Regex, RegexComplex)
{
    test_parse("abc[a-zA-Z]+?(?<name>st|uv)(?:pid)*\\b\\d*\\?\\p{Script=Han}$");
}

TEST(Regex, RegexTrie)
{
    auto dfa = test_parse("(k[aeiou]\\.){3}k");
    auto trie = corpus_search::dfa_trie(get_tok());

    int state = dfa.start_state;
    auto my_bitmap = trie.get_next_tids(dfa, state);
    auto my_next_tokens = nonzero_pos(my_bitmap);

    auto tokens = std::vector<std::string>{};
    for (int tid : my_next_tokens) {
        tokens.push_back(get_tok().get_tid_to_token().at(tid));
    }

    fmt::println("next tokens = [{}]", fmt::join(tokens, ", "));

    state = trie.consume_token(dfa, state, "ka");
    ASSERT_NE(state, corpus_search::dfa_trie::ACCEPTED);
    ASSERT_NE(state, corpus_search::dfa_trie::REJECTED);

    my_bitmap = trie.get_next_tids(dfa, state);
    my_next_tokens = nonzero_pos(my_bitmap);
    tokens.clear();
    for (int tid : my_next_tokens) {
        tokens.push_back(get_tok().get_tid_to_token().at(tid));
    }

    fmt::println("next state = {}", state);
    fmt::println("next tokens = [{}]", fmt::join(tokens, ", "));

    state = trie.consume_token(dfa, state, ".ku");
    ASSERT_NE(state, corpus_search::dfa_trie::ACCEPTED);
    ASSERT_NE(state, corpus_search::dfa_trie::REJECTED);

    my_bitmap = trie.get_next_tids(dfa, state);
    my_next_tokens = nonzero_pos(my_bitmap);
    tokens.clear();
    for (int tid : my_next_tokens) {
        tokens.push_back(get_tok().get_tid_to_token().at(tid));
    }

    fmt::println("next state = {}", state);
    fmt::println("next tokens = [{}]", fmt::join(tokens, ", "));

    state = trie.consume_token(dfa, state, ".ko");
    ASSERT_NE(state, corpus_search::dfa_trie::ACCEPTED);
    ASSERT_NE(state, corpus_search::dfa_trie::REJECTED);

    my_bitmap = trie.get_next_tids(dfa, state);
    my_next_tokens = nonzero_pos(my_bitmap);
    tokens.clear();
    for (int tid : my_next_tokens) {
        tokens.push_back(get_tok().get_tid_to_token().at(tid));
    }

    fmt::println("next state = {}", state);
    fmt::println("next tokens = [{}]", fmt::join(tokens, ", "));

    state = trie.consume_token(dfa, state, ".k");
    ASSERT_EQ(state, corpus_search::dfa_trie::ACCEPTED);
}

TEST(Regex, RegexTrieParity)
{
    const std::string regex = "[^\u4FCD-\u9FCC\u3400-\u4DB5]`i";

    auto file = std::ifstream(TOKENIZER_FILE);
    auto json = nlohmann::json::parse((std::stringstream{} << file.rdbuf()).str());

    auto tokenizer = load_llg_tokenizer(get_tok(), json);

    // llg implementation
    LlgConstraintInit init;
    llg_constraint_init_set_defaults(&init, tokenizer);
    struct LlgMatcherDeleter
    {
        void operator()(LlgMatcher* p) const { llg_free_matcher(p); }
    };
    auto m = std::unique_ptr<LlgMatcher, LlgMatcherDeleter>(
        llg_new_matcher(&init, "regex", (regex + ".*").c_str()));
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
