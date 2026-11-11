#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "rei/core/error.hpp"
#include "rei/net/error_code.hpp"
#include "rei/net/socket.hpp"
#include "rei/net/tls.hpp"
#include "rei/net/tls/detail.hpp"
#include "rei/net/tls/error_code.hpp"
#include "rei/net/transport.hpp"

namespace rei::net::tls {
    namespace {
        constexpr auto largest_call = static_cast<std::size_t>(std::numeric_limits<int>::max());

        auto bio_read(BIO* bio, char* buffer, int size) -> int {
            auto& transport = *static_cast<detail::transport*>(BIO_get_data(bio));

            auto received = transport.socket.receive(
                std::as_writable_bytes(std::span{buffer, static_cast<std::size_t>(size)}));
            if (received) {
                return static_cast<int>(*received);
            }

            if (received.error().code() == net::error_code::connection_closed) {
                return 0;
            }

            transport.error = std::move(received.error());
            return -1;
        }

        auto bio_write(BIO* bio, const char* data, int size) -> int {
            auto& transport = *static_cast<detail::transport*>(BIO_get_data(bio));

            auto sent = transport.socket.send(std::as_bytes(std::span{data, static_cast<std::size_t>(size)}));
            if (!sent) {
                transport.error = std::move(sent.error());
                return -1;
            }

            return static_cast<int>(*sent);
        }

        auto bio_ctrl(BIO* /*bio*/, int command, long /*number*/, void* /*pointer*/) -> long {
            return command == BIO_CTRL_FLUSH ? 1 : 0;
        }

        auto make_bio(detail::transport& transport) -> bssl::UniquePtr<BIO> {
            static const auto method = [] -> bssl::UniquePtr<BIO_METHOD> {
                bssl::UniquePtr<BIO_METHOD> made{
                    BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "rei::net::socket")};
                if (made) {
                    BIO_meth_set_read(made.get(), bio_read);
                    BIO_meth_set_write(made.get(), bio_write);
                    BIO_meth_set_ctrl(made.get(), bio_ctrl);
                }

                return made;
            }();

            bssl::UniquePtr<BIO> bio{method ? BIO_new(method.get()) : nullptr};
            if (bio) {
                BIO_set_data(bio.get(), &transport);
                BIO_set_init(bio.get(), 1);
            }

            return bio;
        }
    } // namespace

    auto stream::state::handshake(net::socket socket, bssl::UniquePtr<SSL> ssl, detail::resumption resumption)
        -> std::expected<tls::stream, rei::error> {
        auto state = std::make_unique<stream::state>(
            detail::transport{.socket = std::move(socket), .error = std::nullopt}, std::move(resumption),
            std::move(ssl));

        ERR_clear_error();
        auto bio = make_bio(state->transport);
        if (!bio) {
            return detail::failure(std::make_error_code(std::errc::not_enough_memory),
                                   detail::drain_errors());
        }

        SSL_set_bio(state->ssl.get(), bio.get(), bio.get());
        (void)bio.release();

        SSL_set_app_data(state->ssl.get(), &state->resumption);
        SSL_set_shed_handshake_config(state->ssl.get(), 1);

        if (auto result = SSL_do_handshake(state->ssl.get()); result != 1) {
            return std::unexpected(state->failure(result));
        }

        return tls::stream{std::move(state)};
    }

    auto stream::state::failure(int result, std::source_location location) -> rei::error {
        auto kind = SSL_get_error(ssl.get(), result);
        auto last = ERR_peek_last_error();
        auto queued = detail::drain_errors();
        auto socket_error = std::exchange(transport.error, std::nullopt);

        if (kind == SSL_ERROR_ZERO_RETURN) {
            return rei::error{net::error_code::connection_closed, "peer closed the connection", location};
        }

        if (kind == SSL_ERROR_SYSCALL) {
            if (socket_error) {
                return std::move(*socket_error);
            }

            return rei::error{tls::error_code::truncated,
                              SSL_in_init(ssl.get()) != 0 ? "peer closed the connection during the handshake"
                                                          : "peer closed the connection without close_notify",
                              location};
        }

        if (SSL_in_init(ssl.get()) == 0) {
            return rei::error{net::error_code::protocol_error, std::move(queued), location};
        }

        if (ERR_GET_LIB(last) == ERR_LIB_SSL && ERR_GET_REASON(last) == SSL_R_CERTIFICATE_VERIFY_FAILED) {
            return rei::error{tls::error_code::certificate_rejected,
                              X509_verify_cert_error_string(SSL_get_verify_result(ssl.get())), location};
        }

        return rei::error{tls::error_code::handshake_failed, std::move(queued), location};
    }

    stream::stream(std::unique_ptr<stream::state> state) noexcept : m_state(std::move(state)) {}

    stream::stream(stream&& other) noexcept = default;

    auto stream::operator=(stream&& other) noexcept -> stream& = default;

    stream::~stream() = default;

    auto stream::send(std::span<const std::byte> data) -> std::expected<std::size_t, rei::error> {
        if (data.empty()) {
            return 0uz;
        }

        ERR_clear_error();
        auto sent =
            SSL_write(m_state->ssl.get(), data.data(), static_cast<int>(std::min(data.size(), largest_call)));
        if (sent <= 0) {
            return std::unexpected(m_state->failure(sent));
        }

        return static_cast<std::size_t>(sent);
    }

    auto stream::receive(std::span<std::byte> buffer) -> std::expected<std::size_t, rei::error> {
        if (buffer.empty()) {
            return 0uz;
        }

        ERR_clear_error();
        auto received = SSL_read(m_state->ssl.get(), buffer.data(),
                                 static_cast<int>(std::min(buffer.size(), largest_call)));
        if (received <= 0) {
            return std::unexpected(m_state->failure(received));
        }

        return static_cast<std::size_t>(received);
    }

    auto stream::readable() const noexcept -> bool {
        return SSL_has_pending(m_state->ssl.get()) != 0 || m_state->transport.socket.readable();
    }

    auto stream::shutdown(net::shutdown_mode mode) -> std::expected<void, rei::error> {
        auto notified = (SSL_get_shutdown(m_state->ssl.get()) & SSL_SENT_SHUTDOWN) != 0;

        if (mode != net::shutdown_mode::receive && !notified) {
            ERR_clear_error();
            if (auto result = SSL_shutdown(m_state->ssl.get()); result < 0) {
                return std::unexpected(m_state->failure(result));
            }
        }

        return m_state->transport.socket.shutdown(mode);
    }

    auto stream::set_timeout(std::optional<std::chrono::milliseconds> timeout)
        -> std::expected<void, rei::error> {
        return m_state->transport.socket.set_timeout(timeout);
    }

    auto stream::set_deadline(std::optional<std::chrono::steady_clock::time_point> deadline) noexcept
        -> void {
        m_state->transport.socket.set_deadline(deadline);
    }

    auto stream::socket() noexcept -> net::socket& {
        return m_state->transport.socket;
    }

    auto stream::version() const noexcept -> tls::version {
        return SSL_version(m_state->ssl.get()) == TLS1_3_VERSION ? tls::version::tls_1_3
                                                                 : tls::version::tls_1_2;
    }

    auto stream::application_protocol() const noexcept -> std::string_view {
        const std::uint8_t* protocol{};
        unsigned size{};
        SSL_get0_alpn_selected(m_state->ssl.get(), &protocol, &size);

        return {reinterpret_cast<const char*>(protocol), size};
    }

    auto stream::resumed() const noexcept -> bool {
        return SSL_session_reused(m_state->ssl.get()) != 0;
    }
} // namespace rei::net::tls
