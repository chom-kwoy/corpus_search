#include "searcher.h"

#include "utils.h"

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <re2/re2.h>
#include <utf8.h>

idset idset::from_vec(std::vector<index_entry> &&vec, bool needs_sort)
{
    auto output = idset::empty();
    if (needs_sort) {
        std::sort(vec.begin(), vec.end());
    }
    output.data.emplace(std::move(vec));
    return output;
}

std::size_t idset::size() const
{
    if (is_all()) {
        throw std::runtime_error("size() called on universal set");
    }
    return data->size();
}

idset idset::followed_by(idset const &other) const
{
    if (other.is_all()) {
        return *this;
    }

    if (is_all()) {
        std::vector<index_entry> vec;
        for (index_entry const &entry : other.data.value()) {
            if (entry.pos - 1 >= 0) {
                vec.push_back({
                    entry.sent_id,
                    static_cast<unsigned int>(entry.pos - 1),
                });
            }
        }
        return idset::from_vec(std::move(vec));
    }

    auto const &arr1 = data.value();
    auto const &arr2 = other.data.value();

    std::vector<index_entry> vec;

    int idx1 = 0, idx2 = 0;
    while (idx1 < arr1.size() && idx2 < arr2.size()) {
        if (arr1[idx1].sent_id < arr2[idx2].sent_id) {
            ++idx1;
        } else {
            if (arr1[idx1].sent_id == arr2[idx2].sent_id) {
                if (arr1[idx1].pos + 1 < arr2[idx2].pos) {
                    ++idx1;
                } else if (arr1[idx1].pos + 1 == arr2[idx2].pos) {
                    vec.push_back(arr1[idx1]);
                    ++idx1;
                    ++idx2;
                } else {
                    ++idx2;
                }
            } else {
                ++idx2;
            }
        }
    }

    return idset::from_vec(std::move(vec), false);
}

idset idset::operator|(idset const &other) const
{
    if (is_all() || other.is_all()) {
        return idset::all();
    }

    auto const &arr1 = data.value();
    auto const &arr2 = other.data.value();

    std::vector<index_entry> vec;
    std::set_union(arr1.begin(), arr1.end(), arr2.begin(), arr2.end(), std::back_inserter(vec));

    return idset::from_vec(std::move(vec), false);
}

idset::operator std::set<int>() const
{
    if (is_all()) {
        throw std::runtime_error("cannot convert universal set");
    }
    std::set<int> output;
    for (auto entry : data.value()) {
        output.insert(entry.sent_id);
    }
    return output;
}

static auto load_file(std::string const &path) -> std::unordered_map<int, std::vector<int>>
{
    // Open the file in binary mode, at the end to get the size
    auto file = std::ifstream(path, std::ios::binary);

    if (!file) {
        fmt::print(stderr, "error opening file.\n");
        return {};
    }

    auto unpacker = msgpack::unpacker();

    std::unordered_map<int, std::vector<int>> result;

    int load_count = 0;

    constexpr int try_read_size = 16 * 1024 * 1024;
    while (file.good()) {
        unpacker.reserve_buffer(try_read_size);

        auto n_bytes_read = file.readsome(unpacker.buffer(), try_read_size);

        if (n_bytes_read == 0) {
            file.peek(); // Check if end-of-file is reached
        }

        if (n_bytes_read > 0) {
            unpacker.buffer_consumed(n_bytes_read);

            msgpack::object_handle handle;
            while (unpacker.next(handle)) {
                if (load_count % 100'000 == 0) {
                    fmt::println("Loaded {} sentences...", load_count);
                    std::fflush(stdout);
                }

                auto map = std::unordered_map<std::string, msgpack::object>(handle.get().convert());
                int tok_id = int(map.at("id").convert());
                auto sent_ids = std::vector<int>(map.at("tokens").convert());
                sent_ids.shrink_to_fit();
                result[tok_id] = std::move(sent_ids);

                load_count += 1;
            }

        } else if (file.eof()) {
            break;
        } else if (file.fail()) {
            throw std::runtime_error("Error reading file.");
        }
    }

    return result;
}

searcher::searcher()
{
    // load index
    fmt::println("Loading sentences...");
    std::fflush(stdout);

    sentences = load_file("/home/park/PycharmProjects/mk-tokenizer/tokenized_sentences.msgpack");

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
        bytes += sizeof(tok) + entries.size() * sizeof(index_entry);
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

    const char sample_input[] = "kaxnanxho ngixta 國家";
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
    std::fflush(stdout);
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
    int cnt = mask.count();
    if (cnt == 0) {
        return result;
    }
    result.reserve(cnt);
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
                              std::string const &search_regex,
                              std::unordered_map<std::string, idset> &cache,
                              std::string const &prev_prefix,
                              int level) const -> idset
{
    if (llg_matcher_compute_mask(matcher)) {
        throw std::runtime_error("Error computing mask");
    }

    auto bitmask = to_bitset(llg_matcher_get_mask(matcher), VOCAB_SIZE);

    if (level == 0) {
        bitmask &= gt_n_char_masks.at(pad_size);
    }

    auto next_tokens = nonzero_pos(bitmask);
    fmt::println("lvl {}: {}", level, prev_prefix);
    std::fflush(stdout);

    auto result = idset::empty();
    for (int token : next_tokens) {
        if (token == EOS_TOKEN_ID) {
            result = {};
            continue;
        }

        auto cur_prefix = prev_prefix + tid_to_token.at(token);
        if (level == 0) {
            auto it = cur_prefix.begin();
            for (int i = 0; i < pad_size; ++i) {
                utf8::next(it, cur_prefix.end());
            }
            cur_prefix = cur_prefix.substr(it - cur_prefix.begin());
        }

        auto matches = idset::empty();
        if (tok_to_sid.count(token) > 0) {
            matches = tok_to_sid.at(token);
        }

        idset cands = idset::all();
        if (cache.count(cur_prefix) > 0) {
            cands = cache.at(cur_prefix);
        } else {
            auto cur_prefix_view = absl::string_view(cur_prefix);
            if (RE2::Consume(&cur_prefix_view, search_regex)) {
                result = result | matches;
                continue;
            }

            if (llg_matcher_consume_token(matcher, token)) {
                throw std::runtime_error("llg_matcher_consume_token returned error");
            }

            cands = generate_cands(matcher, pad_size, search_regex, cache, cur_prefix, level + 1);

            if (llg_matcher_rollback(matcher, 1)) {
                throw std::runtime_error("llg_matcher_rollback returned error");
            }
        }

        result = result | matches.followed_by(cands);
    }

    if (level > 0) {
        cache[prev_prefix] = result;
    }

    return result;
}

auto searcher::search(std::string const &search_term) const -> std::set<int>
{
    LlgConstraintInit init;
    llg_constraint_init_set_defaults(&init, ll_tokenizer);

    struct LlgMatcherDeleter
    {
        void operator()(LlgMatcher *p) const { llg_free_matcher(p); }
    };

    auto search_regex = RE2::QuoteMeta(search_term);

    auto result = idset::empty();

    std::unordered_map<std::string, idset> cache;
    for (int pad_size = 0; pad_size < MAX_TOKEN_LENGTH; ++pad_size) {
        fmt::println("======= pad size = {} ========", pad_size);

        auto regex = fmt::format(".{{{}}}{}.*", pad_size, search_regex);
        fmt::println("Regex = {}", regex);

        auto m = std::unique_ptr<LlgMatcher, LlgMatcherDeleter>(
            llg_new_matcher(&init, "regex", regex.c_str()));
        if (llg_matcher_get_error(m.get())) {
            throw std::runtime_error("Error constructing constraint");
        }

        auto cands = generate_cands(m.get(), pad_size, search_regex, cache);
        result = result | cands;
    }

    return std::set<int>(result);
}
