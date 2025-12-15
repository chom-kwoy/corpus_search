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

auto get_sent_ids(std::span<const token_range> self) -> std::vector<sentid_t>
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

auto followed_by(std::span<const token_range> arr1,
                 std::span<const token_range> arr2) -> std::vector<token_range>
{
    std::vector<token_range> result;

    auto it1 = arr1.begin();
    auto it2 = arr2.begin();
    while (it1 != arr1.end() && it2 != arr2.end()) {
        auto entry1 = *it1;
        auto entry2 = *it2;
        if (entry1.sent_id < entry2.sent_id) {
            ++it1;
        } else {
            if (entry1.sent_id == entry2.sent_id) {
                if (entry1.j < entry2.i) {
                    ++it1;
                } else if (entry1.j == entry2.i) {
                    result.push_back({entry1.sent_id, entry1.i, entry2.j});
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

auto merge_sorted_lists(std::vector<std::vector<token_range>> const &cand_lists)
    -> std::vector<token_range>
{
    auto result = std::vector<token_range>{};

    using queue_item = std::tuple<token_range, int, int>;
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

auto generate_cands(int state,
                    std::set<int> &visited_states,
                    std::string const &prev_prefix, // for debugging only
                    tokenizer const &tok,
                    regex::sm::graph const &dfa,
                    std::function<index_accessor> const &index,
                    std::unordered_map<int, std::optional<std::vector<token_range>>> &cache,
                    int level = 1) -> std::optional<std::vector<token_range>>
{
    if (cache.contains(state)) {
        return cache.at(state);
    }

    auto next_tokens = tok.trie().get_next_tids(dfa, state);

    fmt::println("lvl {} (state={}): '{}' (+ {} tokens)",
                 level,
                 state,
                 prev_prefix,
                 next_tokens.cardinality());
    std::fflush(stdout);

    auto full_cands = std::vector<std::vector<token_range>>{};

    int num_elems = 0;
    for (int token : next_tokens) {
        assert(token != tok.EOS_TOKEN_ID);

        auto token_str = tok.get_tid_to_token().at(token);
        auto cur_prefix = prev_prefix + token_str;

        auto matches = index(token);
        if (matches.size() == 0) {
            continue;
        }

        int new_state = tok.trie().consume_token(dfa, state, token_str);
        assert(new_state != dfa_trie::REJECTED);
        if (new_state == dfa_trie::ACCEPTED) {
            full_cands.push_back(std::move(matches));
        } else if (visited_states.contains(new_state)) {
            // infinite recursion detected
            fmt::println("Warning: infinite recursion detected; aborting..");
            std::fflush(stdout);

            return cache[state] = std::nullopt;
        } else {
            visited_states.insert(new_state);
            auto cands = generate_cands(new_state,
                                        visited_states,
                                        cur_prefix,
                                        tok,
                                        dfa,
                                        index,
                                        cache,
                                        level + 1);
            visited_states.erase(new_state);
            if (cands.has_value()) {
                full_cands.push_back(followed_by(matches, cands.value()));
            } else {
                full_cands.push_back(matches);
            }
        }

        num_elems += full_cands.back().size();

        if (num_elems > CANDS_THRESHOLD) {
            fmt::println("Warning: more than {} candidate matches generated; aborting..",
                         CANDS_THRESHOLD);
            std::fflush(stdout);

            return cache[state] = std::nullopt;
        }
    }

    // union the sorted sequences in vec_result
    auto full_result = merge_sorted_lists(full_cands);

    return cache[state] = full_result;
}

} // namespace

auto search(tokenizer const &tok,
            std::function<index_accessor> const &index,
            std::string const &regex) -> std::vector<sentid_t>
{
    fmt::println("Regex = {}", regex);

    auto cst = corpus_search::regex::parse(regex);
    fmt::println("CST: {}", corpus_search::regex::print_cst(cst));

    auto ast = corpus_search::regex::cst_to_ast(cst);
    fmt::println("AST: {}", corpus_search::regex::print_ast(ast));

    auto dfa = corpus_search::regex::ast_to_dfa(ast);
    fmt::println("DFA: start_state={}, accept_states=[{}], num_states={}",
                 dfa.start_state,
                 fmt::join(dfa.accept_states, ", "),
                 dfa.num_states);

    if (dfa.accept_states.contains(dfa.start_state)) {
        // every string matches
        fmt::println("DFA accepts empty string; returning all sentence IDs.");
        std::vector<sentid_t> output;
        sentid_t last_sent_id = -1;
        for (auto entry : index(tok.BOS_TOKEN_ID)) {
            if (entry.sent_id != last_sent_id) {
                output.push_back(entry.sent_id);
            }
            last_sent_id = entry.sent_id;
        }
        return output;
    }

    auto cand_lists = std::vector<std::vector<token_range>>{};

    std::unordered_map<int, std::optional<std::vector<token_range>>> cache;
    std::set<int> visited_states = {dfa.start_state};

    struct token_and_offset
    {
        int token;
        int pad_size;
    };
    auto next_tokens = std::vector<token_and_offset>{};
    for (int pad = 0; pad < tok.max_token_bytes(); ++pad) {
        for (int token : tok.trie().get_next_tids(dfa, dfa.start_state, -1, pad)) {
            next_tokens.push_back({token, pad});
        }
    }

    fmt::println("lvl {}: '{}' (+ {} tokens)", 0, "", next_tokens.size());

    int num_elems = 0;
    for (auto [tid, pad] : next_tokens) {
        auto matches = index(tid);
        if (matches.size() == 0) {
            continue;
        }

        auto token_str = tok.get_tid_to_token().at(tid).substr(pad);
        int new_state = tok.trie().consume_token(dfa, dfa.start_state, token_str);
        assert(new_state != dfa_trie::REJECTED);

        if (new_state == dfa_trie::ACCEPTED) {
            cand_lists.push_back(std::move(matches));
        } else {
            visited_states.insert(new_state);
            auto cands = generate_cands(new_state, visited_states, token_str, tok, dfa, index, cache);
            visited_states.erase(new_state);
            if (cands.has_value()) {
                cand_lists.push_back(followed_by(matches, cands.value()));
            } else {
                cand_lists.push_back(matches);
            }
        }

        num_elems += cand_lists.back().size();

        if (num_elems > CANDS_THRESHOLD) {
            fmt::println("Warning: more than {} candidate matches generated; aborting..",
                         CANDS_THRESHOLD);
            std::fflush(stdout);

            // return everything
            std::vector<sentid_t> output;
            sentid_t last_sent_id = -1;
            for (auto entry : index(tok.BOS_TOKEN_ID)) {
                if (entry.sent_id != last_sent_id) {
                    output.push_back(entry.sent_id);
                }
                last_sent_id = entry.sent_id;
            }
            return output;
        }
    }

    std::fflush(stdout);

    return get_sent_ids(merge_sorted_lists(cand_lists));
}

} // namespace corpus_search
