#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <future>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "rei/core/error.hpp"
#include "rei/net/error_code.hpp"
#include "rei/net/socket.hpp"
#include "rei/net/tests/certificates.hpp"
#include "rei/net/tests/support.hpp"
#include "rei/net/tls.hpp"
#include "rei/net/tls/error_code.hpp"

namespace rei::net::tests {
    using namespace std::chrono_literals;
    using namespace std::string_view_literals;

    namespace {
        struct tls_pair {
            tls::stream client;
            tls::stream server;
        };

        struct handshake_options {
            std::vector<std::string_view> trusted{tests::test_ca_certificate};
            std::string_view server_name{"localhost"};
            std::vector<std::string> client_protocols;
            std::vector<std::string> server_protocols;
        };

        auto connect(const tls::connector& connector, const tls::acceptor& acceptor,
                     std::string_view server_name = "localhost") -> std::expected<tls_pair, rei::error> {
            auto sockets = tests::make_tcp_pair();
            if (!sockets) {
                return std::unexpected(sockets.error());
            }

            auto accepting = std::async(std::launch::async, [&] -> std::expected<tls::stream, rei::error> {
                return acceptor.accept(std::move(sockets->accepted));
            });
            auto client = connector.connect(std::move(sockets->client), server_name);
            auto server = accepting.get();
            if (!client || !server) {
                return std::unexpected(client ? server.error() : client.error());
            }

            return tls_pair{.client = std::move(*client), .server = std::move(*server)};
        }

        auto handshake(const handshake_options& options = {}) -> std::expected<tls_pair, rei::error> {
            auto roots = tls::roots::make(options.trusted.front());
            if (!roots) {
                return std::unexpected(roots.error());
            }

            for (auto further : options.trusted | std::views::drop(1)) {
                auto more = tls::roots::make(further);
                if (!more) {
                    return std::unexpected(more.error());
                }

                roots->add(*more);
            }

            auto identity = tls::identity::make(tests::localhost_certificate, tests::localhost_key);
            if (!identity) {
                return std::unexpected(identity.error());
            }

            auto connector =
                tls::connector::make({.roots = *roots, .application_protocols = options.client_protocols});
            auto acceptor = tls::acceptor::make(
                {.identity = *identity, .application_protocols = options.server_protocols});
            if (!connector || !acceptor) {
                return std::unexpected(connector ? acceptor.error() : connector.error());
            }

            return connect(*connector, *acceptor, options.server_name);
        }

        auto make_connector(std::size_t max_cached_sessions) -> std::expected<tls::connector, rei::error> {
            return tls::roots::make(tests::test_ca_certificate)
                .and_then([&](const tls::roots& roots) -> std::expected<tls::connector, rei::error> {
                    return tls::connector::make({.roots = roots, .max_cached_sessions = max_cached_sessions});
                });
        }

        auto make_acceptor() -> std::expected<tls::acceptor, rei::error> {
            return tls::identity::make(tests::localhost_certificate, tests::localhost_key)
                .and_then([](const tls::identity& identity) -> std::expected<tls::acceptor, rei::error> {
                    return tls::acceptor::make({.identity = identity});
                });
        }

        auto receive_tickets(tls_pair& pair) -> std::expected<std::string, rei::error> {
            if (auto sent = pair.server.send(tests::as_bytes("x")); !sent) {
                return std::unexpected(sent.error());
            }

            return tests::receive_exactly(pair.client, 1);
        }
    } // namespace

    TEST(tls, a_handshake_settles_tls_1_3_and_the_servers_preferred_protocol) {
        auto pair =
            handshake({.client_protocols = {"h2", "http/1.1"}, .server_protocols = {"http/1.1", "h2"}});
        ASSERT_TRUE(pair) << pair.error();
        EXPECT_EQ(pair->client.version(), tls::version::tls_1_3);
        EXPECT_EQ(pair->server.version(), tls::version::tls_1_3);
        EXPECT_EQ(pair->client.application_protocol(), "http/1.1");
        EXPECT_EQ(pair->server.application_protocol(), "http/1.1");

        auto unnamed = handshake({.server_protocols = {"http/1.1"}});
        ASSERT_TRUE(unnamed) << unnamed.error();
        EXPECT_EQ(unnamed->server.application_protocol(), "");
        EXPECT_TRUE(
            tests::has_error(handshake({.client_protocols = {"h2"}, .server_protocols = {"http/1.1"}}),
                             tls::error_code::handshake_failed));
    }

    TEST(tls, readable_counts_data_that_is_already_decrypted) {
        auto pair = handshake();
        ASSERT_TRUE(pair) << pair.error();

        ASSERT_TRUE(pair->client.send(tests::as_bytes("ping pong")));
        auto start = tests::receive_exactly(pair->server, 4);
        ASSERT_TRUE(start) << start.error();
        EXPECT_EQ(*start, "ping");
        EXPECT_TRUE(pair->server.readable());

        auto rest = tests::receive_exactly(pair->server, 5);
        ASSERT_TRUE(rest) << rest.error();
        EXPECT_EQ(*rest, " pong");
        EXPECT_FALSE(pair->server.readable());
    }

    TEST(tls, a_certificate_for_another_name_or_from_an_unknown_issuer_is_rejected) {
        EXPECT_TRUE(handshake({.server_name = "127.0.0.1"}));
        EXPECT_TRUE(handshake({.server_name = "::1"}));
        EXPECT_TRUE(tests::has_error(handshake({.server_name = "example.com"}),
                                     tls::error_code::certificate_rejected));
        EXPECT_TRUE(
            tests::has_error(handshake({.server_name = "127.0.0.2"}), tls::error_code::certificate_rejected));

        EXPECT_TRUE(tests::has_error(handshake({.trusted = {tests::stranger_certificate}}),
                                     tls::error_code::certificate_rejected));
        EXPECT_TRUE(handshake({.trusted = {tests::stranger_certificate, tests::test_ca_certificate}}));
    }

    TEST(tls, close_notify_ends_the_stream_and_a_dropped_socket_truncates_it) {
        std::array<std::byte, 16> buffer{};

        auto closed = handshake();
        ASSERT_TRUE(closed) << closed.error();
        ASSERT_TRUE(closed->server.send(tests::as_bytes("bye")));
        ASSERT_TRUE(closed->server.shutdown(net::shutdown_mode::send));
        auto last = tests::receive_exactly(closed->client, 3);
        ASSERT_TRUE(last) << last.error();
        EXPECT_EQ(*last, "bye");
        EXPECT_TRUE(tests::has_error(closed->client.receive(buffer), net::error_code::connection_closed));

        ASSERT_TRUE(closed->client.send(tests::as_bytes("ack")));
        auto after = tests::receive_exactly(closed->server, 3);
        ASSERT_TRUE(after) << after.error();
        EXPECT_EQ(*after, "ack");

        auto dropped = handshake();
        ASSERT_TRUE(dropped) << dropped.error();
        ASSERT_TRUE(dropped->server.socket().close());
        EXPECT_TRUE(tests::has_error(dropped->client.receive(buffer), tls::error_code::truncated));
    }

    TEST(tls, a_handshake_fed_byte_by_byte_fails_at_the_deadline) {
        auto sockets = tests::make_tcp_pair();
        ASSERT_TRUE(sockets) << sockets.error();
        auto roots = tls::roots::make(tests::test_ca_certificate);
        ASSERT_TRUE(roots) << roots.error();
        auto connector = tls::connector::make({.roots = *roots});
        ASSERT_TRUE(connector) << connector.error();

        std::jthread peer{[&](const std::stop_token& stopping) -> void {
            constexpr std::array full_size_handshake_record{
                std::byte{0x16}, std::byte{0x03}, std::byte{0x03}, std::byte{0x40}, std::byte{0x00},
            };
            (void)sockets->accepted.send(full_size_handshake_record);

            while (!stopping.stop_requested() && sockets->accepted.send(tests::as_bytes("x"))) {
                std::this_thread::sleep_for(5ms);
            }
        }};

        ASSERT_TRUE(sockets->client.set_timeout(1s));
        const auto started = std::chrono::steady_clock::now();
        sockets->client.set_deadline(started + 100ms);

        EXPECT_TRUE(tests::has_error(connector->connect(std::move(sockets->client), "localhost"),
                                     net::error_code::timed_out));
        const auto elapsed = std::chrono::steady_clock::now() - started;
        EXPECT_GE(elapsed, 100ms);
        EXPECT_LT(elapsed, 2s);
    }

    TEST(tls, a_socket_timeout_comes_back_unchanged_and_the_stream_stays_usable) {
        auto pair = handshake();
        ASSERT_TRUE(pair) << pair.error();
        ASSERT_TRUE(pair->server.set_timeout(50ms));

        std::array<std::byte, 16> buffer{};
        const auto started = std::chrono::steady_clock::now();
        EXPECT_TRUE(tests::has_error(pair->server.receive(buffer), net::error_code::timed_out));
        EXPECT_GE(std::chrono::steady_clock::now() - started, 50ms);

        ASSERT_TRUE(pair->client.send(tests::as_bytes("late")));
        auto received = tests::receive_exactly(pair->server, 4);
        ASSERT_TRUE(received) << received.error();
        EXPECT_EQ(*received, "late");
    }

    TEST(tls, credentials_are_checked_when_they_are_made) {
        EXPECT_TRUE(tls::identity::make(tests::localhost_certificate, tests::localhost_key));
        EXPECT_TRUE(tests::has_error(tls::identity::make(tests::localhost_certificate, tests::stranger_key),
                                     std::errc::invalid_argument));
        EXPECT_TRUE(tests::has_error(tls::identity::make(tests::localhost_key, tests::localhost_key),
                                     std::errc::invalid_argument));

        std::string damaged{tests::test_ca_certificate};
        damaged.erase(100, 40);
        EXPECT_TRUE(tests::has_error(tls::roots::make(std::string{tests::stranger_certificate} + damaged),
                                     std::errc::invalid_argument));
        EXPECT_TRUE(
            tests::has_error(tls::roots::make("no certificate in here"), std::errc::invalid_argument));
        EXPECT_TRUE(
            tests::has_error(tls::roots::load("no/such/bundle.pem"), std::errc::no_such_file_or_directory));
    }

    TEST(tls, a_second_connection_resumes_unless_the_cache_is_off) {
        auto acceptor = make_acceptor();
        ASSERT_TRUE(acceptor) << acceptor.error();

        for (auto max_cached_sessions : {256uz, 0uz}) {
            auto connector = make_connector(max_cached_sessions);
            ASSERT_TRUE(connector) << connector.error();

            auto first = connect(*connector, *acceptor);
            ASSERT_TRUE(first) << first.error();
            EXPECT_FALSE(first->client.resumed());
            ASSERT_TRUE(receive_tickets(*first));

            auto second = connect(*connector, *acceptor);
            ASSERT_TRUE(second) << second.error();
            EXPECT_EQ(second->client.resumed(), max_cached_sessions > 0);
            EXPECT_EQ(second->server.resumed(), max_cached_sessions > 0);
        }
    }

    TEST(tls, a_session_resumes_one_connection_to_its_own_name_and_the_oldest_is_dropped_first) {
        auto acceptor = make_acceptor();
        ASSERT_TRUE(acceptor) << acceptor.error();
        auto connector = make_connector(256);
        ASSERT_TRUE(connector) << connector.error();
        auto bounded = make_connector(1);
        ASSERT_TRUE(bounded) << bounded.error();

        auto resumes = [&](const tls::connector& with, std::string_view server_name) -> bool {
            auto pair = connect(with, *acceptor, server_name);
            EXPECT_TRUE(pair) << pair.error();
            return pair && pair->client.resumed();
        };

        auto first = connect(*connector, *acceptor, "localhost");
        ASSERT_TRUE(first) << first.error();
        ASSERT_TRUE(receive_tickets(*first));
        EXPECT_FALSE(resumes(*connector, "127.0.0.1"));
        EXPECT_TRUE(resumes(*connector, "localhost"));
        EXPECT_TRUE(resumes(*connector, "localhost"));
        EXPECT_FALSE(resumes(*connector, "localhost"));

        for (auto server_name : {"localhost"sv, "127.0.0.1"sv}) {
            auto pair = connect(*bounded, *acceptor, server_name);
            ASSERT_TRUE(pair) << pair.error();
            ASSERT_TRUE(receive_tickets(*pair));
        }
        EXPECT_FALSE(resumes(*bounded, "localhost"));
        EXPECT_TRUE(resumes(*bounded, "127.0.0.1"));
    }
} // namespace rei::net::tests
