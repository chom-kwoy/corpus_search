#include "regex_ast.hpp"
#include "meta_utils.hpp"

#include <boost/variant/recursive_wrapper.hpp>
#include <fmt/core.h>
#include <tao/pegtl.hpp>
#include <tao/pegtl/contrib/analyze.hpp>
#include <tao/pegtl/contrib/parse_tree.hpp>
#include <tao/pegtl/contrib/parse_tree_to_dot.hpp>
#include <tao/pegtl/contrib/utf32.hpp>
#include <utf8.h>

#include <any>
#include <charconv>
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
struct element;
struct quantifier;
struct quantifiable_element;

struct assertion;
struct edge_assertion;
struct word_boundary_assertion;

struct group;
struct capturing_group;
struct character_class;
struct character_class_element;
struct character_class_range;

struct character_set;
struct any_character_set;
struct escape_character_set;
struct unicode_property_character_set;

struct character;
struct character_inside_brackets;
struct alphabet;
struct alphanum;
struct number;

struct pattern : pegtl::seq<pegtl::list<alternative, u8::one<U'|'>>, pegtl::eolf>
{
    std::vector<alternative> alternatives;

    static auto apply(pegtl::parse_tree::node* node, std::vector<std::any> args) -> pattern;
};

struct alternative : pegtl::plus<element>
{
    std::vector<element> elements;

    static auto apply(pegtl::parse_tree::node* node, std::vector<std::any> args) -> alternative;
};

struct element : pegtl::sor<assertion, quantifier, quantifiable_element>
{
    rvariant<assertion, quantifier, quantifiable_element> value;

    static auto apply(pegtl::parse_tree::node* node,
                      std::variant<assertion, quantifier, quantifiable_element> arg) -> element;
};

// non-capturing group
struct group : pegtl::seq<u8::one<U'('>,
                          u8::one<U'?'>,
                          u8::one<U':'>,
                          pegtl::list<alternative, u8::one<U'|'>>,
                          u8::one<U')'>>
{
    std::vector<alternative> alternatives;

    static auto apply(pegtl::parse_tree::node* node, std::vector<std::any> args) -> group;
};

struct capturing_group
    : pegtl::seq<u8::one<U'('>,
                 pegtl::opt<pegtl::seq<u8::one<U'?'>, u8::one<U'<'>, pegtl::until<u8::one<U'>'>>>>,
                 pegtl::list<alternative, u8::one<U'|'>>,
                 u8::one<U')'>>
{
    std::optional<std::string> name;
    std::vector<alternative> alternatives;

    static auto apply(pegtl::parse_tree::node* node, std::vector<std::any> args) -> capturing_group;
};

struct quantifiable_element
    : pegtl::sor<group, capturing_group, character_class, character_set, character>
{
    rvariant<group, capturing_group, character_class, character_set, char32_t> value;

    static auto apply(pegtl::parse_tree::node* node,
                      std::variant<group, capturing_group, character_class, character_set, char32_t>
                          arg) -> quantifiable_element;
};

struct quantifier
    : pegtl::seq<quantifiable_element,
                 pegtl::sor<u8::one<U'*', U'+', U'?'>,
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

    static auto apply(pegtl::parse_tree::node* node, std::vector<std::any> args) -> quantifier;
};

struct character_class : pegtl::seq<u8::one<U'['>,
                                    pegtl::opt<u8::one<U'^'>>, // negate
                                    pegtl::plus<character_class_element>,
                                    u8::one<U']'>>
{
    bool negate;
    std::vector<character_class_element> elements;

    static auto apply(pegtl::parse_tree::node* node, std::vector<std::any> args) -> character_class;
};

struct character_class_element : pegtl::sor<escape_character_set,
                                            unicode_property_character_set,
                                            character_class_range,
                                            character_inside_brackets>
{
    rvariant<escape_character_set, unicode_property_character_set, character_class_range, char32_t>
        value;

    static auto apply(pegtl::parse_tree::node* node,
                      std::variant<escape_character_set,
                                   unicode_property_character_set,
                                   character_class_range,
                                   char32_t> arg) -> character_class_element;
};

struct character_class_range
    : pegtl::seq<character_inside_brackets, u8::one<U'-'>, character_inside_brackets>
{
    char32_t min;
    char32_t max;

    static auto apply(pegtl::parse_tree::node* node,
                      char32_t min,
                      char32_t max) -> character_class_range;
};

struct assertion : pegtl::sor<edge_assertion, word_boundary_assertion>
{
    rvariant<edge_assertion, word_boundary_assertion> value;

    static auto apply(pegtl::parse_tree::node* node,
                      std::variant<edge_assertion, word_boundary_assertion> arg) -> assertion;
};
enum class assertion_kind { start, end, word };

struct edge_assertion : u8::one<U'^', U'$'>
{
    assertion_kind kind;

    static auto apply(pegtl::parse_tree::node* node) -> edge_assertion;
};

struct word_boundary_assertion : pegtl::seq<u8::one<U'\\'>, u8::one<U'b', U'B'>>
{
    static constexpr assertion_kind kind = assertion_kind::word;
    bool negate;

    static auto apply(pegtl::parse_tree::node* node) -> word_boundary_assertion;
};

struct character_set
    : pegtl::sor<any_character_set, escape_character_set, unicode_property_character_set>
{
    rvariant<any_character_set, escape_character_set, unicode_property_character_set> value;

    static auto apply(
        pegtl::parse_tree::node* node,
        std::variant<any_character_set, escape_character_set, unicode_property_character_set> arg)
        -> character_set;
};
enum class character_set_kind { any, digit, space, word, property };

struct any_character_set : u8::one<U'.'>
{
    static constexpr character_set_kind kind = character_set_kind::any;

    static auto apply(pegtl::parse_tree::node* node) -> any_character_set;
};

struct escape_character_set
    : pegtl::seq<u8::one<U'\\'>, u8::one<U'd', U'D', U's', U'S', U'w', U'W'>>
{
    character_set_kind kind;
    bool negate;

    static auto apply(pegtl::parse_tree::node* node) -> escape_character_set;
};

struct unicode_property_character_set : pegtl::seq<u8::one<U'\\'>,
                                                   u8::one<U'p', U'P'>,
                                                   u8::one<U'{'>,
                                                   alphabet,
                                                   pegtl::opt<u8::one<U'='>, alphanum>,
                                                   u8::one<U'}'>>
{
    static constexpr character_set_kind kind = character_set_kind::property;
    bool negate;
    std::string property;
    std::optional<std::string> value;

    static auto apply(pegtl::parse_tree::node* node,
                      std::vector<std::any> args) -> unicode_property_character_set;
};

#define META_CHARS U'.', U'^', U'$', U'*', U'+', U'?', U'(', U')', U'[', U'{', U'\\', U'|'
struct character : pegtl::if_then_else<u8::one<U'\\'>, u8::one<META_CHARS>, u8::not_one<META_CHARS>>
{
    static auto apply(pegtl::parse_tree::node* node) -> char32_t;
};

#define BRACKET_META_CHARS U'^', U'-', U']', U'\\'
struct character_inside_brackets
    : pegtl::if_then_else<u8::one<U'\\'>, u8::one<BRACKET_META_CHARS>, u8::not_one<BRACKET_META_CHARS>>
{
    static auto apply(pegtl::parse_tree::node* node) -> char32_t;
};

struct alphabet : pegtl::plus<u8::ranges<U'a', U'z', U'A', U'Z', U'_'>>
{
    static auto apply(pegtl::parse_tree::node* node) -> std::string;
};

struct alphanum : pegtl::plus<u8::ranges<U'a', U'z', U'A', U'Z', U'0', U'9', U'_'>>
{
    static auto apply(pegtl::parse_tree::node* node) -> std::string;
};

struct number : pegtl::plus<u8::range<U'0', U'9'>>
{
    static auto apply(pegtl::parse_tree::node* node) -> int;
};

// Implementations
auto pattern::apply(pegtl::parse_tree::node* node, std::vector<std::any> args) -> pattern
{
    auto result = pattern{};
    for (auto&& arg : args) {
        result.alternatives.push_back(std::any_cast<alternative>(std::move(arg)));
    }
    return result;
}

auto alternative::apply(pegtl::parse_tree::node* node, std::vector<std::any> args) -> alternative
{
    auto result = alternative{};
    for (auto&& arg : args) {
        result.elements.push_back(std::any_cast<element>(std::move(arg)));
    }
    return result;
}

auto element::apply(pegtl::parse_tree::node* node,
                    std::variant<assertion, quantifier, quantifiable_element> arg) -> element
{
    return {{}, arg};
}

auto group::apply(pegtl::parse_tree::node* node, std::vector<std::any> args) -> group
{
    auto result = group{};
    for (auto&& arg : args) {
        result.alternatives.push_back(std::any_cast<alternative>(std::move(arg)));
    }
    return result;
}

auto capturing_group::apply(pegtl::parse_tree::node* node,
                            std::vector<std::any> args) -> capturing_group
{
    auto result = capturing_group{};
    if (node->children.at(1)->string_view() != "") {
        auto str = node->children.at(1)->children.at(0)->children.at(2)->string_view();
        result.name = str.substr(0, str.size() - 1);
    } else {
        result.name = std::nullopt;
    }
    for (auto&& arg : args) {
        result.alternatives.push_back(std::any_cast<alternative>(std::move(arg)));
    }
    return result;
}

auto quantifiable_element::apply(
    pegtl::parse_tree::node* node,
    std::variant<group, capturing_group, character_class, character_set, char32_t> arg)
    -> quantifiable_element
{
    return {{}, arg};
}

auto quantifier::apply(pegtl::parse_tree::node* node, std::vector<std::any> args) -> quantifier
{
    auto result = quantifier{};
    result.element = std::any_cast<quantifiable_element>(std::move(args[0]));
    if (args.size() == 1) {
        if (node->children.at(1)->string_view() == "*") {
            result.min = 0;
            result.max = std::numeric_limits<int>::max();
        } else if (node->children.at(1)->string_view() == "+") {
            result.min = 1;
            result.max = std::numeric_limits<int>::max();
        } else if (node->children.at(1)->string_view() == "?") {
            result.min = 0;
            result.max = 1;
        }
    } else if (args.size() == 2) {
        result.min = 0;
        result.max = std::any_cast<int>(args[1]);
    } else if (args.size() == 3) {
        result.min = std::any_cast<int>(args[1]);
        result.max = std::any_cast<int>(args[2]);
    }
    result.greedy = node->children.at(2)->string_view() != "?";
    return result;
}

auto character_class::apply(pegtl::parse_tree::node* node,
                            std::vector<std::any> args) -> character_class
{
    auto result = character_class{};
    result.negate = node->children.at(1)->string_view() == "^";
    for (auto&& arg : args) {
        result.elements.push_back(std::any_cast<character_class_element>(std::move(arg)));
    }
    return result;
}

auto character_class_element::apply(
    pegtl::parse_tree::node* node,
    std::variant<escape_character_set, unicode_property_character_set, character_class_range, char32_t>
        arg) -> character_class_element
{
    return {{}, arg};
}

auto character_class_range::apply(pegtl::parse_tree::node* node,
                                  char32_t min,
                                  char32_t max) -> character_class_range
{
    return {{}, min, max};
}

auto assertion::apply(pegtl::parse_tree::node* node,
                      std::variant<edge_assertion, word_boundary_assertion> arg) -> assertion
{
    return {{}, arg};
}

auto edge_assertion::apply(pegtl::parse_tree::node* node) -> edge_assertion
{
    auto result = edge_assertion{};
    if (node->string_view() == "^") {
        result.kind = assertion_kind::start;
    } else if (node->string_view() == "$") {
        result.kind = assertion_kind::end;
    }
    return result;
}

auto word_boundary_assertion::apply(pegtl::parse_tree::node* node) -> word_boundary_assertion
{
    auto result = word_boundary_assertion{};
    if (node->string_view() == "\\b") {
        result.negate = false;
    } else if (node->string_view() == "\\B") {
        result.negate = true;
    }
    return result;
}

auto character_set::apply(
    pegtl::parse_tree::node* node,
    std::variant<any_character_set, escape_character_set, unicode_property_character_set> arg)
    -> character_set
{
    return {{}, arg};
}

auto any_character_set::apply(pegtl::parse_tree::node* node) -> any_character_set
{
    // nothing to do here
    return {};
}

auto escape_character_set::apply(pegtl::parse_tree::node* node) -> escape_character_set
{
    auto result = escape_character_set{};
    auto ch = node->string_view().at(1);
    switch (ch) {
    case 'd':
        result.kind = character_set_kind::digit;
        result.negate = false;
        break;
    case 'D':
        result.kind = character_set_kind::digit;
        result.negate = true;
        break;
    case 's':
        result.kind = character_set_kind::space;
        result.negate = false;
        break;
    case 'S':
        result.kind = character_set_kind::space;
        result.negate = true;
        break;
    case 'w':
        result.kind = character_set_kind::word;
        result.negate = false;
        break;
    case 'W':
        result.kind = character_set_kind::word;
        result.negate = true;
        break;
    }
    return result;
}

auto unicode_property_character_set::apply(
    pegtl::parse_tree::node* node, std::vector<std::any> args) -> unicode_property_character_set
{
    auto result = unicode_property_character_set{};
    result.negate = node->children.at(1)->string_view() == "P";
    result.property = std::any_cast<std::string>(args[0]);
    if (args.size() == 2) {
        result.value = std::any_cast<std::string>(args[1]);
    } else {
        result.value = std::nullopt;
    }
    return result;
}

auto character::apply(pegtl::parse_tree::node* node) -> char32_t
{
    auto str = utf8::utf8to32(node->string_view());
    if (str.starts_with(U'\\')) {
        return str.at(1);
    }
    return str.at(0);
}

auto character_inside_brackets::apply(pegtl::parse_tree::node* node) -> char32_t
{
    auto str = utf8::utf8to32(node->string_view());
    if (str.starts_with(U'\\')) {
        return str.at(1);
    }
    return str.at(0);
}

auto alphabet::apply(pegtl::parse_tree::node* node) -> std::string
{
    return node->string();
}

auto alphanum::apply(pegtl::parse_tree::node* node) -> std::string
{
    return node->string();
}

auto number::apply(pegtl::parse_tree::node* node) -> int
{
    auto str = node->string_view();
    int result;
    auto begin = str.data();
    auto end = str.data() + str.size();
    auto status = std::from_chars(begin, end, result);
    if (status.ec != std::errc{} || status.ptr != end) {
        throw std::runtime_error("cannot parse number");
    }
    return result;
}
} // namespace ast

template<typename... Ts>
static auto visit_impl(pegtl::parse_tree::node* node) -> std::any
{
    if (node->is_root()) {
        node = node->children.at(0).get();
    }
    std::vector<std::any> params;
    for (auto&& child : node->children) {
        auto param = visit_impl<Ts...>(child.get());
        if (param.type() == typeid(std::vector<std::any>)) {
            auto const& vec = std::any_cast<std::vector<std::any> const&>(param);
            params.insert(params.end(), vec.begin(), vec.end());
        } else {
            params.push_back(param);
        }
    }

    std::any result;
    bool invoked = ((node->is_type<Ts>()
                     && ((result = internal::invoke(Ts::apply, params.begin(), params.end(), node),
                          true)))
                    || ...);
    if (!invoked) {
        result = params;
    }
    return result;
}

static ast::pattern visit_tree(pegtl::parse_tree::node* node)
{
    auto result = visit_impl<ast::pattern,
                             ast::alternative,
                             ast::element,
                             ast::group,
                             ast::capturing_group,
                             ast::quantifiable_element,
                             ast::quantifier,
                             ast::character_class,
                             ast::character_class_element,
                             ast::character_class_range,
                             ast::assertion,
                             ast::edge_assertion,
                             ast::word_boundary_assertion,
                             ast::character_set,
                             ast::any_character_set,
                             ast::escape_character_set,
                             ast::unicode_property_character_set,
                             ast::character,
                             ast::character_inside_brackets,
                             ast::alphabet,
                             ast::alphanum,
                             ast::number>(node);

    return std::any_cast<ast::pattern>(std::move(result));
}

void parse(std::string const& input)
{
    pegtl::memory_input in(input.data(), input.data() + input.size(), "");

    const std::size_t issues = tao::pegtl::analyze<ast::pattern>();
    if (issues == 0) {
        try {
            auto root = pegtl::parse_tree::parse<ast::pattern>(in);
            if (root) {
                pegtl::parse_tree::print_dot(std::cout, *root);
                auto ast = visit_tree(root.get());
                fmt::println("Parsed.");
            } else {
                fmt::println("Not parsed.");
            }
        } catch (pegtl::parse_error const& e) {
            fmt::println("error: {}", e.what());
        }
    }
}

} // namespace corpus_search::regex
