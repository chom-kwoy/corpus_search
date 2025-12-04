#ifndef SEARCHER_H
#define SEARCHER_H

#include "index_builder.hpp"
#include "tokenizer.hpp"

#include <string>
#include <vector>

namespace corpus_search {

auto search(tokenizer const &tok,
            index_builder const &index,
            std::string const &search_term) -> std::vector<int>;

} // namespace corpus_search

#endif // SEARCHER_H
