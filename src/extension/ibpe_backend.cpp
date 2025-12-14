#include "ibpe_backend.h"

#include "index_builder.hpp"
#include "searcher.hpp"

#include <algorithm>
#include <fmt/core.h>
#include <nlohmann/json.hpp>
#include <type_traits>
#include <unordered_map>

static_assert(
    std::is_layout_compatible_v<corpus_search::backend::index_entry, corpus_search::index_entry>);
static_assert(std::is_standard_layout_v<corpus_search::backend::index_entry>);
static_assert(std::is_standard_layout_v<corpus_search::index_entry>);

auto corpus_search::backend::create_index_builder() noexcept -> index_builder
{
    try {
        return reinterpret_cast<index_builder>(new corpus_search::index_builder{});
    } catch (...) {
        return nullptr;
    }
}

void corpus_search::backend::destroy_index_builder(index_builder builder) noexcept
{
    delete reinterpret_cast<corpus_search::index_builder *>(builder);
}

void corpus_search::backend::index_builder_add_sentence(index_builder builder,
                                                        sentid_t sent_id,
                                                        int *p_tokens,
                                                        int n_tokens) noexcept
{
    reinterpret_cast<corpus_search::index_builder *>(builder)
        ->add_sentence(sent_id, std::span<const int>(p_tokens, p_tokens + n_tokens));
}

void corpus_search::backend::index_builder_finalize(index_builder builder) noexcept
{
    reinterpret_cast<corpus_search::index_builder *>(builder)->finalize_index();
}

void corpus_search::backend::index_builder_iterate(index_builder builder,
                                                   index_builder_iterate_function callback,
                                                   void *user_data) noexcept
{
    auto const &index = reinterpret_cast<corpus_search::index_builder *>(builder)->get_index();

    std::vector<int> tokens;
    for (auto &&[token, sentids] : index) {
        tokens.push_back(token);
    }
    std::sort(tokens.begin(), tokens.end());

    // iterate in order
    for (int token : tokens) {
        auto const &vec = index.at(token);

        // this is normally UB, but allowed because of __may_alias__
        auto data = reinterpret_cast<index_entry const *>(vec.data());
        callback(user_data, token, data, vec.size());
    }
}

auto corpus_search::backend::create_tokenizer(char const *tokenizer_path,
                                              char normalize_mappings[][2],
                                              int n_normalize_mappings,
                                              char *err_msg,
                                              int err_len) noexcept -> tokenizer
{
    try {
        std::unordered_map<char, char> mapping;
        for (int i = 0; i < n_normalize_mappings; ++i) {
            mapping[normalize_mappings[i][0]] = normalize_mappings[i][1];
        }
        return reinterpret_cast<tokenizer>(
            new corpus_search::tokenizer(tokenizer_path, mapping, true));
    } catch (std::exception const &e) {
        fmt::println("Error: {}", e.what());
        if (err_msg) {
            std::strncpy(err_msg, e.what(), err_len - 1);
        }
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

void corpus_search::backend::destroy_tokenizer(tokenizer tok) noexcept
{
    delete reinterpret_cast<corpus_search::tokenizer *>(tok);
}

auto corpus_search::backend::tokenizer_tokenize(tokenizer tok,
                                                char const *string,
                                                int *out_tokens,
                                                std::size_t maxlen) noexcept -> int
{
    auto tokens = reinterpret_cast<corpus_search::tokenizer *>(tok)->tokenize(string, true);
    for (int i = 0; i < std::min(maxlen, tokens.size()); ++i) {
        out_tokens[i] = tokens[i];
    }
    return tokens.size();
}

auto corpus_search::backend::tokenizer_get_vocab_size(tokenizer tok) noexcept -> int
{
    return reinterpret_cast<corpus_search::tokenizer *>(tok)->vocab_size();
}

auto corpus_search::backend::search_corpus(tokenizer tok,
                                           index_accessor_cb callback,
                                           char const *search_term) noexcept -> sentid_vec
{
    try {
        auto tok_ptr = reinterpret_cast<corpus_search::tokenizer *>(tok);
        auto cb = [callback](int token) -> std::vector<corpus_search::token_range> {
            int num_entries = callback.func(callback.user_data, token, nullptr, 0);
            if (num_entries > 0) {
                auto vec = std::vector<corpus_search::index_entry>(num_entries);
                callback.func(callback.user_data,
                              token,
                              reinterpret_cast<index_entry *>(vec.data()),
                              num_entries);

                auto result = std::vector<corpus_search::token_range>(num_entries);
                result.reserve(vec.size());
                for (auto const &entry : vec) {
                    result.push_back(corpus_search::token_range{
                        entry.sent_id,
                        entry.pos,
                        static_cast<tokpos_t>(entry.pos + 1),
                    });
                }
                return result;
            }
            return {};
        };

        auto result = corpus_search::search(*tok_ptr, cb, std::string(search_term));

        auto sentid_vector = new std::vector<sentid_t>(std::move(result));
        return reinterpret_cast<sentid_vec>(sentid_vector);
    } catch (...) {
        return nullptr;
    }
}

auto corpus_search::backend::sentid_vec_get_data(sentid_vec vec) noexcept -> sentid_t const *
{
    return reinterpret_cast<std::vector<sentid_t> *>(vec)->data();
}

auto corpus_search::backend::sentid_vec_get_size(sentid_vec vec) noexcept -> size_t
{
    return reinterpret_cast<std::vector<sentid_t> *>(vec)->size();
}

void corpus_search::backend::destroy_sentid_vec(sentid_vec vec) noexcept
{
    delete reinterpret_cast<std::vector<sentid_t> *>(vec);
}

auto corpus_search::backend::parse_normalize_mappings(char const *json_str,
                                                      char mappings[][2],
                                                      int max_mappings) noexcept -> int
{
    try {
        auto json = nlohmann::json::parse(json_str);
        std::unordered_map<std::string, std::string> parsed = json;
        int count = 0;
        for (auto [k, v] : parsed) {
            if (count >= max_mappings) {
                break;
            }
            if (k.size() != 1 || v.size() != 1) {
                throw std::runtime_error("element is not a single char");
            }
            mappings[count][0] = k[0];
            mappings[count][1] = v[0];
            count++;
        }
        return parsed.size();
    } catch (std::exception const &e) {
        fmt::println(stderr, "ERROR parsing json '{}': {}", json_str, e.what());
        std::fflush(stderr);
        return -1;
    } catch (...) {
        return -1;
    }
}
