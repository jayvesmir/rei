#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rei/core/error.hpp"

namespace rei::net::http {
    enum class version : std::uint8_t {
        http_1_0,
        http_1_1,
    };

    enum class method : std::uint8_t {
        get,
        head,
        post,
        put,
        delete_,
        connect,
        options,
        trace,
        patch,
    };

    constexpr auto to_string(http::method method) noexcept -> std::string_view {
        switch (method) {
        case method::get:
            return "GET";
        case method::head:
            return "HEAD";
        case method::post:
            return "POST";
        case method::put:
            return "PUT";
        case method::delete_:
            return "DELETE";
        case method::connect:
            return "CONNECT";
        case method::options:
            return "OPTIONS";
        case method::trace:
            return "TRACE";
        case method::patch:
            return "PATCH";
        }

        return {};
    }

    enum class status : std::uint16_t {
        continue_ = 100,
        switching_protocols = 101,
        processing = 102,
        early_hints = 103,

        ok = 200,
        created = 201,
        accepted = 202,
        non_authoritative_information = 203,
        no_content = 204,
        reset_content = 205,
        partial_content = 206,
        multi_status = 207,
        already_reported = 208,
        im_used = 226,

        multiple_choices = 300,
        moved_permanently = 301,
        found = 302,
        see_other = 303,
        not_modified = 304,
        use_proxy = 305,
        temporary_redirect = 307,
        permanent_redirect = 308,

        bad_request = 400,
        unauthorized = 401,
        payment_required = 402,
        forbidden = 403,
        not_found = 404,
        method_not_allowed = 405,
        not_acceptable = 406,
        proxy_authentication_required = 407,
        request_timeout = 408,
        conflict = 409,
        gone = 410,
        length_required = 411,
        precondition_failed = 412,
        content_too_large = 413,
        uri_too_long = 414,
        unsupported_media_type = 415,
        range_not_satisfiable = 416,
        expectation_failed = 417,
        misdirected_request = 421,
        unprocessable_content = 422,
        locked = 423,
        failed_dependency = 424,
        too_early = 425,
        upgrade_required = 426,
        precondition_required = 428,
        too_many_requests = 429,
        request_header_fields_too_large = 431,
        unavailable_for_legal_reasons = 451,

        internal_server_error = 500,
        not_implemented = 501,
        bad_gateway = 502,
        service_unavailable = 503,
        gateway_timeout = 504,
        http_version_not_supported = 505,
        variant_also_negotiates = 506,
        insufficient_storage = 507,
        loop_detected = 508,
        network_authentication_required = 511,
    };

    constexpr auto reason_phrase(http::status status) noexcept -> std::string_view {
        switch (status) {
        case status::continue_:
            return "Continue";
        case status::switching_protocols:
            return "Switching Protocols";
        case status::processing:
            return "Processing";
        case status::early_hints:
            return "Early Hints";

        case status::ok:
            return "OK";
        case status::created:
            return "Created";
        case status::accepted:
            return "Accepted";
        case status::non_authoritative_information:
            return "Non-Authoritative Information";
        case status::no_content:
            return "No Content";
        case status::reset_content:
            return "Reset Content";
        case status::partial_content:
            return "Partial Content";
        case status::multi_status:
            return "Multi-Status";
        case status::already_reported:
            return "Already Reported";
        case status::im_used:
            return "IM Used";

        case status::multiple_choices:
            return "Multiple Choices";
        case status::moved_permanently:
            return "Moved Permanently";
        case status::found:
            return "Found";
        case status::see_other:
            return "See Other";
        case status::not_modified:
            return "Not Modified";
        case status::use_proxy:
            return "Use Proxy";
        case status::temporary_redirect:
            return "Temporary Redirect";
        case status::permanent_redirect:
            return "Permanent Redirect";

        case status::bad_request:
            return "Bad Request";
        case status::unauthorized:
            return "Unauthorized";
        case status::payment_required:
            return "Payment Required";
        case status::forbidden:
            return "Forbidden";
        case status::not_found:
            return "Not Found";
        case status::method_not_allowed:
            return "Method Not Allowed";
        case status::not_acceptable:
            return "Not Acceptable";
        case status::proxy_authentication_required:
            return "Proxy Authentication Required";
        case status::request_timeout:
            return "Request Timeout";
        case status::conflict:
            return "Conflict";
        case status::gone:
            return "Gone";
        case status::length_required:
            return "Length Required";
        case status::precondition_failed:
            return "Precondition Failed";
        case status::content_too_large:
            return "Content Too Large";
        case status::uri_too_long:
            return "URI Too Long";
        case status::unsupported_media_type:
            return "Unsupported Media Type";
        case status::range_not_satisfiable:
            return "Range Not Satisfiable";
        case status::expectation_failed:
            return "Expectation Failed";
        case status::misdirected_request:
            return "Misdirected Request";
        case status::unprocessable_content:
            return "Unprocessable Content";
        case status::locked:
            return "Locked";
        case status::failed_dependency:
            return "Failed Dependency";
        case status::too_early:
            return "Too Early";
        case status::upgrade_required:
            return "Upgrade Required";
        case status::precondition_required:
            return "Precondition Required";
        case status::too_many_requests:
            return "Too Many Requests";
        case status::request_header_fields_too_large:
            return "Request Header Fields Too Large";
        case status::unavailable_for_legal_reasons:
            return "Unavailable For Legal Reasons";

        case status::internal_server_error:
            return "Internal Server Error";
        case status::not_implemented:
            return "Not Implemented";
        case status::bad_gateway:
            return "Bad Gateway";
        case status::service_unavailable:
            return "Service Unavailable";
        case status::gateway_timeout:
            return "Gateway Timeout";
        case status::http_version_not_supported:
            return "HTTP Version Not Supported";
        case status::variant_also_negotiates:
            return "Variant Also Negotiates";
        case status::insufficient_storage:
            return "Insufficient Storage";
        case status::loop_detected:
            return "Loop Detected";
        case status::network_authentication_required:
            return "Network Authentication Required";
        }

        return {};
    }

    struct header {
        std::string_view name;
        std::string_view value;

        auto operator==(const header&) const -> bool = default;
    };

    auto find_header(std::span<const http::header> headers, std::string_view name) noexcept
        -> std::optional<std::string_view>;

    struct request_head {
        std::string_view method;
        std::string_view target;
        http::version version{http::version::http_1_1};
        std::vector<http::header> headers;
    };

    struct response_head {
        http::version version{http::version::http_1_1};
        http::status status{http::status::ok};
        std::string_view reason;
        std::vector<http::header> headers;
    };

    // Owned fields in insertion order. Names compare ASCII case-insensitively.
    class headers {
      public:
        using field = std::pair<std::string, std::string>;

      private:
        std::vector<headers::field> m_fields;

      public:
        headers() = default;
        headers(std::initializer_list<http::header> fields);

        [[nodiscard]] auto find(std::string_view name) const noexcept -> std::optional<std::string_view>;
        auto add(std::string_view name, std::string_view value) -> void;
        // Replaces every field called `name`, keeping the position of the first.
        auto set(std::string_view name, std::string_view value) -> void;
        auto remove(std::string_view name) -> void;

        [[nodiscard]] auto begin() const noexcept -> std::vector<headers::field>::const_iterator {
            return m_fields.begin();
        }
        [[nodiscard]] auto end() const noexcept -> std::vector<headers::field>::const_iterator {
            return m_fields.end();
        }

        auto operator==(const headers&) const -> bool = default;
    };

    // Returns 0 at the end of the body. `out` is never empty.
    using body_reader = std::function<auto(std::span<std::byte> out)->std::expected<std::size_t, rei::error>>;

    // Either bytes in memory or a reader that produces them.
    class body {
      private:
        std::vector<std::byte> m_data;
        std::size_t m_read{};
        http::body_reader m_reader;
        std::optional<std::uint64_t> m_size{0};

      public:
        body() = default;
        body(std::vector<std::byte> data);

        // Copies the text.
        template <typename Text>
            requires std::convertible_to<const Text&, std::string_view>
        body(const Text& text) {
            const auto bytes = std::as_bytes(std::span{std::string_view{text}});
            m_data.assign(bytes.begin(), bytes.end());
            m_size = m_data.size();
        }

        // `size` is how many bytes the reader will produce, when that is known.
        body(http::body_reader reader, std::optional<std::uint64_t> size = std::nullopt);

        [[nodiscard]] auto streamed() const noexcept -> bool { return static_cast<bool>(m_reader); }
        [[nodiscard]] auto size() const noexcept -> std::optional<std::uint64_t> { return m_size; }

        // Both are empty for a streamed body.
        [[nodiscard]] auto bytes() const noexcept -> std::span<const std::byte> { return m_data; }
        [[nodiscard]] auto text() const noexcept -> std::string_view {
            return {reinterpret_cast<const char*>(m_data.data()), m_data.size()};
        }

        auto read(std::span<std::byte> out) -> std::expected<std::size_t, rei::error>;
    };

    struct response {
        http::status status{http::status::ok};
        http::headers headers;
        http::body body;
    };
} // namespace rei::net::http
