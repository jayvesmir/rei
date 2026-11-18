#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <utility>

#include "rei/core/error.hpp"

namespace rei::crypto {
    enum class hash_function {
        md4,
        md5,

        sha1,

        sha2_224,
        sha2_256,
        sha2_384,
        sha2_512,

        sha2_512_224,
        sha2_512_256,

        sha3_224,
        sha3_256,
        sha3_384,
        sha3_512,

        shake_128,
        shake_256,

        blake2b_256,
    };

    constexpr auto digest_size(crypto::hash_function function) noexcept -> std::optional<std::size_t> {
        switch (function) {
        case hash_function::md4:
        case hash_function::md5:
            return 16;
        case hash_function::sha1:
            return 20;
        case hash_function::sha2_224:
        case hash_function::sha2_512_224:
        case hash_function::sha3_224:
            return 28;
        case hash_function::sha2_256:
        case hash_function::sha2_512_256:
        case hash_function::sha3_256:
        case hash_function::blake2b_256:
            return 32;
        case hash_function::sha2_384:
        case hash_function::sha3_384:
            return 48;
        case hash_function::sha2_512:
        case hash_function::sha3_512:
            return 64;
        case hash_function::shake_128:
        case hash_function::shake_256:
            return std::nullopt;
        }
    }

    template <crypto::hash_function Hash>
    concept fixed_size_hash_function = crypto::digest_size(Hash).has_value();

    template <crypto::hash_function Hash>
    concept variable_size_hash_function = !crypto::digest_size(Hash).has_value();

    template <crypto::hash_function Hash, std::size_t Size> using digest = std::array<std::byte, Size>;

    namespace detail {
        auto hash(crypto::hash_function function, std::span<const std::byte> data, std::span<std::byte> out)
            -> std::expected<void, rei::error>;
    } // namespace detail

    template <crypto::hash_function Hash, std::size_t Size>
        requires(Size > 0) &&
                (crypto::fixed_size_hash_function<Hash> ? Size == * crypto::digest_size(Hash) : true)
    auto hash(std::span<const std::byte> data) -> std::expected<crypto::digest<Hash, Size>, rei::error> {
        auto out = crypto::digest<Hash, Size>{};
        return detail::hash(Hash, data, out).transform([&] -> crypto::digest<Hash, Size> { return out; });
    }

    template <crypto::hash_function Hash>
        requires crypto::fixed_size_hash_function<Hash>
    auto hash(std::span<const std::byte> data)
        -> std::expected<crypto::digest<Hash, *crypto::digest_size(Hash)>, rei::error> {
        return crypto::hash<Hash, *crypto::digest_size(Hash)>(data);
    }
} // namespace rei::crypto
