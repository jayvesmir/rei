#include "rei/crypto/hash.hpp"

#include <cstdint>
#include <system_error>

#include <openssl/blake2.h>
#include <openssl/digest.h>
#include <openssl/md4.h>
#include <openssl/md5.h>
#include <openssl/sha.h>

namespace rei::crypto {
    namespace {
        auto input(std::span<const std::byte> data) noexcept -> const std::uint8_t* {
            return reinterpret_cast<const std::uint8_t*>(data.data());
        }

        auto output(std::span<std::byte> out) noexcept -> std::uint8_t* {
            return reinterpret_cast<std::uint8_t*>(out.data());
        }

        auto evp_digest(const EVP_MD& digest, std::span<const std::byte> data, std::span<std::byte> out)
            -> std::expected<void, rei::error> {
            bssl::ScopedEVP_MD_CTX context;
            const auto extendable = (EVP_MD_flags(&digest) & EVP_MD_FLAG_XOF) != 0;
            const auto finished = EVP_DigestInit_ex(context.get(), &digest, nullptr) == 1 &&
                                  EVP_DigestUpdate(context.get(), data.data(), data.size()) == 1 &&
                                  (extendable ? EVP_DigestFinalXOF(context.get(), output(out), out.size())
                                              : EVP_DigestFinal_ex(context.get(), output(out), nullptr)) == 1;
            if (!finished) {
                return std::unexpected(rei::error{std::make_error_code(std::errc::not_enough_memory)});
            }

            return {};
        }
    } // namespace

    auto detail::hash(crypto::hash_function function, std::span<const std::byte> data,
                      std::span<std::byte> out) -> std::expected<void, rei::error> {
        switch (function) {
        case hash_function::md4:
            MD4(input(data), data.size(), output(out));
            return {};
        case hash_function::md5:
            MD5(input(data), data.size(), output(out));
            return {};
        case hash_function::sha1:
            SHA1(input(data), data.size(), output(out));
            return {};
        case hash_function::sha2_224:
            SHA224(input(data), data.size(), output(out));
            return {};
        case hash_function::sha2_256:
            SHA256(input(data), data.size(), output(out));
            return {};
        case hash_function::sha2_384:
            SHA384(input(data), data.size(), output(out));
            return {};
        case hash_function::sha2_512:
            SHA512(input(data), data.size(), output(out));
            return {};
        case hash_function::sha2_512_224:
            SHA512_224(input(data), data.size(), output(out));
            return {};
        case hash_function::sha2_512_256:
            SHA512_256(input(data), data.size(), output(out));
            return {};
        case hash_function::sha3_224:
            return evp_digest(*EVP_sha3_224(), data, out);
        case hash_function::sha3_256:
            return evp_digest(*EVP_sha3_256(), data, out);
        case hash_function::sha3_384:
            return evp_digest(*EVP_sha3_384(), data, out);
        case hash_function::sha3_512:
            return evp_digest(*EVP_sha3_512(), data, out);
        case hash_function::shake_128:
            return evp_digest(*EVP_shake128(), data, out);
        case hash_function::shake_256:
            return evp_digest(*EVP_shake256(), data, out);
        case hash_function::blake2b_256:
            BLAKE2B256(input(data), data.size(), output(out));
            return {};
        }

        std::unreachable();
    }
} // namespace rei::crypto
