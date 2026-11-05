#include <cstddef>
#include <expected>
#include <format>
#include <string>
#include <string_view>

#include "rei/core/url.hpp"
#include "rei/core/url/detail.hpp"

namespace rei::core::detail {
    auto serialize_url(const detail::url_record_view& url) -> std::string {
        std::string output{url.scheme};
        output += ':';

        if (url.host) {
            output += "//";
            if (detail::includes_credentials(url)) {
                output += url.username;
                if (!url.password.empty()) {
                    output += ':';
                    output += url.password;
                }

                output += '@';
            }

            output += *url.host;
            if (url.port) {
                output += std::format(":{}", *url.port);
            }
        } else if (url.path.starts_with("//")) {
            // Keeps the path's empty first segment from being read back as a host.
            output += "/.";
        }

        output += url.path;
        if (url.query) {
            output += '?';
            output += *url.query;
        }
        if (url.fragment) {
            output += '#';
            output += *url.fragment;
        }

        return output;
    }

    auto view_serialized(std::string_view text, const detail::url_record& url)
        -> std::expected<detail::url_record_view, rei::error> {
        if (detail::serialize_url(detail::fields_cast<std::string_view>(url)) != text) {
            return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                              "a url_view can only view text that is already serialized"});
        }

        detail::url_record_view view;
        view.port = url.port;
        std::size_t offset = 0;
        auto take = [&](std::size_t size) -> std::string_view {
            auto part = text.substr(offset, size);
            offset += size;
            return part;
        };

        view.scheme = take(url.scheme.size());
        offset += 1;

        if (url.host) {
            offset += 2;
            if (detail::includes_credentials(url)) {
                view.username = take(url.username.size());
                if (!url.password.empty()) {
                    offset += 1;
                    view.password = take(url.password.size());
                }

                offset += 1;
            }

            view.host = take(url.host->size());
            if (url.port) {
                offset += std::format(":{}", *url.port).size();
            }
        } else if (url.path.starts_with("//")) {
            offset += 2;
        }

        view.path = take(url.path.size());
        if (url.query) {
            offset += 1;
            view.query = take(url.query->size());
        }
        if (url.fragment) {
            offset += 1;
            view.fragment = take(url.fragment->size());
        }

        return view;
    }
} // namespace rei::core::detail
