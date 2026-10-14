#ifndef RESIDUE_FILTER_HPP
#define RESIDUE_FILTER_HPP

#include <algorithm>
#include <array>
#include <bitset>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace corpus_search {

namespace detail {

template<typename T>
concept residue_integer = std::integral<std::remove_cvref_t<T>>
                          && !std::same_as<std::remove_cvref_t<T>, bool>;

template<typename Range>
concept residue_integer_range = std::ranges::input_range<Range>
                                && residue_integer<std::ranges::range_value_t<Range>>;

template<residue_integer Integer>
constexpr auto normalized_mod(Integer value, std::size_t modulus) noexcept -> std::size_t
{
    using value_type = std::remove_cvref_t<Integer>;
    using unsigned_type = std::make_unsigned_t<value_type>;

    if constexpr (std::signed_integral<value_type>) {
        if (value < 0) {
            auto const unsigned_value = static_cast<unsigned_type>(value);
            auto const magnitude = static_cast<std::uintmax_t>(
                static_cast<unsigned_type>(unsigned_type{0} - unsigned_value));
            auto const remainder = static_cast<std::size_t>(magnitude % modulus);
            return remainder == 0 ? 0 : modulus - remainder;
        }
    }

    return static_cast<std::size_t>(static_cast<std::uintmax_t>(value) % modulus);
}

template<int... Moduli>
consteval auto pairwise_coprime() -> bool
{
    constexpr auto values = std::array<int, sizeof...(Moduli)>{Moduli...};
    for (std::size_t i = 0; i < values.size(); ++i) {
        for (std::size_t j = i + 1; j < values.size(); ++j) {
            if (std::gcd(values[i], values[j]) != 1) {
                return false;
            }
        }
    }
    return true;
}

template<int Modulus>
struct residue_bank
{
    std::bitset<static_cast<std::size_t>(Modulus)> bits;
    std::uint64_t sigma = 0;

    auto operator==(residue_bank const&) const -> bool = default;
};

template<std::size_t Width>
auto rotate_bits(std::bitset<Width> const& value, std::size_t delta) -> std::bitset<Width>
{
    if (delta == 0) {
        return value;
    }
    return (value << delta) | (value >> (Width - delta));
}

} // namespace detail

/**
 * A shift-equivariant filter for finite integer sets.
 *
 * Each bank records occupied residues for one modulus. In addition to the bitmap, bank i
 * maintains an upper bound sigma_i on the number of excess collisions:
 *
 *     |S| - |{x mod p_i : x in S}| <= sigma_i.
 *
 * It follows that |S| <= popcount(bank_i) + sigma_i. Shifting only rotates bank bits and
 * preserves sigma. An intersection is a subset of both operands, so its excess bound is the
 * componentwise minimum of their bounds. Never recompute sigma from an intermediate bitmap:
 * intersections can retain bits that are not witnessed by members of the represented set.
 */
template<int... Moduli>
struct residue_filter
{
    static_assert(sizeof...(Moduli) > 0, "residue_filter requires at least one modulus");
    static_assert(((Moduli >= 2) && ...), "residue_filter moduli must be at least 2");
    static_assert(detail::pairwise_coprime<Moduli...>(),
                  "residue_filter moduli must be pairwise coprime");

    using count_type = std::uint64_t;

    static constexpr std::size_t bank_count = sizeof...(Moduli);
    static constexpr std::size_t payload_bits = (static_cast<std::size_t>(Moduli) + ...);
    inline static constexpr auto moduli = std::array<int, bank_count>{Moduli...};

private:
    std::tuple<detail::residue_bank<Moduli>...> m_banks;

    template<detail::residue_integer Integer, std::size_t... Is>
    auto would_overflow(Integer value, std::index_sequence<Is...>) const noexcept -> bool
    {
        constexpr auto max_count = std::numeric_limits<count_type>::max();
        return ((std::get<Is>(m_banks)
                     .bits[detail::normalized_mod(value, static_cast<std::size_t>(moduli[Is]))]
                 && std::get<Is>(m_banks).sigma == max_count)
                || ...);
    }

    template<detail::residue_integer Integer, std::size_t... Is>
    void insert_impl(Integer value, std::index_sequence<Is...>) noexcept
    {
        auto insert_into_bank = [value]<std::size_t I>(std::integral_constant<std::size_t, I>,
                                                       auto& bank) {
            auto const residue = detail::normalized_mod(value, static_cast<std::size_t>(moduli[I]));
            if (bank.bits[residue]) {
                ++bank.sigma;
            } else {
                bank.bits[residue] = true;
            }
        };
        (insert_into_bank(std::integral_constant<std::size_t, Is>{}, std::get<Is>(m_banks)), ...);
    }

    template<detail::residue_integer Integer, std::size_t... Is>
    auto contains_impl(Integer value, std::index_sequence<Is...>) const noexcept -> bool
    {
        return (std::get<Is>(m_banks)
                    .bits[detail::normalized_mod(value, static_cast<std::size_t>(moduli[Is]))]
                && ...);
    }

    template<detail::residue_integer Integer, std::size_t... Is>
    void shift_impl(Integer delta, std::index_sequence<Is...>)
    {
        auto shift_bank = [delta]<std::size_t I>(std::integral_constant<std::size_t, I>,
                                                 auto& bank) {
            constexpr auto width = static_cast<std::size_t>(moduli[I]);
            bank.bits = detail::rotate_bits(bank.bits, detail::normalized_mod(delta, width));
        };
        (shift_bank(std::integral_constant<std::size_t, Is>{}, std::get<Is>(m_banks)), ...);
    }

    template<std::size_t... Is>
    void intersect_impl(residue_filter const& other, std::index_sequence<Is...>) noexcept
    {
        auto intersect_bank = []<typename Bank>(Bank& self, Bank const& rhs) {
            self.bits &= rhs.bits;
            self.sigma = std::min(self.sigma, rhs.sigma);
        };
        (intersect_bank(std::get<Is>(m_banks), std::get<Is>(other.m_banks)), ...);
    }

    template<std::size_t... Is>
    auto is_empty_impl(std::index_sequence<Is...>) const noexcept -> bool
    {
        return (std::get<Is>(m_banks).bits.none() || ...);
    }

    template<std::size_t... Is>
    auto popcounts_impl(std::index_sequence<Is...>) const noexcept
        -> std::array<std::size_t, bank_count>
    {
        return {std::get<Is>(m_banks).bits.count()...};
    }

    template<std::size_t... Is>
    auto sigmas_impl(std::index_sequence<Is...>) const noexcept -> std::array<count_type, bank_count>
    {
        return {std::get<Is>(m_banks).sigma...};
    }

public:
    residue_filter() = default;

    /** Encode an exact distinct set, using temporary storage to remove duplicates. */
    template<detail::residue_integer_range Range>
    static auto encode(Range&& values) -> residue_filter
    {
        using value_type = std::ranges::range_value_t<Range>;

        auto result = residue_filter{};
        auto seen = std::unordered_set<value_type>{};
        for (auto&& value : values) {
            if (seen.insert(value).second) {
                result.insert(value);
            }
        }
        return result;
    }

    /** Encode without deduplication. Every insertion attempt contributes to the upper bound. */
    template<detail::residue_integer_range Range>
    static auto encode_streaming(Range&& values) -> residue_filter
    {
        auto result = residue_filter{};
        for (auto&& value : values) {
            result.insert(value);
        }
        return result;
    }

    /**
     * Insert one value while counting every call as a possible new set member.
     *
     * Repeated values can loosen the cardinality bound but cannot make it unsound. The update
     * remains valid for filters produced by previous intersections.
     */
    template<detail::residue_integer Integer>
    void insert(Integer value)
    {
        auto const indices = std::make_index_sequence<bank_count>{};
        if (would_overflow(value, indices)) {
            throw std::overflow_error("residue_filter sigma overflow");
        }
        insert_impl(value, indices);
    }

    template<detail::residue_integer Integer>
    [[nodiscard]] auto contains(Integer value) const noexcept -> bool
    {
        return contains_impl(value, std::make_index_sequence<bank_count>{});
    }

    template<detail::residue_integer Integer>
    [[nodiscard]] auto shift(Integer delta) const -> residue_filter
    {
        auto result = *this;
        result.shift_impl(delta, std::make_index_sequence<bank_count>{});
        return result;
    }

    [[nodiscard]] auto intersection(residue_filter const& other) const noexcept -> residue_filter
    {
        auto result = *this;
        result.intersect_impl(other, std::make_index_sequence<bank_count>{});
        return result;
    }

    [[nodiscard]] auto operator&(residue_filter const& other) const noexcept -> residue_filter
    {
        return intersection(other);
    }

    auto operator&=(residue_filter const& other) noexcept -> residue_filter&
    {
        intersect_impl(other, std::make_index_sequence<bank_count>{});
        return *this;
    }

    [[nodiscard]] auto definitely_empty() const noexcept -> bool
    {
        return is_empty_impl(std::make_index_sequence<bank_count>{});
    }

    [[nodiscard]] auto popcounts() const noexcept -> std::array<std::size_t, bank_count>
    {
        return popcounts_impl(std::make_index_sequence<bank_count>{});
    }

    [[nodiscard]] auto sigmas() const noexcept -> std::array<count_type, bank_count>
    {
        return sigmas_impl(std::make_index_sequence<bank_count>{});
    }

    [[nodiscard]] auto cardinality_upper_bound() const -> count_type
    {
        auto const counts = popcounts();
        if (std::ranges::find(counts, 0) != counts.end()) {
            return 0;
        }

        auto const excess_bounds = sigmas();
        auto result = std::numeric_limits<count_type>::max();
        for (std::size_t i = 0; i < bank_count; ++i) {
            auto const count = static_cast<count_type>(counts[i]);
            if (excess_bounds[i] > std::numeric_limits<count_type>::max() - count) {
                throw std::overflow_error("residue_filter cardinality overflow");
            }
            result = std::min(result, count + excess_bounds[i]);
        }
        return result;
    }

    template<std::size_t I>
    [[nodiscard]] auto bank() const noexcept -> auto const&
    {
        static_assert(I < bank_count, "residue_filter bank index out of range");
        return std::get<I>(m_banks).bits;
    }

    template<std::size_t I>
    [[nodiscard]] auto sigma() const noexcept -> count_type
    {
        static_assert(I < bank_count, "residue_filter bank index out of range");
        return std::get<I>(m_banks).sigma;
    }

    auto operator==(residue_filter const&) const -> bool = default;
};

} // namespace corpus_search

#endif // RESIDUE_FILTER_HPP
