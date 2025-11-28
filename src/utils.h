#ifndef UTILS_H
#define UTILS_H

#include <vector>

#include <llguidance.h>
#include <msgpack.hpp>
#include <roaring.hh>

struct index_entry;

auto tokenize(LlgTokenizer *tokenizer, std::string const &string) -> std::vector<std::uint32_t>;

auto make_index(std::unordered_map<int, std::vector<int>> sentences)
    -> std::unordered_map<int, std::vector<index_entry>>;

auto to_bytes(std::string s) -> std::string;

auto to_unicode(std::string s) -> std::string;

class measure_time
{
    std::chrono::duration<float> &m_timer;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_time;

public:
    measure_time(std::chrono::duration<float> &timer)
        : m_timer(timer)
        , start_time(std::chrono::high_resolution_clock::now())
    {}
    ~measure_time() { m_timer += std::chrono::high_resolution_clock::now() - start_time; }
};

#endif // UTILS_H
