#include "regex_ast.hpp"

#include <boost/variant/recursive_wrapper.hpp>
#include <fmt/core.h>
#include <tao/pegtl.hpp>
#include <tao/pegtl/contrib/analyze.hpp>
#include <tao/pegtl/contrib/parse_tree.hpp>
#include <tao/pegtl/contrib/parse_tree_to_dot.hpp>
#include <tao/pegtl/contrib/utf32.hpp>
#include <utf8.h>

#include <iostream>
#include <string>
#include <variant>

namespace corpus_search::regex {

namespace pegtl = tao::pegtl;
namespace u8 = tao::pegtl::utf8;

template<typename... Ts>
using rvariant = boost::recursive_wrapper<std::variant<Ts...>>;

namespace ast {

struct pattern;
struct alternative;
struct quantifier;
struct quantifiable_element;

struct assertion;
struct edge_assertion;
struct word_boundary_assertion;

struct group;
struct capturing_group;
struct character_class;
struct character_class_range;

struct character_set;
struct any_character_set;
struct escape_character_set;
struct unicode_property_character_set;

#define META_CHARS U'.', U'^', U'$', U'*', U'+', U'?', U'(', U'[', U'{', U'\\', U'|'
struct character : pegtl::if_then_else<u8::one<U'\\'>, u8::one<META_CHARS>, u8::not_one<META_CHARS>>
{
    char32_t ch;
};

#define BRACKET_META_CHARS U'^', U'-', U']', U'\\'
struct character_inside_brackets
    : pegtl::if_then_else<u8::one<U'\\'>, u8::one<BRACKET_META_CHARS>, u8::not_one<BRACKET_META_CHARS>>
{
    char32_t ch;
};

struct number : pegtl::plus<u8::range<U'0', U'9'>>
{
    int value;
};

struct element : pegtl::sor<assertion, quantifier, quantifiable_element>,
                 rvariant<assertion, quantifier, quantifiable_element>
{};

struct quantifiable_element
    : pegtl::sor<group, capturing_group, character_class, character_set, character>,
      rvariant<group, capturing_group, character_class, character_set, character>
{};

struct character_class_element : pegtl::sor<escape_character_set,
                                            unicode_property_character_set,
                                            character_class_range,
                                            character_inside_brackets>,
                                 rvariant<escape_character_set,
                                          unicode_property_character_set,
                                          character_class_range,
                                          character_inside_brackets>
{};

struct pattern : pegtl::seq<pegtl::plus<alternative>, pegtl::eolf>
{
    std::vector<alternative> alternatives;
};

struct alternative : pegtl::plus<element>
{
    std::vector<element> elements;
};

// non-capturing group
struct group
    : pegtl::seq<u8::one<U'('>, u8::one<U'?'>, u8::one<U':'>, pegtl::plus<alternative>, u8::one<U')'>>
{
    std::vector<alternative> alternatives;
};

struct capturing_group
    : pegtl::seq<u8::one<U'('>,
                 pegtl::opt<pegtl::seq<u8::one<U'?'>, u8::one<U'<'>, pegtl::until<u8::one<U'>'>>>>,
                 pegtl::plus<alternative>,
                 u8::one<U')'>>
{
    std::optional<std::string> name;
    std::vector<alternative> alternatives;
};

struct quantifier
    : pegtl::seq<quantifiable_element,
                 pegtl::sor<u8::one<U'*'>,
                            u8::one<U'+'>,
                            u8::one<U'?'>,
                            pegtl::seq<u8::one<U'{'>,
                                       pegtl::opt<pegtl::seq<number, u8::one<U','>>>, // min
                                       number,                                        // max
                                       u8::one<U'}'>>>,
                 pegtl::opt<u8::one<U'?'>> // greedy
                 >
{
    int min;
    int max;
    bool greedy;
    quantifiable_element element;
};

struct character_class : pegtl::seq<u8::one<U'['>,
                                    pegtl::opt<u8::one<U'^'>>, // negate
                                    pegtl::plus<character_class_element>,
                                    u8::one<U']'>>
{
    bool negate;
    std::vector<character_class_element> elements;
};

struct character_class_range
    : pegtl::seq<character_inside_brackets, u8::one<U'-'>, character_inside_brackets>
{
    char32_t min;
    char32_t max;
};

struct assertion : pegtl::sor<edge_assertion, word_boundary_assertion>,
                   rvariant<edge_assertion, word_boundary_assertion>
{};
enum class assertion_kind { start, end, word };

struct edge_assertion : pegtl::seq<pegtl::sor<u8::one<U'^'>, u8::one<U'$'>>>
{
    assertion_kind kind;
};

struct word_boundary_assertion
    : pegtl::seq<u8::one<U'\\'>, pegtl::sor<u8::one<U'b'>, u8::one<U'B'>>>
{
    static constexpr assertion_kind kind = assertion_kind::word;
    bool negate;
};

struct character_set
    : pegtl::sor<any_character_set, escape_character_set, unicode_property_character_set>,
      rvariant<any_character_set, escape_character_set, unicode_property_character_set>
{};
enum class character_set_kind { any, digit, space, word, property };

struct any_character_set : u8::one<U'.'>
{
    static constexpr character_set_kind kind = character_set_kind::any;
};

struct escape_character_set : pegtl::seq<u8::one<U'\\'>,
                                         pegtl::sor<u8::one<U'd'>,
                                                    u8::one<U'D'>,
                                                    u8::one<U's'>,
                                                    u8::one<U'S'>,
                                                    u8::one<U'w'>,
                                                    u8::one<U'W'>>>
{
    character_set_kind kind;
    bool negate;
};

struct unicode_property_character_set
    : pegtl::seq<u8::one<U'\\'>,
                 pegtl::sor<u8::one<U'p'>, u8::one<U'P'>>,
                 u8::one<U'{'>,
                 pegtl::plus<u8::range<U'a', U'z'>, u8::range<U'A', U'Z'>, u8::one<U'_'>>,
                 u8::one<U'}'>>
{
    static constexpr character_set_kind kind = character_set_kind::property;
    bool negate;
    std::string key;
    std::string value;
};

using result_t = std::variant<pattern,
                              alternative,
                              group,
                              capturing_group,
                              quantifier,
                              character_class,
                              character_class_range,
                              assertion,
                              character_set,
                              char32_t,
                              int>;

struct node : pegtl::parse_tree::node
{};

} // namespace ast

static void visit_tree(pegtl::parse_tree::node* node)
{
    fmt::println("type {}", node->type);
    for (auto&& child : node->children) {
        visit_tree(child.get());
    }
}

void parse(std::string const& input)
{
    pegtl::memory_input in(input.data(), input.data() + input.size(), "");

    const std::size_t issues = tao::pegtl::analyze<ast::pattern>();
    if (issues == 0) {
        try {
            auto root = pegtl::parse_tree::parse<ast::pattern, ast::node>(in);
            if (root) {
                // pegtl::parse_tree::print_dot(std::cout, *root);
                visit_tree(root.get());
            } else {
                fmt::println("Not parsed.");
            }
        } catch (pegtl::parse_error const& e) {
            fmt::println("error: {}", e.what());
        }
    }
}

} // namespace corpus_search::regex
