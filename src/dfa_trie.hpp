#ifndef DFA_TRIE_HPP
#define DFA_TRIE_HPP

#include "regex_dfa.hpp"

#include <roaring.hh>

namespace corpus_search {

class tokenizer;

struct trie;
class dfa_trie
{
    std::vector<trie> tries;

public:
    dfa_trie(tokenizer const& tok);
    ~dfa_trie();

    auto get_next_tids(regex::sm::graph const& dfa,
                       int state,
                       int prefix_length = 0) const -> roaring::Roaring;

    static constexpr int ACCEPTED = -1;
    static constexpr int REJECTED = -2;
    auto consume_token(regex::sm::graph const& dfa, int state, std::string_view token) const -> int;
};

} // namespace corpus_search

#endif // DFA_TRIE_HPP
