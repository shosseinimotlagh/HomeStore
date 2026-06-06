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
 *********************************************************************************/
#pragma once

#include <expected>
#include <string>
#include <system_error>

#include <fmt/format.h>
#include <sisl/async/task.hpp>

// homestore's ONE error surface.
//
// Every operation on the public API reports operational failure the same way: it returns a value or a
// std::error_condition. Synchronous calls return `homestore::result<T>`; asynchronous calls return
// `homestore::async_result<T>` (a coroutine you co_await). This matches iomgr's data-plane convention
// (`iomgr::io_result == std::expected<size_t, std::error_condition>`), so a single helper can unwrap any
// homestore or iomgr result.
//
// The specific reason is carried in the std::error_condition. homestore's domain enums (ReplServiceError,
// BlkAllocStatus, ...) are registered as std::error_condition enums next to their definitions, so a caller
// can branch on them directly:
//
//     auto r = co_await repl_dev->create(...);
//     if (!r) {
//         if (r.error() == ReplServiceError::NOT_LEADER) { /* redirect to leader */ }
//         else { LOGERROR("create failed: {}", r.error().message()); }
//     }
//
// Reserve exceptions for precondition violations that signal a bug in the caller, never for operational
// failures that are part of the contract.

namespace homestore {

// Operational result of a synchronous call: a value, or a std::error_condition describing the failure.
template < class T >
using result = std::expected< T, std::error_condition >;

// Operational result of an asynchronous call: a coroutine that resolves to a `result<T>`.
template < class T >
using async_result = sisl::async::task< result< T > >;

// A result that carries no value on success (just success/failure).
using status = result< std::monostate >;
using async_status = async_result< std::monostate >;

inline status ok() noexcept { return status{std::monostate{}}; }

} // namespace homestore

// Make the one error type loggable: fmt does not format std::error_condition out of the box. message() routes
// through the condition's category, so a homestore domain enum (ReplServiceError, ...) logs as its enum name.
template <>
struct fmt::formatter< std::error_condition > : fmt::formatter< std::string > {
    auto format(std::error_condition const& ec, fmt::format_context& ctx) const {
        return fmt::formatter< std::string >::format(ec.message(), ctx);
    }
};
