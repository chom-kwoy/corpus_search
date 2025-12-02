#ifndef SEARCHER_H
#define SEARCHER_H

#include "index_builder.hpp"
#include "utils.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/dynamic_bitset.hpp>
#include <re2/re2.h>

namespace corpus_search {

using candset = std::optional<std::vector<index_entry>>;

class searcher
{
    std::unordered_map<int, std::vector<int>> sentences;

    tokenizer tok;
    std::unordered_map<int, std::vector<index_entry>> tok_to_sid;
    std::unordered_map<int, boost::dynamic_bitset<>> gt_n_char_masks;

    auto generate_cands(LlgMatcher *matcher,
                        int pad_size,
                        RE2 const &search_regex,
                        std::unordered_map<std::string, candset> &cache,
                        std::string const &prev_prefix = "",
                        int level = 0) const -> std::vector<index_entry>;

public:
    searcher(std::string const &tokenized_sentences_path, std::string const &tokenizer_json_path);
    ~searcher() = default;

    auto search(std::string const &search_term) const -> std::vector<int>;
};

} // namespace corpus_search

#endif // SEARCHER_H
