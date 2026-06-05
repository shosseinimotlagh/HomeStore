/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
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

#include <system_error>
#include <utility>

#include <iomgr/iomgr.hpp> // iomanager (am_i_io_reactor)
#include <iomgr/drive.hpp> // iomgr::async_* return iomgr::io_op
#include <iomgr/io_op.hpp> // iomgr::io_op, io_result, sync_wait

#include <sisl/async/task.hpp> // sisl::async::task

namespace homestore::detail {

// iomgr::io_result is std::expected<size_t, std::error_condition>: truthy on SUCCESS (value = bytes
// transferred). Reduce it to a std::error_code (truthy on FAILURE) for the cold-path sync I/O below. The
// condition iomgr produces is in the generic category, so an error_code built from (value, category) compares
// correctly against std::errc.
inline std::error_code to_error_code(const iomgr::io_result& r) {
    if (r) { return {}; }
    auto const& cond = r.error();
    return std::error_code(cond.value(), cond.category());
}

// io_op -> task< io_result >. co_await marshals the op onto a reactor (iomgr owns the hop), so the device-layer
// coroutine bodies can await drive I/O from any thread. The io_result flows through unchanged; trivial 1:1
// pass-through device methods use this directly, the ones that also record metrics co_await the io_op inline.
inline sisl::async::task< iomgr::io_result > to_task(iomgr::io_op op) { co_return co_await std::move(op); }

// Blocking bridge for the cold-path synchronous device I/O (superblock / first-block reads & writes).
// Delegates to iomgr::sync_wait, which runs the op on iomgr's dedicated sync reactor and blocks the caller
// until completion -- so it is safe to call from ANY thread, reactor or not. (v13 has no fibers, so a reactor
// can't yield mid-op; iomgr owns the off-caller hop, replacing v12's fiber-based DriveInterface::sync_write.)
inline std::error_code sync_wait(iomgr::io_op op) { return to_error_code(iomgr::sync_wait(std::move(op))); }

} // namespace homestore::detail
