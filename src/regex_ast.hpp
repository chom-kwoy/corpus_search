#ifndef REGEX_AST_HPP
#define REGEX_AST_HPP

#include "regex_parse.hpp"

namespace corpus_search::regex {

namespace ast {

struct node_empty;
struct node_range;
struct node_union;
struct node_concat;
struct node_star;

using node = rvariant<node_empty, node_range, node_union, node_concat, node_star>;

struct node_empty
{};
struct node_range
{
    char min;
    char max;
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
