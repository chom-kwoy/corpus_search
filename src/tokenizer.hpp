#ifndef TOKENIZER_HPP
#define TOKENIZER_HPP

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/dynamic_bitset.hpp>
#include <nlohmann/json_fwd.hpp>

// forward declarations
class LlgTokenizer;
class LlgMatcher;
namespace tokenizers {
class Tokenizer;
}

namespace corpus_search {

class tokenizer
{
    LlgTokenizer *ll_tokenizer = nullptr;
    std::unique_ptr<tokenizers::Tokenizer> hf_tokenizer;

    std::unordered_map<int, std::string> tid_to_token;

    int max_token_bytes;
    std::unordered_map<int, boost::dynamic_bitset<>> gt_n_char_masks;

    std::unordered_map<char, char> normalize_mapping;
    std::unordered_map<char, char> inv_normalize_mapping;

    auto llg_tokenize(std::string_view string) const -> std::vector<std::uint32_t>;

    auto load_llg_tokenizer(tokenizers::Tokenizer *tok_tokenizer,
                            nlohmann::json json) -> LlgTokenizer *;

public:
    // TODO: un-hardcode this
    static constexpr int EOS_TOKEN_ID = 1;

    tokenizer(std::string tokenizer_json_path,
              std::unordered_map<char, char> normalize_mapping,
              bool verbose);
    ~tokenizer();

    auto vocab_size() const -> int;
    auto max_token_length() const -> int;

    auto get_hf_tokenizer() const { return hf_tokenizer.get(); }
    auto get_ll_tokenizer() const { return ll_tokenizer; }
    auto get_tid_to_token() const -> auto const & { return tid_to_token; }
    auto gt_n_char_mask(int n) const -> auto const & { return gt_n_char_masks.at(n); }
    auto get_normalize_mapping() const -> auto const & { return normalize_mapping; }

    auto normalize(std::string_view string) const -> std::string;
    auto unnormalize(std::string_view string) const -> std::string;

    auto tokenize(std::string_view string) const -> std::vector<int>;
};

auto to_bytes(std::string_view s) -> std::string;
auto to_unicode(std::string_view s) -> std::string;

} // namespace corpus_search

#endif // TOKENIZER_HPP
