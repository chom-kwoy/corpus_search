#include "regex_dfa.hpp"

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
};

auto mark(ast::node const& node, mark_state& state) -> node_table
{
    const int my_pos = state.cur_pos;
    const int my_index = state.cur_index++;

    auto pos = std::visit(
        [my_pos, &state](auto&& node) -> node_table {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, ast::node_empty>) {
                return {{}, {}, true};
            } else if constexpr (std::is_same_v<T, ast::node_range>) {
                state.cur_pos++;
                state.leaf_map[my_pos] = node;
                return {{my_pos}, {my_pos}, false};
            } else if constexpr (std::is_same_v<T, ast::node_union>) {
                auto result = node_table{};
                result.nullable = false;
                for (auto&& arg : node.args) {
                    int ch_idx = state.cur_index;
                    auto p = mark(arg, state);
                    result.firstpos.insert(p.firstpos.begin(), p.firstpos.end());
                    result.lastpos.insert(p.lastpos.begin(), p.lastpos.end());
                    result.nullable = result.nullable || p.nullable;
                    result.children.push_back(ch_idx);
                }
                return result;
            } else if constexpr (std::is_same_v<T, ast::node_concat>) {
                auto result = node_table{};
                result.nullable = true;
                result.type = node_type::concat;

                std::vector<node_table> args_pos;
                for (auto&& arg : node.args) {
                    int ch_idx = state.cur_index;
                    auto p = mark(arg, state);
                    result.nullable = result.nullable && p.nullable;
                    args_pos.push_back(std::move(p));
                    result.children.push_back(ch_idx);
                }

                for (auto&& p : args_pos) {
                    result.firstpos.insert(p.firstpos.begin(), p.firstpos.end());
                    if (!p.nullable) {
                        break;
                    }
                }
                for (auto&& p : args_pos | std::views::reverse) {
                    result.lastpos.insert(p.lastpos.begin(), p.lastpos.end());
                    if (!p.nullable) {
                        break;
                    }
                }

                return result;
            } else if constexpr (std::is_same_v<T, ast::node_star>) {
                int ch_idx = state.cur_index;
                auto p = mark(node.arg, state);
                auto result = node_table{};
                result.firstpos = p.firstpos;
                result.lastpos = p.lastpos;
                result.nullable = true;
                result.type = node_type::star;
                return result;
            }
        },
        node.get());

    state.nodes[my_index] = pos;

    return pos;
}

struct comparator
{
    bool operator()(const ast::node_range& a, const ast::node_range& b) const
    {
        return a.min < b.min;
    }
};

// McNaughton-Yamada-Thompson algorithm
auto ast_to_dfa(ast::node const& node) -> sm::graph
{
    auto aug_node = ast::node{ast::node_concat{{
        node,
        {ast::node_range{}},
    }}};

    mark_state visit_state{};
    auto const& nodes = visit_state.nodes;
    auto const& leaf_map = visit_state.leaf_map;
    mark(aug_node, visit_state);
    const int final_pos = visit_state.cur_pos - 1;

    auto followpos = std::vector<std::set<int>>(visit_state.cur_pos);
    for (int i = 0; i < nodes.size(); ++i) {
        auto const& pos = nodes.at(i);

        if (pos.type == node_type::concat) {
            int last_ch_idx = -1;
            for (int ch_idx : pos.children) {
                auto const& child = nodes.at(ch_idx);
                if (last_ch_idx != -1) {
                    for (int p : nodes.at(last_ch_idx).lastpos) {
                        followpos[p].insert(child.firstpos.begin(), child.firstpos.end());
                    }
                }
                last_ch_idx = ch_idx;
            }
        } else if (pos.type == node_type::star) {
            for (int p : pos.lastpos) {
                followpos[p].insert(pos.firstpos.begin(), pos.firstpos.end());
            }
        }
    }

    std::vector<std::set<int>> states;
    std::map<std::set<int>, int> seen_states;

    auto init_state = nodes.at(0).firstpos;
    states.push_back(init_state);
    seen_states[init_state] = 0;

    sm::graph result;
    result.start_state = 0;
    result.num_states = 1;

    for (int i = 0; i < states.size(); ++i) {
        auto const& state = states[i];

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

        std::vector<sm::transition> vec;
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
            vec.push_back(sm::transition{range, seen_states[new_state]});
        }
        result.edges[i] = std::move(vec);
    }

    return result;
}

} // namespace corpus_search::regex
