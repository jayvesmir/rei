#include <expected>
#include <source_location>
#include <string>
#include <utility>

#include "rei/core/error.hpp"
#include "rei/core/error_code.hpp"
#include "rei/net/http/connection.hpp"

namespace rei::net::http::detail {
    auto misuse(std::string detail, std::source_location location) -> std::unexpected<rei::error> {
        return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                          std::move(detail), location});
    }
} // namespace rei::net::http::detail
