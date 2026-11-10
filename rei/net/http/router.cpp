#include <algorithm>
#include <compare>
#include <expected>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rei/core/error.hpp"
#include "rei/core/error_code.hpp"
#include "rei/net/http.hpp"
#include "rei/net/http/server.hpp"

namespace rei::net::http {
    namespace {
        using params = std::vector<std::pair<std::string_view, std::string_view>>;

        auto invalid(std::string detail, std::source_location location = std::source_location::current())
            -> std::unexpected<rei::error> {
            return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                              std::move(detail), location});
        }

        constexpr auto is_alpha_numeric(char unit) -> bool {
            return (unit >= '0' && unit <= '9') || (unit >= 'a' && unit <= 'z') ||
                   (unit >= 'A' && unit <= 'Z');
        }

        constexpr auto is_token(std::string_view text) -> bool {
            constexpr std::string_view symbols = "!#$%&'*+-.^_`|~";
            return !text.empty() && std::ranges::all_of(text, [&](char unit) -> bool {
                return is_alpha_numeric(unit) || symbols.contains(unit);
            });
        }

        constexpr auto is_name(std::string_view text) -> bool {
            return !text.empty() && std::ranges::all_of(text, [](char unit) -> bool {
                return is_alpha_numeric(unit) || unit == '_';
            });
        }

        // The segments after the leading '/': "/" is one empty segment and "/a/" is "a" and "".
        class segments {
          private:
            std::optional<std::string_view> m_rest;

          public:
            explicit segments(std::string_view path) : m_rest(path.substr(1)) {}

            [[nodiscard]] auto done() const -> bool { return !m_rest; }
            [[nodiscard]] auto rest() const -> std::string_view { return *m_rest; }

            auto next() -> std::string_view {
                const auto end = m_rest->find('/');
                const auto segment = m_rest->substr(0, end);

                m_rest =
                    end == std::string_view::npos ? std::nullopt : std::optional{m_rest->substr(end + 1)};
                return segment;
            }
        };

        auto parse_pattern(std::string_view pattern) -> std::expected<detail::route, rei::error> {
            detail::route route;

            if (const auto space = pattern.find(' '); space != std::string_view::npos) {
                route.method = pattern.substr(0, space);
                pattern.remove_prefix(space + 1);

                if (!is_token(route.method)) {
                    return invalid("invalid method in route pattern");
                }
            }
            if (!pattern.starts_with('/')) {
                return invalid("route path must start with '/'");
            }

            for (segments path{pattern}; !path.done();) {
                const auto text = path.next();

                if (!route.segments.empty() && route.segments.back().kind == detail::segment_kind::rest) {
                    return invalid("{name...} must be the last segment");
                }

                if (!text.starts_with('{')) {
                    if (text.contains('{') || text.contains('}')) {
                        return invalid("a wildcard must be a whole segment");
                    }

                    route.segments.push_back(
                        {.kind = detail::segment_kind::literal, .text = std::string{text}});
                    continue;
                }

                auto name = text.substr(1, text.size() - 1);
                if (!name.ends_with('}')) {
                    return invalid("a wildcard must be a whole segment");
                }

                name.remove_suffix(1);
                auto kind = detail::segment_kind::param;
                if (name.ends_with("...")) {
                    name.remove_suffix(3);
                    kind = detail::segment_kind::rest;
                }

                const auto taken =
                    std::ranges::any_of(route.segments, [&](const detail::route_segment& segment) -> bool {
                        return segment.kind != detail::segment_kind::literal && segment.text == name;
                    });
                if (!is_name(name) || taken) {
                    return invalid("invalid or repeated wildcard name");
                }

                route.segments.push_back({.kind = kind, .text = std::string{name}});
            }

            return route;
        }

        // Less means more precedence.
        auto compare_shapes(const detail::route& lhs, const detail::route& rhs) -> std::strong_ordering {
            const auto count = std::min(lhs.segments.size(), rhs.segments.size());

            for (auto i = 0uz; i < count; i++) {
                const auto& left = lhs.segments[i];
                const auto& right = rhs.segments[i];

                if (left.kind != right.kind) {
                    return left.kind <=> right.kind;
                }
                if (left.kind == detail::segment_kind::literal && left.text != right.text) {
                    return left.text <=> right.text;
                }
            }

            return lhs.segments.size() <=> rhs.segments.size();
        }

        auto compare_routes(const detail::route& lhs, const detail::route& rhs) -> std::strong_ordering {
            if (const auto shape = compare_shapes(lhs, rhs); std::is_neq(shape)) {
                return shape;
            }
            if (lhs.method.empty() != rhs.method.empty()) {
                return lhs.method.empty() ? std::strong_ordering::greater : std::strong_ordering::less;
            }

            return lhs.method <=> rhs.method;
        }

        auto match(const detail::route& route, std::string_view path, params& captured) -> bool {
            captured.clear();

            // The `*` of an OPTIONS request is not a path.
            if (!path.starts_with('/')) {
                return false;
            }

            segments remaining{path};
            for (const auto& segment : route.segments) {
                if (remaining.done()) {
                    return false;
                }
                if (segment.kind == detail::segment_kind::rest) {
                    captured.emplace_back(segment.text, remaining.rest());
                    return true;
                }

                const auto text = remaining.next();
                if (segment.kind == detail::segment_kind::literal ? text != segment.text : text.empty()) {
                    return false;
                }
                if (segment.kind == detail::segment_kind::param) {
                    captured.emplace_back(segment.text, text);
                }
            }

            return remaining.done();
        }

        constexpr auto hex_value(char unit) -> int {
            if (unit >= '0' && unit <= '9') {
                return unit - '0';
            }
            if (unit >= 'a' && unit <= 'f') {
                return unit - 'a' + 10;
            }
            if (unit >= 'A' && unit <= 'F') {
                return unit - 'A' + 10;
            }

            return -1;
        }

        // Appends to `out`, which has room reserved, so earlier views into it stay valid.
        auto percent_decode(std::string_view text, std::string& out) -> std::string_view {
            const auto start = out.size();

            for (auto i = 0uz; i < text.size(); i++) {
                const auto high = i + 2 < text.size() && text[i] == '%' ? hex_value(text[i + 1]) : -1;
                const auto low = high >= 0 ? hex_value(text[i + 2]) : -1;
                if (low < 0) {
                    out.push_back(text[i]);
                    continue;
                }

                out.push_back(static_cast<char>((high * 16) + low));
                i += 2;
            }

            return std::string_view{out}.substr(start);
        }
    } // namespace

    auto request::param(std::string_view name) const noexcept -> std::string_view {
        const auto found =
            std::ranges::find(m_params, name, &std::pair<std::string_view, std::string_view>::first);
        return found == m_params.end() ? std::string_view{} : found->second;
    }

    auto router::route(std::string_view pattern, http::handler handler) -> std::expected<void, rei::error> {
        auto parsed = parse_pattern(pattern);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }

        const auto position = std::ranges::lower_bound(
            m_routes, *parsed, [](const detail::route& lhs, const detail::route& rhs) -> bool {
                return std::is_lt(compare_routes(lhs, rhs));
            });
        if (position != m_routes.end() && std::is_eq(compare_routes(*position, *parsed))) {
            return invalid("route conflicts with an earlier one");
        }

        parsed->handler = std::move(handler);
        m_routes.insert(position, std::move(*parsed));

        return {};
    }

    auto router::operator()(http::request& request) const -> std::expected<http::response, rei::error> {
        const auto method = request.head().method;
        std::vector<std::string_view> allowed;

        for (auto group = 0uz; group < m_routes.size();) {
            auto group_end = group + 1;
            while (group_end < m_routes.size() &&
                   std::is_eq(compare_shapes(m_routes[group_end], m_routes[group]))) {
                group_end++;
            }

            if (!match(m_routes[group], request.m_path, request.m_params)) {
                group = group_end;
                continue;
            }

            const detail::route* exact = nullptr;
            const detail::route* get = nullptr;
            const detail::route* any = nullptr;
            for (auto i = group; i < group_end; i++) {
                const auto& route = m_routes[i];

                if (route.method == method) {
                    exact = &route;
                } else if (route.method == "GET") {
                    get = &route;
                } else if (route.method.empty()) {
                    any = &route;
                }
            }

            const auto* chosen = exact;
            if (chosen == nullptr) {
                chosen = method == "HEAD" && get != nullptr ? get : any;
            }
            if (chosen == nullptr) {
                for (auto i = group; i < group_end; i++) {
                    allowed.emplace_back(m_routes[i].method);
                }

                group = group_end;
                continue;
            }

            match(*chosen, request.m_path, request.m_params);

            request.m_decoded.clear();
            request.m_decoded.reserve(request.m_path.size());
            for (auto& [name, value] : request.m_params) {
                if (value.contains('%')) {
                    value = percent_decode(value, request.m_decoded);
                }
            }

            return chosen->handler(request);
        }

        if (allowed.empty()) {
            return http::response{.status = http::status::not_found};
        }

        if (std::ranges::contains(allowed, "GET") && !std::ranges::contains(allowed, "HEAD")) {
            allowed.emplace_back("HEAD");
        }
        std::ranges::sort(allowed);
        const auto [first, last] = std::ranges::unique(allowed);
        allowed.erase(first, last);

        std::string allow;
        for (const auto method_name : allowed) {
            allow += allow.empty() ? "" : ", ";
            allow += method_name;
        }

        return http::response{
            .status = http::status::method_not_allowed,
            .headers = {{.name = "Allow", .value = allow}},
        };
    }
} // namespace rei::net::http
