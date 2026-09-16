#pragma once

#include <source_location>
#include <string>
#include <system_error>
#include <utility>

namespace rei {
    class error {
      private:
        std::error_code m_code;
        std::string m_detail;
        std::source_location m_location;

      public:
        explicit error(std::error_code code, std::string detail = {},
                       std::source_location location = std::source_location::current()) noexcept
            : m_code(code), m_detail(std::move(detail)), m_location(location) {}

        [[nodiscard]] auto code() const noexcept -> std::error_code { return m_code; }
        [[nodiscard]] auto detail() const noexcept -> const std::string& { return m_detail; }
        [[nodiscard]] auto location() const noexcept -> std::source_location { return m_location; }

        auto operator==(const error& other) const noexcept -> bool { return m_code == other.m_code; }
    };
} // namespace rei
