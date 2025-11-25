#ifndef UTILS_H
#define UTILS_H

#include <llguidance.h>
#include <msgpack.hpp>
#include <vector>

struct IndexEntry;

auto tokenize(LlgTokenizer *tokenizer, std::string const &string) -> std::vector<std::uint32_t>;

auto make_index(std::unordered_map<int, std::vector<int>> sentences)
    -> std::unordered_map<int, std::vector<IndexEntry>>;

auto to_bytes(std::string s) -> std::string;

#endif // UTILS_H
