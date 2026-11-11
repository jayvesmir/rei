#include <array>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "rei/core/error.hpp"
#include "rei/net/tls.hpp"
#include "rei/net/tls/detail.hpp"

namespace rei::net::tls {
    namespace {
        using certificate_list = std::vector<bssl::UniquePtr<X509>>;

        constexpr std::array<std::string_view, 5> system_bundles{
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/pki/tls/certs/ca-bundle.crt",
            "/etc/ssl/ca-bundle.pem",
            "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
            "/etc/ssl/cert.pem",
        };

        auto parse(std::string_view pem, certificate_list& list) -> std::expected<void, rei::error> {
            auto source = detail::memory_bio(pem);
            if (!source) {
                return std::unexpected(source.error());
            }

            while (true) {
                ERR_clear_error();
                bssl::UniquePtr<X509> certificate{
                    PEM_read_bio_X509(source->get(), nullptr, nullptr, nullptr)};
                if (!certificate) {
                    break;
                }

                list.push_back(std::move(certificate));
            }

            if (!detail::pem_ended()) {
                return detail::failure(std::make_error_code(std::errc::invalid_argument),
                                       "invalid certificate: " + detail::drain_errors());
            }

            return {};
        }

        auto parse_file(const std::filesystem::path& file, certificate_list& list)
            -> std::expected<void, rei::error> {
            auto pem = detail::read_file(file);
            if (!pem) {
                return std::unexpected(pem.error());
            }

            return parse(*pem, list);
        }

        auto environment(const std::string& name) -> std::optional<std::string> {
            const auto* value = std::getenv(name.c_str());
            if (value == nullptr || std::string_view{value}.empty()) {
                return std::nullopt;
            }

            return std::string{value};
        }

        auto system_certificates() -> std::expected<certificate_list, rei::error> {
            certificate_list list;
            auto file = environment("SSL_CERT_FILE");
            auto directories = environment("SSL_CERT_DIR");

            if (file) {
                if (auto parsed = parse_file(*file, list); !parsed) {
                    return std::unexpected(parsed.error());
                }
            }

            if (directories) {
                for (auto directory : *directories | std::views::split(':')) {
                    std::error_code failed;
                    std::filesystem::directory_iterator entry{std::string_view{directory}, failed};

                    for (; !failed && entry != std::filesystem::directory_iterator{};
                         entry.increment(failed)) {
                        (void)parse_file(entry->path(), list);
                    }
                }
            }

            if (!file && !directories) {
                for (auto bundle : system_bundles) {
                    std::error_code failed;
                    if (!std::filesystem::exists(bundle, failed)) {
                        continue;
                    }

                    if (auto parsed = parse_file(bundle, list); !parsed) {
                        return std::unexpected(parsed.error());
                    }

                    break;
                }
            }

            if (list.empty()) {
                return detail::failure(std::make_error_code(std::errc::no_such_file_or_directory),
                                       "no system root certificates found: install ca-certificates or set "
                                       "SSL_CERT_FILE");
            }

            return list;
        }
    } // namespace

    roots::roots(std::shared_ptr<const roots::certificates> certificates) noexcept
        : m_certificates(std::move(certificates)) {}

    auto roots::system() -> std::expected<tls::roots, rei::error> {
        static const auto loaded = system_certificates().transform([](certificate_list&& list) -> tls::roots {
            return roots{std::make_shared<roots::certificates>(std::move(list))};
        });

        return loaded;
    }

    auto roots::load(const std::filesystem::path& file) -> std::expected<tls::roots, rei::error> {
        auto pem = detail::read_file(file);
        if (!pem) {
            return std::unexpected(pem.error());
        }

        return roots::make(*pem);
    }

    auto roots::make(std::string_view pem) -> std::expected<tls::roots, rei::error> {
        certificate_list list;
        if (auto parsed = parse(pem, list); !parsed) {
            return std::unexpected(parsed.error());
        }

        if (list.empty()) {
            return detail::failure(std::make_error_code(std::errc::invalid_argument), "no certificate found");
        }

        return roots{std::make_shared<roots::certificates>(std::move(list))};
    }

    auto roots::add(const tls::roots& other) -> void {
        auto merged = std::make_shared<roots::certificates>();

        for (const auto& certificate : *m_certificates) {
            merged->push_back(bssl::UpRef(certificate));
        }
        for (const auto& certificate : *other.m_certificates) {
            merged->push_back(bssl::UpRef(certificate));
        }

        m_certificates = std::move(merged);
    }
} // namespace rei::net::tls
