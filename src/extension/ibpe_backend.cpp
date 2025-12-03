#include "ibpe_backend.h"

#include "index_builder.hpp"
#include "utils.hpp"

#include <algorithm>
#include <fmt/core.h>

static_assert(index_entry::SENTID_BITS == corpus_search::index_entry::SENTID_BITS);
static_assert(index_entry::POS_BITS == corpus_search::index_entry::POS_BITS);

struct index_builder_data
{
    corpus_search::index_builder builder;
};

auto create_index_builder() noexcept -> index_builder
{
    try {
        auto builder = new index_builder_data{};
        return builder;
    } catch (...) {
        return nullptr;
    }
}

void destroy_index_builder(index_builder builder) noexcept
{
    delete builder;
}

void index_builder_add_sentence(index_builder builder,
                                int sent_id,
                                int *p_tokens,
                                int n_tokens) noexcept
{
    builder->builder.add_sentence(sent_id, std::span<const int>(p_tokens, p_tokens + n_tokens));
}

void index_builder_finalize(index_builder builder) noexcept
{
    builder->builder.finalize_index();
}

void index_builder_iterate(index_builder builder,
                           index_builder_iterate_function callback,
                           void *user_data) noexcept
{
    auto const &index = builder->builder.get_index();

    std::vector<int> tokens;
    for (auto &&[token, sentids] : index) {
        tokens.push_back(token);
    }
    std::sort(tokens.begin(), tokens.end());

    // iterate in order
    std::vector<index_entry> buf;
    for (int token : tokens) {
        for (auto sentid : index.at(token)) {
            buf.push_back({sentid.sent_id, sentid.pos});
        }
        callback(user_data, token, buf.data(), buf.size());
        buf.clear();
    }
}

struct tokenizer_data
{
    corpus_search::tokenizer tok;
};

auto create_tokenizer(char const *tokenizer_path, char *err_msg, int err_len) noexcept -> tokenizer
{
    try {
        auto tokenizer = new tokenizer_data{
            corpus_search::tokenizer(tokenizer_path, true),
        };
        return tokenizer;
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
    delete tok;
}

auto tokenizer_tokenize(tokenizer tok,
                        char const *string,
                        int *out_tokens,
                        std::size_t maxlen) noexcept -> int
{
    auto tokens = tok->tok.tokenize(string);
    for (int i = 0; i < std::min(maxlen, tokens.size()); ++i) {
        out_tokens[i] = tokens[i];
    }
    return tokens.size();
}
