#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <unordered_map>
#include <vector>

#include <llguidance.h>

struct index_entry;

auto tokenize(LlgTokenizer *tokenizer, std::string const &string) -> std::vector<std::uint32_t>;

auto make_index(std::unordered_map<int, std::vector<int>> sentences)
    -> std::unordered_map<int, std::vector<index_entry>>;

auto to_bytes(std::string s) -> std::string;

auto to_unicode(std::string s) -> std::string;

#endif // UTILS_H
