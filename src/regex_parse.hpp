#ifndef REGEX_PARSE_HPP
#define REGEX_PARSE_HPP

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <boost/variant/recursive_wrapper.hpp>
#include <fmt/core.h>
#include <fmt/ranges.h>

namespace corpus_search::regex {

template<typename... Ts>
using rvariant = boost::recursive_wrapper<std::variant<Ts...>>;

namespace cst {

struct pattern;
struct alternative;
struct quantifier;

struct edge_assertion;
struct word_boundary_assertion;

struct group;
struct capturing_group;
struct character_class;
struct character_class_range;

struct any_character_set;
struct escape_character_set;
struct unicode_property_character_set;

struct character;
struct character_inside_brackets;
struct alphabet;
struct alphanum;
struct number;

using character_class_element
    = rvariant<escape_character_set, unicode_property_character_set, character_class_range, char32_t>;

using character_set
    = rvariant<any_character_set, escape_character_set, unicode_property_character_set>;

using assertion = rvariant<edge_assertion, word_boundary_assertion>;

using quantifiable_element
    = rvariant<group, capturing_group, character_class, character_set, char32_t>;

using element = rvariant<assertion, quantifier, quantifiable_element>;

struct pattern
{
    std::vector<alternative> alternatives;
};

struct alternative
{
    std::vector<element> elements;
};

// non-capturing group
struct group
{
    std::vector<alternative> alternatives;
};

struct capturing_group
{
    std::optional<std::string> name;
    std::vector<alternative> alternatives;
};

struct quantifier
{
    int min;
    int max;
    bool greedy;
    quantifiable_element element;
};

struct character_class
{
    bool negate;
    std::vector<character_class_element> elements;
};

struct character_class_range
{
    char32_t min;
    char32_t max;
};

enum class assertion_kind { start, end, word };

struct edge_assertion
{
    assertion_kind kind;
};

struct word_boundary_assertion
{
    static constexpr assertion_kind kind = assertion_kind::word;
    bool negate;
};

enum class character_set_kind { any, digit, space, word, property };

struct any_character_set
{
    static constexpr character_set_kind kind = character_set_kind::any;
};

struct escape_character_set
{
    character_set_kind kind;
    bool negate;
};

struct unicode_property_character_set
{
    static constexpr character_set_kind kind = character_set_kind::property;
    bool negate;
    std::string property;
    std::optional<std::string> value;
};

} // namespace cst

auto parse(std::string const& input, bool verbose = false) -> cst::pattern;

auto print_cst(cst::pattern const& pattern) -> std::string;

} // namespace corpus_search::regex

#endif // REGEX_PARSE_HPP
