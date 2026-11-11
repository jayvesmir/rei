#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "rei/core/error.hpp"
#include "rei/net/socket.hpp"
#include "rei/net/tls.hpp"
#include "rei/net/tls/detail.hpp"

namespace rei::net::tls {
    struct connector::state {
        bssl::UniquePtr<SSL_CTX> context;
        std::shared_ptr<detail::session_cache> sessions;
    };

    struct acceptor::state {
        bssl::UniquePtr<SSL_CTX> context;
        std::vector<std::uint8_t> protocols;
    };

    namespace {
        constexpr auto read_buffer_size = 0xffffuz;
        constexpr auto longest_protocol_name = 255uz;
        constexpr auto sessions_per_server = std::ptrdiff_t{8};

        auto make_context(tls::version minimum_version)
            -> std::expected<bssl::UniquePtr<SSL_CTX>, rei::error> {
            ERR_clear_error();
            bssl::UniquePtr<SSL_CTX> context{SSL_CTX_new(TLS_method())};
            if (!context) {
                return detail::failure(std::make_error_code(std::errc::not_enough_memory),
                                       detail::drain_errors());
            }

            auto minimum = minimum_version == tls::version::tls_1_3 ? TLS1_3_VERSION : TLS1_2_VERSION;
            if (SSL_CTX_set_min_proto_version(context.get(), static_cast<std::uint16_t>(minimum)) != 1) {
                return detail::failure(std::make_error_code(std::errc::invalid_argument),
                                       detail::drain_errors());
            }

            SSL_CTX_set_read_ahead(context.get(), 1);
            SSL_CTX_set_default_read_buffer_len(context.get(), read_buffer_size);

            return context;
        }

        auto make_ssl(const bssl::UniquePtr<SSL_CTX>& context)
            -> std::expected<bssl::UniquePtr<SSL>, rei::error> {
            ERR_clear_error();
            bssl::UniquePtr<SSL> ssl{SSL_new(context.get())};
            if (!ssl) {
                return detail::failure(std::make_error_code(std::errc::not_enough_memory),
                                       detail::drain_errors());
            }

            return ssl;
        }

        auto alpn_list(const std::vector<std::string>& protocols)
            -> std::expected<std::vector<std::uint8_t>, rei::error> {
            std::vector<std::uint8_t> list;

            for (const auto& protocol : protocols) {
                if (protocol.empty() || protocol.size() > longest_protocol_name) {
                    return detail::failure(std::make_error_code(std::errc::invalid_argument),
                                           "an application protocol's name takes 1 to 255 bytes");
                }

                list.push_back(static_cast<std::uint8_t>(protocol.size()));
                list.insert(list.end(), protocol.begin(), protocol.end());
            }

            return list;
        }

        auto select_protocol(SSL* /*ssl*/, const std::uint8_t** selected, std::uint8_t* selected_size,
                             const std::uint8_t* offered, unsigned offered_size, void* context) -> int {
            const auto& preferred = *static_cast<const std::vector<std::uint8_t>*>(context);

            auto result =
                SSL_select_next_proto(const_cast<std::uint8_t**>(selected), selected_size, preferred.data(),
                                      static_cast<unsigned>(preferred.size()), offered, offered_size);

            return result == OPENSSL_NPN_NEGOTIATED ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_ALERT_FATAL;
        }

        auto keep_session(SSL* ssl, SSL_SESSION* session) -> int {
            auto& resumption = *static_cast<detail::resumption*>(SSL_get_app_data(ssl));

            resumption.sessions->keep(resumption.server_name, bssl::UniquePtr<SSL_SESSION>{session});
            return 1;
        }
    } // namespace

    detail::session_cache::session_cache(std::size_t capacity) noexcept : m_capacity(capacity) {}

    auto detail::session_cache::take(std::string_view server_name) -> bssl::UniquePtr<SSL_SESSION> {
        std::scoped_lock lock{m_mutex};

        auto found = std::ranges::find(m_entries, server_name, &entry::server_name);
        if (found == m_entries.end()) {
            return nullptr;
        }

        if (SSL_SESSION_should_be_single_use(found->session.get()) != 0) {
            auto session = std::move(found->session);
            m_entries.erase(found);
            return session;
        }

        m_entries.splice(m_entries.begin(), m_entries, found);
        return bssl::UpRef(found->session);
    }

    auto detail::session_cache::keep(std::string server_name, bssl::UniquePtr<SSL_SESSION> session) -> void {
        std::scoped_lock lock{m_mutex};

        auto same_server = [&](const entry& cached) -> bool { return cached.server_name == server_name; };
        if (std::ranges::count_if(m_entries, same_server) >= sessions_per_server) {
            m_entries.erase(std::ranges::find_last_if(m_entries, same_server).begin());
        }

        m_entries.push_front({.server_name = std::move(server_name), .session = std::move(session)});
        if (m_entries.size() > m_capacity) {
            m_entries.pop_back();
        }
    }

    connector::connector(std::shared_ptr<connector::state> state) noexcept : m_state(std::move(state)) {}

    auto connector::make() -> std::expected<tls::connector, rei::error> {
        auto trusted = tls::roots::system();
        if (!trusted) {
            return std::unexpected(trusted.error());
        }

        return connector::make({.roots = *trusted});
    }

    auto connector::make(const tls::connector_options& options) -> std::expected<tls::connector, rei::error> {
        auto context = make_context(options.minimum_version);
        if (!context) {
            return std::unexpected(context.error());
        }

        auto protocols = alpn_list(options.application_protocols);
        if (!protocols) {
            return std::unexpected(protocols.error());
        }

        ERR_clear_error();
        for (const auto& certificate : *options.roots.m_certificates) {
            if (X509_STORE_add_cert(SSL_CTX_get_cert_store(context->get()), certificate.get()) != 1) {
                return detail::failure(std::make_error_code(std::errc::invalid_argument),
                                       "unusable root certificate: " + detail::drain_errors());
            }
        }

        SSL_CTX_set_verify(context->get(), SSL_VERIFY_PEER, nullptr);

        if (!protocols->empty() &&
            SSL_CTX_set_alpn_protos(context->get(), protocols->data(), protocols->size()) != 0) {
            return detail::failure(std::make_error_code(std::errc::not_enough_memory),
                                   detail::drain_errors());
        }

        std::shared_ptr<detail::session_cache> sessions;
        if (options.max_cached_sessions > 0) {
            sessions = std::make_shared<detail::session_cache>(options.max_cached_sessions);
            SSL_CTX_set_session_cache_mode(context->get(), SSL_SESS_CACHE_CLIENT);
            SSL_CTX_sess_set_new_cb(context->get(), keep_session);
        }

        return connector{std::make_shared<connector::state>(std::move(*context), std::move(sessions))};
    }

    auto connector::connect(net::socket socket, std::string_view server_name) const
        -> std::expected<tls::stream, rei::error> {
        std::string name{server_name};
        if (name.empty() || name.contains('\0')) {
            return detail::failure(std::make_error_code(std::errc::invalid_argument), "invalid server name");
        }

        auto ssl = make_ssl(m_state->context);
        if (!ssl) {
            return std::unexpected(ssl.error());
        }

        SSL_set_connect_state(ssl->get());

        SSL_set_hostflags(ssl->get(),
                          X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        auto is_address = X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl->get()), name.c_str()) == 1;
        if (!is_address && (SSL_set1_host(ssl->get(), name.c_str()) != 1 ||
                            SSL_set_tlsext_host_name(ssl->get(), name.c_str()) != 1)) {
            return detail::failure(std::make_error_code(std::errc::invalid_argument),
                                   "invalid server name: " + detail::drain_errors());
        }

        if (m_state->sessions) {
            if (auto session = m_state->sessions->take(name)) {
                SSL_set_session(ssl->get(), session.get());
            }
        }

        return tls::stream::state::handshake(std::move(socket), std::move(*ssl),
                                             {.sessions = m_state->sessions, .server_name = std::move(name)});
    }

    acceptor::acceptor(std::shared_ptr<acceptor::state> state) noexcept : m_state(std::move(state)) {}

    auto acceptor::make(const tls::acceptor_options& options) -> std::expected<tls::acceptor, rei::error> {
        auto context = make_context(options.minimum_version);
        if (!context) {
            return std::unexpected(context.error());
        }

        auto protocols = alpn_list(options.application_protocols);
        if (!protocols) {
            return std::unexpected(protocols.error());
        }

        const auto& identity = *options.identity.m_chain_and_key;
        std::vector<CRYPTO_BUFFER*> chain;
        chain.reserve(identity.chain.size());
        for (const auto& certificate : identity.chain) {
            chain.push_back(certificate.get());
        }

        ERR_clear_error();
        if (SSL_CTX_set_chain_and_key(context->get(), chain.data(), chain.size(), identity.key.get(),
                                      nullptr) != 1) {
            return detail::failure(std::make_error_code(std::errc::invalid_argument),
                                   "unusable identity: " + detail::drain_errors());
        }

        SSL_CTX_set_session_cache_mode(context->get(), SSL_SESS_CACHE_OFF);

        auto state = std::make_shared<acceptor::state>(std::move(*context), std::move(*protocols));
        if (!state->protocols.empty()) {
            SSL_CTX_set_alpn_select_cb(state->context.get(), select_protocol, &state->protocols);
        }

        return acceptor{std::move(state)};
    }

    auto acceptor::accept(net::socket socket) const -> std::expected<tls::stream, rei::error> {
        auto ssl = make_ssl(m_state->context);
        if (!ssl) {
            return std::unexpected(ssl.error());
        }

        SSL_set_accept_state(ssl->get());

        return tls::stream::state::handshake(std::move(socket), std::move(*ssl));
    }
} // namespace rei::net::tls
