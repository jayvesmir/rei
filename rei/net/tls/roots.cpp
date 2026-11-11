#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "rei/core/error.hpp"
#include "rei/net/tls.hpp"
#include "rei/net/tls/detail.hpp"

#if defined(_WIN32)
#include <windows.h>

#include <wincrypt.h>
#endif

namespace rei::net::tls {
    namespace {
        using certificate_list = std::vector<bssl::UniquePtr<X509>>;

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

#if defined(_WIN32)
        constexpr auto path_separator = ';';
        constexpr std::string_view missing_roots{"no system root certificates found: set SSL_CERT_FILE"};

        auto server_authentication(const CERT_CONTEXT& context) -> bool {
            DWORD size = 0;
            if (!CertGetEnhancedKeyUsage(&context, 0, nullptr, &size)) {
                return false;
            }

            std::vector<std::byte> buffer(size);
            auto& usage = *reinterpret_cast<CERT_ENHKEY_USAGE*>(buffer.data());
            if (!CertGetEnhancedKeyUsage(&context, 0, &usage, &size)) {
                return false;
            }

            if (usage.cUsageIdentifier == 0) {
                return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
            }

            return std::ranges::any_of(
                std::span{usage.rgpszUsageIdentifier, usage.cUsageIdentifier},
                [](const char* oid) { return std::string_view{oid} == szOID_PKIX_KP_SERVER_AUTH; });
        }

        auto parse_system(certificate_list& list) -> std::expected<void, rei::error> {
            auto* store = CertOpenSystemStoreW(0, L"ROOT");
            if (store == nullptr) {
                return detail::failure({static_cast<int>(GetLastError()), std::system_category()},
                                       "failed to open the ROOT certificate store");
            }

            for (auto* context = CertEnumCertificatesInStore(store, nullptr); context != nullptr;
                 context = CertEnumCertificatesInStore(store, context)) {
                if (!server_authentication(*context)) {
                    continue;
                }

                const auto* der = context->pbCertEncoded;
                bssl::UniquePtr<X509> certificate{
                    d2i_X509(nullptr, &der, static_cast<long>(context->cbCertEncoded))};
                if (certificate) {
                    list.push_back(std::move(certificate));
                }
            }

            CertCloseStore(store, 0);
            return {};
        }
#else
        constexpr auto path_separator = ':';
        constexpr std::string_view missing_roots{
            "no system root certificates found: install ca-certificates or set SSL_CERT_FILE"};

        constexpr std::array<std::string_view, 5> system_bundles{
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/pki/tls/certs/ca-bundle.crt",
            "/etc/ssl/ca-bundle.pem",
            "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
            "/etc/ssl/cert.pem",
        };

        auto parse_system(certificate_list& list) -> std::expected<void, rei::error> {
            for (auto bundle : system_bundles) {
                std::error_code failed;
                if (std::filesystem::exists(bundle, failed)) {
                    return parse_file(bundle, list);
                }
            }

            return {};
        }
#endif

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
                for (auto directory : *directories | std::views::split(path_separator)) {
                    std::error_code failed;
                    std::filesystem::directory_iterator entry{std::string_view{directory}, failed};

                    for (; !failed && entry != std::filesystem::directory_iterator{};
                         entry.increment(failed)) {
                        (void)parse_file(entry->path(), list);
                    }
                }
            }

            if (!file && !directories) {
                if (auto parsed = parse_system(list); !parsed) {
                    return std::unexpected(parsed.error());
                }
            }

            if (list.empty()) {
                return detail::failure(std::make_error_code(std::errc::no_such_file_or_directory),
                                       std::string{missing_roots});
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
