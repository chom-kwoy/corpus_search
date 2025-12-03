#include "index_builder.hpp"

#include <algorithm>
#include <fmt/core.h>

namespace corpus_search {

void index_builder::add_sentence(int sent_id, std::span<const int> tokens)
{
    if (sent_id < 0 || sent_id > index_entry::MAX_SENTID) {
        throw std::runtime_error(fmt::format("Invalid sentid {}.", sent_id));
    }

    int pos = 0;
    for (int token : tokens) {
        if (pos > index_entry::MAX_POS) {
            throw std::runtime_error(fmt::format("Invalid token pos {}.", pos));
        }
        result[token].push_back({
            static_cast<unsigned int>(sent_id),
            static_cast<unsigned int>(pos),
        });
        pos += 1;
    }
}

auto index_builder::finalize_index() -> std::unordered_map<int, std::vector<index_entry>>
{
    for (auto &&[tok_id, entries] : result) {
        std::sort(entries.begin(), entries.end());
    }
    return result;
}

auto index_builder::get_index() const -> std::unordered_map<int, std::vector<index_entry>> const &
{
    return result;
}

auto make_index(std::unordered_map<int, std::vector<int>> sentences)
    -> std::unordered_map<int, std::vector<index_entry>>
{
    auto builder = index_builder();
    for (auto const &[sent_id, tokens] : sentences) {
        builder.add_sentence(sent_id, tokens);
    }
    return builder.finalize_index();
}

} // namespace corpus_search
