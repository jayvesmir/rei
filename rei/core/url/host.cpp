#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "rei/core/url.hpp"
#include "rei/core/url/detail.hpp"
#include "rei/core/utf8.hpp"

namespace rei::core::detail {
    namespace {
        auto is_forbidden_host_code_point(char unit) -> bool {
            return unit == '\0' || std::string_view{"\t\n\r #/:<>?@[\\]^|"}.contains(unit);
        }

        auto is_forbidden_domain_code_point(char unit) -> bool {
            auto byte = static_cast<std::uint8_t>(unit);
            return is_forbidden_host_code_point(unit) || byte <= 0x1F || unit == '%' || byte == 0x7F;
        }

        auto parse_ipv4_number(std::string_view input) -> std::optional<std::uint64_t> {
            if (input.empty()) {
                return std::nullopt;
            }

            std::int32_t radix = 10;
            if (input.starts_with("0x") || input.starts_with("0X")) {
                input.remove_prefix(2);
                radix = 16;
            } else if (input.size() >= 2 && input.front() == '0') {
                input.remove_prefix(1);
                radix = 8;
            }

            if (input.empty()) {
                return 0;
            }

            std::uint64_t value{};
            auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value, radix);
            if (end != input.data() + input.size()) {
                return std::nullopt;
            }

            return error == std::errc::result_out_of_range ? std::numeric_limits<std::uint64_t>::max()
                                                           : value;
        }

        auto ends_in_a_number(std::string_view input) -> bool {
            if (input.ends_with('.')) {
                input.remove_suffix(1);
            }

            auto last = input.substr(input.rfind('.') + 1);
            if (!last.empty() && std::ranges::all_of(last, is_ascii_digit)) {
                return true;
            }

            return detail::parse_ipv4_number(last).has_value();
        }

        auto parse_ipv4(std::string_view input) -> std::expected<std::string, rei::error> {
            if (input.ends_with('.')) {
                input.remove_suffix(1);
            }

            if (std::ranges::count(input, '.') > 3) {
                return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                                  "IPv4 address has more than four parts"});
            }

            std::vector<std::uint64_t> numbers;
            for (auto part : input | std::views::split('.')) {
                auto number = detail::parse_ipv4_number(std::string_view{part});
                if (!number) {
                    return std::unexpected(
                        rei::error{core::make_error_code(core::error_code::invalid_argument),
                                   "IPv4 address has a non-numeric part"});
                }

                numbers.push_back(*number);
            }

            auto address = numbers.back();
            numbers.pop_back();
            if (std::ranges::any_of(numbers, [](std::uint64_t number) -> bool { return number > 255; }) ||
                address >= std::uint64_t{1} << (8 * (4 - numbers.size()))) {
                return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                                  "IPv4 address part is out of range"});
            }

            for (auto i = 0uz; i < numbers.size(); i++) {
                address += numbers[i] << (8 * (3 - i));
            }

            return std::format("{}.{}.{}.{}", address >> 24, (address >> 16) & 0xFF, (address >> 8) & 0xFF,
                               address & 0xFF);
        }

        auto parse_ipv6(std::string_view input) -> std::expected<std::array<std::uint16_t, 8>, rei::error> {
            std::array<std::uint16_t, 8> address{};
            std::size_t piece_index = 0;
            std::optional<std::size_t> compress;
            std::size_t pointer = 0;
            auto unit = [&] -> std::int32_t {
                return pointer < input.size() ? static_cast<std::uint8_t>(input[pointer]) : eof;
            };

            if (unit() == ':') {
                if (!input.substr(1).starts_with(':')) {
                    return std::unexpected(
                        rei::error{core::make_error_code(core::error_code::invalid_argument),
                                   "IPv6 address starts with a single ':'"});
                }

                pointer += 2;
                piece_index++;
                compress = piece_index;
            }

            while (unit() != eof) {
                if (piece_index == 8) {
                    return std::unexpected(
                        rei::error{core::make_error_code(core::error_code::invalid_argument),
                                   "IPv6 address has too many pieces"});
                }

                if (unit() == ':') {
                    if (compress) {
                        return std::unexpected(
                            rei::error{core::make_error_code(core::error_code::invalid_argument),
                                       "IPv6 address has multiple '::'"});
                    }

                    pointer++;
                    piece_index++;
                    compress = piece_index;
                    continue;
                }

                std::uint16_t value = 0;
                auto digits = input.substr(pointer, 4);
                auto length = static_cast<std::size_t>(
                    std::from_chars(digits.data(), digits.data() + digits.size(), value, 16).ptr -
                    digits.data());
                pointer += length;

                if (unit() == '.') {
                    if (length == 0 || piece_index > 6) {
                        return std::unexpected(
                            rei::error{core::make_error_code(core::error_code::invalid_argument),
                                       "IPv6 address has a misplaced IPv4 part"});
                    }

                    pointer -= length;
                    std::size_t numbers_seen = 0;
                    while (unit() != eof) {
                        if (numbers_seen > 0) {
                            if (unit() != '.' || numbers_seen >= 4) {
                                return std::unexpected(
                                    rei::error{core::make_error_code(core::error_code::invalid_argument),
                                               "IPv6 address has an invalid IPv4 part"});
                            }

                            pointer++;
                        }

                        if (!is_ascii_digit(unit())) {
                            return std::unexpected(
                                rei::error{core::make_error_code(core::error_code::invalid_argument),
                                           "IPv6 address has an invalid IPv4 part"});
                        }

                        std::optional<std::uint16_t> ipv4_piece;
                        while (is_ascii_digit(unit())) {
                            auto number = static_cast<std::uint16_t>(unit() - '0');
                            if (ipv4_piece == 0U) {
                                return std::unexpected(
                                    rei::error{core::make_error_code(core::error_code::invalid_argument),
                                               "IPv6 address has an IPv4 part with a leading zero"});
                            }

                            ipv4_piece = static_cast<std::uint16_t>((ipv4_piece.value_or(0) * 10) + number);
                            if (*ipv4_piece > 255) {
                                return std::unexpected(
                                    rei::error{core::make_error_code(core::error_code::invalid_argument),
                                               "IPv6 address has an out of range IPv4 part"});
                            }

                            pointer++;
                        }

                        address.at(piece_index) =
                            static_cast<std::uint16_t>((address.at(piece_index) * 0x100) + *ipv4_piece);
                        numbers_seen++;
                        if (numbers_seen == 2 || numbers_seen == 4) {
                            piece_index++;
                        }
                    }

                    if (numbers_seen != 4) {
                        return std::unexpected(
                            rei::error{core::make_error_code(core::error_code::invalid_argument),
                                       "IPv6 address has too few IPv4 parts"});
                    }

                    break;
                }

                if (unit() == ':') {
                    pointer++;
                    if (unit() == eof) {
                        return std::unexpected(
                            rei::error{core::make_error_code(core::error_code::invalid_argument),
                                       "IPv6 address ends with a single ':'"});
                    }
                } else if (unit() != eof) {
                    return std::unexpected(
                        rei::error{core::make_error_code(core::error_code::invalid_argument),
                                   "IPv6 address has an invalid code point"});
                }

                address.at(piece_index) = value;
                piece_index++;
            }

            if (compress) {
                auto swaps = piece_index - *compress;
                piece_index = 7;
                while (piece_index != 0 && swaps > 0) {
                    std::swap(address.at(piece_index), address.at(*compress + swaps - 1));
                    piece_index--;
                    swaps--;
                }
            } else if (piece_index != 8) {
                return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                                  "IPv6 address has too few pieces"});
            }

            return address;
        }

        auto serialize_ipv6(const std::array<std::uint16_t, 8>& address) -> std::string {
            std::optional<std::size_t> compress;
            std::size_t longest = 1;
            std::size_t run_start = 0;
            std::size_t run_size = 0;
            for (auto i = 0uz; i < address.size(); i++) {
                if (address.at(i) != 0) {
                    run_size = 0;
                    continue;
                }

                if (run_size++ == 0) {
                    run_start = i;
                }
                if (run_size > longest) {
                    longest = run_size;
                    compress = run_start;
                }
            }

            std::string output = "[";
            bool ignore0 = false;
            for (auto i = 0uz; i < address.size(); i++) {
                if (ignore0 && address.at(i) == 0) {
                    continue;
                }

                ignore0 = false;
                if (compress == i) {
                    output += i == 0 ? "::" : ":";
                    ignore0 = true;
                    continue;
                }

                output += std::format("{:x}", address.at(i));
                if (i != 7) {
                    output += ':';
                }
            }

            return output + ']';
        }

        auto parse_opaque_host(std::string_view input) -> std::expected<std::string, rei::error> {
            if (std::ranges::any_of(input, is_forbidden_host_code_point)) {
                return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                                  "host contains a forbidden code point"});
            }

            return detail::percent_encode(input, encode_set::c0_control);
        }
    } // namespace

    auto parse_host(std::string_view input, bool is_opaque) -> std::expected<std::string, rei::error> {
        if (input.starts_with('[')) {
            if (!input.ends_with(']')) {
                return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                                  "IPv6 address is missing its closing ']'"});
            }

            return detail::parse_ipv6(input.substr(1, input.size() - 2)).transform(detail::serialize_ipv6);
        }

        if (is_opaque) {
            return detail::parse_opaque_host(input);
        }

        auto domain = detail::percent_decode(input);
        if (!utf8::is_valid(domain)) {
            return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                              "domain is not valid UTF-8"});
        }

        if (std::ranges::any_of(domain,
                                [](char unit) -> bool { return static_cast<std::uint8_t>(unit) > 0x7F; })) {
            return std::unexpected(
                rei::error{core::make_error_code(core::error_code::not_supported),
                           "non-ASCII domains need IDNA processing, which isn't implemented"});
        }

        std::ranges::transform(domain, domain.begin(), to_ascii_lower);
        if (domain.empty()) {
            return std::unexpected(
                rei::error{core::make_error_code(core::error_code::invalid_argument), "domain is empty"});
        }
        if (std::ranges::any_of(domain, is_forbidden_domain_code_point)) {
            return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                              "domain contains a forbidden code point"});
        }

        if (detail::ends_in_a_number(domain)) {
            return detail::parse_ipv4(domain);
        }

        return domain;
    }
} // namespace rei::core::detail
