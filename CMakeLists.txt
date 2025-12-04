cmake_minimum_required(VERSION 3.28)

include(FetchContent)
include(GNUInstallDirs)
include(cmake/CPM.cmake)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

project(corpus_search LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED OFF)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

add_library(lib_corpus_search
    src/utils.cpp
    src/utils.hpp
    src/searcher.cpp
    src/searcher.hpp
    src/index_builder.cpp
    src/index_builder.hpp
)
set_property(TARGET lib_corpus_search PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(lib_corpus_search INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src/")

########### Link External libraries ###########
## fmt
FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt
  GIT_TAG        e69e5f977d458f2650bb346dadf2ad30c5320281  # 10.2.1
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(fmt)
set_property(TARGET fmt PROPERTY POSITION_INDEPENDENT_CODE ON)

target_link_libraries(lib_corpus_search PRIVATE fmt::fmt)

## nlohmann json
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(json)
set_property(TARGET nlohmann_json PROPERTY POSITION_INDEPENDENT_CODE ON)

target_link_libraries(lib_corpus_search PRIVATE nlohmann_json::nlohmann_json)

## re2 (version 2023-11-01)
FetchContent_Declare(
  re2
  GIT_REPOSITORY https://github.com/google/re2
  GIT_TAG        "2023-11-01"
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(re2)
set_property(TARGET re2 PROPERTY POSITION_INDEPENDENT_CODE ON)

target_link_libraries(lib_corpus_search PRIVATE re2::re2)

## tokenizers-cpp
FetchContent_Declare(
  tokenizers_cpp
  GIT_REPOSITORY https://github.com/mlc-ai/tokenizers-cpp
  GIT_TAG        55d53aa38dc8df7d9c8bd9ed50907e82ae83ce66
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(tokenizers_cpp)
set_property(TARGET tokenizers_cpp PROPERTY POSITION_INDEPENDENT_CODE ON)

target_link_libraries(lib_corpus_search PRIVATE tokenizers_cpp)

## utf8-cpp header-only library
CPMAddPackage(
  NAME utfcpp
  GITHUB_REPOSITORY "nemtrif/utfcpp"
  GIT_TAG 70795627871251f6c57f952e667303a2381c2fc5
)
add_library(utfcpp INTERFACE)
target_include_directories(utfcpp INTERFACE "${utfcpp_SOURCE_DIR}/source")

target_link_libraries(lib_corpus_search PRIVATE utfcpp)

## msgpack-cxx
CPMAddPackage(
  NAME msgpack-cxx
  GITHUB_REPOSITORY "msgpack/msgpack-c"
  GIT_TAG b0d379a6f29af7dd65423ff78a94de8948f01bd8
)

target_link_libraries(lib_corpus_search PUBLIC msgpack-cxx)

## llguidance
find_package(LLGuidance REQUIRED)

target_link_libraries(lib_corpus_search PRIVATE LLGuidance::LLGuidance)

## boost dynamic_bitset
find_package(Boost REQUIRED)

target_include_directories(lib_corpus_search PRIVATE ${Boost_INCLUDE_DIRS})

## roaring bitmap
# CPMAddPackage(
#   NAME roaring
#   GITHUB_REPOSITORY "RoaringBitmap/CRoaring"
#   GIT_TAG v2.0.4
#   OPTIONS "ENABLE_ROARING_TESTS OFF"
# )
# target_link_libraries(lib_corpus_search PRIVATE roaring::roaring)
##################################################

# Installation
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

# Testing
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
  EXCLUDE_FROM_ALL
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(test_corpus_search test/test.cpp)
target_link_libraries(test_corpus_search PUBLIC
    lib_corpus_search
    GTest::gmock_main
    fmt::fmt
)
target_include_directories(test_corpus_search PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

include(GoogleTest)
gtest_discover_tests(test_corpus_search)
