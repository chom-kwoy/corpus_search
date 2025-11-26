#include "searcher.h"

#include <chrono>
#include <fmt/chrono.h>

void measure_time(searcher const &s, std::string search_term)
{
    using namespace std::chrono;
    auto start_time = high_resolution_clock::now();

    auto result = s.search(search_term);

    auto end_time = high_resolution_clock::now();

    if (result.size() < 200) {
        fmt::println("Result for '{}' = Array[{}]{{{}}}",
                     search_term,
                     result.size(),
                     fmt::join(result, ", "));
    } else {
        fmt::println("Result for '{}' = Array[{}]{{...}}", search_term, result.size());
    }

    auto elapsed = end_time - start_time;
    fmt::println("Took {}.", duration_cast<duration<float>>(elapsed));
}

int main()
{
    auto s = searcher("/home/park/PycharmProjects/mk-tokenizer/tokenized_sentences.msgpack",
                      "/home/park/PycharmProjects/mk-tokenizer/bpe_tokenizer/tokenizer.json");

    // fmt::print("Press enter to continue");
    // std::getchar();

    measure_time(s, "z");
    measure_time(s, "o");
    measure_time(s, "ho");
    measure_time(s, "sixtaxsoxngixta");
    measure_time(s, "ngixta");
    measure_time(s, "kaxnanxho");

    return 0;
}
