cmake_minimum_required(VERSION 3.16)

project(corpus_search LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

add_executable(corpus_search main.cpp
    cmake/FindLLGuidance.cmake
    utils.cpp
    utils.h
    searcher.cpp
    searcher.h
)

# msgpack-cxx (included by tokenizers-cpp)
#find_package(msgpack-cxx REQUIRED)
#
#target_link_libraries(corpus_search PUBLIC msgpack-cxx)

# fmt
include(FetchContent)
FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt
  GIT_TAG        e69e5f977d458f2650bb346dadf2ad30c5320281) # 10.2.1
FetchContent_MakeAvailable(fmt)

target_link_libraries(corpus_search PUBLIC fmt::fmt)

# nlohmann json
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(json)

target_link_libraries(corpus_search PUBLIC nlohmann_json::nlohmann_json)

# tokenizers-cpp
set(TOKENZIER_CPP_PATH "../tokenizers-cpp" CACHE PATH
    "Path to tokenizers-cpp library. Use `git clone https://github.com/mlc-ai/tokenizers-cpp.git --recurse-submodules`.")
add_subdirectory(${TOKENZIER_CPP_PATH} tokenizers EXCLUDE_FROM_ALL)

target_include_directories(corpus_search PUBLIC ${TOKENZIER_CPP_PATH}/include)
target_link_libraries(corpus_search PUBLIC tokenizers_cpp)

# llguidance
find_package(LLGuidance REQUIRED)

target_link_libraries(corpus_search PUBLIC LLGuidance::LLGuidance)

# install
include(GNUInstallDirs)
install(TARGETS corpus_search
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
