#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <winsock2.h>

#include <ws2tcpip.h>

#include <mstcpip.h>

#include "magic_enum/magic_enum.hpp"

#include "rei/net/backend/socket.hpp"
#include "rei/net/error_code.hpp"

namespace rei::net::backend::detail {
    namespace {
        using namespace std::chrono_literals;

        static_assert(std::same_as<backend::native_handle, SOCKET> &&
                      backend::invalid_handle == INVALID_SOCKET);

        struct socket_address {
            sockaddr_storage storage{};
            std::int32_t size{};
        };

        enum class readiness {
            readable,
            writable,
            connected,
        };

        constexpr auto shutdown_check_interval = 250ms;
        constexpr std::array syn_retransmission_times{1s, 3s, 7s, 15s};

        auto socket_error_code(std::int32_t code) -> std::error_code {
            if (code == WSAEWOULDBLOCK) {
                return std::make_error_code(std::errc::resource_unavailable_try_again);
            }

            return {code, std::system_category()};
        }

        auto last_error_code() -> std::error_code {
            return detail::socket_error_code(::WSAGetLastError());
        }

        auto transfer_error_code(std::int32_t code) -> std::error_code {
            if (code == WSAECONNRESET || code == WSAECONNABORTED || code == WSAESHUTDOWN) {
                return net::make_error_code(net::error_code::connection_closed);
            }

            return detail::socket_error_code(code);
        }

        auto startup() -> std::expected<void, rei::error> {
            static const auto result = [] -> std::int32_t {
                WSADATA data{};
                return ::WSAStartup(MAKEWORD(2, 2), &data);
            }();
            if (result != 0) {
                return std::unexpected(
                    rei::error{detail::socket_error_code(result), "failed to start Winsock"});
            }

            return {};
        }

        auto transfer_size(std::size_t size) -> std::int32_t {
            return static_cast<std::int32_t>(
                std::min<std::size_t>(size, std::numeric_limits<std::int32_t>::max()));
        }

        auto wait_ready(backend::native_handle handle, detail::readiness awaited,
                        std::chrono::steady_clock::time_point deadline) -> std::expected<bool, rei::error> {
            const auto remaining =
                std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining <= std::chrono::milliseconds::zero()) {
                return std::unexpected(rei::error{net::make_error_code(net::error_code::timed_out),
                                                  "socket operation timed out"});
            }

            const auto wait = std::chrono::duration_cast<std::chrono::microseconds>(
                std::min(remaining, detail::shutdown_check_interval));
            const timeval timeout{.tv_sec = 0, .tv_usec = static_cast<long>(wait.count())};

            fd_set sockets{.fd_count = 1, .fd_array = {handle}};
            fd_set failed = sockets;
            const auto result = ::select(0, awaited == readiness::readable ? &sockets : nullptr,
                                         awaited == readiness::readable ? nullptr : &sockets,
                                         awaited == readiness::connected ? &failed : nullptr, &timeout);
            if (result == SOCKET_ERROR) {
                return std::unexpected(rei::error{detail::last_error_code(), "failed to wait for socket"});
            }

            return result > 0;
        }

        auto native_domain(net::communication_domain domain) -> std::int32_t {
            return domain == net::communication_domain::ipv4 ? AF_INET : AF_INET6;
        }

        auto native_type(net::transport_protocol protocol) -> std::int32_t {
            return protocol == net::transport_protocol::tcp ? SOCK_STREAM : SOCK_DGRAM;
        }

        auto native_protocol(net::transport_protocol protocol) -> std::int32_t {
            return protocol == net::transport_protocol::tcp ? IPPROTO_TCP : IPPROTO_UDP;
        }

        auto resolve_address(const net::ip_endpoint& endpoint, net::communication_domain domain,
                             net::transport_protocol protocol, bool passive)
            -> std::expected<detail::socket_address, rei::error> {
            if ((!passive && endpoint.address.empty()) || endpoint.address.contains('\0')) {
                return std::unexpected(
                    rei::error{std::make_error_code(std::errc::invalid_argument), "invalid socket address"});
            }

            auto started = detail::startup();
            if (!started) {
                return std::unexpected(started.error());
            }

            addrinfo hints{};
            hints.ai_family = detail::native_domain(domain);
            hints.ai_socktype = detail::native_type(protocol);
            hints.ai_protocol = detail::native_protocol(protocol);
            hints.ai_flags = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0);

            const auto service = std::to_string(endpoint.port);
            addrinfo* addresses = nullptr;
            const auto result = ::getaddrinfo(endpoint.address.empty() ? nullptr : endpoint.address.c_str(),
                                              service.c_str(), &hints, &addresses);
            if (result != 0) {
                return std::unexpected(
                    rei::error{detail::socket_error_code(result),
                               std::format("failed to resolve [{}]:{}", endpoint.address, endpoint.port)});
            }

            const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> owner(addresses, ::freeaddrinfo);
            if (addresses == nullptr || addresses->ai_addrlen > sizeof(sockaddr_storage)) {
                return std::unexpected(rei::error{std::make_error_code(std::errc::address_not_available),
                                                  "no usable socket address"});
            }

            detail::socket_address address{};
            address.size = static_cast<std::int32_t>(addresses->ai_addrlen);
            std::memcpy(&address.storage, addresses->ai_addr, addresses->ai_addrlen);

            return address;
        }

        auto to_endpoint(const detail::socket_address& address)
            -> std::expected<net::ip_endpoint, rei::error> {
            std::array<char, NI_MAXHOST> host;
            const auto result =
                ::getnameinfo(reinterpret_cast<const sockaddr*>(&address.storage), address.size, host.data(),
                              static_cast<DWORD>(host.size()), nullptr, 0, NI_NUMERICHOST);
            if (result != 0) {
                return std::unexpected(
                    rei::error{detail::socket_error_code(result), "failed to format socket address"});
            }

            std::uint16_t port;
            if (address.storage.ss_family == AF_INET) {
                port = ::ntohs(reinterpret_cast<const sockaddr_in*>(&address.storage)->sin_port);
            } else if (address.storage.ss_family == AF_INET6) {
                port = ::ntohs(reinterpret_cast<const sockaddr_in6*>(&address.storage)->sin6_port);
            } else {
                return std::unexpected(
                    rei::error{std::make_error_code(std::errc::address_family_not_supported),
                               "unsupported socket address family"});
            }

            return net::ip_endpoint{
                .address = host.data(),
                .port = port,
            };
        }

        auto receive_message(backend::native_handle handle, std::span<std::byte> buffer,
                             std::optional<std::chrono::steady_clock::time_point> deadline,
                             detail::socket_address* sender = nullptr)
            -> std::expected<std::size_t, rei::error> {
            sockaddr* name = nullptr;
            std::int32_t* name_size = nullptr;
            if (sender != nullptr) {
                sender->size = sizeof(sender->storage);
                name = reinterpret_cast<sockaddr*>(&sender->storage);
                name_size = &sender->size;
            }

            while (true) {
                const auto result = ::recvfrom(handle, reinterpret_cast<char*>(buffer.data()),
                                               detail::transfer_size(buffer.size()), 0, name, name_size);
                if (result != SOCKET_ERROR) {
                    return static_cast<std::size_t>(result);
                }

                const auto code = ::WSAGetLastError();
                if (code == WSAEMSGSIZE) {
                    return std::unexpected(rei::error{std::make_error_code(std::errc::message_size),
                                                      "receive buffer is too small for datagram"});
                }
                if (code != WSAEWOULDBLOCK || !deadline) {
                    return std::unexpected(
                        rei::error{detail::transfer_error_code(code), "failed to receive socket data"});
                }

                auto ready = detail::wait_ready(handle, readiness::readable, *deadline);
                if (!ready) {
                    return std::unexpected(ready.error());
                }
            }
        }

        auto send_message(backend::native_handle handle, std::span<const std::byte> data,
                          const detail::socket_address* receiver,
                          std::optional<std::chrono::steady_clock::time_point> deadline)
            -> std::expected<std::size_t, rei::error> {
            const auto* name =
                receiver != nullptr ? reinterpret_cast<const sockaddr*>(&receiver->storage) : nullptr;
            const auto name_size = receiver != nullptr ? receiver->size : 0;

            while (true) {
                const auto result = ::sendto(handle, reinterpret_cast<const char*>(data.data()),
                                             detail::transfer_size(data.size()), 0, name, name_size);
                if (result != SOCKET_ERROR) {
                    return static_cast<std::size_t>(result);
                }

                const auto code = ::WSAGetLastError();
                if (code != WSAEWOULDBLOCK || !deadline) {
                    return std::unexpected(
                        rei::error{detail::transfer_error_code(code), "failed to send socket data"});
                }

                auto ready = detail::wait_ready(handle, readiness::writable, *deadline);
                if (!ready) {
                    return std::unexpected(ready.error());
                }
            }
        }

        auto limit_syn_retransmissions(backend::native_handle handle,
                                       std::chrono::steady_clock::time_point deadline) -> void {
            const auto wait = deadline - std::chrono::steady_clock::now();
            const auto timely = std::ranges::count_if(detail::syn_retransmission_times,
                                                      [wait](auto time) -> bool { return time < wait; });
            if (timely == std::ssize(detail::syn_retransmission_times)) {
                return;
            }

            TCP_INITIAL_RTO_PARAMETERS parameters{
                .Rtt = TCP_INITIAL_RTO_UNSPECIFIED_RTT,
                .MaxSynRetransmissions =
                    timely == 0 ? TCP_INITIAL_RTO_NO_SYN_RETRANSMISSIONS : static_cast<UCHAR>(timely),
            };
            DWORD returned{};
            (void)::WSAIoctl(handle, SIO_TCP_INITIAL_RTO, &parameters, sizeof(parameters), nullptr, 0,
                             &returned, nullptr, nullptr);
        }

        auto finish_connect(backend::native_handle handle, std::chrono::steady_clock::time_point deadline)
            -> std::expected<void, rei::error> {
            while (true) {
                auto ready = detail::wait_ready(handle, readiness::connected, deadline);
                if (!ready) {
                    return std::unexpected(ready.error());
                }
                if (*ready) {
                    break;
                }
            }

            std::int32_t error{};
            std::int32_t size = sizeof(error);
            if (::getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) ==
                SOCKET_ERROR) {
                return std::unexpected(
                    rei::error{detail::last_error_code(), "failed to get connection status"});
            }
            if (error != 0) {
                return std::unexpected(
                    rei::error{detail::socket_error_code(error), "failed to connect socket"});
            }

            return {};
        }
    } // namespace
} // namespace rei::net::backend::detail

namespace rei::net::backend {
    auto resolve(const net::ip_endpoint& endpoint, net::communication_domain domain,
                 net::transport_protocol protocol) -> std::expected<net::ip_endpoint, rei::error> {
        auto address = detail::resolve_address(endpoint, domain, protocol, false);
        if (!address) {
            return std::unexpected(address.error());
        }

        return detail::to_endpoint(*address);
    }

    socket_backend::socket_backend(net::communication_domain domain,
                                   net::transport_protocol protocol) noexcept
        : m_domain(domain), m_protocol(protocol) {}

    socket_backend::socket_backend(socket_backend&& other) noexcept
        : m_socket(std::exchange(other.m_socket, backend::invalid_handle)), m_domain(other.m_domain),
          m_protocol(other.m_protocol), m_timeout(other.m_timeout), m_deadline(other.m_deadline),
          m_non_blocking(other.m_non_blocking) {}

    auto socket_backend::operator=(socket_backend&& other) noexcept -> socket_backend& {
        if (this != &other) {
            if (is_open()) {
                ::closesocket(m_socket);
            }

            m_socket = std::exchange(other.m_socket, backend::invalid_handle);
            m_domain = other.m_domain;
            m_protocol = other.m_protocol;
            m_timeout = other.m_timeout;
            m_deadline = other.m_deadline;
            m_non_blocking = other.m_non_blocking;
        }

        return *this;
    }

    socket_backend::~socket_backend() {
        if (is_open()) {
            ::closesocket(m_socket);
        }
    }

    auto socket_backend::_configure() -> std::expected<void, rei::error> {
        if (::SetHandleInformation(reinterpret_cast<HANDLE>(m_socket), HANDLE_FLAG_INHERIT, 0) == 0) {
            const std::error_code code{static_cast<std::int32_t>(::GetLastError()), std::system_category()};
            return std::unexpected(rei::error{code, "failed to mark socket non-inheritable"});
        }

        u_long non_blocking = 1;
        if (::ioctlsocket(m_socket, FIONBIO, &non_blocking) == SOCKET_ERROR) {
            return std::unexpected(
                rei::error{detail::last_error_code(), "failed to set socket blocking mode"});
        }

        if (m_protocol == net::transport_protocol::tcp) {
            (void)set_no_delay(true);
        }

        return {};
    }

    auto socket_backend::_wait_end() const
        -> std::expected<std::optional<std::chrono::steady_clock::time_point>, rei::error> {
        if (m_non_blocking) {
            return std::nullopt;
        }

        const auto now = std::chrono::steady_clock::now();
        if (m_deadline && *m_deadline <= now) {
            return std::unexpected(
                rei::error{net::make_error_code(net::error_code::timed_out), "socket deadline passed"});
        }

        const auto latest = m_deadline.value_or(std::chrono::steady_clock::time_point::max());
        if (!m_timeout) {
            return latest;
        }

        const auto ends_first = *m_timeout < std::chrono::floor<std::chrono::milliseconds>(latest - now);

        return ends_first ? now + *m_timeout : latest;
    }

    auto socket_backend::make(net::communication_domain domain, net::transport_protocol protocol)
        -> std::expected<backend::socket_backend, rei::error> {
        if (domain != net::communication_domain::ipv4 && domain != net::communication_domain::ipv6) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::address_family_not_supported),
                                              "unsupported socket domain"});
        }
        if (protocol != net::transport_protocol::tcp && protocol != net::transport_protocol::udp) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::protocol_not_supported),
                                              "only TCP and UDP sockets are supported"});
        }

        auto started = detail::startup();
        if (!started) {
            return std::unexpected(started.error());
        }

        backend::socket_backend out{domain, protocol};
        out.m_socket = ::socket(detail::native_domain(domain), detail::native_type(protocol),
                                detail::native_protocol(protocol));
        if (!out.is_open()) {
            return std::unexpected(
                rei::error{detail::last_error_code(),
                           std::format("failed to create Windows socket ({}, {})",
                                       magic_enum::enum_name(domain), magic_enum::enum_name(protocol))});
        }

        auto configured = out._configure();
        if (!configured) {
            return std::unexpected(configured.error());
        }

        return out;
    }

    auto socket_backend::readable() const noexcept -> bool {
        fd_set sockets{.fd_count = 1, .fd_array = {m_socket}};
        const timeval no_wait{};

        return !is_open() || ::select(0, &sockets, nullptr, nullptr, &no_wait) != 0;
    }

    auto socket_backend::close() -> std::expected<void, rei::error> {
        const auto handle = std::exchange(m_socket, backend::invalid_handle);

        if (handle != backend::invalid_handle && ::closesocket(handle) == SOCKET_ERROR) {
            return std::unexpected(rei::error{detail::last_error_code(), "failed to close socket"});
        }

        return {};
    }

    auto socket_backend::connect(const net::ip_endpoint& endpoint) -> std::expected<void, rei::error> {
        if (!is_open()) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::bad_file_descriptor),
                                              "cannot connect a closed socket"});
        }

        auto address = detail::resolve_address(endpoint, m_domain, m_protocol, false);
        if (!address) {
            return std::unexpected(address.error());
        }

        const auto wait_end = _wait_end();
        if (!wait_end) {
            return std::unexpected(wait_end.error());
        }

        if (*wait_end && m_protocol == net::transport_protocol::tcp) {
            detail::limit_syn_retransmissions(m_socket, **wait_end);
        }

        if (::connect(m_socket, reinterpret_cast<const sockaddr*>(&address->storage), address->size) == 0) {
            return {};
        }

        const auto code = ::WSAGetLastError();
        if (code != WSAEWOULDBLOCK) {
            return std::unexpected(rei::error{detail::socket_error_code(code), "failed to connect socket"});
        }
        if (!*wait_end) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::operation_in_progress),
                                              "failed to connect socket"});
        }

        return detail::finish_connect(m_socket, **wait_end);
    }

    auto socket_backend::bind(const net::ip_endpoint& local) -> std::expected<void, rei::error> {
        if (!is_open()) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::bad_file_descriptor),
                                              "cannot bind a closed socket"});
        }

        auto address = detail::resolve_address(local, m_domain, m_protocol, true);
        if (!address) {
            return std::unexpected(address.error());
        }

        if (::bind(m_socket, reinterpret_cast<const sockaddr*>(&address->storage), address->size) ==
            SOCKET_ERROR) {
            return std::unexpected(rei::error{detail::last_error_code(), "failed to bind socket"});
        }

        return {};
    }

    auto socket_backend::listen(std::int32_t backlog) -> std::expected<void, rei::error> {
        if (m_protocol != net::transport_protocol::tcp) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::operation_not_supported),
                                              "listen requires a TCP socket"});
        }

        if (backlog < 0) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::invalid_argument),
                                              "listen backlog cannot be negative"});
        }

        if (::listen(m_socket, backlog) == SOCKET_ERROR) {
            return std::unexpected(rei::error{detail::last_error_code(), "failed to listen on socket"});
        }

        return {};
    }

    auto socket_backend::accept() -> std::expected<backend::socket_backend, rei::error> {
        if (m_protocol != net::transport_protocol::tcp) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::operation_not_supported),
                                              "accept requires a TCP socket"});
        }

        if (!is_open()) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::bad_file_descriptor),
                                              "cannot accept on a closed socket"});
        }

        const auto wait_end = _wait_end();
        if (!wait_end) {
            return std::unexpected(wait_end.error());
        }

        backend::socket_backend out{m_domain, m_protocol};
        while (true) {
            out.m_socket = ::accept(m_socket, nullptr, nullptr);
            if (out.is_open()) {
                break;
            }

            const auto code = ::WSAGetLastError();
            if (code != WSAEWOULDBLOCK || !*wait_end) {
                return std::unexpected(
                    rei::error{detail::socket_error_code(code), "failed to accept socket"});
            }

            auto ready = detail::wait_ready(m_socket, detail::readiness::readable, **wait_end);
            if (!ready) {
                return std::unexpected(ready.error());
            }
        }

        auto configured = out._configure();
        if (!configured) {
            return std::unexpected(configured.error());
        }

        return out;
    }

    auto socket_backend::send(std::span<const std::byte> data) -> std::expected<std::size_t, rei::error> {
        if (!is_open()) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::bad_file_descriptor),
                                              "cannot send on a closed socket"});
        }

        const auto wait_end = _wait_end();
        if (!wait_end) {
            return std::unexpected(wait_end.error());
        }

        return detail::send_message(m_socket, data, nullptr, *wait_end);
    }

    auto socket_backend::receive(std::span<std::byte> buffer) -> std::expected<std::size_t, rei::error> {
        if (!is_open()) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::bad_file_descriptor),
                                              "cannot receive on a closed socket"});
        }

        if (m_protocol == net::transport_protocol::tcp && buffer.empty()) {
            return 0;
        }

        const auto wait_end = _wait_end();
        if (!wait_end) {
            return std::unexpected(wait_end.error());
        }

        auto received = detail::receive_message(m_socket, buffer, *wait_end);
        if (received && *received == 0 && m_protocol == net::transport_protocol::tcp) {
            return std::unexpected(rei::error{net::make_error_code(net::error_code::connection_closed),
                                              "peer closed the connection"});
        }

        return received;
    }

    auto socket_backend::send_to(std::span<const std::byte> data, const net::ip_endpoint& remote)
        -> std::expected<std::size_t, rei::error> {
        if (m_protocol != net::transport_protocol::udp) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::operation_not_supported),
                                              "send_to requires a UDP socket"});
        }

        if (!is_open()) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::bad_file_descriptor),
                                              "cannot send on a closed socket"});
        }

        auto address = detail::resolve_address(remote, m_domain, m_protocol, false);
        if (!address) {
            return std::unexpected(address.error());
        }

        const auto wait_end = _wait_end();
        if (!wait_end) {
            return std::unexpected(wait_end.error());
        }

        return detail::send_message(m_socket, data, &*address, *wait_end);
    }

    auto socket_backend::receive_from(std::span<std::byte> buffer)
        -> std::expected<net::datagram_result, rei::error> {
        if (m_protocol != net::transport_protocol::udp) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::operation_not_supported),
                                              "receive_from requires a UDP socket"});
        }

        if (!is_open()) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::bad_file_descriptor),
                                              "cannot receive on a closed socket"});
        }

        const auto wait_end = _wait_end();
        if (!wait_end) {
            return std::unexpected(wait_end.error());
        }

        detail::socket_address address{};
        auto received = detail::receive_message(m_socket, buffer, *wait_end, &address);
        if (!received) {
            return std::unexpected(received.error());
        }

        auto sender = detail::to_endpoint(address);
        if (!sender) {
            return std::unexpected(sender.error());
        }

        return net::datagram_result{.size = *received, .origin = std::move(*sender)};
    }

    auto socket_backend::shutdown(net::shutdown_mode mode) -> std::expected<void, rei::error> {
        std::int32_t how{};
        switch (mode) {
        case net::shutdown_mode::receive:
            how = SD_RECEIVE;
            break;
        case net::shutdown_mode::send:
            how = SD_SEND;
            break;
        case net::shutdown_mode::both:
            how = SD_BOTH;
            break;
        default:
            return std::unexpected(
                rei::error{std::make_error_code(std::errc::invalid_argument), "invalid shutdown mode"});
        }

        if (::shutdown(m_socket, how) == SOCKET_ERROR) {
            return std::unexpected(rei::error{detail::last_error_code(), "failed to shut down socket"});
        }

        return {};
    }

    auto socket_backend::local_endpoint() const -> std::expected<net::ip_endpoint, rei::error> {
        detail::socket_address address{};
        address.size = sizeof(address.storage);

        if (::getsockname(m_socket, reinterpret_cast<sockaddr*>(&address.storage), &address.size) ==
            SOCKET_ERROR) {
            return std::unexpected(
                rei::error{detail::last_error_code(), "failed to get local socket address"});
        }

        return detail::to_endpoint(address);
    }

    auto socket_backend::peer_endpoint() const -> std::expected<net::ip_endpoint, rei::error> {
        detail::socket_address address{};
        address.size = sizeof(address.storage);

        if (::getpeername(m_socket, reinterpret_cast<sockaddr*>(&address.storage), &address.size) ==
            SOCKET_ERROR) {
            return std::unexpected(
                rei::error{detail::last_error_code(), "failed to get peer socket address"});
        }

        return detail::to_endpoint(address);
    }

    auto socket_backend::set_non_blocking(bool enabled) -> std::expected<void, rei::error> {
        if (!is_open()) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::bad_file_descriptor),
                                              "failed to set socket blocking mode"});
        }

        m_non_blocking = enabled;

        return {};
    }

    auto socket_backend::set_reuse_address(bool) -> std::expected<void, rei::error> {
        if (!is_open()) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::bad_file_descriptor),
                                              "failed to set socket address reuse"});
        }

        return {};
    }

    auto socket_backend::set_no_delay(bool enabled) -> std::expected<void, rei::error> {
        const std::int32_t value = enabled ? 1 : 0;
        if (::setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&value),
                         sizeof(value)) == SOCKET_ERROR) {
            return std::unexpected(
                rei::error{detail::last_error_code(), "failed to set socket no-delay mode"});
        }

        return {};
    }

    auto socket_backend::set_timeout(std::optional<std::chrono::milliseconds> timeout)
        -> std::expected<void, rei::error> {
        if (timeout && timeout->count() < 0) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::invalid_argument),
                                              "socket timeout cannot be negative"});
        }

        m_timeout = timeout;

        return {};
    }
} // namespace rei::net::backend
