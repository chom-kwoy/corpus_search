#ifndef REGEX_AST_HPP
#define REGEX_AST_HPP

#include "regex_parse.hpp"

#include <cassert>

namespace corpus_search::regex {

namespace ast {

struct node_empty;
struct node_range;
struct node_union;
struct node_concat;
struct node_star;

using node = rvariant<node_empty, node_range, node_union, node_concat, node_star>;

enum class assertion_kind { none, start, end, word };
struct node_empty
{
    assertion_kind assertion;
};
struct node_range
{
    int min;
    int max;

    node_range(int min = 0, int max = 0)
        : min(min)
        , max(max)
    {
        assert(min >= 0 && max >= 0);
    }
};
struct node_union
{
    std::vector<node> args;
};
struct node_concat
{
    std::vector<node> args;
};
struct node_star
{
    node arg;
};

} // namespace ast

auto cst_to_ast(cst::pattern const& cst) -> ast::node;

auto print_ast(ast::node const& n) -> std::string;

} // namespace corpus_search::regex

#endif // REGEX_AST_HPP
