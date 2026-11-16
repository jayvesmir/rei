#pragma once

#include <winsock2.h>

#include <ws2tcpip.h>

inline auto getsockopt(SOCKET socket, int level, int name, void* value, socklen_t* size) -> int {
    return ::getsockopt(socket, level, name, static_cast<char*>(value), size);
}
