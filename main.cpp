#include "searcher.h"

#include <chrono>
#include <fmt/chrono.h>

void measure_time(searcher const &s, std::string search_term)
{
    using namespace std::chrono;
    auto start_time = high_resolution_clock::now();

    auto result = s.search(search_term);

    auto end_time = high_resolution_clock::now();

    if (result.size() < 10000) {
        fmt::println("Result for '{}' = Array[{}]{{{}}}",
                     search_term,
                     result.size(),
                     fmt::join(result, ", "));
    } else {
        fmt::println("Result for '{}' = Array[{}]{{...}}", search_term, result.size());
    }

    fmt::println("Took {}.", duration_cast<milliseconds>(end_time - start_time));
}

int main()
{
    auto s = searcher();

    // fmt::print("Press enter to continue");
    // std::getchar();

    // measure_time(s, "z");
    measure_time(s, "a");
    // measure_time(s, "ho");
    // measure_time(s, "sixtaxsoxngixta");
    // measure_time(s, "ngixta");
    // measure_time(s, "kaxnanxho");

    return 0;
}
