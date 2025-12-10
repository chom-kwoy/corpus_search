#include "regex_parse.hpp"
#include "meta_utils.hpp"

#include <fmt/core.h>
#include <fmt/ranges.h>
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

namespace parser {

// Forward declarations

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

// Define grammar rules

struct pattern : pegtl::seq<pegtl::list<alternative, u8::one<U'|'>>, pegtl::eolf>
{
    static auto apply(pegtl::parse_tree::node* node, std::vector<std::any> args) -> cst::pattern;
};

struct alternative : pegtl::plus<element>
{
    static auto apply(pegtl::parse_tree::node* node, std::vector<std::any> args) -> cst::alternative;
};

struct element : pegtl::sor<assertion, quantifier, quantifiable_element>
{
    static auto apply(pegtl::parse_tree::node* node,
                      std::variant<cst::assertion, cst::quantifier, cst::quantifiable_element> arg)
        -> cst::element;
};

// non-capturing group
struct group : pegtl::seq<u8::one<U'('>,
                          u8::one<U'?'>,
                          u8::one<U':'>,
                          pegtl::list<alternative, u8::one<U'|'>>,
                          u8::one<U')'>>
{
    static auto apply(pegtl::parse_tree::node* node, std::vector<std::any> args) -> cst::group;
};

struct capturing_group
    : pegtl::seq<u8::one<U'('>,
                 pegtl::opt<pegtl::seq<u8::one<U'?'>, u8::one<U'<'>, pegtl::until<u8::one<U'>'>>>>,
                 pegtl::list<alternative, u8::one<U'|'>>,
                 u8::one<U')'>>
{
    static auto apply(pegtl::parse_tree::node* node,
                      std::vector<std::any> args) -> cst::capturing_group;
};

struct quantifiable_element
    : pegtl::sor<group, capturing_group, character_class, character_set, character>
{
    static auto apply(
        pegtl::parse_tree::node* node,
        std::variant<cst::group, cst::capturing_group, cst::character_class, cst::character_set, char32_t>
            arg) -> cst::quantifiable_element;
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
    static auto apply(pegtl::parse_tree::node* node, std::vector<std::any> args) -> cst::quantifier;
};

struct character_class : pegtl::seq<u8::one<U'['>,
                                    pegtl::opt<u8::one<U'^'>>, // negate
                                    pegtl::plus<character_class_element>,
                                    u8::one<U']'>>
{
    static auto apply(pegtl::parse_tree::node* node,
                      std::vector<std::any> args) -> cst::character_class;
};

struct character_class_element : pegtl::sor<escape_character_set,
                                            unicode_property_character_set,
                                            character_class_range,
                                            character_inside_brackets>
{
    static auto apply(pegtl::parse_tree::node* node,
                      std::variant<cst::escape_character_set,
                                   cst::unicode_property_character_set,
                                   cst::character_class_range,
                                   char32_t> arg) -> cst::character_class_element;
};

struct character_class_range
    : pegtl::seq<character_inside_brackets, u8::one<U'-'>, character_inside_brackets>
{
    static auto apply(pegtl::parse_tree::node* node,
                      char32_t min,
                      char32_t max) -> cst::character_class_range;
};

struct assertion : pegtl::sor<edge_assertion, word_boundary_assertion>
{
    static auto apply(pegtl::parse_tree::node* node,
                      std::variant<cst::edge_assertion, cst::word_boundary_assertion> arg)
        -> cst::assertion;
};

struct edge_assertion : u8::one<U'^', U'$'>
{
    static auto apply(pegtl::parse_tree::node* node) -> cst::edge_assertion;
};

struct word_boundary_assertion : pegtl::seq<u8::one<U'\\'>, u8::one<U'b', U'B'>>
{
    static auto apply(pegtl::parse_tree::node* node) -> cst::word_boundary_assertion;
};

struct character_set
    : pegtl::sor<any_character_set, escape_character_set, unicode_property_character_set>
{
    static auto apply(pegtl::parse_tree::node* node,
                      std::variant<cst::any_character_set,
                                   cst::escape_character_set,
                                   cst::unicode_property_character_set> arg) -> cst::character_set;
};

struct any_character_set : u8::one<U'.'>
{
    static auto apply(pegtl::parse_tree::node* node) -> cst::any_character_set;
};

struct escape_character_set
    : pegtl::seq<u8::one<U'\\'>, u8::one<U'd', U'D', U's', U'S', U'w', U'W'>>
{
    static auto apply(pegtl::parse_tree::node* node) -> cst::escape_character_set;
};

struct unicode_property_character_set : pegtl::seq<u8::one<U'\\'>,
                                                   u8::one<U'p', U'P'>,
                                                   u8::one<U'{'>,
                                                   alphabet,
                                                   pegtl::opt<u8::one<U'='>, alphanum>,
                                                   u8::one<U'}'>>
{
    static auto apply(pegtl::parse_tree::node* node,
                      std::vector<std::any> args) -> cst::unicode_property_character_set;
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

// Parser implementation

auto pattern::apply(pegtl::parse_tree::node* node, std::vector<std::any> args) -> cst::pattern
{
    auto result = cst::pattern{};
    for (auto&& arg : args) {
        result.alternatives.push_back(std::any_cast<cst::alternative>(std::move(arg)));
    }
    return result;
}

auto alternative::apply(pegtl::parse_tree::node* node,
                        std::vector<std::any> args) -> cst::alternative
{
    auto result = cst::alternative{};
    for (auto&& arg : args) {
        result.elements.push_back(std::any_cast<cst::element>(std::move(arg)));
    }
    return result;
}

auto element::apply(pegtl::parse_tree::node* node,
                    std::variant<cst::assertion, cst::quantifier, cst::quantifiable_element> arg)
    -> cst::element
{
    return {arg};
}

auto group::apply(pegtl::parse_tree::node* node, std::vector<std::any> args) -> cst::group
{
    auto result = cst::group{};
    for (auto&& arg : args) {
        result.alternatives.push_back(std::any_cast<cst::alternative>(std::move(arg)));
    }
    return result;
}

auto capturing_group::apply(pegtl::parse_tree::node* node,
                            std::vector<std::any> args) -> cst::capturing_group
{
    auto result = cst::capturing_group{};
    if (node->children.at(1)->string_view() != "") {
        auto str = node->children.at(1)->children.at(0)->children.at(2)->string_view();
        result.name = str.substr(0, str.size() - 1);
    } else {
        result.name = std::nullopt;
    }
    for (auto&& arg : args) {
        result.alternatives.push_back(std::any_cast<cst::alternative>(std::move(arg)));
    }
    return result;
}

auto quantifiable_element::apply(
    pegtl::parse_tree::node* node,
    std::variant<cst::group, cst::capturing_group, cst::character_class, cst::character_set, char32_t>
        arg) -> cst::quantifiable_element
{
    return {arg};
}

auto quantifier::apply(pegtl::parse_tree::node* node, std::vector<std::any> args) -> cst::quantifier
{
    auto result = cst::quantifier{};
    result.element = std::any_cast<cst::quantifiable_element>(std::move(args[0]));
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
        result.min = std::any_cast<int>(args[1]);
        result.max = std::any_cast<int>(args[1]);
    } else if (args.size() == 3) {
        result.min = std::any_cast<int>(args[1]);
        result.max = std::any_cast<int>(args[2]);
    }
    result.greedy = node->children.at(2)->string_view() != "?";
    return result;
}

auto character_class::apply(pegtl::parse_tree::node* node,
                            std::vector<std::any> args) -> cst::character_class
{
    auto result = cst::character_class{};
    result.negate = node->children.at(1)->string_view() == "^";
    for (auto&& arg : args) {
        result.elements.push_back(std::any_cast<cst::character_class_element>(std::move(arg)));
    }
    return result;
}

auto character_class_element::apply(pegtl::parse_tree::node* node,
                                    std::variant<cst::escape_character_set,
                                                 cst::unicode_property_character_set,
                                                 cst::character_class_range,
                                                 char32_t> arg) -> cst::character_class_element
{
    return {arg};
}

auto character_class_range::apply(pegtl::parse_tree::node* node,
                                  char32_t min,
                                  char32_t max) -> cst::character_class_range
{
    if (min > max) {
        throw std::runtime_error("invalid character class range");
    }
    return {min, max};
}

auto assertion::apply(pegtl::parse_tree::node* node,
                      std::variant<cst::edge_assertion, cst::word_boundary_assertion> arg)
    -> cst::assertion
{
    return {arg};
}

auto edge_assertion::apply(pegtl::parse_tree::node* node) -> cst::edge_assertion
{
    auto result = cst::edge_assertion{};
    if (node->string_view() == "^") {
        result.kind = cst::assertion_kind::start;
    } else if (node->string_view() == "$") {
        result.kind = cst::assertion_kind::end;
    }
    return result;
}

auto word_boundary_assertion::apply(pegtl::parse_tree::node* node) -> cst::word_boundary_assertion
{
    auto result = cst::word_boundary_assertion{};
    if (node->string_view() == "\\b") {
        result.negate = false;
    } else if (node->string_view() == "\\B") {
        result.negate = true;
    }
    return result;
}

auto character_set::apply(pegtl::parse_tree::node* node,
                          std::variant<cst::any_character_set,
                                       cst::escape_character_set,
                                       cst::unicode_property_character_set> arg) -> cst::character_set
{
    return {arg};
}

auto any_character_set::apply(pegtl::parse_tree::node* node) -> cst::any_character_set
{
    // nothing to do here
    return {};
}

auto escape_character_set::apply(pegtl::parse_tree::node* node) -> cst::escape_character_set
{
    auto result = cst::escape_character_set{};
    auto ch = node->string_view().at(1);
    switch (ch) {
    case 'd':
        result.kind = cst::character_set_kind::digit;
        result.negate = false;
        break;
    case 'D':
        result.kind = cst::character_set_kind::digit;
        result.negate = true;
        break;
    case 's':
        result.kind = cst::character_set_kind::space;
        result.negate = false;
        break;
    case 'S':
        result.kind = cst::character_set_kind::space;
        result.negate = true;
        break;
    case 'w':
        result.kind = cst::character_set_kind::word;
        result.negate = false;
        break;
    case 'W':
        result.kind = cst::character_set_kind::word;
        result.negate = true;
        break;
    }
    return result;
}

auto unicode_property_character_set::apply(pegtl::parse_tree::node* node, std::vector<std::any> args)
    -> cst::unicode_property_character_set
{
    auto result = cst::unicode_property_character_set{};
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
} // namespace parser

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

static cst::pattern visit_tree(pegtl::parse_tree::node* node)
{
    auto result = visit_impl<parser::pattern,
                             parser::alternative,
                             parser::element,
                             parser::group,
                             parser::capturing_group,
                             parser::quantifiable_element,
                             parser::quantifier,
                             parser::character_class,
                             parser::character_class_element,
                             parser::character_class_range,
                             parser::assertion,
                             parser::edge_assertion,
                             parser::word_boundary_assertion,
                             parser::character_set,
                             parser::any_character_set,
                             parser::escape_character_set,
                             parser::unicode_property_character_set,
                             parser::character,
                             parser::character_inside_brackets,
                             parser::alphabet,
                             parser::alphanum,
                             parser::number>(node);

    return std::any_cast<cst::pattern>(std::move(result));
}

auto parse(std::string const& input, bool verbose) -> cst::pattern
{
    pegtl::memory_input in(input.data(), input.data() + input.size(), "");

    const std::size_t issues = tao::pegtl::analyze<parser::pattern>();
    if (issues > 0) {
        throw std::runtime_error("Grammar has issues.");
    }

    auto root = pegtl::parse_tree::parse<parser::pattern>(in);
    if (!root) {
        throw std::runtime_error("Not parsed.");
    }

    if (verbose) {
        pegtl::parse_tree::print_dot(std::cout, *root);
    }

    return visit_tree(root.get());
}

template<typename T>
auto print(T const& node) -> std::string
{
    if constexpr (std::is_same_v<T, cst::pattern>) {
        auto vec = std::vector<std::string>{};
        for (auto& alt : node.alternatives) {
            vec.push_back(print(alt));
        }
        return fmt::format("pattern({})", fmt::join(vec, " | "));
    } else if constexpr (std::is_same_v<T, cst::alternative>) {
        auto vec = std::vector<std::string>{};
        for (auto& elem : node.elements) {
            vec.push_back(print(elem));
        }
        return fmt::format("alternative({})", fmt::join(vec, " "));
    } else if constexpr (std::is_same_v<T, cst::element>) {
        return std::visit([](auto&& arg) { return fmt::format("element({})", print(arg)); },
                          node.get());
    } else if constexpr (std::is_same_v<T, cst::quantifier>) {
        return fmt::format("quantifier(element={}, min={}, max={}, greedy={})",
                           print(node.element),
                           node.min,
                           node.max == std::numeric_limits<int>::max() ? "inf"
                                                                       : std::to_string(node.max),
                           node.greedy);
    } else if constexpr (std::is_same_v<T, cst::quantifiable_element>) {
        return std::visit(
            [](auto&& arg) { return fmt::format("quantifiable_element({})", print(arg)); },
            node.get());
    } else if constexpr (std::is_same_v<T, cst::assertion>) {
        return std::visit([](auto&& arg) { return fmt::format("assertion({})", print(arg)); },
                          node.get());
    } else if constexpr (std::is_same_v<T, cst::edge_assertion>) {
        return fmt::format("edge_assertion(kind={})",
                           node.kind == cst::assertion_kind::start ? "start"
                           : node.kind == cst::assertion_kind::end ? "end"
                                                                   : "unknown");
    } else if constexpr (std::is_same_v<T, cst::word_boundary_assertion>) {
        return fmt::format("word_boundary_assertion(negate={})", node.negate);
    } else if constexpr (std::is_same_v<T, cst::group>) {
        auto vec = std::vector<std::string>{};
        for (auto& alt : node.alternatives) {
            vec.push_back(print(alt));
        }
        return fmt::format("group({})", fmt::join(vec, " | "));
    } else if constexpr (std::is_same_v<T, cst::capturing_group>) {
        auto vec = std::vector<std::string>{};
        for (auto& alt : node.alternatives) {
            vec.push_back(print(alt));
        }
        return fmt::format("capturing_group(name={}, alternatives=[{}])",
                           node.name.has_value() ? *node.name : "nullopt",
                           fmt::join(vec, " | "));
    } else if constexpr (std::is_same_v<T, cst::character_class>) {
        auto vec = std::vector<std::string>{};
        for (auto& elem : node.elements) {
            vec.push_back(print(elem));
        }
        return fmt::format("character_class(negate={}, elements=[{}])",
                           node.negate,
                           fmt::join(vec, ", "));
    } else if constexpr (std::is_same_v<T, cst::character_class_element>) {
        return std::visit(
            [](auto&& arg) { return fmt::format("character_class_element({})", print(arg)); },
            node.get());
    } else if constexpr (std::is_same_v<T, cst::character_class_range>) {
        auto printch = [](char32_t ch) { return utf8::utf32to8(std::u32string_view(&ch, 1)); };
        return fmt::format("character_class_range(min='{}', max='{}')",
                           printch(node.min),
                           printch(node.max));
    } else if constexpr (std::is_same_v<T, cst::character_set>) {
        return std::visit([](auto&& arg) { return fmt::format("character_set({})", print(arg)); },
                          node.get());
    } else if constexpr (std::is_same_v<T, cst::any_character_set>) {
        return "any_character_set()";
    } else if constexpr (std::is_same_v<T, cst::escape_character_set>) {
        return fmt::format("escape_character_set(kind={})",
                           node.kind == cst::character_set_kind::digit   ? "digit"
                           : node.kind == cst::character_set_kind::word  ? "word"
                           : node.kind == cst::character_set_kind::space ? "space"
                                                                         : "unknown");
    } else if constexpr (std::is_same_v<T, cst::unicode_property_character_set>) {
        return fmt::format("unicode_property_character_set(negate={}, property={}, value={})",
                           node.negate,
                           node.property,
                           node.value.has_value() ? *node.value : "nullopt");
    } else if constexpr (std::is_same_v<T, char32_t>) {
        auto s = std::u32string{};
        s.push_back(node);
        return fmt::format("{}", utf8::utf32to8(s));
    } else {
        return fmt::format("{}", node);
    }
}

auto print_cst(cst::pattern const& pattern) -> std::string
{
    return print(pattern);
}

} // namespace corpus_search::regex
