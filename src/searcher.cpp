#include "searcher.hpp"

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

using candset = std::optional<std::vector<index_entry>>;

namespace { // static linkage

auto get_sent_ids(std::vector<index_entry> const &self) -> std::vector<sentid_t>
{
    std::vector<sentid_t> output;
    sentid_t last_sent_id = -1;
    for (auto entry : self) {
        if (entry.sent_id != last_sent_id) {
            output.push_back(entry.sent_id);
        }
        last_sent_id = entry.sent_id;
    }
    return output;
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

auto followed_by(std::vector<index_entry> &&self, candset const &other) -> std::vector<index_entry>
{
    if (!other.has_value()) {
        return self;
    }

    auto const &arr1 = self;
    auto const &arr2 = other.value();

    std::vector<index_entry> result;

    auto it1 = arr1.begin();
    auto it2 = arr2.begin();
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

using vec_pointer = std::span<const index_entry>;
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

    auto get_size = [](auto &&x) { return std::visit([](auto &&arg) { return arg.size(); }, x); };
    auto get_item = [](auto &&x, std::size_t index) {
        return std::visit([index](auto &&arg) { return arg[index]; }, x);
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

auto generate_cands(tokenizer const &tok,
                    std::function<index_accessor> const &index,
                    LlgMatcher *matcher,
                    int pad_size,
                    RE2 const &search_regex,
                    std::unordered_map<std::string, candset> &cache,
                    std::string const &prev_prefix = "",
                    int level = 0) -> std::vector<index_entry>
{
    if (llg_matcher_compute_mask(matcher)) {
        throw std::runtime_error("Error computing mask");
    }

    auto bitmask = to_bitset(llg_matcher_get_mask(matcher), tok.vocab_size());

    if (level == 0) {
        bitmask &= tok.gt_n_char_mask(pad_size);
    }

    auto next_tokens = nonzero_pos(bitmask);
    fmt::println("lvl {}: '{}' (+ {} tokens)", level, prev_prefix, next_tokens.size());

    auto cand_lists = std::vector<pointer_or_object>{};

    for (int token : next_tokens) {
        assert(token != tok.EOS_TOKEN_ID);

        auto token_str = tok.unnormalize(tok.get_tid_to_token().at(token));
        auto cur_prefix = prev_prefix + token_str;
        if (level == 0) {
            auto it = cur_prefix.begin();
            for (int i = 0; i < pad_size; ++i) {
                utf8::next(it, cur_prefix.end());
            }
            cur_prefix = cur_prefix.substr(it - cur_prefix.begin());
        }

        auto matches = index(token);
        if (matches.size() == 0) {
            continue;
        }

        if (cache.count(cur_prefix) > 0) {
            auto const &cands = cache.at(cur_prefix);

            cand_lists.push_back(followed_by(std::move(matches), cands));
            continue;
        }

        auto cur_prefix_view = absl::string_view(cur_prefix);
        if (RE2::Consume(&cur_prefix_view, search_regex)) {
            cand_lists.push_back(matches);
            continue;
        }

        if (llg_matcher_consume_token(matcher, token)) {
            throw std::runtime_error("llg_matcher_consume_token returned error");
        }

        auto cands = generate_cands(tok,
                                    index,
                                    matcher,
                                    pad_size,
                                    search_regex,
                                    cache,
                                    cur_prefix,
                                    level + 1);

        if (llg_matcher_rollback(matcher, 1)) {
            throw std::runtime_error("llg_matcher_rollback returned error");
        }

        cand_lists.push_back(followed_by(std::move(matches), cands));
    }

    // union the sorted sequences in vec_result
    auto result = merge_sorted_lists(cand_lists);

    if (level > 0) {
        cache[prev_prefix] = result;
    }

    return result;
}

} // namespace

auto search(tokenizer const &tok,
            std::function<index_accessor> const &index,
            std::string const &search_term) -> std::vector<sentid_t>
{
    LlgConstraintInit init;
    llg_constraint_init_set_defaults(&init, tok.get_ll_tokenizer());

    struct LlgMatcherDeleter
    {
        void operator()(LlgMatcher *p) const { llg_free_matcher(p); }
    };

    fmt::println("Regex = {}", search_term);
    std::fflush(stdout);

    auto cand_lists = std::vector<pointer_or_object>{};

    std::unordered_map<std::string, candset> cache;
    for (int pad_size = 0; pad_size < tok.MAX_TOKEN_LENGTH; ++pad_size) {
        fmt::println("======= pad size = {} ========", pad_size);

        auto regex = fmt::format(".{{{}}}{}.*", pad_size, search_term);

        auto m = std::unique_ptr<LlgMatcher, LlgMatcherDeleter>(
            llg_new_matcher(&init, "regex", regex.c_str()));
        if (llg_matcher_get_error(m.get())) {
            throw std::runtime_error("Error constructing constraint");
        }

        auto cands = generate_cands(tok, index, m.get(), pad_size, RE2(search_term), cache);

        cand_lists.push_back(std::move(cands));
    }

    return get_sent_ids(merge_sorted_lists(cand_lists));
}

} // namespace corpus_search
