#pragma once

#include <array>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pool.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "rei/core/error.hpp"
#include "rei/net/socket.hpp"
#include "rei/net/tls.hpp"

namespace rei::net::tls {
    namespace detail {
        struct transport {
            net::socket socket;
            std::optional<rei::error> error;
        };

        inline auto failure(std::error_code code, std::string detail,
                            std::source_location location = std::source_location::current())
            -> std::unexpected<rei::error> {
            return std::unexpected(rei::error{code, std::move(detail), location});
        }

        inline auto drain_errors() -> std::string {
            std::string text;

            while (auto packed = ERR_get_error()) {
                std::array<char, 256> line{};
                ERR_error_string_n(packed, line.data(), line.size());

                if (!text.empty()) {
                    text += "; ";
                }
                text += line.data();
            }

            return text;
        }

        inline auto pem_ended() -> bool {
            auto last = ERR_peek_last_error();
            if (ERR_GET_LIB(last) != ERR_LIB_PEM || ERR_GET_REASON(last) != PEM_R_NO_START_LINE) {
                return false;
            }

            ERR_clear_error();
            return true;
        }

        inline auto read_file(const std::filesystem::path& file) -> std::expected<std::string, rei::error> {
            errno = 0;
            std::ifstream source{file, std::ios::binary};

            std::string text;
            std::array<char, 16uz * 1024> chunk{};
            while (source.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) ||
                   source.gcount() > 0) {
                text.append(chunk.data(), static_cast<std::size_t>(source.gcount()));
            }

            if (!source.is_open() || source.bad()) {
                return detail::failure({errno != 0 ? errno : EIO, std::generic_category()},
                                       "failed to read " + file.string());
            }

            return text;
        }

        inline auto memory_bio(std::string_view data) -> std::expected<bssl::UniquePtr<BIO>, rei::error> {
            bssl::UniquePtr<BIO> bio{BIO_new_mem_buf(data.data(), static_cast<ossl_ssize_t>(data.size()))};
            if (!bio) {
                return detail::failure(std::make_error_code(std::errc::not_enough_memory),
                                       detail::drain_errors());
            }

            return bio;
        }
    } // namespace detail

    struct identity::chain_and_key {
        std::vector<bssl::UniquePtr<CRYPTO_BUFFER>> chain;
        bssl::UniquePtr<EVP_PKEY> key;
    };

    struct stream::state {
        detail::transport transport;
        bssl::UniquePtr<SSL> ssl;

        static auto handshake(net::socket socket, bssl::UniquePtr<SSL> ssl)
            -> std::expected<tls::stream, rei::error>;

        auto failure(int result, std::source_location location = std::source_location::current())
            -> rei::error;
    };
} // namespace rei::net::tls
