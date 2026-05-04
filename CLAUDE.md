# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

`corpus_search` is a C++20 library and PostgreSQL extension for efficient regex search over large text corpora. It uses a BPE (Byte Pair Encoding) tokenizer-based inverted index — the **IBPE** access method — to accelerate PostgreSQL `~` (regex match) queries on `TEXT` columns.

## Build Commands

### Prerequisites
```bash
apt install libabsl-dev libicu-dev libboost-dev
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

### Build
```bash
cmake -DCMAKE_BUILD_TYPE=Release -S . -B build
cmake --build build --parallel
```

Debug builds enable ASAN + UBSAN sanitizers automatically.

### Install PostgreSQL extension
```bash
cmake --install build
```

Or use the helper script:
```bash
./build_install.sh
```

### Run tests
```bash
./build/test_corpus_search
# Run a single test
./build/test_corpus_search --gtest_filter='TestName'
```

Tests depend on external files hardcoded in `test/test.hpp` (tokenizer JSON and msgpack corpus). They are currently disabled in CI.

## Architecture

### Data flow for a regex query

```
Regex string
  → [regex_parse.cpp]  CST via PEGTL PEG parser
  → [regex_ast.cpp]    AST (CST lowering)
  → [regex_dfa.cpp]    DFA (subset construction)
  → [dfa_trie.cpp]     trie walk to enumerate matching token sequences
  → [searcher.cpp]     merge index postings → candidate sentence IDs
```

### Core library (`src/`)

| File | Role |
|---|---|
| `sizes.h` | Compile-time bit layout: `CORPUS_SEARCH_INDEX_ENTRY_BITS=64`, `CORPUS_SEARCH_POSITION_BITS=12` define `sentid_t` (52 bits) and `tokpos_t` (12 bits) packed into one `uint64_t` index entry |
| `tokenizer.hpp/.cpp` | Wraps HuggingFace tokenizers-cpp; loads `tokenizer.json`; builds a trie of all vocab tokens; applies `normalize_mappings` (character substitution table) |
| `index_builder.hpp/.cpp` | Builds the inverted index: `token_id → vector<index_entry>`; input is a msgpack-serialized tokenized corpus |
| `searcher.hpp/.cpp` | Executes search: walks DFA × trie to get candidate token sequences, looks up postings, intersects with Roaring bitmaps, returns `{candidates, needs_recheck}` |
| `dfa_trie.hpp/.cpp` | For each DFA state, enumerates which token IDs are accepted by trie traversal |
| `regex_parse/ast/dfa` | Full regex pipeline: PEGTL parser → variant-based AST → DFA via Thompson/subset construction; supports Unicode properties |

### PostgreSQL extension (`src/extension/`)

- Access method name: `ibpe`; operator class: `text_ops` (TEXT ~)
- `ibpe_backend.h/.cpp` — C++ wrapper exposing a C-callable API; bridges PostgreSQL AM callbacks to the core library; converts C++ exceptions to NULL returns
- `ibpe_build.c` — `ibpe_build()` / `ibpe_insert()`: index construction
- `ibpe_scan.c` — `ibpe_beginscan()` / `ibpe_getbitmap()`: query execution (bitmap scan)
- `ibpe_relcache.c` — Holds the in-memory index during a query session
- `ibpe_vacuum.c` — VACUUM support
- `corpussearch--1.0.sql` / `corpussearch.control` — SQL definitions and extension manifest

### Key compile-time constants

Defined in `CMakeLists.txt`, consumed via `sizes.h`:
- `CORPUS_SEARCH_INDEX_ENTRY_BITS=64` — total bits per index entry
- `CORPUS_SEARCH_POSITION_BITS=12` — bits for within-sentence token position (max 4096 tokens/sentence)
- Remaining 52 bits → sentence ID (max ~4.5 × 10¹⁵ sentences)

### External dependencies (fetched by CMake)

`fmt`, `nlohmann/json`, `tokenizers-cpp` (mlc-ai fork), `utf8-cpp`, `msgpack-cxx`, `PEGTL`, `Boost` (dynamic_bitset), `roaring`, `re2` (tests only), `LLGuidance` (tests only), `GoogleTest`.

## PostgreSQL usage example

```sql
CREATE EXTENSION corpussearch;
CREATE INDEX my_ibpe_index ON sentences USING ibpe (text) WITH (
    tokenizer_path = '/var/lib/postgresql/tokenizer1.json',
    normalize_mappings = '{".": "x", "/": "Z", "\\": "X", "`": "C"}'
);

SET enable_seqscan = off;
EXPLAIN ANALYZE SELECT text FROM sentences WHERE text ~ 'si\.ta\.so\.ngi\.ta';
```
