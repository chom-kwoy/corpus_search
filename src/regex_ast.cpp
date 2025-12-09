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

static auto utf8_range_n(std::string_view smin,
                         std::string_view smax,
                         int n) -> std::vector<ast::node>
{
    std::vector<ast::node> result;

    if (n == 1) {
        result.push_back({ast::node_range{smin[0], smax[0]}});
    } else if (smin[0] < smax[0]) {
        auto start = ast::node_concat{{{ast::node_range{smin[0], smin[0]}}}};
        for (int i = 1; i < n; ++i) {
            start.args.push_back({ast::node_range{smin[i], char(0b1011'1111)}});
        }
        result.push_back({std::move(start)});
        if (smax[0] > smin[0] + 1) {
            auto middle = ast::node_concat{{
                {ast::node_range{
                    static_cast<char>(smin[0] + 1),
                    static_cast<char>(smax[0] - 1),
                }},
            }};
            for (int i = 1; i < n; ++i) {
                middle.args.push_back({ast::node_range{
                    char(0b1000'0000),
                    char(0b1011'1111),
                }});
            }
            result.push_back({std::move(middle)});
        }
        auto last = ast::node_concat{{{ast::node_range{smax[0], smax[0]}}}};
        for (int i = 1; i < n; ++i) {
            last.args.push_back({ast::node_range{char(0b1000'0000), smax[i]}});
        }
        result.push_back({std::move(last)});
    } else {
        auto vec = utf8_range_n(smin.substr(1), smax.substr(1), n - 1);
        if (vec.size() == 1) {
            result.push_back({ast::node_concat{{
                {ast::node_range{smin[0], smax[0]}},
                vec[0],
            }}});
        } else {
            result.push_back({ast::node_concat{{
                {ast::node_range{smin[0], smax[0]}},
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

    constexpr std::size_t PAGE_SIZE = 8192;
    std::vector<std::uint32_t> page(PAGE_SIZE);
    for (int offset = 0; offset <= UNICODE_MAX; offset += PAGE_SIZE) {
        int sz = roaring::api::roaring_bitmap_range_cardinality(&set.roaring,
                                                                offset,
                                                                offset + PAGE_SIZE);
        if (sz == PAGE_SIZE) {
            if (range_max == -1) {
                range_min = offset;
            } else if (range_max + 1 < offset) {
                auto vec = range_to_node(range_min, range_max);
                result.args.insert(result.args.end(), vec.begin(), vec.end());
                range_min = offset;
            }
            range_max = offset + PAGE_SIZE - 1;
        } else if (sz > 0) {
            page.resize(sz);
            set.rangeUint32Array(page.data(), offset, PAGE_SIZE);
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

        auto end = ast::node{};
        if (node.max == std::numeric_limits<int>::max()) {
            end = ast::node_star{elem};
        } else {
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
        roaring::Roaring set = std::visit([](auto&& arg) { return character_set(arg); }, node.get());
        return {ast::node_empty{}};
    } else if constexpr (std::is_same_v<T, char32_t>) {
        auto str = utf8::utf32to8(std::u32string_view(&node, 1));
        if (str.size() == 1) {
            return {ast::node_range{str[0], str[0]}};
        } else {
            auto result = ast::node_concat{};
            for (auto c : str) {
                result.args.push_back({ast::node_range{c, c}});
            }
            return {result};
        }
    } else {
        static_assert(std::is_same_v<T, void>, "not implemented");
    }
}

auto cst_to_ast(cst::pattern const& cst) -> ast::node
{
    return convert(cst);
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
