
#ifndef _HOMESTORE_CONFIG_HPP_
#define _HOMESTORE_CONFIG_HPP_

#include "homestore_header.hpp"
#include <error/error.h>
#include <cassert>

namespace homestore {

struct HomeStoreConfig {
    static size_t phys_page_size; // physical block size supported by ssd
    static size_t hs_page_size; // minimum page size supported by homestore system
    static size_t atomic_phys_page_size; // atomix page size supported by disk
    static size_t align_size;
    static uint64_t max_chunks;
    static uint64_t max_vdevs;
    static uint64_t max_pdevs;
};

constexpr uint32_t ID_BITS = 32;
constexpr uint32_t NBLKS_BITS = 8;
constexpr uint32_t CHUNK_NUM_BITS = 8;
constexpr uint32_t BLKID_SIZE_BITS = ID_BITS + NBLKS_BITS + CHUNK_NUM_BITS;
constexpr uint32_t MEMPIECE_ENCODE_MAX_BITS = 8;
constexpr uint64_t MAX_NBLKS = ((1 << NBLKS_BITS) - 1);
constexpr uint64_t MAX_CHUNK_ID = ((1 << CHUNK_NUM_BITS) - 1);
constexpr uint64_t BLKID_SIZE = ((ID_BITS + NBLKS_BITS + CHUNK_NUM_BITS) / 8);
constexpr uint32_t BLKS_PER_PORTION = 1024;
constexpr uint32_t TOTAL_SEGMENTS = 8;

/* DM info size depends on these three parameters. If below parameter changes then we have to add the code for upgrade/revert. */
constexpr uint32_t MAX_CHUNKS = 128;
constexpr uint32_t MAX_VDEVS = 8;
constexpr uint32_t MAX_PDEVS = 8;

#define MAX_CHUNK_SIZE (((1lu << ID_BITS) - 1) * (HomeStoreConfig::hs_page_size))
#define MEMVEC_MAX_IO_SIZE (HomeStoreConfig::hs_page_size * ((1 << MEMPIECE_ENCODE_MAX_BITS) - 1))
#define MIN_CHUNK_SIZE (HomeStoreConfig::phys_page_size * BLKS_PER_PORTION * TOTAL_SEGMENTS)

/* NOTE: it can give size more then the size passed in argument to make it aligned */
#define ALIGN_SIZE(size, align) (((size % align) == 0) ? size : (size + (align - (size % align))))

}
#endif