#ifndef META_UTILS_HPP
#define META_UTILS_HPP

#include <any>
#include <fmt/core.h>
#include <optional>
#include <variant>
#include <vector>

namespace corpus_search {
namespace internal {
namespace {

// Helper to check if T is an instantiation of a template U
template<class T, template<class...> class U>
struct is_instance_of : std::false_type
{};

template<template<class...> class U, class... Args>
struct is_instance_of<U<Args...>, U> : std::true_type
{};

template<class T, template<class...> class U>
inline constexpr bool is_instance_of_v = is_instance_of<T, U>::value;

template<typename T>
using is_variant = is_instance_of<T, std::variant>;

template<typename T>
inline constexpr bool is_variant_v = is_variant<T>::value;

template<typename... Args>
void any_to_variant_cast_(std::any a, std::variant<Args...>& out)
{
    if (!a.has_value())
        throw std::bad_any_cast();

    std::optional<std::variant<Args...>> v = std::nullopt;
    bool found = ((a.type() == typeid(Args) && (v = std::any_cast<Args>(std::move(a)), true))
                  || ...);
    if (!found)
        throw std::bad_any_cast{};

    out = std::move(*v);
}

template<typename T>
auto any_to_variant_cast(std::any a) -> T
{
    if constexpr (is_variant_v<T>) {
        T variant;
        any_to_variant_cast_(a, variant);
        return variant;
    } else {
        try {
            return std::any_cast<T>(std::move(a));
        } catch (std::bad_any_cast const& e) {
            throw std::runtime_error(
                fmt::format("bad_any_cast: expected {}, got {}", typeid(T).name(), a.type().name()));
        }
    }
}

template<typename It, typename ReturnT, typename Arg0, typename... ArgsT, std::size_t... Is>
auto invoke_(ReturnT (*func)(Arg0, ArgsT...),
             std::index_sequence<Is...>,
             It begin,
             It end,
             Arg0 arg0) -> ReturnT
{
    constexpr bool is_vector_arg = [] {
        if constexpr (sizeof...(ArgsT) == 1) {
            using Arg1 = typename std::tuple_element<0, std::tuple<ArgsT...>>::type;
            return std::is_same_v<std::decay_t<Arg1>, std::vector<std::any>>;
        }
        return false;
    }();

    if constexpr (is_vector_arg) {
        return func(arg0, std::vector<std::any>(begin, end));
    } else {
        const auto nargs = std::distance(begin, end);
        if (nargs != sizeof...(ArgsT)) {
            throw std::runtime_error(
                fmt::format("Invalid number of arguments for {}: expected {}, got {}",
                            arg0->type,
                            sizeof...(ArgsT),
                            nargs));
        }
        return func(arg0, any_to_variant_cast<ArgsT>(*(begin + Is))...);
    }
}

template<typename It, typename ReturnT, typename Arg0, typename... ArgsT>
auto invoke(ReturnT (*func)(Arg0, ArgsT...), It begin, It end, Arg0 arg0) -> ReturnT
{
    return invoke_(func, std::make_index_sequence<sizeof...(ArgsT)>{}, begin, end, arg0);
}

} // namespace
} // namespace internal
} // namespace corpus_search

#endif // META_UTILS_HPP
