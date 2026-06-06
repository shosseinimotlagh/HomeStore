/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <vector>

#include <sisl/logging/logging.h>
#include <iomgr/iomgr.hpp>

#include <homestore/homestore_decl.hpp>
#include <homestore/blk.hpp>

namespace spdlog {
class logger;
} // namespace spdlog

namespace sisl {
class Evictor;
}

namespace homestore {
class DeviceManager;
class ResourceMgr;
class HomeStoreStatusMgr;
class meta_blk_service;
class log_store_service;
class blk_data_service;
class IndexService;
class replication_service;
class IndexServiceCallbacks;
struct vdev_info;
class home_store;
class CPManager;
class VirtualDev;
class ChunkSelector;
class repl_dev_listener;
class repl_application;
class FaultContainmentService;
class FaultContainmentCallback;

#ifdef _PRERELEASE
class CrashSimulator;
#endif

using HomeStoreSafePtr = std::shared_ptr< home_store >;

ENUM(hs_vdev_type_t, uint32_t, DATA_VDEV = 1, INDEX_VDEV = 2, META_VDEV = 3, LOGDEV_VDEV = 4);

#pragma pack(1)
struct hs_vdev_context {
    enum hs_vdev_type_t type;

    sisl::blob to_blob() { return sisl::blob{reinterpret_cast< uint8_t* >(this), sizeof(*this)}; }
};
#pragma pack()

using hs_before_services_starting_cb_t = std::function< void(void) >;

struct hs_stats {
    uint64_t total_capacity{0ul};
    uint64_t used_capacity{0ul};
};

struct HS_SERVICE {
    static constexpr uint32_t META = 1 << 0;
    static constexpr uint32_t LOG = 1 << 1;
    static constexpr uint32_t DATA = 1 << 2;
    static constexpr uint32_t INDEX = 1 << 3;
    static constexpr uint32_t REPLICATION = 1 << 4;
    static constexpr uint32_t FAULT_CMT = 1 << 5;

    uint32_t svcs;

    HS_SERVICE() : svcs{META} {}

    std::string list() const {
        std::string str;
        if (svcs & META) { str += "meta,"; }
        if (svcs & DATA) { str += "data,"; }
        if (svcs & INDEX) { str += "index,"; }
        if (svcs & LOG) { str += "log,"; }
        if (svcs & REPLICATION) { str += "replication,"; }
        if (svcs & FAULT_CMT) { str += "fault_containment,"; }
        return str;
    }
};

/*
 * IO errors handling by homestore.
 * Write error :- Reason :- Disk error, space full,btree node read fail
 *                Handling :- Writeback cache,logdev and meta blk mgr doesn't handle any write errors.
 *                            It panics the system for write errors.
 * Read error :- Reason :- Disk error
 *               Handling :- logdev doesn't support any read error. It panic for read errors.
 * If HS see write error/read error during recovery then it panic the system.
 */

class home_store {
private:
    std::unique_ptr< blk_data_service > m_data_service;
    std::unique_ptr< meta_blk_service > m_meta_service;
    std::unique_ptr< log_store_service > m_log_service;
    std::unique_ptr< IndexService > m_index_service;
    std::shared_ptr< replication_service > m_repl_service;
    std::unique_ptr< FaultContainmentService > m_fc_service;

    std::unique_ptr< DeviceManager > m_dev_mgr;
    shared< sisl::logging::logger_t > m_periodic_logger;
    std::unique_ptr< HomeStoreStatusMgr > m_status_mgr;
    std::unique_ptr< ResourceMgr > m_resource_mgr;
    std::unique_ptr< CPManager > m_cp_mgr;
    shared< sisl::Evictor > m_evictor;

    HS_SERVICE m_services; // Services homestore is starting with
    hs_before_services_starting_cb_t m_before_services_starting_cb{nullptr};
    std::atomic< bool > m_init_done{false};

public:
    home_store() = default;
    virtual ~home_store() = default;

    /////////////////////////////////////////// static home_store member functions /////////////////////////////////
    static HomeStoreSafePtr s_instance;

    static void set_instance(const HomeStoreSafePtr& instance) { s_instance = instance; }
    static void reset_instance() { s_instance.reset(); }
    static home_store* instance();
    static shared< home_store > safe_instance() { return s_instance; }

    static shared< spdlog::logger >& periodic_logger() { return instance()->m_periodic_logger; }

    ///////////////////////////// Member functions /////////////////////////////////////////////
    home_store& with_data_service(cshared< ChunkSelector >& custom_chunk_selector = nullptr);
    home_store& with_log_service();
    home_store& with_index_service(std::unique_ptr< IndexServiceCallbacks > cbs,
                                   cshared< ChunkSelector >& custom_chunk_selector = nullptr);
    home_store& with_repl_data_service(cshared< repl_application >& repl_app,
                                       cshared< ChunkSelector >& custom_chunk_selector = nullptr);
    home_store& with_fault_containment(std::unique_ptr< FaultContainmentCallback > cb);

    bool start(const hs_input_params& input, hs_before_services_starting_cb_t svcs_starting_cb = nullptr);
    void format_and_start(std::map< uint32_t, hs_format_params >&& format_opts);
    void shutdown();

    // cap_attrs get_system_capacity() const; // Need to move this to homeblks/homeobj
    bool is_first_time_boot() const;
    bool is_initializing() const { return !m_init_done; }

    // Getters
    bool has_index_service() const;
    bool has_data_service() const;
    bool has_meta_service() const;
    bool has_log_service() const;
    bool has_repl_data_service() const;
    bool has_fc_service() const;

    blk_data_service& data_service() { return *m_data_service; }
    meta_blk_service& meta_service() { return *m_meta_service; }
    log_store_service& logstore_service() { return *m_log_service; }
    IndexService& index_service() {
        if (!m_index_service) { throw std::runtime_error("index_service is nullptr"); }
        return *m_index_service;
    }
    replication_service& repl_service() { return *m_repl_service; }
    FaultContainmentService& fc_service() {
        if (!m_fc_service) { throw std::runtime_error("fc_service is nullptr"); }
        return *m_fc_service;
    }
    DeviceManager* device_mgr() { return m_dev_mgr.get(); }
    ResourceMgr& resource_mgr() { return *m_resource_mgr.get(); }
    CPManager& cp_mgr() { return *m_cp_mgr.get(); }
    shared< sisl::Evictor > evictor() { return m_evictor; }

#ifdef _PRERELEASE
    home_store& with_crash_simulator(std::function< void(void) > restart_cb);
    CrashSimulator& crash_simulator() { return *m_crash_simulator; }
    unique< CrashSimulator > m_crash_simulator;
#endif

private:
    void init_cache();
    shared< VirtualDev > create_vdev_cb(const vdev_info& vinfo, bool load_existing);
    uint64_t pct_to_size(float pct, HSDevType dev_type) const;
    void do_start();
};

static home_store* hs() { return home_store::instance(); }
} // namespace homestore
