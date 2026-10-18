#include "searcher.hpp"

#include "dfa_trie.hpp"

#include <fmt/chrono.h>
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <msgpack.hpp>
#include <nlohmann/json.hpp>
#include <queue>
#include <tokenizers_cpp.h>
#include <utf8.h>

namespace corpus_search {

namespace { // static linkage

auto get_sent_ids(std::span<const token_match> self) -> std::vector<sentid_t>
{
    std::vector<sentid_t> output;
    sentid_t last_sent_id = -1;
    for (auto entry : self) {
        if (entry.sent_id != last_sent_id) {
            output.push_back(entry.sent_id);
        }
        last_sent_id = entry.sent_id;
    }
    return output;
}

auto get_all_sent_ids(std::function<index_accessor> const &index) -> std::vector<sentid_t>
{
    auto matches = index(tokenizer::BOS_TOKEN_ID);
    return get_sent_ids(matches);
}

// Filter out elements from arr1 that are not followed by arr2
// i.e., result = A \cap (B - 1)
auto followed_by(std::span<const token_match> arr1,
                 std::span<const token_match> arr2) -> std::vector<token_match>
{
    std::vector<token_match> result;

    auto it1 = arr1.begin();
    auto it2 = arr2.begin();
    while (it1 != arr1.end() && it2 != arr2.end()) {
        auto entry1 = *it1;
        auto entry2 = *it2;
        if (entry1.sent_id < entry2.sent_id) {
            ++it1;
        } else {
            if (entry1.sent_id == entry2.sent_id) {
                if (entry1.pos + 1 < entry2.pos) {
                    ++it1;
                } else if (entry1.pos + 1 == entry2.pos) {
                    result.push_back({entry1.sent_id, entry1.pos});
                    ++it1;
                    ++it2;
                } else {
                    ++it2;
                }
            } else {
                ++it2;
            }
        }
    }

    return result;
}

auto merge_sorted_lists(std::vector<std::vector<token_match>> const &cand_lists)
    -> std::vector<token_match>
{
    auto result = std::vector<token_match>{};

    using queue_item = std::tuple<token_match, int, int>;
    struct comparator
    {
        auto operator()(queue_item const &l, queue_item const &r) -> bool
        {
            return std::get<0>(r) < std::get<0>(l);
        }
    };
    auto pending = std::priority_queue<queue_item, std::vector<queue_item>, comparator>{};

    for (int i = 0; i < cand_lists.size(); ++i) {
        if (cand_lists[i].size() >= 1) {
            pending.push({cand_lists[i][0], i, 0});
        }
    }
    while (!pending.empty()) {
        auto [item, vec_index, item_index] = pending.top();
        pending.pop();
        if (item_index + 1 < cand_lists[vec_index].size()) {
            pending.push({
                cand_lists[vec_index][item_index + 1],
                vec_index,
                item_index + 1,
            });
        }
        if (result.size() == 0 || item != result.back()) {
            result.push_back(item);
        }
    }

    assert(std::is_sorted(result.begin(), result.end()));

    return result;
}

constexpr int CANDS_THRESHOLD = 10'000'000;

struct cand_result
{
    std::optional<std::vector<token_match>> cands;
    bool needs_recheck;
};

enum class token_start {
    boundary,
    any_byte,
};

struct token_and_offset
{
    int token;
    int offset;
};

auto generate_cands(int state,
                    token_start start,
                    std::set<int> &visited_states,
                    std::string const &prev_prefix, // for debugging only
                    tokenizer const &tok,
                    regex::sm::graph const &dfa,
                    std::function<index_accessor> const &index,
                    std::unordered_map<int, cand_result> &cache,
                    int level = 1) -> cand_result
{
    if (start == token_start::boundary && cache.contains(state)) {
        return cache.at(state);
    }

    auto next_tokens = std::vector<token_and_offset>{};
    if (start == token_start::any_byte) {
        for (int offset = 0; offset < tok.max_token_bytes(); ++offset) {
            for (int token : tok.trie().get_next_tids(dfa, state, -1, offset)) {
                next_tokens.push_back({token, offset});
            }
        }
    } else {
        for (int token : tok.trie().get_next_tids(dfa, state)) {
            next_tokens.push_back({token, 0});
        }
    }

    fmt::println(stderr,
                 "lvl {} (state={}): '{}' (+ {} tokens)",
                 level,
                 state,
                 prev_prefix,
                 next_tokens.size());
    std::fflush(stderr);

    auto full_cands = std::vector<std::vector<token_match>>{};
    bool needs_recheck = false;

    int num_elems = 0;
    for (auto [token, offset] : next_tokens) {
        assert(token != tok.EOS_TOKEN_ID);

        auto token_str = tok.get_tid_to_token().at(token).substr(offset);
        auto cur_prefix = prev_prefix + token_str;

        auto matches = index(token);
        if (matches.size() == 0) {
            continue;
        }

        int new_state = tok.trie().consume_token(dfa, state, token_str);
        assert(new_state != dfa_trie::REJECTED);
        if (new_state == dfa_trie::ACCEPTED) {
            full_cands.push_back(std::move(matches));
        } else if (start == token_start::boundary && visited_states.contains(new_state)) {
            // infinite recursion detected
            fmt::println(stderr, "Warning: infinite recursion detected; aborting..");
            std::fflush(stderr);

            return cache[state] = {std::nullopt, true};
        } else {
            visited_states.insert(new_state);
            auto r = generate_cands(new_state,
                                    token_start::boundary,
                                    visited_states,
                                    cur_prefix,
                                    tok,
                                    dfa,
                                    index,
                                    cache,
                                    level + 1);
            visited_states.erase(new_state);
            if (r.cands.has_value()) {
                full_cands.push_back(followed_by(matches, r.cands.value()));
            } else {
                full_cands.push_back(matches);
            }
            needs_recheck = needs_recheck || r.needs_recheck;
        }

        num_elems += full_cands.back().size();

        if (num_elems > CANDS_THRESHOLD) {
            fmt::println(stderr,
                         "Warning: more than {} candidate matches generated; aborting..",
                         CANDS_THRESHOLD);
            std::fflush(stderr);

            if (start == token_start::boundary) {
                return cache[state] = {std::nullopt, true};
            }
            return {std::nullopt, true};
        }
    }

    // union the sorted sequences in vec_result
    auto full_result = merge_sorted_lists(full_cands);

    if (start == token_start::boundary) {
        return cache[state] = {full_result, needs_recheck};
    }
    return {full_result, needs_recheck};
}

} // namespace

auto search(tokenizer const &tok,
            std::function<index_accessor> const &index,
            std::function<filter_accessor> const &filter,
            std::string const &regex) -> search_result
{
    fmt::println(stderr, "Regex = {}", regex);

    auto cst = corpus_search::regex::parse(regex);
    fmt::println(stderr, "CST: {}", corpus_search::regex::print_cst(cst));

    auto ast = corpus_search::regex::cst_to_ast(cst);
    fmt::println(stderr, "AST: {}", corpus_search::regex::print_ast(ast));

    auto dfa = corpus_search::regex::ast_to_dfa(ast);
    corpus_search::regex::print_dfa(stderr, dfa);

    if (dfa.accept_states.contains(dfa.start_state)) {
        // every string matches
        fmt::println(stderr, "DFA accepts empty string; returning all sentence IDs.");
        return {get_all_sent_ids(index), true};
    }

    std::unordered_map<int, cand_result> cache;
    std::set<int> visited_states = {dfa.start_state};

    auto result = generate_cands(dfa.start_state,
                                 token_start::any_byte,
                                 visited_states,
                                 "",
                                 tok,
                                 dfa,
                                 index,
                                 cache,
                                 0);

    std::fflush(stderr);

    if (!result.cands.has_value()) {
        return {get_all_sent_ids(index), true};
    }

    return {
        get_sent_ids(result.cands.value()),
        dfa.needs_recheck || result.needs_recheck,
    };
}

} // namespace corpus_search
