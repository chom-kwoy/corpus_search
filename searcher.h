#ifndef SEARCHER_H
#define SEARCHER_H

#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/dynamic_bitset.hpp>
#include <llguidance.h>
#include <roaring.hh>
#include <tokenizers_cpp.h>

constexpr int POS_BITS = 11;
constexpr int MAX_POS = (1 << POS_BITS) - 1;
constexpr int SENTID_BITS = 32 - POS_BITS;
constexpr int MAX_SENTID = (1 << SENTID_BITS) - 1;

static_assert(MAX_POS >= 2'000);
static_assert(MAX_SENTID >= 2'000'000);

struct index_entry
{
    unsigned int sent_id : SENTID_BITS;
    unsigned int pos : POS_BITS;

    constexpr std::uint32_t hash() const { return (sent_id << POS_BITS) | pos; }

    bool operator<(index_entry const &other) const
    {
        return std::tie(sent_id, pos) < std::tie(other.sent_id, other.pos);
    }

    static constexpr index_entry from_hash(std::uint32_t hash) { return {hash >> POS_BITS, hash}; }
};
static_assert(sizeof(index_entry) == 4);
static_assert(index_entry::from_hash(0xabcddeadU).hash() == 0xabcddeadU);

// TODO: un-hardcode these
constexpr int EOS_TOKEN_ID = 1;
constexpr int VOCAB_SIZE = 65536;
constexpr int MAX_TOKEN_LENGTH = 8; // in unicode characters

struct candset
{
    std::optional<roaring::Roaring> data = roaring::Roaring{};

    static candset from_set(roaring::Roaring &&set);
    static candset from_vec(std::vector<index_entry> const &vec);

    static candset all() { return candset{std::optional<roaring::Roaring>{}}; }
    static candset empty() { return candset{}; }

    bool is_all() const { return !data.has_value(); }
    std::size_t size() const;

    candset followed_by(candset const &other) const;
    candset &operator|=(candset const &other);
    candset &operator|=(candset &&other);
    candset &operator|=(std::vector<index_entry> const &other);

    std::vector<int> sent_ids() const;
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
                        std::unordered_map<std::string, candset> &cache,
                        std::string const &prev_prefix = "",
                        int level = 0) const -> candset;

public:
    searcher(std::string const &tokenized_sentences_path, std::string const &tokenizer_json_path);
    ~searcher();

    auto search(std::string const &search_term) const -> std::vector<int>;
};

#endif // SEARCHER_H
