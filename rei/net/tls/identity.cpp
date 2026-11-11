#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
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
        using certificate_chain = std::vector<bssl::UniquePtr<CRYPTO_BUFFER>>;

        auto parse_chain(std::string_view pem) -> std::expected<certificate_chain, rei::error> {
            auto source = detail::memory_bio(pem);
            if (!source) {
                return std::unexpected(source.error());
            }

            certificate_chain chain;
            while (true) {
                char* name{};
                char* header{};
                std::uint8_t* data{};
                long size{};

                ERR_clear_error();
                if (PEM_read_bio(source->get(), &name, &header, &data, &size) != 1) {
                    break;
                }

                bssl::UniquePtr<char> owned_name{name};
                bssl::UniquePtr<char> owned_header{header};
                bssl::UniquePtr<std::uint8_t> owned_data{data};

                if (std::string_view{name} != PEM_STRING_X509) {
                    continue;
                }

                bssl::UniquePtr<CRYPTO_BUFFER> certificate{
                    CRYPTO_BUFFER_new(data, static_cast<std::size_t>(size), nullptr)};
                bssl::UniquePtr<X509> parsed{certificate ? X509_parse_from_buffer(certificate.get())
                                                         : nullptr};
                if (!parsed) {
                    return detail::failure(std::make_error_code(std::errc::invalid_argument),
                                           "invalid certificate: " + detail::drain_errors());
                }

                chain.push_back(std::move(certificate));
            }

            if (!detail::pem_ended()) {
                return detail::failure(std::make_error_code(std::errc::invalid_argument),
                                       "invalid certificate chain: " + detail::drain_errors());
            }
            if (chain.empty()) {
                return detail::failure(std::make_error_code(std::errc::invalid_argument),
                                       "no certificate found");
            }

            return chain;
        }

        auto refuse_passphrase(char* /*buffer*/, int /*size*/, int /*writing*/, void* context) -> int {
            auto& asked = *static_cast<bool*>(context);
            asked = true;

            return -1;
        }

        auto parse_key(std::string_view pem) -> std::expected<bssl::UniquePtr<EVP_PKEY>, rei::error> {
            auto source = detail::memory_bio(pem);
            if (!source) {
                return std::unexpected(source.error());
            }

            bool encrypted{};
            ERR_clear_error();
            bssl::UniquePtr<EVP_PKEY> key{
                PEM_read_bio_PrivateKey(source->get(), nullptr, refuse_passphrase, &encrypted)};

            if (!key) {
                auto errors = detail::drain_errors();
                if (encrypted) {
                    return detail::failure(std::make_error_code(std::errc::not_supported),
                                           "encrypted private keys aren't supported");
                }

                return detail::failure(std::make_error_code(std::errc::invalid_argument),
                                       "invalid private key: " + errors);
            }

            return key;
        }
    } // namespace

    identity::identity(std::shared_ptr<const identity::chain_and_key> chain_and_key) noexcept
        : m_chain_and_key(std::move(chain_and_key)) {}

    auto identity::load(const std::filesystem::path& certificate_chain,
                        const std::filesystem::path& private_key)
        -> std::expected<tls::identity, rei::error> {
        auto chain = detail::read_file(certificate_chain);
        if (!chain) {
            return std::unexpected(chain.error());
        }

        auto key = detail::read_file(private_key);
        if (!key) {
            return std::unexpected(key.error());
        }

        return identity::make(*chain, *key);
    }

    auto identity::make(std::string_view certificate_chain, std::string_view private_key)
        -> std::expected<tls::identity, rei::error> {
        auto chain = parse_chain(certificate_chain);
        if (!chain) {
            return std::unexpected(chain.error());
        }

        auto key = parse_key(private_key);
        if (!key) {
            return std::unexpected(key.error());
        }

        ERR_clear_error();
        bssl::UniquePtr<X509> leaf{X509_parse_from_buffer(chain->front().get())};
        if (!leaf || X509_check_private_key(leaf.get(), key->get()) != 1) {
            return detail::failure(std::make_error_code(std::errc::invalid_argument),
                                   "the private key doesn't belong to the certificate: " +
                                       detail::drain_errors());
        }

        return identity{std::make_shared<identity::chain_and_key>(std::move(*chain), std::move(*key))};
    }
} // namespace rei::net::tls
