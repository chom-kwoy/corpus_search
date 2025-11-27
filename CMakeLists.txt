cmake_minimum_required(VERSION 3.16)

include(FetchContent)
include(GNUInstallDirs)
include(cmake/CPM.cmake)

project(corpus_search LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

add_library(lib_corpus_search SHARED
    src/utils.cpp
    src/utils.h
    src/searcher.cpp
    src/searcher.h
    src/node_register.cpp
)

########### Link External libraries ###########
# fmt
FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt
  GIT_TAG        e69e5f977d458f2650bb346dadf2ad30c5320281) # 10.2.1
FetchContent_MakeAvailable(fmt)
set_property(TARGET fmt PROPERTY POSITION_INDEPENDENT_CODE ON)

target_link_libraries(lib_corpus_search PUBLIC fmt::fmt)

# nlohmann json
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(json)

target_link_libraries(lib_corpus_search PUBLIC nlohmann_json::nlohmann_json)

# tokenizers-cpp
set(TOKENZIER_CPP_PATH "../tokenizers-cpp" CACHE PATH
    "Path to tokenizers-cpp library. Use `git clone https://github.com/mlc-ai/tokenizers-cpp.git --recurse-submodules`.")
add_subdirectory(${TOKENZIER_CPP_PATH} tokenizers EXCLUDE_FROM_ALL)
set_property(TARGET tokenizers_cpp PROPERTY POSITION_INDEPENDENT_CODE ON)

target_link_libraries(lib_corpus_search PUBLIC tokenizers_cpp)

# msgpack-cxx (included by tokenizers-cpp)
#find_package(msgpack-cxx REQUIRED)
#target_link_libraries(lib_corpus_search PUBLIC msgpack-cxx)

# llguidance
find_package(LLGuidance REQUIRED)

target_link_libraries(lib_corpus_search PUBLIC LLGuidance::LLGuidance)

# boost dynamic_bitset
find_package(Boost REQUIRED)

target_include_directories(lib_corpus_search PUBLIC ${Boost_INCLUDE_DIRS})

# re2 (version 2023-11-01)
find_package(re2 REQUIRED)

target_link_libraries(lib_corpus_search PUBLIC re2::re2)

# utf8cpp
add_library(utf8cpp INTERFACE)
target_include_directories(utf8cpp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/utf8cpp")

target_link_libraries(lib_corpus_search PUBLIC utf8cpp)

# roaring bitmap
CPMAddPackage(
  NAME roaring
  GITHUB_REPOSITORY "RoaringBitmap/CRoaring"
  GIT_TAG v2.0.4
  OPTIONS "ENABLE_ROARING_TESTS OFF"
)

target_link_libraries(lib_corpus_search PUBLIC roaring::roaring)
##################################################

# main executable
add_executable(corpus_search src/main.cpp)

target_link_libraries(corpus_search PUBLIC lib_corpus_search)

# install
install(TARGETS corpus_search
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Testing
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(test_corpus_search test/test.cpp)
target_link_libraries(test_corpus_search PUBLIC
    lib_corpus_search
    GTest::gmock_main
)
target_include_directories(test_corpus_search PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

include(GoogleTest)
gtest_discover_tests(test_corpus_search)
