#ifndef SEARCHER_H
#define SEARCHER_H

#include <bitset>
#include <llguidance.h>
#include <string>
#include <tokenizers_cpp.h>
#include <unordered_map>
#include <vector>

struct IndexEntry
{
    int sent_id : 21; // up to 2'097'151
    int pos : 11;     // up to 2'047
};
static_assert(sizeof(IndexEntry) == 4);

constexpr int EOS_TOKEN_ID = 1; // TODO: un-hardcode this
constexpr int VOCAB_SIZE = 65536;
constexpr int MAX_TOKEN_LENGTH = 8; // in unicode characters

class searcher
{
    std::unordered_map<int, std::vector<int>> sentences;
    std::unordered_map<int, std::vector<IndexEntry>> tok_to_sid;
    LlgTokenizer *ll_tokenizer = nullptr;
    std::unique_ptr<tokenizers::Tokenizer> tokenizer;
    std::unordered_map<int, std::string> tid_to_token;
    std::unordered_map<int, std::bitset<VOCAB_SIZE>> gt_n_char_masks;

public:
    searcher();
    ~searcher();

    std::size_t call_tokenize(const uint8_t *bytes,
                              size_t bytes_len,
                              uint32_t *output_tokens,
                              size_t output_tokens_len) const;

    void search(std::string const &search_term) const;
};

#endif // SEARCHER_H
