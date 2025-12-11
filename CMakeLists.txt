cmake_minimum_required(VERSION 3.28)
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

include(FetchContent)
include(GNUInstallDirs)
include(cmake/CPM.cmake)

project(corpus_search LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED OFF)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS_INIT "-fsanitize=undefined -fsanitize=address")
    set(CMAKE_CXX_FLAGS_INIT "-fsanitize=undefined -fsanitize=address")
    message(NOTICE "Sanitizers enabled.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

add_library(lib_corpus_search
    src/sizes.h
    src/tokenizer.cpp
    src/tokenizer.hpp
    src/searcher.cpp
    src/searcher.hpp
    src/index_builder.cpp
    src/index_builder.hpp
    src/meta_utils.hpp
    src/regex_parse.hpp
    src/regex_parse.cpp
    src/regex_ast.hpp
    src/regex_ast.cpp
    src/regex_dfa.hpp
    src/regex_dfa.cpp
    src/dfa_trie.cpp
    src/dfa_trie.hpp
)
set_property(TARGET lib_corpus_search PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(lib_corpus_search INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src/")

target_compile_definitions(lib_corpus_search PUBLIC
    CORPUS_SEARCH_INDEX_ENTRY_BITS=64
    CORPUS_SEARCH_POSITION_BITS=12
)


##################################################
########### Link External libraries ##############
## fmt ##
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt
    GIT_TAG        e69e5f977d458f2650bb346dadf2ad30c5320281  # 10.2.1
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(fmt)
set_property(TARGET fmt PROPERTY POSITION_INDEPENDENT_CODE ON)
target_link_libraries(lib_corpus_search PRIVATE fmt::fmt)

## nlohmann json ##
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(json)
set_property(TARGET nlohmann_json PROPERTY POSITION_INDEPENDENT_CODE ON)
target_link_libraries(lib_corpus_search PUBLIC nlohmann_json::nlohmann_json)

## re2 (version 2023-11-01) ##
CPMAddPackage(
    re2
    GITHUB_REPOSITORY "google/re2"
    GIT_TAG           "2023-11-01"
    OPTIONS           "RE2_USE_ICU ON"
    EXCLUDE_FROM_ALL  ON
)
set_property(TARGET re2 PROPERTY POSITION_INDEPENDENT_CODE ON)
target_link_libraries(lib_corpus_search PRIVATE re2::re2)

## tokenizers-cpp ##
CPMAddPackage(
    tokenizers_cpp
    GITHUB_REPOSITORY "mlc-ai/tokenizers-cpp"
    GIT_TAG           55d53aa38dc8df7d9c8bd9ed50907e82ae83ce66
    EXCLUDE_FROM_ALL  ON
)
set_property(TARGET tokenizers_cpp PROPERTY POSITION_INDEPENDENT_CODE ON)
target_link_libraries(lib_corpus_search PRIVATE tokenizers_cpp)

## utf8-cpp header-only library ##
FetchContent_Declare(
    utf8cpp
    GIT_REPOSITORY https://github.com/nemtrif/utfcpp
    GIT_TAG 70795627871251f6c57f952e667303a2381c2fc5
    SOURCE_SUBDIR "ignore"  # ignore CMakeLists.txt
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(utf8cpp)
add_library(utf8cpp INTERFACE)
target_include_directories(utf8cpp INTERFACE "${utf8cpp_SOURCE_DIR}/source")
target_link_libraries(lib_corpus_search PRIVATE utf8cpp)

## msgpack-cxx ##
CPMAddPackage(
    NAME msgpack-cxx
    GITHUB_REPOSITORY "msgpack/msgpack-c"
    GIT_TAG b0d379a6f29af7dd65423ff78a94de8948f01bd8
    EXCLUDE_FROM_ALL ON
)
target_link_libraries(lib_corpus_search PUBLIC msgpack-cxx)

## PEGTL ##
CPMAddPackage(
    NAME pegtl
    GITHUB_REPOSITORY "taocpp/PEGTL"
    GIT_TAG "3.2.8"
    EXCLUDE_FROM_ALL ON
)
target_link_libraries(lib_corpus_search PUBLIC taocpp::pegtl)

## boost dynamic_bitset ##
find_package(Boost REQUIRED)
target_include_directories(lib_corpus_search PRIVATE ${Boost_INCLUDE_DIRS})

## roaring bitmap ##
CPMAddPackage(
  NAME roaring
  GITHUB_REPOSITORY "RoaringBitmap/CRoaring"
  GIT_TAG "v2.0.4"
  OPTIONS "ENABLE_ROARING_TESTS OFF"
  EXCLUDE_FROM_ALL ON
)
target_link_libraries(lib_corpus_search PUBLIC roaring::roaring)
##################################################
##################################################


# Postgres extension installation
execute_process(
    COMMAND pg_config --includedir-server
    OUTPUT_VARIABLE pg_includedir
    RESULT_VARIABLE pg_includedir_obtained
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if ((pg_includedir_obtained EQUAL 0) AND (EXISTS "${pg_includedir}"))
    message(STATUS "Found PostgreSQL server header files in ${pg_includedir}.")
    add_library(ibpe SHARED
        src/extension/ibpe_utils.h
        src/extension/ibpe_utils.c
        src/extension/ibpe_relcache.h
        src/extension/ibpe_relcache.c
        src/extension/ibpe_build.h
        src/extension/ibpe_build.c
        src/extension/ibpe_scan.h
        src/extension/ibpe_scan.c
        src/extension/ibpe_backend.h
        src/extension/ibpe_backend.cpp
        test/test.hpp
    )

    target_compile_definitions(ibpe PRIVATE USE_ASSERT_CHECKING)
    target_link_libraries(ibpe PRIVATE lib_corpus_search fmt::fmt)
    target_include_directories(ibpe PRIVATE ${pg_includedir})

    set_target_properties(ibpe PROPERTIES PREFIX "")

    execute_process(
        COMMAND pg_config --sharedir
        OUTPUT_VARIABLE pg_sharedir
        RESULT_VARIABLE pg_sharedir_obtained
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND pg_config --pkglibdir
        OUTPUT_VARIABLE pg_pkglibdir
        RESULT_VARIABLE pg_pkglibdir_obtained
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    message(STATUS "Will install postgres extension to ${pg_sharedir}/extension and ${pg_pkglibdir}")

    # install
    install(FILES
        "src/extension/corpussearch--1.0.sql"
        "src/extension/corpussearch.control"
        DESTINATION "${pg_sharedir}/extension"
    )
    install(TARGETS ibpe LIBRARY DESTINATION ${pg_pkglibdir})

else()
    message(AUTHOR_WARNING "Cannot find PostgreSQL server header files (${pg_includedir}). Extension installation disabled.")
    message(AUTHOR_WARNING "Hint: Try installing postgresql-server-dev-18.")
endif()


##################################################
#################### Testing #####################
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
    EXCLUDE_FROM_ALL
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(test_corpus_search
    test/test_search.cpp
    test/test_regex.cpp
)

find_package(LLGuidance REQUIRED)

target_link_libraries(test_corpus_search PUBLIC
    lib_corpus_search
    LLGuidance::LLGuidance
    GTest::gmock_main
    fmt::fmt
)
target_include_directories(test_corpus_search PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

include(GoogleTest)
gtest_discover_tests(test_corpus_search)
