#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "rei/core/error.hpp"
#include "rei/core/error_code.hpp"

namespace rei::core {
    struct url_parts {
        std::string_view scheme;
        std::string_view username;
        std::string_view password;
        std::optional<std::string_view> host;
        std::optional<std::uint16_t> port;
        std::string_view path;
        std::optional<std::string_view> query;
        std::optional<std::string_view> fragment;
    };

    namespace detail {
        template <typename String> struct url_fields {
            String scheme;
            String username;
            String password;
            std::optional<String> host;
            std::optional<std::uint16_t> port;
            String path;
            std::optional<String> query;
            std::optional<String> fragment;

            auto operator==(const url_fields&) const -> bool = default;
        };

        using url_record = detail::url_fields<std::string>;
        using url_record_view = detail::url_fields<std::string_view>;

        template <typename To, typename From>
        auto fields_cast(const detail::url_fields<From>& from) -> detail::url_fields<To> {
            return {
                .scheme = To(from.scheme),
                .username = To(from.username),
                .password = To(from.password),
                .host = std::optional<To>(from.host),
                .port = from.port,
                .path = To(from.path),
                .query = std::optional<To>(from.query),
                .fragment = std::optional<To>(from.fragment),
            };
        }

        template <typename String> auto has_opaque_path(const detail::url_fields<String>& url) -> bool {
            return !url.host && !url.path.starts_with('/');
        }

        auto parse_url(std::string_view input, const detail::url_record_view* base)
            -> std::expected<detail::url_record, rei::error>;
        auto record_from_parts(const core::url_parts& parts) -> std::expected<detail::url_record, rei::error>;
        auto record_from_path(const std::filesystem::path& path)
            -> std::expected<detail::url_record, rei::error>;

        auto serialize_url(const detail::url_record_view& url) -> std::string;
        auto view_serialized(std::string_view text, const detail::url_record& url)
            -> std::expected<detail::url_record_view, rei::error>;

        auto write_scheme(detail::url_record& url, std::string_view value) -> std::expected<void, rei::error>;
        auto write_username(detail::url_record& url, std::string_view value)
            -> std::expected<void, rei::error>;
        auto write_password(detail::url_record& url, std::string_view value)
            -> std::expected<void, rei::error>;
        auto write_host(detail::url_record& url, std::string_view value) -> std::expected<void, rei::error>;
        auto write_port(detail::url_record& url, std::optional<std::uint16_t> port)
            -> std::expected<void, rei::error>;
        auto write_path(detail::url_record& url, std::string_view value) -> std::expected<void, rei::error>;
        auto write_query(detail::url_record& url, std::optional<std::string_view> value)
            -> std::expected<void, rei::error>;
        auto write_fragment(detail::url_record& url, std::optional<std::string_view> value)
            -> std::expected<void, rei::error>;
    } // namespace detail

    class url_view {
      private:
        friend class url;

        detail::url_record_view m_fields;

        url_view() = default;

        static auto _from(detail::url_record_view components) -> core::url_view {
            core::url_view result;
            result.m_fields = components;
            return result;
        }

      public:
        // Fails when `text` isn't a valid URL
        static auto make(std::string_view text) -> std::expected<core::url_view, rei::error> {
            auto record = detail::parse_url(text, nullptr);
            if (!record) {
                return std::unexpected(record.error());
            }

            return detail::view_serialized(text, *record).transform(_from);
        }

        [[nodiscard]] auto scheme() const noexcept -> std::string_view { return m_fields.scheme; }
        [[nodiscard]] auto username() const noexcept -> std::string_view { return m_fields.username; }
        [[nodiscard]] auto password() const noexcept -> std::string_view { return m_fields.password; }
        [[nodiscard]] auto host() const noexcept -> std::optional<std::string_view> { return m_fields.host; }
        [[nodiscard]] auto port() const noexcept -> std::optional<std::uint16_t> { return m_fields.port; }
        [[nodiscard]] auto path() const noexcept -> std::string_view { return m_fields.path; }
        [[nodiscard]] auto query() const noexcept -> std::optional<std::string_view> {
            return m_fields.query;
        }
        [[nodiscard]] auto fragment() const noexcept -> std::optional<std::string_view> {
            return m_fields.fragment;
        }
        [[nodiscard]] auto serialize() const -> std::string { return detail::serialize_url(m_fields); }

        auto operator==(const core::url_view&) const -> bool = default;
    };

    class url {
      private:
        detail::url_record m_fields;

        url() = default;

        static auto _from(detail::url_record components) -> core::url {
            core::url result;
            result.m_fields = std::move(components);
            return result;
        }

        template <typename Writer, typename Value>
        auto _write(Writer writer, const Value& value) -> std::expected<void, rei::error> {
            auto updated = m_fields;
            if (auto result = writer(updated, value); !result) {
                return result;
            }

            m_fields = std::move(updated);
            return {};
        }

      public:
        explicit url(core::url_view view) : m_fields(detail::fields_cast<std::string>(view.m_fields)) {}

        static auto make(std::string_view input) -> std::expected<core::url, rei::error> {
            return detail::parse_url(input, nullptr).transform(_from);
        }

        static auto make(std::string_view input, core::url_view base)
            -> std::expected<core::url, rei::error> {
            return detail::parse_url(input, &base.m_fields).transform(_from);
        }

        static auto make(const core::url_parts& parts) -> std::expected<core::url, rei::error> {
            return detail::record_from_parts(parts).transform(_from);
        }

        static auto from_path(const std::filesystem::path& path) -> std::expected<core::url, rei::error> {
            return detail::record_from_path(path).transform(_from);
        }

        operator core::url_view() const noexcept {
            return url_view::_from(detail::fields_cast<std::string_view>(m_fields));
        }

        [[nodiscard]] auto scheme() const noexcept -> std::string_view { return m_fields.scheme; }
        [[nodiscard]] auto username() const noexcept -> std::string_view { return m_fields.username; }
        [[nodiscard]] auto password() const noexcept -> std::string_view { return m_fields.password; }
        [[nodiscard]] auto host() const noexcept -> std::optional<std::string_view> { return m_fields.host; }
        [[nodiscard]] auto port() const noexcept -> std::optional<std::uint16_t> { return m_fields.port; }
        [[nodiscard]] auto path() const noexcept -> std::string_view { return m_fields.path; }
        [[nodiscard]] auto query() const noexcept -> std::optional<std::string_view> {
            return m_fields.query;
        }
        [[nodiscard]] auto fragment() const noexcept -> std::optional<std::string_view> {
            return m_fields.fragment;
        }
        [[nodiscard]] auto serialize() const -> std::string {
            return detail::serialize_url(detail::fields_cast<std::string_view>(m_fields));
        }

        auto scheme(std::string_view value) -> std::expected<void, rei::error> {
            return _write(detail::write_scheme, value);
        }

        auto username(std::string_view value) -> std::expected<void, rei::error> {
            return _write(detail::write_username, value);
        }

        auto password(std::string_view value) -> std::expected<void, rei::error> {
            return _write(detail::write_password, value);
        }

        auto host(std::string_view value) -> std::expected<void, rei::error> {
            if (detail::has_opaque_path(m_fields)) {
                return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                                  "URLs with an opaque path can't have a host"});
            }

            return _write(detail::write_host, value);
        }

        auto port(std::optional<std::uint16_t> value) -> std::expected<void, rei::error> {
            return _write(detail::write_port, value);
        }

        auto path(std::string_view value) -> std::expected<void, rei::error> {
            if (detail::has_opaque_path(m_fields)) {
                return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                                  "opaque paths can't be changed"});
            }

            return _write(detail::write_path, value);
        }

        auto query(std::optional<std::string_view> value) -> std::expected<void, rei::error> {
            return _write(detail::write_query, value);
        }

        auto fragment(std::optional<std::string_view> value) -> std::expected<void, rei::error> {
            return _write(detail::write_fragment, value);
        }

        auto operator==(const core::url&) const -> bool = default;
    };
} // namespace rei::core
