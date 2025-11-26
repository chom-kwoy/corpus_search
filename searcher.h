#ifndef SEARCHER_H
#define SEARCHER_H

#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/dynamic_bitset.hpp>
#include <llguidance.h>
#include <tokenizers_cpp.h>

struct index_entry
{
    unsigned int sent_id : 21; // up to 2'097'151
    unsigned int pos : 11;     // up to 2'047

    bool operator<(index_entry const &other) const
    {
        return std::tie(sent_id, pos) < std::tie(other.sent_id, other.pos);
    }
};
static_assert(sizeof(index_entry) == 4);

// TODO: un-hardcode these
constexpr int EOS_TOKEN_ID = 1;
constexpr int VOCAB_SIZE = 65536;
constexpr int MAX_TOKEN_LENGTH = 8; // in unicode characters

struct idset
{
    std::optional<std::set<index_entry>> data = std::set<index_entry>{};

    static idset from_set(std::set<index_entry> &&set);

    static idset all() { return idset{std::optional<std::set<index_entry>>{}}; }
    static idset empty() { return idset{}; }

    bool is_all() const { return !data.has_value(); }
    std::size_t size() const;

    idset followed_by(idset const &other) const;
    idset &operator|=(idset const &other);
    idset &operator|=(idset &&other);

    explicit operator std::set<int>() const;
};

class searcher
{
    std::unordered_map<int, std::vector<int>> sentences;
    std::unordered_map<int, std::vector<index_entry>> tok_to_sid;
    LlgTokenizer *ll_tokenizer = nullptr;
    std::unique_ptr<tokenizers::Tokenizer> tokenizer;
    std::unordered_map<int, std::string> tid_to_token;
    std::unordered_map<int, boost::dynamic_bitset<>> gt_n_char_masks;

    auto call_tokenize(const uint8_t *bytes,
                       size_t bytes_len,
                       uint32_t *output_tokens,
                       size_t output_tokens_len) const -> std::size_t;

    auto generate_cands(LlgMatcher *matcher,
                        int pad_size,
                        std::string const &search_regex,
                        std::unordered_map<std::string, idset> &cache,
                        std::string const &prev_prefix = "",
                        int level = 0) const -> idset;

public:
    searcher();
    ~searcher();

    auto search(std::string const &search_term) const -> std::set<int>;
};

#endif // SEARCHER_H
