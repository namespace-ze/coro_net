// =============================================================================
// coro_net/tcp.hpp —— TCP 模块伞形头文件（向后兼容）
// =============================================================================
// 各组件已拆分到独立头文件：
//
//   idle_entry.hpp              IdleEntry
//   tcp_connection.hpp          TcpConnection
//   idle_connection_wheel.hpp   IdleConnectionWheel
//   tcp_server.hpp              TcpServer
//
// 既有代码 `#include "coro_net/tcp.hpp"` 不需要修改。新代码鼓励按需引入。
// =============================================================================

#pragma once

#include "coro_net/idle_entry.hpp"
#include "coro_net/tcp_connection.hpp"
#include "coro_net/idle_connection_wheel.hpp"
#include "coro_net/tcp_server.hpp"
