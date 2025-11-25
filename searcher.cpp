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
        auto bitmask = boost::dynamic_bitset<>(VOCAB_SIZE);
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

auto searcher::call_tokenize(const uint8_t *bytes,
                             size_t bytes_len,
                             uint32_t *output_tokens,
                             size_t output_tokens_len) const -> std::size_t
{
    auto result = tokenizer->Encode(std::string(bytes, bytes + bytes_len));
    for (int i = 0; i < std::min(result.size(), output_tokens_len); ++i) {
        output_tokens[i] = result[i];
    }
    return result.size();
}

static auto to_bitset(std::uint32_t const *mask, int len) -> boost::dynamic_bitset<>
{
    auto result = boost::dynamic_bitset<>(VOCAB_SIZE);
    for (int i = 0; i < (len + 31) / 32; ++i) {
        if (mask[i] == 0) {
            continue;
        }
        for (int b = 0; b < 32; ++b) {
            if ((mask[i] >> b) & 0b1) {
                result.set(i * 32 + b);
            }
        }
    }
    return result;
}

static auto nonzero_pos(boost::dynamic_bitset<> const &mask) -> std::vector<int>
{
    std::vector<int> result;
    result.reserve(mask.count());
    int i = mask.find_first();
    do {
        result.push_back(i);
        i = mask.find_next(i);
    } while (i != mask.npos);
    return result;
}

static auto do_intersect(std::optional<std::vector<int>> const &a,
                         std::optional<std::vector<int>> const &b) -> std::optional<std::vector<int>>
{
    if (!a.has_value()) {
        return b;
    }
    if (!b.has_value()) {
        return a;
    }
    std::vector<int> result;
    std::set_intersection(a->begin(), a->end(), b->begin(), b->end(), std::back_inserter(result));
    return result;
}

static auto do_union(std::optional<std::vector<int>> const &a,
                     std::optional<std::vector<int>> const &b) -> std::optional<std::vector<int>>
{
    if (!a.has_value() || !b.has_value()) {
        return {};
    }
    std::vector<int> result;
    std::set_union(a->begin(), a->end(), b->begin(), b->end(), std::back_inserter(result));
    return result;
}

auto searcher::generate_cands(LlgMatcher *matcher,
                              int pad_size,
                              std::optional<std::vector<int>> cur_cands,
                              std::string cur_prefix,
                              int level) const -> std::optional<std::vector<int>>
{
    if (cur_cands.has_value() && cur_cands->empty()) {
        return std::vector<int>{};
    }

    if (llg_matcher_compute_mask(matcher)) {
        throw std::runtime_error("Error computing mask");
    }

    auto bitmask = to_bitset(llg_matcher_get_mask(matcher), VOCAB_SIZE);

    if (level == 0) {
        bitmask &= gt_n_char_masks.at(pad_size);
    }

    auto next_tokens = nonzero_pos(bitmask);
    // fmt::println("lvl {}: nonzero pos = [{}]", level, fmt::join(next_tokens, ", "));
    // std::fflush(stdout);

    auto result = std::optional<std::vector<int>>(std::vector<int>{});
    for (int token : next_tokens) {
        if (token == EOS_TOKEN_ID) {
            result = do_union(result, cur_cands);
            continue;
        }

        if (llg_matcher_consume_token(matcher, token)) {
            throw std::runtime_error("llg_matcher_consume_token returned error");
        }

        auto matches = std::vector<int>{};
        if (tok_to_sid.count(token) > 0) {
            for (auto entry : tok_to_sid.at(token)) {
                matches.push_back(entry.sent_id);
            }
            std::sort(matches.begin(), matches.end());
        }

        auto cands = generate_cands(matcher,
                                    pad_size,
                                    do_intersect(cur_cands, matches),
                                    cur_prefix + tid_to_token.at(token),
                                    level + 1);
        result = do_union(result, cands);

        if (llg_matcher_rollback(matcher, 1)) {
            throw std::runtime_error("llg_matcher_rollback returned error");
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

    auto result = std::unordered_set<int>{};
    for (int pad_size = 0; pad_size < MAX_TOKEN_LENGTH; ++pad_size) {
        auto regex = fmt::format(".{{{}}}{}", pad_size, search_term);

        auto m = std::unique_ptr<LlgMatcher, LlgMatcherDeleter>(
            llg_new_matcher(&init, "regex", regex.c_str()));
        if (llg_matcher_get_error(m.get())) {
            throw std::runtime_error("Error constructing constraint");
        }

        auto cands = generate_cands(m.get(), pad_size, {}, "", 0);
        result.insert(cands.value().begin(), cands.value().end());
    }

    fmt::println("Result for '{}' = Array[{}]{{{}}}",
                 search_term,
                 result.size(),
                 fmt::join(result, ", "));
}
