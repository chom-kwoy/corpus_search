# AGENTS.md

## Project overview

`corpus_search` is a C++20 library and a PostgreSQL 18 extension for accelerating regular-expression searches over a text corpus. It tokenizes text with a Hugging Face BPE tokenizer, builds an inverted index of token occurrences, converts supported regular expressions into DFAs, and walks a token trie to obtain candidate sentence or heap tuple IDs. PostgreSQL exposes the implementation as the `ibpe` index access method for the `text ~ text` operator.

The index is a candidate generator. Some regex constructs, including assertions, are deliberately over-approximated; `search_result::needs_recheck` must remain correct so PostgreSQL can filter false positives. False negatives are never acceptable.

## Repository map

- `src/tokenizer.*`: tokenizer JSON loading, normalization mappings, and byte-level BPE conversion.
- `src/regex_parse.*`: PEGTL grammar and concrete syntax tree (CST).
- `src/regex_ast.*`: CST-to-AST conversion, Unicode/ICU character properties, and UTF-8 range expansion.
- `src/regex_dfa.*`: AST-to-DFA conversion and DFA matching.
- `src/dfa_trie.*`: intersection of DFA states with the tokenizer vocabulary trie.
- `src/index_builder.*`: in-memory token posting-list construction and MessagePack corpus loading.
- `src/searcher.*`: candidate generation from the DFA, trie, and sorted posting lists.
- `src/sizes.h`: shared C/C++ integer types and configurable bit allocation for stored index entries.
- `src/extension/`: PostgreSQL access-method implementation. The `.c` files own PostgreSQL lifecycle, page, scan, insert, relcache, and vacuum behavior; `ibpe_backend.cpp` is the C-to-C++ bridge.
- `src/extension/corpussearch--1.0.sql` and `.control`: extension registration and SQL-visible access method/operator class.
- `test/test_regex.cpp`: parser, AST, DFA, Unicode, and tokenizer-trie tests.
- `test/test_search.cpp`: corpus-dependent end-to-end candidate-count tests.
- `test/test_insert.sql` and `test/test_vacuum.sql`: PostgreSQL integration tests, with matching cleanup scripts.
- `CMakeLists.txt`: the authoritative build graph, dependencies, compile definitions, installation, and test registration.

## Build setup

Use an out-of-tree CMake build. CMake downloads several pinned dependencies, and LLGuidance is built with Cargo, so initial configuration/build requires network access and a Rust toolchain.

Required system packages on Ubuntu are at least `libabsl-dev`, `libicu-dev`, and `libboost-dev`. Building the PostgreSQL extension additionally requires `pg_config` and matching server headers; CI targets PostgreSQL 18. If those headers are unavailable, CMake still builds the core library but disables the extension target.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

For sanitizer work, configure a separate Debug directory; the project enables AddressSanitizer and UndefinedBehaviorSanitizer for Debug builds.

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --parallel
```

Installation copies `ibpe` and the extension metadata into directories reported by `pg_config` and will commonly require elevated privileges:

```bash
cmake --install build
```

Do not use `build_install.sh` merely to validate a change: it performs a privileged system-wide install.

## Testing

The C++ tests are discovered by CTest. The complete suite needs two external fixtures that are intentionally not stored in this repository:

- `IBPE_TEST_TOKENIZER_FILE`: a compatible Hugging Face `tokenizer.json`.
- `IBPE_TEST_CORPUS_FILE`: the corresponding `tokenized_sentences.msgpack` corpus.

```bash
IBPE_TEST_TOKENIZER_FILE=/path/to/tokenizer.json \
IBPE_TEST_CORPUS_FILE=/path/to/tokenized_sentences.msgpack \
ctest --test-dir build --output-on-failure
```

Most parser/AST/DFA tests do not need fixtures. After building, run that fast subset with:

```bash
./build/test_corpus_search \
  --gtest_filter='Regex.*-Regex.RegexTrie:Regex.RegexTrieParity'
```

Use a different executable path if a separate build directory was configured. Search tests assert counts for one specific corpus and will fail with unrelated fixture data. `IBPE_TEST_WRITE_SEARCH_RESULTS=1` writes result IDs to `test_search_<test-name>.txt`; leave it unset unless those artifacts are explicitly needed.

PostgreSQL integration tests require the extension to be installed, a running database, and a tokenizer at the path declared by each SQL script (currently `/var/lib/postgresql/tokenizer.json`):

```bash
psql -v ON_ERROR_STOP=1 -f test/test_insert.sql
psql -v ON_ERROR_STOP=1 -f test/cleanup_insert.sql
psql -v ON_ERROR_STOP=1 -f test/test_vacuum.sql
psql -v ON_ERROR_STOP=1 -f test/cleanup_vacuum.sql
```

Run cleanup after failures as well. The SQL tests compare forced index scans with sequential scans; preserve that comparison when adding cases. CI currently configures and builds only—the CTest step is commented out—so local test evidence matters.

## Architecture and correctness constraints

- Keep the flow `regex text -> CST -> AST -> DFA -> tokenizer trie -> posting lists` explicit. Add parser tests and DFA match cases when changing regex syntax or semantics.
- Regex matching is over UTF-8 bytes after AST range expansion. Do not silently switch individual stages to code-point semantics.
- Assertions may be represented as epsilon-like over-approximations. Any change that can admit false positives must propagate `needs_recheck = true` through recursive candidate generation and the PostgreSQL bitmap scan.
- Posting lists consumed by `searcher.cpp` must be sorted by `(sent_id, pos)`. `index_builder::finalize_index`, pending/main-list merging, and adjacency operations depend on this ordering.
- The compile definitions `CORPUS_SEARCH_INDEX_ENTRY_BITS`, `CORPUS_SEARCH_POSITION_BITS`, and `CORPUS_SEARCH_NEXT_TOKEN_BITS` determine persistent layout and capacity. Treat changes as storage-format changes, not ordinary refactors.
- `corpus_search::index_entry` and the C-facing `backend::index_entry` must remain layout-compatible. Preserve the `static_assert`s, bitfield order, conditional `next_tok`, and C/C++ types in `sizes.h`.
- `ibpe_backend.h` is the language boundary. Do not let C++ exceptions cross it; keep ownership pairs (`create`/`destroy`) and null/error behavior consistent.
- PostgreSQL block 0 is the metapage. Data-page flags, opaque data, linked block numbers, `data_len`, magic values, and pending counts are on-disk invariants shared by build, scan, relcache, and vacuum code.
- PostgreSQL writes to index pages must use the appropriate buffer locks and WAL/generic WAL APIs. Release every buffer and access strategy on success and error paths. Allocate backend-lifetime memory with PostgreSQL memory APIs where required.
- Heap TIDs are encoded as `sentid_t`; zero is treated as invalid/dead in scan and vacuum paths. Respect `CORPUS_SEARCH_MAX_SENTID` and position limits, and test insert/vacuum behavior after changing the encoding.
- The index currently supports one regex scan key on the first indexed text column. Update the access-method callbacks, SQL operator class, validation, and integration tests together if this contract changes.
- Token IDs 0 and 1 are currently hard-coded as BOS and EOS. Changes require a tokenizer compatibility review and targeted trie/search tests.

## Code style

- C++ is C++20; C extension sources use C11 with GNU extensions.
- Follow the existing four-space style and the root `.clang-format` (100-column limit). Format only touched C/C++ files, for example: `clang-format -i src/file.cpp src/file.hpp`.
- Preserve nearby conventions: snake_case identifiers, `corpus_search` namespaces, trailing return types for most C++ functions, header guards, and `const`/reference placement used in the edited file.
- Keep C-compatible headers valid in both languages. PostgreSQL headers have order-sensitive includes; retain the warning and ordering in `ibpe_utils.h`.
- Avoid broad formatting churn, especially in generated-looking byte/Unicode mapping tables.
- Prefer focused comments that explain storage, ownership, approximation, or PostgreSQL lifecycle constraints rather than restating code.

## Change workflow

1. Inspect both sides of the affected boundary before editing: parser/AST/DFA, trie/searcher, or PostgreSQL C/C++ bridge and page readers/writers.
2. Add or update the smallest relevant unit or SQL integration test. For bug fixes, include a regression case that fails without the fix.
3. Build the affected targets and run the narrowest relevant tests first, then the complete fixture-backed or PostgreSQL suite when available.
4. Report tests that were not run and name the missing fixture, service, or privilege; do not claim the commented-out CI test step as coverage.
5. Keep unrelated local changes intact and do not commit build outputs, fetched dependencies, search-result text files, or database artifacts.
