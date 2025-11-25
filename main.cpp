#include "searcher.h"

#include <chrono>
#include <fmt/chrono.h>

void measure_time(searcher const &s, std::string search_term)
{
    using namespace std::chrono;
    auto start_time = high_resolution_clock::now();

    s.search(search_term);

    auto end_time = high_resolution_clock::now();
    fmt::println("Took {}.", duration_cast<milliseconds>(end_time - start_time));
}

int main()
{
    auto s = searcher();

    measure_time(s, "sixtaxsoxngixta");
    measure_time(s, "ngixta");

    return 0;
}
