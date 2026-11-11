#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/base.h>

#include "rei/core/error.hpp"
#include "rei/net/socket.hpp"
#include "rei/net/stream.hpp"
#include "rei/net/transport.hpp"

namespace rei::net::tls {
    enum class version : std::uint8_t {
        tls_1_2,
        tls_1_3,
    };

    class roots {
      private:
        friend class connector;

        using certificates = std::vector<bssl::UniquePtr<X509>>;

        std::shared_ptr<const roots::certificates> m_certificates;

        explicit roots(std::shared_ptr<const roots::certificates> certificates) noexcept;

      public:
        static auto system() -> std::expected<tls::roots, rei::error>;
        static auto load(const std::filesystem::path& file) -> std::expected<tls::roots, rei::error>;
        static auto make(std::string_view pem) -> std::expected<tls::roots, rei::error>;

        auto add(const tls::roots& other) -> void;
    };

    class identity {
      private:
        friend class acceptor;

        struct chain_and_key;

        std::shared_ptr<const identity::chain_and_key> m_chain_and_key;

        explicit identity(std::shared_ptr<const identity::chain_and_key> chain_and_key) noexcept;

      public:
        static auto load(const std::filesystem::path& certificate_chain,
                         const std::filesystem::path& private_key)
            -> std::expected<tls::identity, rei::error>;
        static auto make(std::string_view certificate_chain, std::string_view private_key)
            -> std::expected<tls::identity, rei::error>;
    };

    struct connector_options {
        tls::roots roots;
        tls::version minimum_version{tls::version::tls_1_2};
        std::vector<std::string> application_protocols;
    };

    struct acceptor_options {
        tls::identity identity;
        tls::version minimum_version{tls::version::tls_1_2};
        std::vector<std::string> application_protocols;
    };

    class stream {
      private:
        friend class connector;
        friend class acceptor;

        struct state;

        std::unique_ptr<stream::state> m_state;

        explicit stream(std::unique_ptr<stream::state> state) noexcept;

      public:
        stream(stream&& other) noexcept;
        auto operator=(stream&& other) noexcept -> stream&;
        ~stream();

        auto send(std::span<const std::byte> data) -> std::expected<std::size_t, rei::error>;
        auto receive(std::span<std::byte> buffer) -> std::expected<std::size_t, rei::error>;

        [[nodiscard]] auto readable() const noexcept -> bool;

        auto shutdown(net::shutdown_mode mode = net::shutdown_mode::both) -> std::expected<void, rei::error>;

        auto set_timeout(std::optional<std::chrono::milliseconds> timeout) -> std::expected<void, rei::error>;
        auto set_deadline(std::optional<std::chrono::steady_clock::time_point> deadline) noexcept -> void;

        [[nodiscard]] auto socket() noexcept -> net::socket&;
        [[nodiscard]] auto version() const noexcept -> tls::version;
        [[nodiscard]] auto application_protocol() const noexcept -> std::string_view;
    };

    static_assert(net::stream<tls::stream>);

    class connector {
      private:
        struct state;

        std::shared_ptr<connector::state> m_state;

        explicit connector(std::shared_ptr<connector::state> state) noexcept;

      public:
        static auto make() -> std::expected<tls::connector, rei::error>;
        static auto make(const tls::connector_options& options) -> std::expected<tls::connector, rei::error>;

        auto connect(net::socket socket, std::string_view server_name) const
            -> std::expected<tls::stream, rei::error>;
    };

    class acceptor {
      private:
        struct state;

        std::shared_ptr<acceptor::state> m_state;

        explicit acceptor(std::shared_ptr<acceptor::state> state) noexcept;

      public:
        static auto make(const tls::acceptor_options& options) -> std::expected<tls::acceptor, rei::error>;

        auto accept(net::socket socket) const -> std::expected<tls::stream, rei::error>;
    };
} // namespace rei::net::tls
