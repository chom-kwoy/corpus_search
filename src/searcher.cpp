#include "searcher.hpp"

#include "index_builder.hpp"
#include "utils.hpp"

#include <fstream>
#include <queue>

#include <fmt/chrono.h>
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <llguidance.h>
#include <msgpack.hpp>
#include <nlohmann/json.hpp>
#include <re2/re2.h>
#include <tokenizers_cpp.h>
#include <utf8.h>

namespace corpus_search {

namespace { // static linkage

auto get_sent_ids(std::vector<index_entry> const &self) -> std::vector<int>
{
    std::vector<int> output;
    int last_sent_id = -1;
    for (auto entry : self) {
        if (entry.sent_id != last_sent_id) {
            output.push_back(entry.sent_id);
        }
        last_sent_id = entry.sent_id;
    }
    return output;
}

auto load_file(std::string const &path) -> std::unordered_map<int, std::vector<int>>
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

static auto to_bitset(std::uint32_t const *mask, int len) -> boost::dynamic_bitset<>
{
    auto result = boost::dynamic_bitset<>(len);
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
} // namespace

searcher::searcher(std::string const &tokenized_sentences_path,
                   std::string const &tokenizer_json_path)
    : tok(tokenizer_json_path, true)
{
    // load index
    fmt::println("Loading sentences...");
    std::fflush(stdout);

    sentences = load_file(tokenized_sentences_path);

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

    if (max_len > MAX_POS + 1) {
        std::runtime_error(
            fmt::format("Sentence too long: max_len = {} > {}", max_len, MAX_POS + 1));
    }
    if (max_id > MAX_SENTID) {
        std::runtime_error(
            fmt::format("Too many sentences: max_sent_id = {} > {}", max_id, MAX_SENTID));
    }

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

    auto nchars_to_tid = std::unordered_map<int, std::vector<int>>{};
    for (auto const &[tid, token] : tok.get_tid_to_token()) {
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

    for (int n = 0; n <= tok.MAX_TOKEN_LENGTH; ++n) {
        auto bitmask = boost::dynamic_bitset<>(tok.VOCAB_SIZE);
        for (int gt_n = n + 1; gt_n <= tok.MAX_TOKEN_LENGTH; ++gt_n) {
            for (auto tid : nchars_to_tid.at(gt_n)) {
                bitmask.set(tid);
            }
        }
        gt_n_char_masks[n] = bitmask;
    }

    fmt::println("Done.");
    std::fflush(stdout);
}

namespace {

auto followed_by(std::vector<index_entry> const &self,
                 candset const &other) -> std::vector<index_entry>
{
    if (!other.has_value()) {
        return self;
    }

    auto const &arr1 = self;
    auto const &arr2 = other.value();

    std::vector<index_entry> result;

    auto it1 = arr1.begin(), it2 = arr2.begin();
    while (it1 != arr1.end() && it2 != arr2.end()) {
        auto entry1 = *it1;
        auto entry2 = *it2;
        if (entry1.sent_id < entry2.sent_id) {
            ++it1;
        } else {
            if (entry1.sent_id == entry2.sent_id) {
                if (entry1.pos + 1 < entry2.pos) {
                    ++it1;
                } else if (entry1.pos + 1 == entry2.pos) {
                    result.push_back(*it1);
                    ++it1;
                    ++it2;
                } else {
                    ++it2;
                }
            } else {
                ++it2;
            }
        }
    }

    return result;
}

using vec_pointer = std::vector<index_entry> const *;
using vec_object = std::vector<index_entry>;
using pointer_or_object = std::variant<vec_pointer, vec_object>;

auto merge_sorted_lists(std::vector<pointer_or_object> const &cand_lists) -> std::vector<index_entry>
{
    auto result = std::vector<index_entry>{};

    using queue_item = std::tuple<index_entry, int, int>;
    struct comparator
    {
        auto operator()(queue_item const &l, queue_item const &r) -> bool
        {
            return std::get<0>(r) < std::get<0>(l);
        }
    };
    auto pending = std::priority_queue<queue_item, std::vector<queue_item>, comparator>{};

    auto get_size = [](auto &&x) {
        return std::visit(
            [](auto &&arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, vec_pointer>) {
                    return arg->size();
                } else {
                    return arg.size();
                }
            },
            x);
    };
    auto get_item = [](auto &&x, std::size_t index) {
        return std::visit(
            [index](auto &&arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, vec_pointer>) {
                    return (*arg)[index];
                } else {
                    return arg[index];
                }
            },
            x);
    };

    for (int i = 0; i < cand_lists.size(); ++i) {
        if (get_size(cand_lists[i]) >= 1) {
            pending.push({get_item(cand_lists[i], 0), i, 0});
        }
    }
    while (!pending.empty()) {
        auto [item, vec_index, item_index] = pending.top();
        pending.pop();
        if (item_index + 1 < get_size(cand_lists[vec_index])) {
            pending.push({
                get_item(cand_lists[vec_index], item_index + 1),
                vec_index,
                item_index + 1,
            });
        }
        if (result.size() == 0 || item != result.back()) {
            result.push_back(item);
        }
    }

    assert(std::is_sorted(result.begin(), result.end()));

    return result;
}
} // namespace

auto searcher::generate_cands(LlgMatcher *matcher,
                              int pad_size,
                              RE2 const &search_regex,
                              std::unordered_map<std::string, candset> &cache,
                              std::string const &prev_prefix,
                              int level) const -> std::vector<index_entry>
{
    if (llg_matcher_compute_mask(matcher)) {
        throw std::runtime_error("Error computing mask");
    }

    auto bitmask = to_bitset(llg_matcher_get_mask(matcher), tok.VOCAB_SIZE);

    if (level == 0) {
        bitmask &= gt_n_char_masks.at(pad_size);
    }

    auto next_tokens = nonzero_pos(bitmask);
    fmt::println("lvl {}: '{}' (+ {} tokens)", level, prev_prefix, next_tokens.size());

    auto cand_lists = std::vector<pointer_or_object>{};

    for (int token : next_tokens) {
        assert(token != EOS_TOKEN_ID);

        auto cur_prefix = prev_prefix + tok.get_tid_to_token().at(token);
        if (level == 0) {
            auto it = cur_prefix.begin();
            for (int i = 0; i < pad_size; ++i) {
                utf8::next(it, cur_prefix.end());
            }
            cur_prefix = cur_prefix.substr(it - cur_prefix.begin());
        }

        if (tok_to_sid.count(token) == 0) {
            continue;
        }

        auto const &matches = tok_to_sid.at(token);

        if (cache.count(cur_prefix) > 0) {
            auto const &cands = cache.at(cur_prefix);

            cand_lists.push_back(followed_by(matches, cands));
            continue;
        }

        auto cur_prefix_view = absl::string_view(cur_prefix);
        if (RE2::Consume(&cur_prefix_view, search_regex)) {
            cand_lists.push_back(&matches);
            continue;
        }

        if (llg_matcher_consume_token(matcher, token)) {
            throw std::runtime_error("llg_matcher_consume_token returned error");
        }

        auto cands = generate_cands(matcher, pad_size, search_regex, cache, cur_prefix, level + 1);

        if (llg_matcher_rollback(matcher, 1)) {
            throw std::runtime_error("llg_matcher_rollback returned error");
        }

        cand_lists.push_back(followed_by(matches, cands));
    }

    // union the sorted sequences in vec_result
    auto result = merge_sorted_lists(cand_lists);

    if (level > 0) {
        cache[prev_prefix] = result;
    }

    return result;
}

auto searcher::search(std::string const &search_term) const -> std::vector<int>
{
    LlgConstraintInit init;
    llg_constraint_init_set_defaults(&init, tok.get_ll_tokenizer());

    struct LlgMatcherDeleter
    {
        void operator()(LlgMatcher *p) const { llg_free_matcher(p); }
    };

    auto search_regex = RE2::QuoteMeta(search_term);

    auto cand_lists = std::vector<pointer_or_object>{};

    std::unordered_map<std::string, candset> cache;
    for (int pad_size = 0; pad_size < tok.MAX_TOKEN_LENGTH; ++pad_size) {
        fmt::println("======= pad size = {} ========", pad_size);

        auto regex = fmt::format(".{{{}}}{}.*", pad_size, search_regex);
        fmt::println("Regex = {}", regex);
        std::fflush(stdout);

        auto m = std::unique_ptr<LlgMatcher, LlgMatcherDeleter>(
            llg_new_matcher(&init, "regex", regex.c_str()));
        if (llg_matcher_get_error(m.get())) {
            throw std::runtime_error("Error constructing constraint");
        }

        auto cands = generate_cands(m.get(), pad_size, RE2(search_regex), cache);

        cand_lists.push_back(std::move(cands));
    }

    return get_sent_ids(merge_sorted_lists(cand_lists));
}

} // namespace corpus_search
