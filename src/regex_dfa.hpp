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
};

struct graph
{
    // start -> (target, range)
    std::map<int, std::vector<transition>> edges;
    int start_state;
    std::set<int> accept_states;
    int num_states = 0;
};
} // namespace sm

auto ast_to_dfa(ast::node const& node) -> sm::graph;

auto dfa_match(sm::graph const& dfa, std::string str) -> bool;

} // namespace corpus_search::regex

#endif // REGEX_DFA_HPP
