#include "index_builder.hpp"

#include <algorithm>
#include <fmt/core.h>
#include <fstream>
#include <msgpack.hpp>

namespace corpus_search {

namespace {

auto load_file(std::string const &path) -> std::unordered_map<int, std::vector<int>>
{
    // Open the file in binary mode, at the end to get the size
    auto file = std::ifstream(path, std::ios::binary);

    if (!file) {
        throw std::runtime_error("Error reading file.");
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

} // namespace

auto index_builder::from_file(const std::string &tokenized_sentences_path) -> index_builder
{
    // load index
    fmt::println("Loading sentences...");
    std::fflush(stdout);

    auto sentences = load_file(tokenized_sentences_path);

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

    index_builder index;

    for (auto &&[sent_id, tokens] : sentences) {
        index.add_sentence(sent_id, tokens);
    };
    index.finalize_index();

    int bytes = 0;
    for (auto const &[tok, entries] : index.get_index()) {
        bytes += sizeof(tok) + entries.size() * sizeof(index_entry);
    }

    fmt::println("Made index. Index size = {} MB", bytes / 1'000'000);
    std::fflush(stdout);

    return index;
}

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

void index_builder::finalize_index()
{
    for (auto &&[tok_id, entries] : result) {
        std::sort(entries.begin(), entries.end());
    }
}

auto index_builder::get_index() const -> std::unordered_map<int, std::vector<index_entry>> const &
{
    return result;
}

} // namespace corpus_search
