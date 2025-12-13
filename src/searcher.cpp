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

using candset = std::optional<std::vector<index_entry>>;

namespace { // static linkage

auto get_sent_ids(std::vector<index_entry> const &self) -> std::vector<sentid_t>
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

auto followed_by(std::vector<index_entry> &&self, candset const &other) -> std::vector<index_entry>
{
    if (!other.has_value()) {
        return self;
    }

    auto const &arr1 = self;
    auto const &arr2 = other.value();

    std::vector<index_entry> result;

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
                    result.push_back(*it1);
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

using vec_pointer = std::span<const index_entry>;
using vec_object = std::vector<index_entry>;
using pointer_or_object = std::variant<vec_pointer, vec_object>;

auto merge_sorted_lists(std::vector<pointer_or_object> const &cand_lists) -> std::vector<index_entry>
{
    auto result = std::vector<index_entry>{};

    using queue_item = std::tuple<index_entry, int, int>;
    struct comparator
    {
        auto operator()(queue_item const &l, queue_item const &r) -> bool
        {
            return std::get<0>(r) < std::get<0>(l);
        }
    };
    auto pending = std::priority_queue<queue_item, std::vector<queue_item>, comparator>{};

    auto get_size = [](auto &&x) { return std::visit([](auto &&arg) { return arg.size(); }, x); };
    auto get_item = [](auto &&x, std::size_t index) {
        return std::visit([index](auto &&arg) { return arg[index]; }, x);
    };

    for (int i = 0; i < cand_lists.size(); ++i) {
        if (get_size(cand_lists[i]) >= 1) {
            pending.push({get_item(cand_lists[i], 0), i, 0});
        }
    }
    while (!pending.empty()) {
        auto [item, vec_index, item_index] = pending.top();
        pending.pop();
        if (item_index + 1 < get_size(cand_lists[vec_index])) {
            pending.push({
                get_item(cand_lists[vec_index], item_index + 1),
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

struct cache_entry
{
    std::vector<index_entry> cands;
};

auto generate_cands(int state,
                    std::set<int> &visited_states,
                    std::string const &prev_prefix, // for debugging only
                    tokenizer const &tok,
                    regex::sm::graph const &dfa,
                    std::function<index_accessor> const &index,
                    std::unordered_map<int, cache_entry> &cache,
                    int level = 1) -> std::vector<index_entry>
{
    if (cache.contains(state)) {
        return cache.at(state).cands;
    }

    auto next_tokens = tok.trie().get_next_tids(dfa, state);

    fmt::println("lvl {} (state={}): '{}' (+ {} tokens)",
                 level,
                 state,
                 prev_prefix,
                 next_tokens.cardinality());

    auto cand_lists = std::vector<pointer_or_object>{};

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
            cand_lists.push_back(matches);
            continue;
        }

        if (visited_states.contains(new_state)) {
            throw std::runtime_error("infinite recursion detected");
        }

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
        cand_lists.push_back(followed_by(std::move(matches), cands));
    }

    // union the sorted sequences in vec_result
    auto result = merge_sorted_lists(cand_lists);

    if (level > 0) {
        cache[state] = {result};
    }

    return result;
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
        return get_sent_ids(index(tok.EOS_TOKEN_ID));
    }

    auto cand_lists = std::vector<pointer_or_object>{};

    std::unordered_map<int, cache_entry> cache;
    std::set<int> visited_states = {dfa.start_state};
    for (int p = 0; p < tok.max_token_bytes(); ++p) {
        auto next_tokens = tok.trie().get_next_tids(dfa, dfa.start_state, p);

        fmt::println("p={}\nlvl {}: '{}' (+ {} tokens)", p, 0, "", next_tokens.cardinality());

        for (auto tid : next_tokens) {
            auto matches = index(tid);
            if (matches.size() == 0) {
                continue;
            }

            auto token_str = tok.get_tid_to_token().at(tid).substr(p);
            int new_state = tok.trie().consume_token(dfa, dfa.start_state, token_str);
            assert(new_state != dfa_trie::REJECTED);

            if (visited_states.contains(new_state)) {
                throw std::runtime_error("infinite recursion detected");
            }

            if (new_state == dfa_trie::ACCEPTED) {
                cand_lists.push_back(matches);
            } else {
                visited_states.insert(new_state);
                auto cands
                    = generate_cands(new_state, visited_states, token_str, tok, dfa, index, cache);
                visited_states.erase(new_state);
                cand_lists.push_back(followed_by(std::move(matches), cands));
            }
        }
    }

    std::fflush(stdout);

    return get_sent_ids(merge_sorted_lists(cand_lists));
}

} // namespace corpus_search
