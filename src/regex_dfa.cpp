#include "regex_dfa.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <set>

namespace corpus_search::regex {

enum class node_type { concat, star, other };
struct node_table
{
    std::set<int> firstpos;
    std::set<int> lastpos;
    bool nullable;

    node_type type = node_type::other;
    std::vector<int> children{};
};

struct mark_state
{
    int cur_pos = 0;
    int cur_index = 0;

    // node index -> positions
    std::map<int, node_table> nodes{};

    // leaf pos -> character range
    std::map<int, ast::node_range> leaf_map{};

    auto visit_ast(ast::node const& node) -> node_table
    {
        const int my_pos = cur_pos;
        const int my_index = cur_index++;

        auto pos = std::visit(
            [my_pos, this](auto&& node) -> node_table {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, ast::node_empty>) {
                    if (node.assertion != ast::assertion_kind::none) {
                        // TODO: handle assertions
                        throw std::runtime_error("Assertions not implemented");
                    }
                    return {{}, {}, true};
                } else if constexpr (std::is_same_v<T, ast::node_range>) {
                    cur_pos++;
                    leaf_map[my_pos] = node;
                    return {{my_pos}, {my_pos}, false};
                } else if constexpr (std::is_same_v<T, ast::node_union>) {
                    auto result = node_table{};
                    result.nullable = false;
                    for (auto&& arg : node.args) {
                        auto p = visit_ast(arg);
                        result.firstpos.insert(p.firstpos.begin(), p.firstpos.end());
                        result.lastpos.insert(p.lastpos.begin(), p.lastpos.end());
                        result.nullable = result.nullable || p.nullable;
                    }
                    return result;
                } else if constexpr (std::is_same_v<T, ast::node_concat>) {
                    assert(node.args.size() == 2);

                    auto result = node_table{};
                    result.type = node_type::concat;

                    int ch0_idx = cur_index;
                    auto p0 = visit_ast(node.args[0]);
                    result.children.push_back(ch0_idx);

                    int ch1_idx = cur_index;
                    auto p1 = visit_ast(node.args[1]);
                    result.children.push_back(ch1_idx);

                    result.nullable = p0.nullable && p1.nullable;
                    result.firstpos.insert(p0.firstpos.begin(), p0.firstpos.end());
                    if (p0.nullable) {
                        result.firstpos.insert(p1.firstpos.begin(), p1.firstpos.end());
                    }

                    result.lastpos.insert(p1.lastpos.begin(), p1.lastpos.end());
                    if (p1.nullable) {
                        result.lastpos.insert(p0.lastpos.begin(), p0.lastpos.end());
                    }

                    return result;
                } else if constexpr (std::is_same_v<T, ast::node_star>) {
                    int ch_idx = cur_index;
                    auto p = visit_ast(node.arg);
                    auto result = node_table{};
                    result.firstpos = p.firstpos;
                    result.lastpos = p.lastpos;
                    result.nullable = true;
                    result.type = node_type::star;
                    return result;
                }
            },
            node.get());

        nodes[my_index] = pos;

        return pos;
    }
};

struct comparator
{
    bool operator()(const ast::node_range& a, const ast::node_range& b) const
    {
        return a.min < b.min;
    }
};

static auto merge_identical_states(sm::graph dfa) -> sm::graph
{
    using state_key = std::pair<std::set<sm::transition>, bool>;

    bool is_changed;
    do {
        std::map<state_key, int> unique_states;
        std::map<int, int> old_to_new_state;

        is_changed = false;
        for (auto&& [state_id, transition] : dfa.edges) {
            auto key = state_key{
                {transition.begin(), transition.end()},
                dfa.accept_states.contains(state_id),
            };
            if (unique_states.contains(key)) {
                old_to_new_state[state_id] = unique_states[key];
                is_changed = true;
            } else {
                int new_id = unique_states.size();
                old_to_new_state[state_id] = new_id;
                unique_states[key] = new_id;
            }
        }

        sm::graph result;
        result.start_state = old_to_new_state.at(dfa.start_state);
        result.num_states = unique_states.size();
        for (int state : dfa.accept_states) {
            result.accept_states.insert(old_to_new_state.at(state));
        }
        for (auto&& [state_id, transitions] : dfa.edges) {
            int new_id = old_to_new_state.at(state_id);
            if (result.edges.contains(new_id)) {
                continue;
            }
            result.edges[new_id] = {};
            for (auto&& tr : transitions) {
                int new_target = old_to_new_state.at(tr.target_state);
                result.edges[new_id].push_back({tr.range, new_target});
            }
        }

        dfa = std::move(result);

    } while (is_changed);

    return dfa;
}

// McNaughton-Yamada-Thompson algorithm
auto ast_to_dfa(ast::node const& node) -> sm::graph
{
    auto aug_node = ast::node{ast::node_concat{{
        node,
        {ast::node_range{}},
    }}};

    // fill firstpos, lastpos, and nullable
    mark_state visit_state{};
    visit_state.visit_ast(aug_node);

    auto const& nodes = visit_state.nodes;
    auto const& leaf_map = visit_state.leaf_map;
    int const final_pos = visit_state.cur_pos - 1;

    auto followpos = std::vector<std::set<int>>(visit_state.cur_pos);
    for (int i = 0; i < nodes.size(); ++i) {
        auto const& pos = nodes.at(i);

        if (pos.type == node_type::concat) {
            int ch0_idx = pos.children[0];
            const auto ch1 = nodes.at(pos.children[1]);
            for (int p : nodes.at(ch0_idx).lastpos) {
                followpos[p].insert(ch1.firstpos.begin(), ch1.firstpos.end());
            }
        } else if (pos.type == node_type::star) {
            for (int p : pos.lastpos) {
                followpos[p].insert(pos.firstpos.begin(), pos.firstpos.end());
            }
        }
    }

    std::vector<std::set<int>> states;
    std::map<std::set<int>, int> seen_states;

    sm::graph result;
    result.start_state = 0;
    result.num_states = 1;

    auto init_state = nodes.at(0).firstpos;
    states.push_back(init_state);
    seen_states[init_state] = 0;

    // initial state could also be an accept state
    if (init_state.contains(final_pos)) {
        result.accept_states.insert(0);
    }

    for (int s = 0; s < states.size(); ++s) {
        auto const& state = states[s];

        auto transitions = std::map<ast::node_range, std::set<int>, comparator>{};
        for (int p : state) {
            if (p == final_pos) {
                continue;
            }
            auto ch_range = leaf_map.at(p);

            auto to_add = std::map<ast::node_range, std::set<int>, comparator>{};
            auto lbound = transitions.upper_bound({ch_range.min, 0});
            if (lbound != transitions.begin()) {
                --lbound;
            }
            auto ubound = transitions.upper_bound({ch_range.max, 0});
            while (lbound != ubound && ch_range.min <= ch_range.max) {
                auto other_range = lbound->first;
                auto omin = std::max(ch_range.min, other_range.min);
                auto omax = std::min(ch_range.max, other_range.max);
                if (omin <= omax) {
                    // there is overlap
                    auto other_state = lbound->second;

                    // add non-overlapping parts
                    if (ch_range.min < omin) {
                        to_add[{ch_range.min, omin - 1}] = followpos[p];
                    }
                    if (other_range.min < omin) {
                        to_add[{other_range.min, omin - 1}] = other_state;
                    }
                    if (omax < other_range.max) {
                        to_add[{omax + 1, other_range.max}] = other_state;
                    }

                    // add overlapping part
                    auto union_state = followpos[p];
                    union_state.insert(other_state.begin(), other_state.end());
                    to_add[{omin, omax}] = std::move(union_state);

                    // remove overlapping part from current range
                    ch_range.min = omax + 1;
                    lbound = transitions.erase(lbound);
                } else {
                    lbound++;
                }
            }
            transitions.insert(to_add.begin(), to_add.end());
            if (ch_range.min <= ch_range.max) {
                transitions[ch_range] = followpos[p];
            }
        }

        auto vec = std::vector<sm::transition>{};
        for (auto&& [range, new_state] : transitions) {
            // add as new state if not seen before
            if (seen_states.count(new_state) == 0) {
                int new_state_id = result.num_states++;
                seen_states[new_state] = new_state_id;
                states.push_back(new_state);
                if (new_state.contains(final_pos)) {
                    result.accept_states.insert(new_state_id);
                }
            }
            vec.push_back(sm::transition{
                range,
                seen_states[new_state],
            });
        }
        result.edges[s] = std::move(vec);
    }

    assert(result.accept_states.size() > 0);

    return merge_identical_states(std::move(result));
}

auto sm::graph::next_state(int state, char ch) const -> int
{
    int const idx = ch & 0xFF;

    // binary search for range containing idx
    auto const& e_list = edges.at(state);
    auto ubound = std::upper_bound(e_list.begin(),
                                   e_list.end(),
                                   regex::sm::transition{{idx}},
                                   [](auto const& a, auto const& b) {
                                       return a.range.min < b.range.min;
                                   });
    if (ubound != e_list.begin()) {
        --ubound;
    }

    if (ubound != e_list.end()) {
        if (ubound->range.min <= idx && idx <= ubound->range.max) {
            return ubound->target_state;
        }
    }

    return -1;
}

auto sm::graph::match(std::string_view str) const -> bool
{
    int state = start_state;
    for (char ch : str) {
        state = next_state(state, ch);
        if (state == -1) {
            return false;
        }
    }
    return accept_states.contains(state);
}

} // namespace corpus_search::regex
