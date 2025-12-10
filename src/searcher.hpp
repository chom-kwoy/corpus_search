#ifndef SEARCHER_HPP
#define SEARCHER_HPP

#include "index_builder.hpp"
#include "tokenizer.hpp"

#include <functional>
#include <string>
#include <vector>

namespace corpus_search {

using index_accessor = auto(int token) -> std::vector<index_entry>;

auto search(tokenizer const &tok,
            std::function<index_accessor> const &index,
            std::string const &regex) -> std::vector<sentid_t>;

} // namespace corpus_search

#endif // SEARCHER_HPP
