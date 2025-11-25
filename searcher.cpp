#include "searcher.h"

#include "utils.h"

#include "utf8.h"
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <fstream>
#include <nlohmann/json.hpp>

searcher::searcher()
{
    // load index
    fmt::println("Loading sentences...");
    std::fflush(stdout);

    auto data_handle = load_file(
        "/home/park/PycharmProjects/mk-tokenizer/tokenized_sentences.msgpack");
    if (!data_handle.has_value()) {
        throw std::runtime_error("Error loading file");
    }
    auto data = data_handle.value().get();
    data.convert(sentences);

    std::size_t max_len = 0;
    int max_id = 0;
    for (auto const &[sent_id, sentence] : sentences) {
        max_len = std::max(max_len, sentence.size());
        max_id = std::max(max_id, sent_id);
    }

    fmt::println("Loaded {} sentences. Max sentence length = {}, Max id = {}",
                 sentences.size(),
                 max_len,
                 max_id);
    std::fflush(stdout);

    // make index
    fmt::println("Making index...");
    std::fflush(stdout);

    tok_to_sid = make_index(sentences);

    int bytes = 0;
    for (auto const &[tok, entries] : tok_to_sid) {
        bytes += sizeof(tok) + entries.size() * sizeof(IndexEntry);
    }

    fmt::println("Made index. Index size = {} MB", bytes / 1'000'000);
    std::fflush(stdout);

    // load tokenizer
    fmt::println("Loading tokenizer...");
    std::fflush(stdout);

    auto file = std::ifstream(
        "/home/park/PycharmProjects/mk-tokenizer/bpe_tokenizer/tokenizer.json");
    if (!file.is_open()) {
        throw std::runtime_error("Error opening tokenizer file");
    }
    std::string tokenizer_json = (std::stringstream{} << file.rdbuf()).str();

    tokenizer = tokenizers::Tokenizer::FromBlobJSON(tokenizer_json);

    const char sample_input[] = "ngixta 國家";
    fmt::println("Loaded hf tokenizer. \"{}\" -> [{}]",
                 sample_input,
                 fmt::join(tokenizer->Encode(sample_input), ", "));

    LlgTokenizerInit tok_init = {};

    tok_init.tok_eos = EOS_TOKEN_ID;
    tok_init.use_approximate_greedy_tokenize_fn = false;
    tok_init.tokenize_user_data = this;
    tok_init.tokenize_fn = [](const void *user_data,
                              const uint8_t *bytes,
                              size_t bytes_len,
                              uint32_t *output_tokens,
                              size_t output_tokens_len) -> std::size_t {
        searcher const *self = static_cast<searcher const *>(user_data);
        return self->call_tokenize(bytes, bytes_len, output_tokens, output_tokens_len);
    };
    tok_init.tokenizer_json = tokenizer_json.c_str();

    char error_buf[1024];
    ll_tokenizer = llg_new_tokenizer(&tok_init, error_buf, sizeof(error_buf));
    if (ll_tokenizer == nullptr) {
        throw std::runtime_error(fmt::format("Error creating tokenizer: {}", error_buf));
    }

    fmt::println("Loaded ll_tokenizer. \"{}\" -> [{}]",
                 sample_input,
                 fmt::join(tokenize(ll_tokenizer, sample_input), ", "));
    std::fflush(stdout);

    auto tokenizer_data = nlohmann::json::parse(tokenizer_json);
    auto vocab = tokenizer_data["model"]["vocab"];
    for (auto const &[tok_str, tok_id] : vocab.items()) {
        tid_to_token[tok_id.get<int>()] = to_bytes(tok_str);
    }

    auto nchars_to_tid = std::unordered_map<int, std::vector<int>>{};
    for (auto const &[tid, token] : tid_to_token) {
        if (tid < 2) { // FIXME: special token detection
            // special token; skip
            continue;
        }
        if ((token[0] & 0b1100'0000) == 0b1000'0000) {
            // continuation byte; skip
            continue;
        }
        auto length = utf8::utf8to32(utf8::replace_invalid(token)).size();
        nchars_to_tid[length].push_back(tid);
    }

    for (int n = 0; n <= MAX_TOKEN_LENGTH; ++n) {
        auto bitmask = std::bitset<VOCAB_SIZE>{};
        for (int gt_n = n + 1; gt_n <= MAX_TOKEN_LENGTH; ++gt_n) {
            for (auto tid : nchars_to_tid.at(gt_n)) {
                bitmask.set(tid);
            }
        }
        gt_n_char_masks[n] = bitmask;
    }

    fmt::println("Done.");
}

searcher::~searcher()
{
    if (ll_tokenizer) {
        llg_free_tokenizer(ll_tokenizer);
    }
}

std::size_t searcher::call_tokenize(const uint8_t *bytes,
                                    size_t bytes_len,
                                    uint32_t *output_tokens,
                                    size_t output_tokens_len) const
{
    auto result = tokenizer->Encode(std::string(bytes, bytes + bytes_len));
    for (int i = 0; i < std::min(result.size(), output_tokens_len); ++i) {
        output_tokens[i] = result[i];
    }
    return result.size();
}

std::vector<int> nonzero_pos(std::uint32_t const *mask, int len)
{
    std::vector<int> result;
    for (int i = 0; i < (len + 31) / 32; ++i) {
        if (mask[i] == 0) {
            continue;
        }
        for (int b = 0; b < 32; ++b) {
            if ((mask[i] >> b) & 0b1) {
                result.push_back(i * 32 + b);
            }
        }
    }
    return result;
}

void searcher::search(std::string const &search_term) const
{
    LlgConstraintInit init;
    llg_constraint_init_set_defaults(&init, ll_tokenizer);

    struct LlgMatcherDeleter
    {
        void operator()(LlgMatcher *p) const { llg_free_matcher(p); }
    };

    for (int pad_size = 0; pad_size < 1; ++pad_size) {
        auto regex = fmt::format(".{{{}}}{}", pad_size, search_term);

        fmt::println("Regex = {}", regex);

        auto m = std::unique_ptr<LlgMatcher, LlgMatcherDeleter>(
            llg_new_matcher(&init, "regex", regex.c_str()));
        if (llg_matcher_get_error(m.get())) {
            throw std::runtime_error("Error constructing constraint");
        }

        if (llg_matcher_compute_mask(m.get())) {
            throw std::runtime_error("Error computing mask");
        }

        auto bitmask = llg_matcher_get_mask(m.get());

        fmt::println("nonzero pos = [{}]", fmt::join(nonzero_pos(bitmask, VOCAB_SIZE), ", "));
    }
}
