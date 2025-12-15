#ifndef SEARCHER_HPP
#define SEARCHER_HPP

#include "sizes.h"
#include "tokenizer.hpp"

#include <functional>
#include <string>
#include <vector>

namespace corpus_search {

struct token_range
{
    sentid_t sent_id;
    tokpos_t i, j;

    bool operator<(token_range const &other) const
    {
        return std::tie(sent_id, i, j) < std::tie(other.sent_id, other.i, other.j);
    }
    bool operator!=(token_range const &other) const
    {
        return std::tie(sent_id, i, j) != std::tie(other.sent_id, other.i, other.j);
    }
};

using index_accessor = auto(int token) -> std::vector<token_range>;

struct search_result
{
    std::vector<sentid_t> candidates;
    bool needs_recheck;
};

auto search(tokenizer const &tok,
            std::function<index_accessor> const &index,
            std::string const &regex) -> search_result;

} // namespace corpus_search

#endif // SEARCHER_HPP
