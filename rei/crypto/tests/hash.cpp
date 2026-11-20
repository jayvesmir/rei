#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "rei/core/error.hpp"
#include "rei/crypto/hash.hpp"

namespace rei::crypto::tests {
    template <crypto::hash_function Hash>
    concept hashable = requires(std::span<const std::byte> data) { crypto::hash<Hash>(data); };

    template <crypto::hash_function Hash, std::size_t Size>
    concept hashable_to = requires(std::span<const std::byte> data) { crypto::hash<Hash, Size>(data); };

    static_assert(hashable<hash_function::sha3_256> && !hashable<hash_function::shake_128>);
    static_assert(hashable_to<hash_function::shake_256, 64> && !hashable_to<hash_function::shake_256, 0> &&
                  hashable_to<hash_function::sha2_256, 32>);
    static_assert(std::same_as<decltype(crypto::hash<hash_function::sha2_384>(std::span<const std::byte>{})),
                               std::expected<std::array<std::byte, 48>, rei::error>>);

    template <typename Digest> auto hex(const std::expected<Digest, rei::error>& digest) -> std::string {
        if (!digest) {
            return "error: " + digest.error().code().message();
        }

        auto text = std::string{};
        for (auto byte : *digest) {
            text += std::format("{:02x}", std::to_integer<std::uint8_t>(byte));
        }

        return text;
    }

    auto bytes(std::string_view text) -> std::span<const std::byte> {
        return std::as_bytes(std::span{text});
    }

    TEST(hash, matches_known_digests_of_abc) {
        const auto data = bytes("abc");

        EXPECT_EQ(hex(crypto::hash<hash_function::md4>(data)), "a448017aaf21d8525fc10ae87aa6729d");
        EXPECT_EQ(hex(crypto::hash<hash_function::md5>(data)), "900150983cd24fb0d6963f7d28e17f72");
        EXPECT_EQ(hex(crypto::hash<hash_function::sha1>(data)), "a9993e364706816aba3e25717850c26c9cd0d89d");
        EXPECT_EQ(hex(crypto::hash<hash_function::sha2_224>(data)),
                  "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7");
        EXPECT_EQ(hex(crypto::hash<hash_function::sha2_256>(data)),
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        EXPECT_EQ(hex(crypto::hash<hash_function::sha2_384>(data)),
                  "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
                  "8086072ba1e7cc2358baeca134c825a7");
        EXPECT_EQ(hex(crypto::hash<hash_function::sha2_512>(data)),
                  "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                  "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
        EXPECT_EQ(hex(crypto::hash<hash_function::sha2_512_224>(data)),
                  "4634270f707b6a54daae7530460842e20e37ed265ceee9a43e8924aa");
        EXPECT_EQ(hex(crypto::hash<hash_function::sha2_512_256>(data)),
                  "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23");
        EXPECT_EQ(hex(crypto::hash<hash_function::sha3_224>(data)),
                  "e642824c3f8cf24ad09234ee7d3c766fc9a3a5168d0c94ad73b46fdf");
        EXPECT_EQ(hex(crypto::hash<hash_function::sha3_256>(data)),
                  "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532");
        EXPECT_EQ(hex(crypto::hash<hash_function::sha3_384>(data)),
                  "ec01498288516fc926459f58e2c6ad8df9b473cb0fc08c2596da7cf0e49be4b2"
                  "98d88cea927ac7f539f1edf228376d25");
        EXPECT_EQ(hex(crypto::hash<hash_function::sha3_512>(data)),
                  "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e"
                  "10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0");
        EXPECT_EQ(hex(crypto::hash<hash_function::blake2b_256>(data)),
                  "bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319");
    }

    TEST(hash, matches_known_digests_of_empty_input) {
        const auto data = std::span<const std::byte>{};

        EXPECT_EQ(hex(crypto::hash<hash_function::md4>(data)), "31d6cfe0d16ae931b73c59d7e0c089c0");
        EXPECT_EQ(hex(crypto::hash<hash_function::md5>(data)), "d41d8cd98f00b204e9800998ecf8427e");
        EXPECT_EQ(hex(crypto::hash<hash_function::sha1>(data)), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
        EXPECT_EQ(hex(crypto::hash<hash_function::sha2_256>(data)),
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        EXPECT_EQ(hex(crypto::hash<hash_function::sha2_512>(data)),
                  "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                  "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
        EXPECT_EQ(hex(crypto::hash<hash_function::sha3_256>(data)),
                  "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a");
        EXPECT_EQ(hex(crypto::hash<hash_function::blake2b_256>(data)),
                  "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8");
        EXPECT_EQ(hex(crypto::hash<hash_function::shake_128, 32>(data)),
                  "7f9c2ba4e88f827d616045507605853ed73b8093f6efbc88eb1a6eacfa66ef26");
    }

    TEST(hash, shake_produces_as_many_bytes_as_requested) {
        const auto data = bytes("abc");

        EXPECT_EQ(hex(crypto::hash<hash_function::shake_128, 1>(data)), "58");
        EXPECT_EQ(hex(crypto::hash<hash_function::shake_128, 32>(data)),
                  "5881092dd818bf5cf8a3ddb793fbcba74097d5c526a6d35f97b83351940f2cc8");
        EXPECT_EQ(hex(crypto::hash<hash_function::shake_256, 64>(data)),
                  "483366601360a8771c6863080cc4114d8db44530f8f1e1ee4f94ea37e78b5739"
                  "d5a15bef186a5386c75744c0527e1faa9f8726e462a12a4feb06bd8801e751e4");

        const auto past_first_block = hex(crypto::hash<hash_function::shake_128, 200>(data));
        ASSERT_EQ(past_first_block.size(), 400uz);
        EXPECT_EQ(past_first_block.substr(336),
                  "6aa01b3f5af057805f973ff8ecb8b226ac32ada6f01c1fcd4818cb006aa5b4cd");
    }
} // namespace rei::crypto::tests
