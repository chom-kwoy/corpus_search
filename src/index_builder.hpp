#ifndef INDEX_BUILDER_HPP
#define INDEX_BUILDER_HPP

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace corpus_search {

struct index_entry
{
    static constexpr int POS_BITS = 11;
    static constexpr int MAX_POS = (1 << POS_BITS) - 1;
    static constexpr int SENTID_BITS = 32 - POS_BITS;
    static constexpr int MAX_SENTID = (1 << SENTID_BITS) - 1;

    static_assert(MAX_POS >= 2'000);
    static_assert(MAX_SENTID >= 2'000'000);

    unsigned int sent_id : SENTID_BITS;
    unsigned int pos : POS_BITS;

    auto operator<(index_entry const &other) const -> bool
    {
        return std::tie(sent_id, pos) < std::tie(other.sent_id, other.pos);
    }
    auto operator==(index_entry const &other) const -> bool
    {
        return std::tie(sent_id, pos) == std::tie(other.sent_id, other.pos);
    }
    auto operator!=(index_entry const &other) const -> bool
    {
        return std::tie(sent_id, pos) != std::tie(other.sent_id, other.pos);
    }

    constexpr auto hash() const -> std::uint32_t { return (sent_id << POS_BITS) | pos; }
    static constexpr auto from_hash(std::uint32_t hash) -> index_entry
    {
        return {hash >> POS_BITS, hash};
    }
};
static_assert(sizeof(index_entry) == 4);

class index_builder
{
    std::unordered_map<int, std::vector<index_entry>> result = {};

public:
    index_builder() = default;
    void add_sentence(int sent_id, std::span<const int> tokens);
    auto finalize_index() -> std::unordered_map<int, std::vector<index_entry>>;
    auto get_index() const -> std::unordered_map<int, std::vector<index_entry>> const &;
};

auto make_index(std::unordered_map<int, std::vector<int>> sentences)
    -> std::unordered_map<int, std::vector<index_entry>>;

} // namespace corpus_search

#endif // INDEX_BUILDER_HPP
