#include "dfa_trie.hpp"

#include <algorithm>
#include <queue>

namespace corpus_search {

struct trie_node
{
    std::vector<int> token_ids;
    std::vector<std::unique_ptr<trie_node>> children;

    trie_node();
};

trie_node::trie_node()
    : token_ids()
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
        node->token_ids.push_back(token_id);
    }
};

dfa_trie::dfa_trie(tokenizer const& tok)
    : tries(tok.max_token_bytes())
{
    for (int i = 0; i < tok.max_token_bytes(); ++i) {
        for (auto&& [tid, token] : tok.get_tid_to_token()) {
            if (token.length() > i) {
                tries[i].insert(tid, std::string_view(token.begin() + i, token.end()));
            }
        }
    }
}

dfa_trie::~dfa_trie() = default;

static void recurse(trie_node* node,
                    regex::sm::graph const& dfa,
                    int state,
                    roaring::Roaring& result)
{
    for (int tid : node->token_ids) {
        result.add(tid);
    }
    if (dfa.accept_states.contains(state)) {
        // collect all tokens in the subtree
        std::queue<trie_node*> pending;
        pending.push(node);
        while (!pending.empty()) {
            auto current = pending.front();
            pending.pop();
            for (int tid : current->token_ids) {
                result.add(tid);
            }
            for (auto& child : current->children) {
                if (child) {
                    pending.push(child.get());
                }
            }
        }
    } else {
        for (auto edge : dfa.edges.at(state)) {
            for (int i = edge.range.min; i <= edge.range.max; ++i) {
                if (node->children[i]) {
                    recurse(node->children[i].get(), dfa, edge.target_state, result);
                }
            }
        }
    }
}

auto dfa_trie::get_next_tids(regex::sm::graph const& dfa,
                             int state,
                             int prefix_length) const -> roaring::Roaring
{
    if (prefix_length >= tries.size()) {
        return {};
    }
    roaring::Roaring result;
    recurse(tries[prefix_length].root.get(), dfa, state, result);
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
