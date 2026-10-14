#include "residue_filter.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numeric>
#include <random>
#include <ranges>
#include <set>
#include <utility>
#include <vector>

namespace {

template<typename Filter, typename Integer>
concept can_insert = requires(Filter filter, Integer value) { filter.insert(value); };

static_assert(can_insert<corpus_search::residue_filter<5, 7>, int>);
static_assert(!can_insert<corpus_search::residue_filter<5, 7>, bool>);

template<typename Integer>
auto normalized_mod(Integer value, int modulus) -> int
{
    auto result = static_cast<std::int64_t>(value % modulus);
    return static_cast<int>(result < 0 ? result + modulus : result);
}

template<typename Filter>
void expect_sound(Filter const& filter, std::set<std::int64_t> const& actual)
{
    for (auto value : actual) {
        EXPECT_TRUE(filter.contains(value)) << "false negative for " << value;
    }

    EXPECT_GE(filter.cardinality_upper_bound(), actual.size());
    auto const sigmas = filter.sigmas();
    for (std::size_t i = 0; i < Filter::bank_count; ++i) {
        auto residues = std::set<int>{};
        for (auto value : actual) {
            residues.insert(normalized_mod(value, Filter::moduli[i]));
        }
        auto const excess = actual.size() - residues.size();
        EXPECT_GE(sigmas[i], excess);
    }

    if (filter.definitely_empty()) {
        EXPECT_TRUE(actual.empty());
        EXPECT_EQ(filter.cardinality_upper_bound(), 0);
    }
}

template<typename Filter>
auto encode_set(std::set<std::int64_t> const& values) -> Filter
{
    return Filter::encode(values);
}

TEST(ResidueFilter, EmptyFilter)
{
    using filter_type = corpus_search::residue_filter<5, 7>;

    auto const filter = filter_type{};
    EXPECT_TRUE(filter.definitely_empty());
    EXPECT_EQ(filter.popcounts(), (std::array<std::size_t, 2>{0, 0}));
    EXPECT_EQ(filter.sigmas(), (std::array<std::uint64_t, 2>{0, 0}));
    EXPECT_EQ(filter.cardinality_upper_bound(), 0);
    EXPECT_FALSE(filter.contains(0));
    EXPECT_EQ(filter.shift(-123).cardinality_upper_bound(), 0);
}

TEST(ResidueFilter, ExactAndStreamingEncoding)
{
    using filter_type = corpus_search::residue_filter<5, 7>;

    auto const values = std::vector<int>{-5, 0, 0, 5, 12, 12, 12};
    auto const exact = filter_type::encode(values);
    auto const streaming = filter_type::encode_streaming(values);
    auto const actual = std::set<std::int64_t>{-5, 0, 5, 12};

    EXPECT_EQ(exact.cardinality_upper_bound(), actual.size());
    EXPECT_EQ(streaming.cardinality_upper_bound(), values.size());
    EXPECT_EQ(exact.bank<0>(), streaming.bank<0>());
    EXPECT_EQ(exact.bank<1>(), streaming.bank<1>());
    EXPECT_EQ(exact.sigmas(), (std::array<std::uint64_t, 2>{2, 1}));
    EXPECT_EQ(streaming.sigmas(), (std::array<std::uint64_t, 2>{5, 4}));
    expect_sound(exact, actual);
    expect_sound(streaming, actual);
}

TEST(ResidueFilter, InsertCountsEveryAttempt)
{
    using filter_type = corpus_search::residue_filter<5, 7>;

    auto filter = filter_type{};
    filter.insert(0);
    EXPECT_TRUE(filter.contains(35));
    filter.insert(35);

    EXPECT_EQ(filter.popcounts(), (std::array<std::size_t, 2>{1, 1}));
    EXPECT_EQ(filter.sigmas(), (std::array<std::uint64_t, 2>{1, 1}));
    EXPECT_EQ(filter.cardinality_upper_bound(), 2);
    expect_sound(filter, {0, 35});
}

TEST(ResidueFilter, NegativeAndLargeShifts)
{
    using filter_type = corpus_search::residue_filter<5, 7, 11>;

    auto const actual = std::set<std::int64_t>{-100, -11, -1, 0, 1, 7, 20};
    auto const filter = encode_set<filter_type>(actual);
    for (auto delta : std::array<std::int64_t, 7>{-10'000, -200, -1, 0, 1, 200, 10'000}) {
        auto shifted_actual = std::set<std::int64_t>{};
        std::ranges::transform(actual,
                               std::inserter(shifted_actual, shifted_actual.end()),
                               [delta](auto value) { return value + delta; });
        auto const shifted = filter.shift(delta);
        EXPECT_EQ(shifted, encode_set<filter_type>(shifted_actual));
        EXPECT_EQ(shifted.sigmas(), filter.sigmas());
        EXPECT_EQ(shifted.cardinality_upper_bound(), filter.cardinality_upper_bound());
    }

    auto const singleton = filter_type::encode(std::array<std::int64_t, 1>{0});
    auto const minimum = std::numeric_limits<std::int64_t>::min();
    EXPECT_TRUE(singleton.shift(minimum).contains(minimum));
    auto const maximum = std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(singleton.shift(maximum).contains(maximum));
}

TEST(ResidueFilter, NarrowSignedMinimumUsesMathematicalModulo)
{
    using filter_type = corpus_search::residue_filter<251, 257>;

    auto const minimum = std::numeric_limits<signed char>::min();
    auto const filter = filter_type::encode(std::array<signed char, 1>{minimum});
    EXPECT_TRUE(filter.bank<0>()[123]);
    EXPECT_TRUE(filter.bank<1>()[129]);
    EXPECT_TRUE(filter.contains(minimum));
}

TEST(ResidueFilter, IntersectionAlgebraAndShiftDistributivity)
{
    using filter_type = corpus_search::residue_filter<5, 7>;

    auto const fa = filter_type::encode(std::array{0, 1, 5});
    auto const fb = filter_type::encode(std::array{0, 5, 6});
    auto const fc = filter_type::encode(std::array{0, 2, 6, 11});

    EXPECT_EQ(fa & fa, fa);
    EXPECT_EQ(fa & fb, fb & fa);
    EXPECT_EQ((fa & fb) & fc, fa & (fb & fc));
    for (int delta : {-13, 0, 7}) {
        EXPECT_EQ((fa & fb).shift(delta), fa.shift(delta) & fb.shift(delta));
    }

    auto in_place = fa;
    auto const* address = &(in_place &= fb);
    EXPECT_EQ(address, &in_place);
    EXPECT_EQ(in_place, fa & fb);
}

TEST(ResidueFilter, CardinalityExamples)
{
    using small_filter = corpus_search::residue_filter<5, 7>;

    auto const fa = small_filter::encode(std::array{0, 1, 2, 3, 5});
    auto const fb = small_filter::encode(std::array{1, 2, 3, 7, 12});
    auto const result = fa.shift(1) & fb;
    EXPECT_EQ(result.popcounts(), (std::array<std::size_t, 2>{3, 3}));
    EXPECT_EQ(result.sigmas(), (std::array<std::uint64_t, 2>{1, 0}));
    EXPECT_EQ(result.cardinality_upper_bound(), 3);
    expect_sound(result, {1, 2, 3});

    using collision_filter = corpus_search::residue_filter<149>;
    auto a = std::set<std::int64_t>{};
    auto b = std::set<std::int64_t>{};
    for (int value = 0; value < 80; ++value) {
        a.insert(value);
    }
    for (int value = 0; value < 20; ++value) {
        a.insert(149 + value);
    }
    for (int value = 70; value < 140; ++value) {
        b.insert(value);
    }
    for (int value = 70; value < 100; ++value) {
        b.insert(149 + value);
    }
    auto const collision_result = encode_set<collision_filter>(a) & encode_set<collision_filter>(b);
    EXPECT_EQ(collision_result.popcounts(), (std::array<std::size_t, 1>{10}));
    EXPECT_EQ(collision_result.sigmas(), (std::array<std::uint64_t, 1>{20}));
    EXPECT_EQ(collision_result.cardinality_upper_bound(), 30);
    auto intersection = std::set<std::int64_t>{};
    std::ranges::set_intersection(a, b, std::inserter(intersection, intersection.end()));
    EXPECT_EQ(intersection.size(), 10);
    expect_sound(collision_result, intersection);
}

TEST(ResidueFilter, EmptySetCanHavePositiveBound)
{
    using filter_type = corpus_search::residue_filter<5, 7>;

    auto const result = filter_type::encode(std::array{0, 1}).shift(1)
                        & filter_type::encode(std::array{16});
    EXPECT_TRUE(result.contains(16));
    EXPECT_FALSE(result.definitely_empty());
    EXPECT_EQ(result.cardinality_upper_bound(), 1);
    expect_sound(result, {});
}

TEST(ResidueFilter, IntermediateSigmaMustBePreserved)
{
    using filter_type = corpus_search::residue_filter<5, 7>;

    auto const fa = filter_type::encode(std::array{0, 1, 5});
    auto const fb = filter_type::encode(std::array{0, 5, 6});
    auto const result = fa & fb;
    EXPECT_EQ(result.popcounts(), (std::array<std::size_t, 2>{2, 2}));
    EXPECT_EQ(result.sigmas(), (std::array<std::uint64_t, 2>{1, 0}));
    EXPECT_EQ(result.cardinality_upper_bound(), 2);

    auto const further = result & filter_type::encode(std::array{0, 5});
    EXPECT_EQ(further.cardinality_upper_bound(), 2);
    expect_sound(further, {0, 5});

    auto extended = result;
    extended.insert(10);
    expect_sound(extended, {0, 5, 10});
}

TEST(ResidueFilter, ExhaustiveShiftedIntersections)
{
    using filter_type = corpus_search::residue_filter<2, 3>;

    constexpr auto universe = std::array<std::int64_t, 6>{-3, -2, -1, 0, 1, 2};
    auto sets = std::vector<std::set<std::int64_t>>{};
    auto filters = std::vector<filter_type>{};
    for (unsigned int mask = 0; mask < (1U << universe.size()); ++mask) {
        auto values = std::set<std::int64_t>{};
        for (std::size_t i = 0; i < universe.size(); ++i) {
            if ((mask >> i) & 1U) {
                values.insert(universe[i]);
            }
        }
        filters.push_back(encode_set<filter_type>(values));
        sets.push_back(std::move(values));
    }

    int cases = 0;
    for (std::size_t ai = 0; ai < sets.size(); ++ai) {
        for (std::size_t bi = 0; bi < sets.size(); ++bi) {
            for (int delta : {-7, -1, 0, 1, 7}) {
                auto shifted_a = std::set<std::int64_t>{};
                std::ranges::transform(sets[ai],
                                       std::inserter(shifted_a, shifted_a.end()),
                                       [delta](auto value) { return value + delta; });
                auto actual = std::set<std::int64_t>{};
                std::ranges::set_intersection(shifted_a,
                                              sets[bi],
                                              std::inserter(actual, actual.end()));
                auto const result = filters[ai].shift(delta) & filters[bi];
                expect_sound(result, actual);
                EXPECT_LE(result.cardinality_upper_bound(),
                          std::min(sets[ai].size(), sets[bi].size()));
                for (int value = -4; value <= 4; ++value) {
                    EXPECT_EQ(result.contains(value),
                              filters[ai].contains(value - delta) && filters[bi].contains(value));
                }

                auto unshifted_actual = std::set<std::int64_t>{};
                std::ranges::transform(actual,
                                       std::inserter(unshifted_actual, unshifted_actual.end()),
                                       [delta](auto value) { return value - delta; });
                auto further_actual = std::set<std::int64_t>{};
                std::ranges::set_intersection(unshifted_actual,
                                              sets[ai],
                                              std::inserter(further_actual, further_actual.end()));
                auto const further = result.shift(-delta) & filters[ai];
                expect_sound(further, further_actual);
                EXPECT_LE(further.cardinality_upper_bound(), result.cardinality_upper_bound());
                ++cases;
            }
        }
    }
    EXPECT_EQ(cases, 20'480);
}

TEST(ResidueFilter, RandomizedComposition)
{
    using filter_type = corpus_search::residue_filter<5, 7, 11>;
    using state = std::pair<filter_type, std::set<std::int64_t>>;

    auto rng = std::mt19937{20261014};
    auto universe = std::vector<std::int64_t>(121);
    std::iota(universe.begin(), universe.end(), -60);
    int steps = 0;
    for (int trial = 0; trial < 100; ++trial) {
        auto pool = std::vector<state>{};
        for (int i = 0; i < 5; ++i) {
            std::ranges::shuffle(universe, rng);
            auto const size = std::uniform_int_distribution<int>{1, 90}(rng);
            auto actual = std::set<std::int64_t>(universe.begin(), universe.begin() + size);
            auto values = std::vector<std::int64_t>(actual.begin(), actual.end());
            if (std::bernoulli_distribution{0.5}(rng)) {
                auto const duplicates = std::uniform_int_distribution<std::size_t>{0, values.size()}(
                    rng);
                auto const prefix = std::vector<std::int64_t>(values.begin(),
                                                              values.begin() + duplicates);
                values.insert(values.end(), prefix.begin(), prefix.end());
            }
            auto filter = std::bernoulli_distribution{0.5}(rng)
                              ? filter_type::encode(values)
                              : filter_type::encode_streaming(values);
            pool.emplace_back(std::move(filter), std::move(actual));
        }

        for (int i = 0; i < 40; ++i) {
            auto const a_index = std::uniform_int_distribution<std::size_t>{0, pool.size() - 1}(rng);
            auto const b_index = std::uniform_int_distribution<std::size_t>{0, pool.size() - 1}(rng);
            auto const delta = std::uniform_int_distribution<int>{-8, 8}(rng);
            auto const& [fa, a] = pool[a_index];
            auto const& [fb, b] = pool[b_index];

            auto shifted_a = std::set<std::int64_t>{};
            std::ranges::transform(a,
                                   std::inserter(shifted_a, shifted_a.end()),
                                   [delta](auto value) { return value + delta; });
            auto actual = std::set<std::int64_t>{};
            std::ranges::set_intersection(shifted_a, b, std::inserter(actual, actual.end()));
            auto result = fa.shift(delta) & fb;
            expect_sound(result, actual);
            EXPECT_LE(result.cardinality_upper_bound(), fa.cardinality_upper_bound());
            EXPECT_LE(result.cardinality_upper_bound(), fb.cardinality_upper_bound());
            for (int sample = 0; sample < 12; ++sample) {
                auto const value = std::uniform_int_distribution<int>{-100, 100}(rng);
                EXPECT_EQ(result.contains(value), fa.contains(value - delta) && fb.contains(value));
            }
            pool.emplace_back(std::move(result), std::move(actual));
            ++steps;
        }
    }
    EXPECT_EQ(steps, 4'000);
}

TEST(ResidueFilter, SaturationDoesNotRestoreSelectivity)
{
    using filter_type = corpus_search::residue_filter<5, 7, 11>;

    auto const full = filter_type::encode_streaming(std::views::iota(0, 10'000));
    auto const small = filter_type::encode(std::views::iota(10'100, 10'200));
    EXPECT_EQ(full.bank<0>().count(), 5);
    EXPECT_EQ(full.bank<1>().count(), 7);
    EXPECT_EQ(full.bank<2>().count(), 11);
    EXPECT_EQ(full.cardinality_upper_bound(), 10'000);

    auto const result = full & small;
    EXPECT_EQ(result.cardinality_upper_bound(), 100);
    for (int value = 10'100; value < 10'200; ++value) {
        EXPECT_TRUE(result.contains(value));
    }
    expect_sound(result, {});
}

} // namespace
