#pragma once
#include <functional>
#include <memory>
#include <string>
#include <variant>

#include <folly/futures/Future.h>

#include "repl_decls.h"
#include "repl_set.h"

namespace nuraft {
class state_machine;
}

namespace homestore {

class ReplDev;
using ReplServiceError = nuraft::cmd_result_code;
using on_replica_dev_init_t = std::function< std::unique_ptr< ReplicaDevListener >(cshared< ReplDev >& rd) >;

class ReplicationService {
public:
    ReplicationService(on_replica_dev_init init_cb);
    virtual ~ReplicationService() = default;

    // using set_var = std::variant< shared< ReplDev >, ReplServiceError >;

    /// Sync APIs
    virtual shared< ReplDev > get_replica_dev(std::string const& group_id) const = 0;
    virtual void iterate_replica_devs(std::function< void(cshared< ReplDev >&) > cb) const = 0;

    /// Async APIs
    // virtual folly::SemiFuture< set_var > create_replica_dev(std::string const& group_id,
    //                                                         std::set< std::string, std::less<> >&& members) = 0;

    /////// OR
    virtual folly::SemiFuture< shared< ReplDev > >
    create_replica_dev(std::string const& group_id, std::set< std::string, std::less<> >&& members) = 0;

    virtual folly::SemiFuture< ReplServiceError >
    replace_member(std::string const& group_id, std::string const& member_out, std::string const& member_in) const = 0;
};
} // namespace homestore
