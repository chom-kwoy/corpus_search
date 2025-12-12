#ifndef REGEX_DFA_HPP
#define REGEX_DFA_HPP

#include "regex_ast.hpp"

#include <map>
#include <set>

namespace corpus_search::regex {
namespace sm {

struct transition
{
    ast::node_range range;
    int target_state;

    bool operator<(const transition& other) const
    {
        return std::make_tuple(range.min, range.max, target_state)
               < std::make_tuple(other.range.min, other.range.max, other.target_state);
    }
};

struct graph
{
    // start -> (target, range)
    std::map<int, std::vector<transition>> edges;
    int start_state;
    std::set<int> accept_states;
    int num_states = 0;

    auto next_state(int state, char ch) const -> int;
    auto match(std::string_view str) const -> bool;
};
} // namespace sm

auto ast_to_dfa(ast::node const& node) -> sm::graph;

} // namespace corpus_search::regex

#endif // REGEX_DFA_HPP
