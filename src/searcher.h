#ifndef SEARCHER_H
#define SEARCHER_H

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/dynamic_bitset.hpp>
#include <re2/re2.h>

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

    bool operator<(index_entry const &other) const
    {
        return std::tie(sent_id, pos) < std::tie(other.sent_id, other.pos);
    }
    bool operator==(index_entry const &other) const
    {
        return std::tie(sent_id, pos) == std::tie(other.sent_id, other.pos);
    }
    bool operator!=(index_entry const &other) const
    {
        return std::tie(sent_id, pos) != std::tie(other.sent_id, other.pos);
    }

    constexpr std::uint32_t hash() const { return (sent_id << POS_BITS) | pos; }
    static constexpr index_entry from_hash(std::uint32_t hash) { return {hash >> POS_BITS, hash}; }
};
static_assert(sizeof(index_entry) == 4);

// TODO: un-hardcode these
constexpr int EOS_TOKEN_ID = 1;
constexpr int VOCAB_SIZE = 65536;
constexpr int MAX_TOKEN_LENGTH = 8; // in unicode characters

using candset = std::optional<std::vector<index_entry>>;

class LlgTokenizer;
class LlgMatcher;
namespace tokenizers {
class Tokenizer;
}
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
                        RE2 const &search_regex,
                        std::unordered_map<std::string, candset> &cache,
                        std::string const &prev_prefix = "",
                        int level = 0) const -> std::vector<index_entry>;

public:
    searcher(std::string const &tokenized_sentences_path, std::string const &tokenizer_json_path);
    ~searcher();

    auto search(std::string const &search_term) const -> std::vector<int>;
};

#endif // SEARCHER_H
