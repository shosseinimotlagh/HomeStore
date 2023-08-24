#pragma once
#include <iostream>
#include <string>

#include <folly/small_vector.h>
#include <sisl/logging/logging.h>
#include <iomgr/iomgr_types.hpp>
#include <homestore/homestore_decl.hpp>
#include <homestore/blk.h>
#include <sisl/fds/buffer.hpp>

SISL_LOGGING_DECL(replication)

#define REPL_LOG_MODS grpc_server, HOMESTORE_LOG_MODS, nuraft_mesg, nuraft, replication

namespace homestore {
using blkid_list_t = folly::small_vector< BlkId, 4 >;

// Fully qualified domain pba, unique pba id across replica set
struct fully_qualified_blkid {
    fully_qualified_blkid(uint32_t s, const BlkId& p) : server_id{s}, pba{p} {}
    uint32_t server_id;
    BlkId blkid;
    std::string to_key_string() const { return fmt::format("{}_{}", std::to_string(server_id), blkid.to_string()); }
};
using fq_blkid_list_t = folly::small_vector< fully_qualified_blkid, 4 >;

// data service api names
static std::string const SEND_DATA{"send_data"};
static std::string const FETCH_DATA{"fetch_data"};

} // namespace homestore
