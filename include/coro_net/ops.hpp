// =============================================================================
// coro_net/ops.hpp —— io_uring 操作 awaiter 伞形头文件（向后兼容）
// =============================================================================
// 5 种基础 awaiter 已拆到独立头：
//
//   ops/accept.hpp              AcceptAwaiter
//   ops/recv.hpp                RecvAwaiter
//   ops/recv_into_buffer.hpp    RecvIntoBufferAwaiter
//   ops/send.hpp                SendAwaiter
//   ops/timeout.hpp             TimeoutAwaiter
//   ops/shutdown.hpp            ShutdownAwaiter
//
// 既有代码 `#include "coro_net/ops.hpp"` 不需要修改。新代码鼓励按需引入。
// =============================================================================

#pragma once

#include "coro_net/ops/accept.hpp"
#include "coro_net/ops/recv.hpp"
#include "coro_net/ops/recv_into_buffer.hpp"
#include "coro_net/ops/recv_fixed.hpp"
#include "coro_net/ops/send.hpp"
#include "coro_net/ops/send_fixed.hpp"
#include "coro_net/ops/timeout.hpp"
#include "coro_net/ops/shutdown.hpp"
