#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "magic_enum/magic_enum.hpp"

#include "rei/net/backend/socket.hpp"
#include "rei/net/error_code.hpp"

namespace rei::net::backend::detail {
    namespace {
        class address_error_category final : public std::error_category {
          private:
          public:
            [[nodiscard]] auto name() const noexcept -> const char* override { return "rei.net.address"; }
            [[nodiscard]] auto message(int code) const -> std::string override {
                return ::gai_strerror(code);
            }

            [[nodiscard]] auto default_error_condition(int code) const noexcept
                -> std::error_condition override {
                switch (code) {
                case EAI_AGAIN:
                    return std::make_error_condition(std::errc::resource_unavailable_try_again);
                case EAI_MEMORY:
                    return std::make_error_condition(std::errc::not_enough_memory);
                case EAI_FAMILY:
                    return std::make_error_condition(std::errc::address_family_not_supported);
                case EAI_NONAME:
                    return std::make_error_condition(std::errc::address_not_available);
                default:
                    return {code, *this};
                }
            }
        };

        struct socket_address {
            sockaddr_storage storage{};
            socklen_t size{};
        };

        // Report EPIPE instead of raising SIGPIPE when the peer has closed the connection.
#ifdef MSG_NOSIGNAL
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif

        auto errno_code() -> std::error_code {
            return {errno, std::generic_category()};
        }

        auto transfer_error_code() -> std::error_code {
            if (errno == ECONNRESET || errno == EPIPE) {
                return net::make_error_code(net::error_code::connection_closed);
            }

            return detail::errno_code();
        }

        auto should_retry(bool waiting) -> bool {
            return errno == EINTR || (waiting && (errno == EAGAIN || errno == EWOULDBLOCK));
        }

        auto wait_ready(backend::native_handle descriptor, short events,
                        std::optional<std::chrono::steady_clock::time_point> deadline)
            -> std::expected<void, rei::error> {
            if (!deadline) {
                return {};
            }

            pollfd target{.fd = descriptor, .events = events, .revents = 0};
            while (true) {
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                    *deadline - std::chrono::steady_clock::now());
                const auto result =
                    ::poll(&target, 1, static_cast<int>(std::max<std::int64_t>(remaining.count(), 0)));
                if (result > 0) {
                    return {};
                }
                if (result == 0) {
                    return std::unexpected(rei::error{net::make_error_code(net::error_code::timed_out),
                                                      "socket operation timed out"});
                }
                if (errno != EINTR) {
                    return std::unexpected(rei::error{detail::errno_code(), "failed to wait for socket"});
                }
            }
        }

        // getaddrinfo and getnameinfo report EAI_SYSTEM failures through errno.
        auto address_error_code(std::int32_t result) -> std::error_code {
            static const detail::address_error_category category;
            return result == EAI_SYSTEM ? detail::errno_code() : std::error_code(result, category);
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
                    rei::error{detail::address_error_code(result),
                               std::format("failed to resolve [{}]:{}", endpoint.address, endpoint.port)});
            }

            const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> owner(addresses, ::freeaddrinfo);
            if (addresses == nullptr || addresses->ai_addrlen > sizeof(sockaddr_storage)) {
                return std::unexpected(rei::error{std::make_error_code(std::errc::address_not_available),
                                                  "no usable socket address"});
            }

            detail::socket_address address{};
            address.size = addresses->ai_addrlen;
            std::memcpy(&address.storage, addresses->ai_addr, address.size);

            return address;
        }

        auto to_endpoint(const detail::socket_address& address)
            -> std::expected<net::ip_endpoint, rei::error> {
            std::array<char, NI_MAXHOST> host;
            const auto result =
                ::getnameinfo(reinterpret_cast<const sockaddr*>(&address.storage), address.size, host.data(),
                              host.size(), nullptr, 0, NI_NUMERICHOST);
            if (result != 0) {
                return std::unexpected(
                    rei::error{detail::address_error_code(result), "failed to format socket address"});
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

        auto receive_message(backend::native_handle descriptor, std::span<std::byte> buffer,
                             std::optional<std::chrono::steady_clock::time_point> deadline,
                             detail::socket_address* sender = nullptr)
            -> std::expected<std::size_t, rei::error> {
            iovec data{
                .iov_base = buffer.data(),
                .iov_len = buffer.size(),
            };

            msghdr message{};
            message.msg_iov = &data;
            message.msg_iovlen = 1;
            if (sender != nullptr) {
                message.msg_name = &sender->storage;
                message.msg_namelen = sizeof(sender->storage);
            }

            ssize_t result{};
            do {
                auto ready = detail::wait_ready(descriptor, POLLIN, deadline);
                if (!ready) {
                    return std::unexpected(ready.error());
                }

                result = ::recvmsg(descriptor, &message, deadline ? MSG_DONTWAIT : 0);
            } while (result == -1 && detail::should_retry(deadline.has_value()));
            if (result == -1) {
                return std::unexpected(
                    rei::error{detail::transfer_error_code(), "failed to receive socket data"});
            }
            if ((message.msg_flags & MSG_TRUNC) != 0) {
                return std::unexpected(rei::error{std::make_error_code(std::errc::message_size),
                                                  "receive buffer is too small for datagram"});
            }

            if (sender != nullptr) {
                sender->size = message.msg_namelen;
            }

            return static_cast<std::size_t>(result);
        }

        // A null receiver sends on the connected peer.
        auto send_message(backend::native_handle descriptor, std::span<const std::byte> data,
                          const detail::socket_address* receiver,
                          std::optional<std::chrono::steady_clock::time_point> deadline)
            -> std::expected<std::size_t, rei::error> {
            const auto* name =
                receiver != nullptr ? reinterpret_cast<const sockaddr*>(&receiver->storage) : nullptr;
            const socklen_t size = receiver != nullptr ? receiver->size : 0;

            ssize_t result{};
            do {
                auto ready = detail::wait_ready(descriptor, POLLOUT, deadline);
                if (!ready) {
                    return std::unexpected(ready.error());
                }

                result = ::sendto(descriptor, data.data(), data.size(),
                                  detail::send_flags | (deadline ? MSG_DONTWAIT : 0), name, size);
            } while (result == -1 && detail::should_retry(deadline.has_value()));
            if (result == -1) {
                return std::unexpected(
                    rei::error{detail::transfer_error_code(), "failed to send socket data"});
            }

            return static_cast<std::size_t>(result);
        }

        // Expects a non-blocking descriptor.
        auto connect_until(backend::native_handle descriptor, const detail::socket_address& address,
                           std::chrono::steady_clock::time_point deadline)
            -> std::expected<void, rei::error> {
            if (::connect(descriptor, reinterpret_cast<const sockaddr*>(&address.storage), address.size) ==
                0) {
                return {};
            }
            if (errno != EINPROGRESS && errno != EINTR) {
                return std::unexpected(rei::error{detail::errno_code(), "failed to connect socket"});
            }

            auto ready = detail::wait_ready(descriptor, POLLOUT, deadline);
            if (!ready) {
                return std::unexpected(ready.error());
            }

            int error{};
            socklen_t size = sizeof(error);
            if (::getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &error, &size) == -1) {
                return std::unexpected(rei::error{detail::errno_code(), "failed to get connection status"});
            }
            if (error != 0) {
                return std::unexpected(
                    rei::error{std::error_code(error, std::generic_category()), "failed to connect socket"});
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
        : m_socket(std::exchange(other.m_socket, -1)), m_domain(other.m_domain), m_protocol(other.m_protocol),
          m_timeout(other.m_timeout), m_non_blocking(other.m_non_blocking) {}

    auto socket_backend::operator=(socket_backend&& other) noexcept -> socket_backend& {
        if (this != &other) {
            if (is_open()) {
                ::close(m_socket);
            }

            m_socket = std::exchange(other.m_socket, -1);
            m_domain = other.m_domain;
            m_protocol = other.m_protocol;
            m_timeout = other.m_timeout;
            m_non_blocking = other.m_non_blocking;
        }

        return *this;
    }

    socket_backend::~socket_backend() {
        if (is_open()) {
            ::close(m_socket);
        }
    }

    auto socket_backend::_configure() -> std::expected<void, rei::error> {
        const auto flags = ::fcntl(m_socket, F_GETFD);
        if (flags == -1 || ::fcntl(m_socket, F_SETFD, flags | FD_CLOEXEC) == -1) {
            return std::unexpected(rei::error{detail::errno_code(), "failed to mark socket close-on-exec"});
        }

#ifdef SO_NOSIGPIPE
        // TODO: OpenBSD something
#endif

        return this->set_non_blocking(false);
    }

    auto socket_backend::_deadline() const -> std::optional<std::chrono::steady_clock::time_point> {
        if (!m_timeout || m_non_blocking) {
            return std::nullopt;
        }

        return std::chrono::steady_clock::now() + *m_timeout;
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

        backend::socket_backend out{domain, protocol};
        out.m_socket = ::socket(detail::native_domain(domain), detail::native_type(protocol),
                                detail::native_protocol(protocol));
        if (!out.is_open()) {
            return std::unexpected(
                rei::error{detail::errno_code(),
                           std::format("failed to create POSIX socket ({}, {})",
                                       magic_enum::enum_name(domain), magic_enum::enum_name(protocol))});
        }

        auto configured = out._configure();
        if (!configured) {
            return std::unexpected(configured.error());
        }

        return out;
    }

    auto socket_backend::close() -> std::expected<void, rei::error> {
        const auto descriptor = std::exchange(m_socket, -1);

        if (descriptor != -1 && ::close(descriptor) == -1) {
            return std::unexpected(rei::error{detail::errno_code(), "failed to close socket"});
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

        const auto deadline = _deadline();
        if (!deadline) {
            if (::connect(m_socket, reinterpret_cast<const sockaddr*>(&address->storage), address->size) ==
                -1) {
                return std::unexpected(rei::error{detail::errno_code(), "failed to connect socket"});
            }

            return {};
        }

        auto non_blocking = set_non_blocking(true);
        if (!non_blocking) {
            return non_blocking;
        }

        auto connected = detail::connect_until(m_socket, *address, *deadline);
        auto restored = set_non_blocking(false);
        if (!connected) {
            return connected;
        }

        return restored;
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

        if (::bind(m_socket, reinterpret_cast<const sockaddr*>(&address->storage), address->size) == -1) {
            return std::unexpected(rei::error{detail::errno_code(), "failed to bind socket"});
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

        if (::listen(m_socket, backlog) == -1) {
            return std::unexpected(rei::error{detail::errno_code(), "failed to listen on socket"});
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

        const auto deadline = _deadline();
        backend::socket_backend out{m_domain, m_protocol};
        do {
            auto ready = detail::wait_ready(m_socket, POLLIN, deadline);
            if (!ready) {
                return std::unexpected(ready.error());
            }

            out.m_socket = ::accept(m_socket, nullptr, nullptr);
        } while (!out.is_open() && errno == EINTR);

        if (!out.is_open()) {
            return std::unexpected(rei::error{detail::errno_code(), "failed to accept socket"});
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

        return detail::send_message(m_socket, data, nullptr, _deadline());
    }

    auto socket_backend::receive(std::span<std::byte> buffer) -> std::expected<std::size_t, rei::error> {
        if (!is_open()) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::bad_file_descriptor),
                                              "cannot receive on a closed socket"});
        }

        if (m_protocol == net::transport_protocol::tcp && buffer.empty()) {
            return 0;
        }

        auto received = detail::receive_message(m_socket, buffer, _deadline());
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

        return detail::send_message(m_socket, data, &*address, _deadline());
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

        detail::socket_address address{};
        auto received = detail::receive_message(m_socket, buffer, _deadline(), &address);
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
        int how{};
        switch (mode) {
        case net::shutdown_mode::receive:
            how = SHUT_RD;
            break;
        case net::shutdown_mode::send:
            how = SHUT_WR;
            break;
        case net::shutdown_mode::both:
            how = SHUT_RDWR;
            break;
        default:
            return std::unexpected(
                rei::error{std::make_error_code(std::errc::invalid_argument), "invalid shutdown mode"});
        }

        if (::shutdown(m_socket, how) == -1) {
            return std::unexpected(rei::error{detail::errno_code(), "failed to shut down socket"});
        }

        return {};
    }

    auto socket_backend::local_endpoint() const -> std::expected<net::ip_endpoint, rei::error> {
        detail::socket_address address{};
        address.size = sizeof(address.storage);

        if (::getsockname(m_socket, reinterpret_cast<sockaddr*>(&address.storage), &address.size) == -1) {
            return std::unexpected(rei::error{detail::errno_code(), "failed to get local socket address"});
        }

        return detail::to_endpoint(address);
    }

    auto socket_backend::peer_endpoint() const -> std::expected<net::ip_endpoint, rei::error> {
        detail::socket_address address{};
        address.size = sizeof(address.storage);

        if (::getpeername(m_socket, reinterpret_cast<sockaddr*>(&address.storage), &address.size) == -1) {
            return std::unexpected(rei::error{detail::errno_code(), "failed to get peer socket address"});
        }

        return detail::to_endpoint(address);
    }

    auto socket_backend::set_non_blocking(bool enabled) -> std::expected<void, rei::error> {
        const auto flags = ::fcntl(m_socket, F_GETFL);
        if (flags == -1 ||
            ::fcntl(m_socket, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) == -1) {
            return std::unexpected(rei::error{detail::errno_code(), "failed to set socket blocking mode"});
        }

        m_non_blocking = enabled;

        return {};
    }

    auto socket_backend::set_reuse_address(bool enabled) -> std::expected<void, rei::error> {
        const int value = enabled ? 1 : 0;
        if (::setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) == -1) {
            return std::unexpected(rei::error{detail::errno_code(), "failed to set socket address reuse"});
        }

        return {};
    }

    auto socket_backend::set_timeout(std::optional<std::chrono::milliseconds> timeout)
        -> std::expected<void, rei::error> {
        if (timeout && (timeout->count() < 0 || timeout->count() > std::numeric_limits<int>::max())) {
            return std::unexpected(rei::error{std::make_error_code(std::errc::invalid_argument),
                                              "socket timeout is out of range"});
        }

        m_timeout = timeout;

        return {};
    }
} // namespace rei::net::backend
