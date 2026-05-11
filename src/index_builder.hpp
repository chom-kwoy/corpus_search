#ifndef INDEX_BUILDER_HPP
#define INDEX_BUILDER_HPP

#include "sizes.h"

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace corpus_search {

struct index_entry
{
    static constexpr int SENTID_BITS = CORPUS_SEARCH_SENTID_BITS;
    static constexpr sentid_t MAX_SENTID = (1uLL << SENTID_BITS) - 1;
    static constexpr int POS_BITS = CORPUS_SEARCH_POSITION_BITS;
    static constexpr tokpos_t MAX_POS = (1 << POS_BITS) - 1;
    static constexpr int NEXT_TOK_BITS = CORPUS_SEARCH_NEXT_TOKEN_BITS;
    static constexpr int MAX_NEXT_TOK = (1 << NEXT_TOK_BITS) - 1;

    static_assert(MAX_POS >= 2'000);
    static_assert(MAX_SENTID >= 2'000'000);

    sentid_t sent_id : SENTID_BITS;
    tokpos_t pos : POS_BITS;
#if CORPUS_SEARCH_NEXT_TOKEN_BITS > 0
    int next_tok : NEXT_TOK_BITS;
#endif

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

    constexpr auto hash() const -> index_entry_hash_t
    {
        return (static_cast<index_entry_hash_t>(sent_id) << (POS_BITS + NEXT_TOK_BITS))
               | (static_cast<index_entry_hash_t>(pos) << NEXT_TOK_BITS)
               | static_cast<index_entry_hash_t>(next_tok);
    }
    static constexpr auto from_hash(index_entry_hash_t hash) -> index_entry
    {
        return
        {
            static_cast<sentid_t>(hash >> (POS_BITS + NEXT_TOK_BITS)),
                static_cast<tokpos_t>((hash >> NEXT_TOK_BITS) & MAX_POS),
#if CORPUS_SEARCH_NEXT_TOKEN_BITS > 0
                static_cast<int>(hash & MAX_NEXT_TOK),
#endif
        };
    }
};

class index_builder
{
    std::unordered_map<int, std::vector<index_entry>> result = {};

public:
    index_builder() = default;
    static index_builder from_file(std::string const &tokenized_sentences_path);

    void add_sentence(sentid_t sent_id, std::span<const int> tokens);
    void finalize_index();
    auto get_index() const -> std::unordered_map<int, std::vector<index_entry>> const &;
};

} // namespace corpus_search

#endif // INDEX_BUILDER_HPP
