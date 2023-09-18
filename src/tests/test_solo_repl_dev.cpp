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
#include <vector>
#include <iostream>
#include <filesystem>

#include <boost/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>
#include <gtest/gtest.h>
#include <iomgr/io_environment.hpp>
#include <sisl/logging/logging.h>
#include <sisl/options/options.h>
#include <sisl/fds/buffer.hpp>
#include <gtest/gtest.h>

#include <homestore/blk.h>
#include <homestore/homestore.hpp>
#include <homestore/homestore_decl.hpp>
#include <homestore/replication_service.hpp>
#include <homestore/replication/repl_dev.h>
#include "common/homestore_config.hpp"
#include "common/homestore_assert.hpp"
#include "common/homestore_utils.hpp"
#include "test_common/homestore_test_common.hpp"
#include "replication/service/repl_service_impl.h"
#include "replication/repl_dev/solo_repl_dev.h"

////////////////////////////////////////////////////////////////////////////
//                                                                        //
//     This test is to test solo repl device                              //
//                                                                        //
////////////////////////////////////////////////////////////////////////////

using namespace homestore;
using namespace test_common;

SISL_LOGGING_INIT(HOMESTORE_LOG_MODS)
SISL_OPTIONS_ENABLE(logging, test_solo_repl_dev, iomgr, test_common_setup)
SISL_LOGGING_DECL(test_solo_repl_dev)

std::vector< std::string > test_common::HSTestHelper::s_dev_names;
static thread_local std::random_device g_rd{};
static thread_local std::default_random_engine g_re{g_rd()};
static uint32_t g_block_size;

static constexpr uint64_t Ki{1024};
static constexpr uint64_t Mi{Ki * Ki};
static constexpr uint64_t Gi{Ki * Mi};

struct Runner {
    uint64_t total_tasks{0};
    uint32_t qdepth{8};
    std::atomic< uint64_t > issued_tasks{0};
    std::atomic< uint64_t > pending_tasks{0};
    std::function< void(void) > task;
    folly::Promise< folly::Unit > comp_promise;

    Runner() : total_tasks{SISL_OPTIONS["num_io"].as< uint64_t >()} {
        if (total_tasks < (uint64_t)qdepth) { total_tasks = qdepth; }
    }

    void set_task(std::function< void(void) > f) { task = std::move(f); }

    folly::Future< folly::Unit > execute() {
        for (uint32_t i{0}; i < qdepth; ++i) {
            run_task();
        }
        return comp_promise.getFuture();
    }

    void next_task() {
        auto ptasks = pending_tasks.fetch_sub(1) - 1;
        if ((issued_tasks.load() < total_tasks)) {
            run_task();
        } else if (ptasks == 0) {
            comp_promise.setValue();
        }
    }

    void run_task() {
        ++issued_tasks;
        ++pending_tasks;
        iomanager.run_on_forget(iomgr::reactor_regex::random_worker, task);
    }
};

struct rdev_req : boost::intrusive_ref_counter< rdev_req > {
    sisl::byte_array header;
    sisl::byte_array key;
    sisl::sg_list write_sgs;
    sisl::sg_list read_sgs;
    int64_t lsn;
    MultiBlkId written_blkids;

    rdev_req() {
        write_sgs.size = 0;
        read_sgs.size = 0;
    }
    ~rdev_req() {
        for (auto const& iov : write_sgs.iovs) {
            iomanager.iobuf_free(uintptr_cast(iov.iov_base));
        }

        for (auto const& iov : read_sgs.iovs) {
            iomanager.iobuf_free(uintptr_cast(iov.iov_base));
        }
    }
    struct journal_header {
        uint32_t key_size;
        uint64_t key_pattern;
        uint64_t data_size;
        uint64_t data_pattern;
    };
};

class SoloReplDevTest : public testing::Test {
public:
    class Listener : public ReplDevListener {
    private:
        SoloReplDevTest& m_test;
        ReplDev& m_rdev;

    public:
        Listener(SoloReplDevTest& test, ReplDev& rdev) : m_test{test}, m_rdev{rdev} {}
        virtual ~Listener() = default;

        void on_commit(int64_t lsn, sisl::blob const& header, sisl::blob const& key, MultiBlkId const& blkids,
                       void* ctx) override {
            if (ctx == nullptr) {
                m_test.validate_replay(m_rdev, lsn, header, key, blkids);
            } else {
                rdev_req* req = r_cast< rdev_req* >(ctx);
                req->lsn = lsn;
                req->written_blkids = std::move(blkids);
                m_test.on_write_complete(m_rdev, intrusive< rdev_req >(req, false));
            }
        }

        void on_pre_commit(int64_t lsn, const sisl::blob& header, const sisl::blob& key, void* ctx) override {}

        void on_rollback(int64_t lsn, const sisl::blob& header, const sisl::blob& key, void* ctx) override {}

        blk_alloc_hints get_blk_alloc_hints(sisl::blob const& header, void* user_ctx) override {
            return blk_alloc_hints{};
        }

        void on_replica_stop() override {}
    };

    class Callbacks : public ReplServiceCallbacks {
    private:
        SoloReplDevTest& m_test;

    public:
        Callbacks(SoloReplDevTest* test) : m_test{*test} {}
        virtual ~Callbacks() = default;

        std::unique_ptr< ReplDevListener > on_repl_dev_init(cshared< ReplDev >& rdev) override {
            m_test.found_repl_dev(rdev);
            return std::make_unique< Listener >(m_test, *rdev);
        }
    };

protected:
    Runner m_runner;
    shared< ReplDev > m_repl_dev1;
    shared< ReplDev > m_repl_dev2;

public:
    virtual void SetUp() override {
        test_common::HSTestHelper::start_homestore(
            "test_solo_repl_dev",
            {{HS_SERVICE::META, {.size_pct = 5.0}},
             {HS_SERVICE::REPLICATION,
              {.size_pct = 60.0, .repl_svc_cbs = new Callbacks(this), .repl_impl = repl_impl_type::solo}},
             {HS_SERVICE::LOG_REPLICATED, {.size_pct = 20.0}},
             {HS_SERVICE::LOG_LOCAL, {.size_pct = 2.0}}});
        hs()->repl_service().create_replica_dev(hs_utils::gen_random_uuid(), {});
        hs()->repl_service().create_replica_dev(hs_utils::gen_random_uuid(), {});
    }

    virtual void TearDown() override {
        m_repl_dev1.reset();
        m_repl_dev2.reset();
        test_common::HSTestHelper::shutdown_homestore();
    }

    void restart() {
        m_repl_dev1.reset();
        m_repl_dev2.reset();
        test_common::HSTestHelper::start_homestore(
            "test_solo_repl_dev",
            {{HS_SERVICE::REPLICATION, {.repl_svc_cbs = new Callbacks(this), .repl_impl = repl_impl_type::solo}},
             {HS_SERVICE::LOG_REPLICATED, {}},
             {HS_SERVICE::LOG_LOCAL, {}}},
            nullptr, true /* restart */);
    }

    void found_repl_dev(cshared< ReplDev >& rdev) {
        if (m_repl_dev1 == nullptr) {
            m_repl_dev1 = rdev;
        } else {
            HS_REL_ASSERT_EQ((void*)m_repl_dev2.get(), (void*)nullptr, "More than one replica dev reported");
            m_repl_dev2 = rdev;
        }
    }

    void write_io(uint32_t key_size, uint64_t data_size, uint32_t max_size_per_iov) {
        auto req = intrusive< rdev_req >(new rdev_req());
        req->header = sisl::make_byte_array(sizeof(rdev_req::journal_header));
        auto hdr = r_cast< rdev_req::journal_header* >(req->header->bytes);
        hdr->key_size = key_size;
        hdr->key_pattern = ((long long)rand() << 32) | rand();
        hdr->data_size = data_size;
        hdr->data_pattern = ((long long)rand() << 32) | rand();

        if (key_size != 0) {
            req->key = sisl::make_byte_array(key_size);
            HSTestHelper::fill_data_buf(req->key->bytes, key_size, hdr->key_pattern);
        }

        if (data_size != 0) {
            req->write_sgs = HSTestHelper::create_sgs(data_size, g_block_size, max_size_per_iov, hdr->data_pattern);
        }

        auto& rdev = (rand() % 2) ? m_repl_dev1 : m_repl_dev2;
        intrusive_ptr_add_ref(req.get());
        rdev->async_alloc_write(*req->header, req->key ? *req->key : sisl::blob{}, req->write_sgs, (void*)req.get());
    }

    void validate_replay(ReplDev& rdev, int64_t lsn, sisl::blob const& header, sisl::blob const& key,
                         MultiBlkId const& blkids) {
        auto jhdr = r_cast< rdev_req::journal_header* >(header.bytes);
        HSTestHelper::validate_data_buf(key.bytes, key.size, jhdr->key_pattern);

        uint32_t size = blkids.blk_count() * g_block_size;
        if (size) {
            auto read_sgs = HSTestHelper::create_sgs(size, g_block_size, size);
            rdev.async_read(blkids, read_sgs, size).thenValue([jhdr, read_sgs](auto&& err) {
                RELEASE_ASSERT(!err, "Error during async_read");
                HS_REL_ASSERT_EQ(jhdr->data_size, read_sgs.size, "journal hdr data size mismatch with actual size");

                for (auto const& iov : read_sgs.iovs) {
                    HSTestHelper::validate_data_buf(uintptr_cast(iov.iov_base), iov.iov_len, jhdr->data_pattern);
                }
            });
        }
    }

    void on_write_complete(ReplDev& rdev, intrusive< rdev_req > req) {
        // If we did send some data to the repl_dev, validate it by doing async_read
        if (req->write_sgs.size != 0) {
            req->read_sgs = HSTestHelper::create_sgs(req->write_sgs.size, g_block_size, req->write_sgs.size);

            rdev.async_read(req->written_blkids, req->read_sgs, req->read_sgs.size)
                .thenValue([this, &rdev, req](auto&& err) {
                    RELEASE_ASSERT(!err, "Error during async_read");

                    LOGDEBUG("Write complete with lsn={} for size={}", req->lsn, req->write_sgs.size);
                    auto hdr = r_cast< rdev_req::journal_header* >(req->header->bytes);
                    HS_REL_ASSERT_EQ(hdr->data_size, req->read_sgs.size,
                                     "journal hdr data size mismatch with actual size");

                    for (auto const& iov : req->read_sgs.iovs) {
                        HSTestHelper::validate_data_buf(uintptr_cast(iov.iov_base), iov.iov_len, hdr->data_pattern);
                    }
                    m_runner.next_task();
                });
        } else {
            m_runner.next_task();
        }
    }
};

TEST_F(SoloReplDevTest, TestSingleDataBlock) {
    LOGINFO("Step 1: run on worker threads to schedule write for {} Bytes.", g_block_size);
    this->m_runner.set_task([this]() { this->write_io(0u, g_block_size, g_block_size); });
    this->m_runner.execute().get();

    LOGINFO("Step 2: Restart homestore and validate replay data.", g_block_size);
    restart();
}

TEST_F(SoloReplDevTest, TestRandomSizedDataBlock) {
    LOGINFO("Step 1: run on worker threads to schedule write for random bytes ranging {}-{}.", 0, 1 * Mi);
    this->m_runner.set_task([this]() {
        uint32_t nblks = rand() % ((1 * Mi) / g_block_size);
        uint32_t key_size = rand() % 512 + 8;
        this->write_io(key_size, nblks * g_block_size, g_block_size);
    });
    this->m_runner.execute().get();
    restart();
}

TEST_F(SoloReplDevTest, TestHeaderOnly) {
    LOGINFO("Step 1: run on worker threads to schedule write");
    this->m_runner.set_task([this]() { this->write_io(0u, 0u, g_block_size); });
    this->m_runner.execute().get();
    restart();
}

SISL_OPTION_GROUP(test_solo_repl_dev,
                  (num_io, "", "num_io", "number of io", ::cxxopts::value< uint64_t >()->default_value("300"),
                   "number"),
                  (block_size, "", "block_size", "block size to io",
                   ::cxxopts::value< uint32_t >()->default_value("4096"), "number"));

int main(int argc, char* argv[]) {
    int parsed_argc{argc};
    ::testing::InitGoogleTest(&parsed_argc, argv);
    SISL_OPTIONS_LOAD(parsed_argc, argv, logging, test_solo_repl_dev, iomgr, test_common_setup);
    sisl::logging::SetLogger("test_solo_repl_dev");
    spdlog::set_pattern("[%D %T%z] [%^%l%$] [%n] [%t] %v");

    g_block_size = SISL_OPTIONS["block_size"].as< uint32_t >();
    return RUN_ALL_TESTS();
}