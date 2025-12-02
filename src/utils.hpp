#ifndef UTILS_H
#define UTILS_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
    std::unique_ptr<tokenizers::Tokenizer> tok_tokenizer;
    std::unordered_map<int, std::string> tid_to_token;

    auto llg_tokenize(std::string_view string) const -> std::vector<std::uint32_t>;

public:
    // TODO: un-hardcode these
    static constexpr int EOS_TOKEN_ID = 1;
    static constexpr int VOCAB_SIZE = 65536;
    static constexpr int MAX_TOKEN_LENGTH = 8; // in unicode characters

    tokenizer(std::string tokenizer_json_path, bool verbose);
    ~tokenizer();

    auto get_tok_tokenizer() const -> tokenizers::Tokenizer * { return tok_tokenizer.get(); }
    auto get_ll_tokenizer() const -> LlgTokenizer * { return ll_tokenizer; }
    auto get_tid_to_token() const -> std::unordered_map<int, std::string> const &
    {
        return tid_to_token;
    }

    auto tokenize(std::string_view string) const -> std::vector<int>;
};

auto to_bytes(std::string_view s) -> std::string;

auto to_unicode(std::string_view s) -> std::string;

} // namespace corpus_search

#endif // UTILS_H
