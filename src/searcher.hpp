#ifndef SEARCHER_HPP
#define SEARCHER_HPP

#include "residue_filter.hpp"
#include "sizes.h"
#include "tokenizer.hpp"

#include <functional>
#include <string>
#include <vector>

namespace corpus_search {

struct token_match
{
    sentid_t sent_id : CORPUS_SEARCH_SENTID_BITS;
    tokpos_t pos : CORPUS_SEARCH_POSITION_BITS;
#if CORPUS_SEARCH_NEXT_TOKEN_BITS > 0
    int next_tok_hash : CORPUS_SEARCH_NEXT_TOKEN_BITS;
#endif

    bool operator<(token_match const &other) const
    {
        return std::tie(sent_id, pos) < std::tie(other.sent_id, other.pos);
    }
    bool operator==(token_match const &other) const
    {
        return std::tie(sent_id, pos) == std::tie(other.sent_id, other.pos);
    }
    bool operator!=(token_match const &other) const
    {
        return std::tie(sent_id, pos) != std::tie(other.sent_id, other.pos);
    }
};

using index_accessor = auto(int token) -> std::vector<token_match>;
using filter_accessor = auto(int token) -> default_residue_filter;

struct search_result
{
    std::vector<sentid_t> candidates;
    bool needs_recheck;
};

auto search(tokenizer const &tok,
            std::function<index_accessor> const &index,
            std::function<filter_accessor> const &filter,
            std::string const &regex) -> search_result;

} // namespace corpus_search

#endif // SEARCHER_HPP
