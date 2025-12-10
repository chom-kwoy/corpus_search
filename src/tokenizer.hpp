#ifndef TOKENIZER_HPP
#define TOKENIZER_HPP

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/dynamic_bitset.hpp>
#include <nlohmann/json_fwd.hpp>

namespace tokenizers {
class Tokenizer;
}

namespace corpus_search {

class tokenizer
{
    std::unique_ptr<tokenizers::Tokenizer> hf_tokenizer;

    std::unordered_map<int, std::string> tid_to_token;
    int m_max_token_bytes;

    std::unordered_map<char, char> m_normalize_mapping;
    std::unordered_map<char, char> m_inv_normalize_mapping;

    auto normalize(std::string_view string) const -> std::string;
    auto unnormalize(std::string_view string) const -> std::string;

public:
    // TODO: un-hardcode this
    static constexpr int EOS_TOKEN_ID = 1;

    tokenizer(std::string tokenizer_json_path,
              std::unordered_map<char, char> normalize_mapping,
              bool verbose);
    ~tokenizer();

    auto vocab_size() const -> int;
    auto max_token_bytes() const -> int;

    auto get_hf_tokenizer() const { return hf_tokenizer.get(); }
    auto get_tid_to_token() const -> auto const & { return tid_to_token; }
    auto normalize_mapping() const -> auto const & { return m_normalize_mapping; }
    auto inv_normalize_mapping() const -> auto const & { return m_inv_normalize_mapping; }

    auto tokenize(std::string_view string) const -> std::vector<int>;
};

auto to_bytes(std::string_view s) -> std::string;
auto to_unicode(std::string_view s) -> std::string;

} // namespace corpus_search

#endif // TOKENIZER_HPP
