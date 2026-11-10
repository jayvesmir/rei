#include <optional>
#include <span>
#include <string_view>

#include "rei/core/ascii.hpp"
#include "rei/net/http.hpp"

namespace rei::net::http {
    auto find_header(std::span<const http::header> headers, std::string_view name) noexcept
        -> std::optional<std::string_view> {
        for (const auto& header : headers) {
            if (core::ascii_iequals(header.name, name)) {
                return header.value;
            }
        }

        return std::nullopt;
    }
} // namespace rei::net::http
