#include "regex_ast.hpp"

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <roaring.hh>
#include <utf8.h>

namespace corpus_search::regex {

static constexpr char32_t UNICODE_MAX = 0x10FFFF;

template<typename T>
static auto character_set(T const& node) -> roaring::Roaring
{
    if constexpr (std::is_same_v<T, cst::character_class_element>) {
        return std::visit([](auto&& arg) { return character_set(arg); }, node.get());
    } else if constexpr (std::is_same_v<T, cst::character_class_range>) {
        auto set = roaring::Roaring{};
        set.addRange(node.min, node.max + 1);
        return set;
    } else if constexpr (std::is_same_v<T, cst::any_character_set>) {
        auto set = roaring::Roaring{};
        set.addRange(0, UNICODE_MAX + 1);
        return set;
    } else if constexpr (std::is_same_v<T, cst::escape_character_set>) {
        // TODO
        throw std::runtime_error("escape char set not implemented");
        return {};
    } else if constexpr (std::is_same_v<T, cst::unicode_property_character_set>) {
        // TODO
        throw std::runtime_error("unicode set not implemented");
        return {};
    } else if constexpr (std::is_same_v<T, char32_t>) {
        auto set = roaring::Roaring{};
        set.add(node);
        return set;
    } else {
        static_assert(std::is_same_v<T, void>, "not implemented");
    }
}

static auto make_start(std::string_view smin) -> ast::node
{
    int smin_0 = smin[0] & 0xFF;
    if (smin.size() == 1) {
        return {ast::node_range{
            smin_0,
            0b1011'1111,
        }};
    }

    auto result = ast::node_union{};

    auto first = ast::node_concat{{
        {ast::node_range{smin_0, smin_0}},
        make_start(smin.substr(1)),
    }};
    result.args.push_back({first});

    if (smin_0 < 0b1011'1111) {
        auto initial = ast::node_concat{{
            {ast::node_range{smin_0 + 1, 0b1011'1111}},
        }};
        for (int i = 1; i < smin.size(); ++i) {
            initial.args.push_back({ast::node_range{
                0b1000'0000,
                0b1011'1111,
            }});
        }
        result.args.push_back({initial});
    }

    return {result};
}

static auto make_last(std::string_view smax) -> ast::node
{
    int smax_0 = smax[0] & 0xFF;
    if (smax.size() == 1) {
        return {ast::node_range{
            0b1000'0000,
            smax_0,
        }};
    }

    auto result = ast::node_union{};

    if (smax_0 > 0b1000'0000) {
        auto initial = ast::node_concat{{
            {ast::node_range{0b1000'0000, smax_0 - 1}},
        }};
        for (int i = 1; i < smax.size(); ++i) {
            initial.args.push_back({ast::node_range{
                0b1000'0000,
                0b1011'1111,
            }});
        }
        result.args.push_back({initial});
    }

    auto last = ast::node_concat{{
        {ast::node_range{smax_0, smax_0}},
        make_last(smax.substr(1)),
    }};
    result.args.push_back({last});

    return {result};
}

static auto utf8_range_n(std::string_view smin,
                         std::string_view smax,
                         int n) -> std::vector<ast::node>
{
    std::vector<ast::node> result;

    int smin_0 = smin[0] & 0xFF;
    int smax_0 = smax[0] & 0xFF;
    if (n == 1) {
        result.push_back({ast::node_range{smin_0, smax_0}});
    } else if (smin_0 < smax_0) {
        auto start = ast::node_concat{{
            {ast::node_range{smin_0, smin_0}},
            make_start(smin.substr(1)),
        }};
        result.push_back({std::move(start)});
        if (smax_0 > smin_0 + 1) {
            auto middle = ast::node_concat{{
                {ast::node_range{smin_0 + 1, smax_0 - 1}},
            }};
            for (int i = 1; i < n; ++i) {
                middle.args.push_back({ast::node_range{
                    0b1000'0000,
                    0b1011'1111,
                }});
            }
            result.push_back({std::move(middle)});
        }
        auto last = ast::node_concat{{
            {ast::node_range{smax_0, smax_0}},
            make_last(smax.substr(1)),
        }};
        result.push_back({std::move(last)});
    } else {
        auto vec = utf8_range_n(smin.substr(1), smax.substr(1), n - 1);
        if (vec.size() == 1) {
            result.push_back({ast::node_concat{{
                {ast::node_range{smin_0, smax_0}},
                vec[0],
            }}});
        } else {
            result.push_back({ast::node_concat{{
                {ast::node_range{smin_0, smax_0}},
                {ast::node_union{std::move(vec)}},
            }}});
        }
    }

    return result;
}

static auto range_to_node(char32_t min, char32_t max) -> std::vector<ast::node>
{
    auto result = std::vector<ast::node>{};

    static constexpr auto utf8_ranges = {
        std::make_pair(0, 0x7F),
        std::make_pair(0x80, 0x7FF),
        std::make_pair(0x800, 0xFFFF),
        std::make_pair(0x10000, 0x10FFFF),
    };
    for (auto crange : utf8_ranges) {
        if (max >= crange.first && crange.second >= min) { // has overlap
            const auto cmin = std::max(min, char32_t(crange.first));
            const auto cmax = std::min(max, char32_t(crange.second));
            auto smin = utf8::utf32to8(std::u32string_view(&cmin, 1));
            auto smax = utf8::utf32to8(std::u32string_view(&cmax, 1));
            assert(smin.size() == smax.size());
            auto vec = utf8_range_n(smin, smax, smin.size());
            result.insert(result.end(), vec.begin(), vec.end());
        }
    }

    return result;
}

static auto bitmap_to_node(roaring::Roaring const& set) -> ast::node
{
    auto result = ast::node_union{};

    char32_t range_min = 0;
    char32_t range_max = -1;

    int size = set.cardinality();
    constexpr int PAGE_SIZE = 8192;
    std::vector<std::uint32_t> page(PAGE_SIZE);
    for (int offset = 0; offset <= size; offset += PAGE_SIZE) {
        int sz = std::min(size - offset, PAGE_SIZE);
        page.resize(sz);
        set.rangeUint32Array(page.data(), offset, sz);
        for (auto cur_idx : page) {
            if (range_max == -1) {
                range_min = cur_idx;
            } else if (range_max + 1 < cur_idx) {
                auto vec = range_to_node(range_min, range_max);
                result.args.insert(result.args.end(), vec.begin(), vec.end());
                range_min = cur_idx;
            }
            range_max = cur_idx;
        }
    }
    if (range_max != -1) {
        auto vec = range_to_node(range_min, range_max);
        result.args.insert(result.args.end(), vec.begin(), vec.end());
    }

    return {result};
}

template<typename T>
static auto convert(T const& node) -> ast::node
{
    if constexpr (std::is_same_v<T, cst::pattern>) {
        if (node.alternatives.size() == 1) {
            return convert(node.alternatives[0]);
        } else {
            auto result = ast::node_union{};
            for (auto& alt : node.alternatives) {
                result.args.push_back(convert(alt));
            }
            return {result};
        }
    } else if constexpr (std::is_same_v<T, cst::alternative>) {
        if (node.elements.size() == 1) {
            return convert(node.elements[0]);
        } else {
            auto result = ast::node_concat{};
            for (auto& elem : node.elements) {
                result.args.push_back(convert(elem));
            }
            return {result};
        }
    } else if constexpr (std::is_same_v<T, cst::element>) {
        return std::visit([](auto&& arg) { return convert(arg); }, node.get());
    } else if constexpr (std::is_same_v<T, cst::quantifier>) {
        if (node.min == 0 && node.max == 0) {
            return {ast::node_empty{}};
        }
        auto elem = convert(node.element);

        ast::node end = {ast::node_empty{}};
        if (node.max == std::numeric_limits<int>::max()) {
            end = ast::node_star{elem};
        } else if (node.min < node.max) {
            auto max_repeat = ast::node_concat{};
            for (int i = node.min + 1; i <= node.max; ++i) {
                max_repeat.args.push_back({ast::node_union{{
                    ast::node{ast::node_empty{}},
                    elem,
                }}});
            }
            end = max_repeat;
        }

        if (node.min == 0) {
            return end;
        } else {
            auto start = ast::node{};
            if (node.min == 1) {
                start = elem;
            } else {
                auto min_repeat = ast::node_concat{};
                for (int i = 0; i < node.min; ++i) {
                    min_repeat.args.push_back(elem);
                }
                start = min_repeat;
            }
            return {ast::node_concat{{start, end}}};
        }
    } else if constexpr (std::is_same_v<T, cst::quantifiable_element>) {
        return std::visit([](auto&& arg) { return convert(arg); }, node.get());
    } else if constexpr (std::is_same_v<T, cst::assertion>) {
        return std::visit([](auto&& arg) { return convert(arg); }, node.get());
    } else if constexpr (std::is_same_v<T, cst::edge_assertion>) {
        // TOOD
        throw std::runtime_error("edge_assertion not implemented");
        return {ast::node_empty{}};
    } else if constexpr (std::is_same_v<T, cst::word_boundary_assertion>) {
        // TODO
        throw std::runtime_error("word_boundary_assertion not implemented");
        return {ast::node_empty{}};
    } else if constexpr (std::is_same_v<T, cst::group>) {
        if (node.alternatives.size() == 1) {
            return convert(node.alternatives[0]);
        } else {
            auto result = ast::node_union{};
            for (auto& alt : node.alternatives) {
                result.args.push_back(convert(alt));
            }
            return {result};
        }
    } else if constexpr (std::is_same_v<T, cst::capturing_group>) {
        if (node.alternatives.size() == 1) {
            return convert(node.alternatives[0]);
        } else {
            auto result = ast::node_union{};
            for (auto& alt : node.alternatives) {
                result.args.push_back(convert(alt));
            }
            return {result};
        }
    } else if constexpr (std::is_same_v<T, cst::character_class>) {
        auto set = roaring::Roaring{};
        for (auto& elem : node.elements) {
            set |= character_set(elem);
        }
        if (node.negate) {
            set.flipClosed(0, UNICODE_MAX);
        }
        set.runOptimize();
        return bitmap_to_node(set);
    } else if constexpr (std::is_same_v<T, cst::character_set>) {
        auto set = std::visit([](auto&& arg) { return character_set(arg); }, node.get());
        set.runOptimize();
        return bitmap_to_node(set);
    } else if constexpr (std::is_same_v<T, char32_t>) {
        auto str = utf8::utf32to8(std::u32string_view(&node, 1));
        if (str.size() == 1) {
            return {ast::node_range{str[0] & 0xff, str[0] & 0xff}};
        } else {
            auto result = ast::node_concat{};
            for (char c : str) {
                result.args.push_back({ast::node_range{c & 0xff, c & 0xff}});
            }
            return {result};
        }
    } else {
        static_assert(std::is_same_v<T, void>, "not implemented");
    }
}

static auto normalize(ast::node const& node) -> ast::node
{
    // collapse single-child nodes
    auto result = std::visit(
        [](auto&& r) -> ast::node {
            using R = std::decay_t<decltype(r)>;
            if constexpr (std::is_same_v<R, ast::node_concat>
                          || std::is_same_v<R, ast::node_union>) {
                if (r.args.size() == 0) {
                    return {ast::node_empty{}};
                } else if (r.args.size() == 1) {
                    return normalize(r.args[0]);
                } else if constexpr (std::is_same_v<R, ast::node_concat>) {
                    // convert to binary tree
                    auto modified = ast::node_concat{{
                        normalize(r.args[0]),
                        normalize(r.args[1]),
                    }};
                    for (int i = 2; i < r.args.size(); ++i) {
                        modified = ast::node_concat{{
                            {std::move(modified)},
                            normalize(r.args[i]),
                        }};
                    }
                    return {std::move(modified)};
                } else {
                    auto modified = ast::node_union{};
                    for (int i = 0; i < r.args.size(); ++i) {
                        modified.args.push_back(normalize(r.args[i]));
                    }
                    return {std::move(modified)};
                }
            } else if constexpr (std::is_same_v<R, ast::node_star>) {
                return {ast::node_star{normalize(r.arg)}};
            }
            return {r};
        },
        node.get());

    return result;
}

auto cst_to_ast(cst::pattern const& cst) -> ast::node
{
    auto result = convert(cst);
    return normalize(result);
}

auto print_ast(ast::node const& n) -> std::string
{
    return std::visit(
        [](auto&& node) -> std::string {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, ast::node_empty>) {
                return "ε";
            } else if constexpr (std::is_same_v<T, ast::node_range>) {
                auto printch = [](char ch) {
                    if (std::isprint(ch)) {
                        return fmt::format("'{}'", ch);
                    }
                    return fmt::format("\\{:x}", ch);
                };
                if (node.min == node.max) {
                    return fmt::format("{}", printch(node.min));
                }
                return fmt::format("[{}-{}]", printch(node.min), printch(node.max));
            } else if constexpr (std::is_same_v<T, ast::node_union>) {
                std::vector<std::string> vec;
                for (auto&& arg : node.args) {
                    vec.push_back(print_ast(arg));
                }
                return fmt::format("({})", fmt::join(vec, "|"));
            } else if constexpr (std::is_same_v<T, ast::node_concat>) {
                std::vector<std::string> vec;
                for (auto&& arg : node.args) {
                    vec.push_back(print_ast(arg));
                }
                return fmt::format("({})", fmt::join(vec, "·"));
            } else if constexpr (std::is_same_v<T, ast::node_star>) {
                return fmt::format("*({})", print_ast(node.arg));
            }
        },
        n.get());
}

} // namespace corpus_search::regex
