#include "ibpe_backend.h"

#include "searcher.hpp"

#include <algorithm>
#include <fmt/core.h>
#include <type_traits>

static_assert(index_entry::SENTID_BITS == corpus_search::index_entry::SENTID_BITS);
static_assert(index_entry::POS_BITS == corpus_search::index_entry::POS_BITS);

static_assert(std::is_layout_compatible_v<index_entry, corpus_search::index_entry>);
static_assert(std::is_standard_layout_v<index_entry>);
static_assert(std::is_standard_layout_v<corpus_search::index_entry>);

auto create_index_builder() noexcept -> index_builder
{
    try {
        return reinterpret_cast<index_builder>(new corpus_search::index_builder{});
    } catch (...) {
        return nullptr;
    }
}

void destroy_index_builder(index_builder builder) noexcept
{
    delete reinterpret_cast<corpus_search::index_builder *>(builder);
}

void index_builder_add_sentence(index_builder builder,
                                int sent_id,
                                int *p_tokens,
                                int n_tokens) noexcept
{
    reinterpret_cast<corpus_search::index_builder *>(builder)
        ->add_sentence(sent_id, std::span<const int>(p_tokens, p_tokens + n_tokens));
}

void index_builder_finalize(index_builder builder) noexcept
{
    reinterpret_cast<corpus_search::index_builder *>(builder)->finalize_index();
}

void index_builder_iterate(index_builder builder,
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

auto create_tokenizer(char const *tokenizer_path, char *err_msg, int err_len) noexcept -> tokenizer
{
    try {
        return reinterpret_cast<tokenizer>(new corpus_search::tokenizer(tokenizer_path, true));
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

void destroy_tokenizer(tokenizer tok) noexcept
{
    delete reinterpret_cast<corpus_search::tokenizer *>(tok);
}

auto tokenizer_tokenize(tokenizer tok,
                        char const *string,
                        int *out_tokens,
                        std::size_t maxlen) noexcept -> int
{
    auto tokens = reinterpret_cast<corpus_search::tokenizer *>(tok)->tokenize(string);
    for (int i = 0; i < std::min(maxlen, tokens.size()); ++i) {
        out_tokens[i] = tokens[i];
    }
    return tokens.size();
}

auto tokenizer_get_vocab_size(tokenizer tok) noexcept -> int
{
    return reinterpret_cast<corpus_search::tokenizer *>(tok)->VOCAB_SIZE;
}

auto search_corpus(tokenizer tok,
                   index_accessor_cb callback,
                   char const *search_term) noexcept -> sentid_vec
{
    try {
        auto tok_ptr = reinterpret_cast<corpus_search::tokenizer *>(tok);
        auto cb = [callback](int token) -> std::vector<corpus_search::index_entry> {
            int num_entries = callback.func(callback.user_data, token, nullptr, 0);
            if (num_entries > 0) {
                auto result = std::vector<corpus_search::index_entry>(num_entries);
                callback.func(callback.user_data,
                              token,
                              reinterpret_cast<index_entry *>(result.data()),
                              num_entries);

                return result;
            }
            return {};
        };

        auto result = corpus_search::search(*tok_ptr, cb, std::string(search_term));

        auto sentid_vector = new std::vector<int>(std::move(result));
        return reinterpret_cast<sentid_vec>(sentid_vector);
    } catch (...) {
        return nullptr;
    }
}

auto sentid_vec_get_data(sentid_vec vec) noexcept -> int const *
{
    return reinterpret_cast<std::vector<int> *>(vec)->data();
}

auto sentid_vec_get_size(sentid_vec vec) noexcept -> int
{
    return reinterpret_cast<std::vector<int> *>(vec)->size();
}

void destroy_sentid_vec(sentid_vec vec) noexcept
{
    delete reinterpret_cast<std::vector<int> *>(vec);
}
