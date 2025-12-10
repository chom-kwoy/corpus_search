#include "dfa_trie.hpp"

#include <algorithm>

namespace corpus_search {

struct trie_node
{
    int token_id;
    std::vector<std::unique_ptr<trie_node>> children;

    trie_node();
};

trie_node::trie_node()
    : token_id(-1)
    , children(256)
{}

struct trie
{
    std::unique_ptr<trie_node> root = std::make_unique<trie_node>();

    void insert(int token_id, std::string_view word)
    {
        trie_node* node = root.get();
        for (char c : word) {
            int idx = c & 0xFF;
            if (!node->children[idx]) {
                node->children[idx] = std::make_unique<trie_node>();
            }
            node = node->children[idx].get();
        }
        node->token_id = token_id;
    }
};

dfa_trie::dfa_trie(tokenizer const& tok)
    : m_trie(std::make_unique<trie>())
{
    for (auto&& [tid, token] : tok.get_tid_to_token()) {
        m_trie->insert(tid, token);
    }
}

dfa_trie::~dfa_trie() = default;

static void recurse(trie_node* node,
                    regex::sm::graph const& dfa,
                    int state,
                    roaring::Roaring& result)
{
    if (node->token_id != -1) {
        result.add(node->token_id);
    }
    for (auto edge : dfa.edges.at(state)) {
        for (int i = edge.range.min; i <= edge.range.max; ++i) {
            if (node->children[i]) {
                recurse(node->children[i].get(), dfa, edge.target_state, result);
            }
        }
    }
}

auto dfa_trie::get_next_tids(regex::sm::graph const& dfa, int state) const -> roaring::Roaring
{
    roaring::Roaring result;
    recurse(m_trie->root.get(), dfa, state, result);
    return result;
}

auto dfa_trie::consume_token(regex::sm::graph const& dfa,
                             int state,
                             std::string_view token) const -> int
{
    for (char ch : token) {
        int idx = ch & 0xff;
        auto const& edges = dfa.edges.at(state);
        auto ubound = std::upper_bound(edges.begin(),
                                       edges.end(),
                                       regex::sm::transition{{idx}},
                                       [](auto const& a, auto const& b) {
                                           return a.range.min < b.range.min;
                                       });
        if (ubound != edges.begin()) {
            --ubound;
        }
        if (ubound->range.min <= idx && idx <= ubound->range.max) {
            state = ubound->target_state;
            if (dfa.accept_states.contains(state)) {
                return ACCEPTED;
            }
        } else {
            return REJECTED;
        }
    }
    return state;
}

} // namespace corpus_search
