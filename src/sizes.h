#ifndef SIZES_H
#define SIZES_H

#ifdef __cplusplus
#include <cstdint>
#define namespace_std std::
#else
#include <stdint.h>
#define namespace_std
#endif

#ifndef CORPUS_SEARCH_INDEX_ENTRY_BITS
#define CORPUS_SEARCH_INDEX_ENTRY_BITS 32
#endif
#ifndef CORPUS_SEARCH_POSITION_BITS
#define CORPUS_SEARCH_POSITION_BITS 11
#endif
#ifndef CORPUS_SEARCH_NEXT_TOKEN_BITS
#define CORPUS_SEARCH_NEXT_TOKEN_BITS 0
#endif
#define CORPUS_SEARCH_SENTID_BITS \
    (CORPUS_SEARCH_INDEX_ENTRY_BITS - CORPUS_SEARCH_POSITION_BITS - CORPUS_SEARCH_NEXT_TOKEN_BITS)

#if CORPUS_SEARCH_POSITION_BITS <= 32
typedef namespace_std uint32_t tokpos_t;
#else
typedef namespace_std uint64_t tokpos_t;
#endif

#if CORPUS_SEARCH_SENTID_BITS <= 32
typedef namespace_std uint32_t sentid_t;
#else
typedef namespace_std uint64_t sentid_t;
#endif
#define CORPUS_SEARCH_MAX_SENTID ((UINT64_C(1) << CORPUS_SEARCH_SENTID_BITS) - 1)
#define CORPUS_SEARCH_MAX_POS ((1 << CORPUS_SEARCH_POSITION_BITS) - 1)

#if CORPUS_SEARCH_INDEX_ENTRY_BITS <= 32
typedef namespace_std uint32_t index_entry_hash_t;
#else
typedef namespace_std uint64_t index_entry_hash_t;
#endif

#undef namespace_std

#endif // SIZES_H
