#ifndef SEARCHER_H
#define SEARCHER_H

#include <boost/dynamic_bitset.hpp>
#include <llguidance.h>
#include <optional>
#include <string>
#include <tokenizers_cpp.h>
#include <unordered_map>
#include <vector>

struct IndexEntry
{
    unsigned int sent_id : 21; // up to 2'097'151
    unsigned int pos : 11;     // up to 2'047
};
static_assert(sizeof(IndexEntry) == 4);

// TODO: un-hardcode these
constexpr int EOS_TOKEN_ID = 1;
constexpr int VOCAB_SIZE = 65536;
constexpr int MAX_TOKEN_LENGTH = 8; // in unicode characters

class searcher
{
    std::unordered_map<int, std::vector<int>> sentences;
    std::unordered_map<int, std::vector<IndexEntry>> tok_to_sid;
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
                        std::optional<std::vector<int>> cur_cands,
                        std::string cur_prefix,
                        int level) const -> std::optional<std::vector<int>>;

public:
    searcher();
    ~searcher();

    void search(std::string const &search_term) const;
};

#endif // SEARCHER_H
