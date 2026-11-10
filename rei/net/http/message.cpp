#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "rei/core/ascii.hpp"
#include "rei/core/error.hpp"
#include "rei/net/http.hpp"

namespace rei::net::http {
    headers::headers(std::initializer_list<http::header> fields) {
        m_fields.reserve(fields.size());
        for (const auto& field : fields) {
            add(field.name, field.value);
        }
    }

    auto headers::find(std::string_view name) const noexcept -> std::optional<std::string_view> {
        for (const auto& [field_name, value] : m_fields) {
            if (core::ascii_iequals(field_name, name)) {
                return value;
            }
        }

        return std::nullopt;
    }

    auto headers::add(std::string_view name, std::string_view value) -> void {
        m_fields.emplace_back(name, value);
    }

    auto headers::set(std::string_view name, std::string_view value) -> void {
        const auto named = [name](const headers::field& field) -> bool {
            return core::ascii_iequals(field.first, name);
        };

        const auto first = std::ranges::find_if(m_fields, named);
        if (first == m_fields.end()) {
            add(name, value);
            return;
        }

        first->second = value;
        m_fields.erase(std::remove_if(std::next(first), m_fields.end(), named), m_fields.end());
    }

    auto headers::remove(std::string_view name) -> void {
        std::erase_if(m_fields, [name](const headers::field& field) -> bool {
            return core::ascii_iequals(field.first, name);
        });
    }

    body::body(std::vector<std::byte> data) : m_data(std::move(data)), m_size(m_data.size()) {}

    body::body(http::body_reader reader, std::optional<std::uint64_t> size)
        : m_reader(std::move(reader)), m_size(size) {}

    auto body::read(std::span<std::byte> out) -> std::expected<std::size_t, rei::error> {
        if (m_reader) {
            return m_reader(out);
        }

        const auto unread = std::span{m_data}.subspan(m_read);
        const auto size = std::min(out.size(), unread.size());

        std::ranges::copy(unread.first(size), out.begin());
        m_read += size;

        return size;
    }
} // namespace rei::net::http
