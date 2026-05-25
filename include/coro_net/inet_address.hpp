// =============================================================================
// coro_net/inet_address.hpp —— IPv4 地址封装
// =============================================================================
// 极简实现，提供与 mymuduo InetAddress 相同的 (ip, port) ↔ sockaddr_in 转换。
// 不引入 IPv6（项目暂未需要，避免膨胀）。
// =============================================================================

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace coro_net {

class InetAddress {
public:
    // 给定端口 + 可选 IP（默认本机所有接口 0.0.0.0）
    explicit InetAddress(uint16_t port, std::string ip = "0.0.0.0") {
        std::memset(&addr_, 0, sizeof(addr_));
        addr_.sin_family = AF_INET;
        addr_.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr);
    }

    explicit InetAddress(const sockaddr_in& a) : addr_(a) {}

    sockaddr_in& raw() noexcept { return addr_; }
    const sockaddr_in& raw() const noexcept { return addr_; }

    uint16_t port() const noexcept { return ntohs(addr_.sin_port); }

    std::string ip() const {
        char buf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf));
        return buf;
    }

    std::string to_string() const {
        return ip() + ":" + std::to_string(port());
    }

private:
    sockaddr_in addr_{};
};

}  // namespace coro_net
