/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>

#include <security/attestation/backends/fake/fake_backend.h>
#include <streaming/attestation/stream.h>
#include <streaming/spsc_queue/stream.h>

#include <coro/coro.hpp>
#include <rpc/rpc.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>

using namespace std::chrono_literals;

namespace
{
    using canopy::security::attestation::attestation_policy;
    using canopy::security::attestation::attestation_service;
    using canopy::security::attestation::attestation_service_options;
    using canopy::security::attestation::fake_backend;
    using canopy::security::attestation::identity;
    using streaming::attestation::stream_options;

    auto make_scheduler() -> std::shared_ptr<coro::scheduler>
    {
        return std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
                .pool = coro::thread_pool::options{.thread_count = 2},
                .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));
    }

    struct stream_pair
    {
        std::shared_ptr<streaming::spsc_queue::queue_type> a_to_b;
        std::shared_ptr<streaming::spsc_queue::queue_type> b_to_a;
        std::shared_ptr<streaming::spsc_queue::stream> a;
        std::shared_ptr<streaming::spsc_queue::stream> b;
    };

    auto make_stream_pair(std::shared_ptr<coro::scheduler> scheduler) -> stream_pair
    {
        stream_pair pair;
        pair.a_to_b = std::make_shared<streaming::spsc_queue::queue_type>();
        pair.b_to_a = std::make_shared<streaming::spsc_queue::queue_type>();
        pair.a = std::make_shared<streaming::spsc_queue::stream>(pair.a_to_b, pair.b_to_a, scheduler);
        pair.b = std::make_shared<streaming::spsc_queue::stream>(pair.b_to_a, pair.a_to_b, scheduler);
        return pair;
    }

    auto make_service(
        std::string security_domain_id,
        std::string zone_id,
        bool send_evidence,
        bool require_peer_evidence,
        bool allow_unattested_peer = false) -> std::shared_ptr<attestation_service>
    {
        attestation_service_options options;
        options.local_identity = identity{std::move(security_domain_id), std::move(zone_id)};
        options.backend = std::make_shared<fake_backend>();
        options.policy = attestation_policy{};
        options.policy.send_local_evidence = send_evidence;
        options.policy.require_peer_evidence = require_peer_evidence;
        options.policy.allow_unattested_peer = allow_unattested_peer;
        options.policy.allow_development_evidence = true;
        options.policy.required_backend_id = canopy::security::attestation::fake_backend_id;
        return std::make_shared<attestation_service>(std::move(options));
    }

    auto make_options(std::shared_ptr<attestation_service> service) -> stream_options
    {
        stream_options options;
        options.service = std::move(service);
        options.transcript_id = 42;
        options.handshake_timeout = 2s;
        return options;
    }

    auto send_string(
        std::shared_ptr<streaming::attestation::stream> from,
        std::shared_ptr<streaming::attestation::stream> to,
        const std::string& value) -> coro::task<std::string>
    {
        auto status = co_await from->send(rpc::byte_span{value});
        if (!status.is_ok())
            co_return {};

        std::array<uint8_t, 128> buffer{};
        auto [receive_status, bytes] = co_await to->receive(rpc::mutable_byte_span{buffer}, 1s);
        if (!receive_status.is_ok())
            co_return {};

        co_return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
} // namespace

TEST(
    AttestationStream,
    MutualFakeAttestationAllowsServerToServerTraffic)
{
    auto scheduler = make_scheduler();
    auto pair = make_stream_pair(scheduler);

    auto server_a_service = make_service("domain-a", "zone-a", true, true);
    auto server_b_service = make_service("domain-b", "zone-b", true, true);
    auto server_a = std::make_shared<streaming::attestation::stream>(pair.a, make_options(server_a_service));
    auto server_b = std::make_shared<streaming::attestation::stream>(pair.b, make_options(server_b_service));

    bool a_ok = false;
    bool b_ok = false;
    coro::sync_wait(
        coro::when_all(
            [&]() -> coro::task<void> { a_ok = co_await server_a->client_handshake(); }(),
            [&]() -> coro::task<void> { b_ok = co_await server_b->server_handshake(); }()));

    EXPECT_TRUE(a_ok);
    EXPECT_TRUE(b_ok);
    EXPECT_TRUE(server_a->handshake_complete());
    EXPECT_TRUE(server_b->handshake_complete());

    auto a_context = server_a->security_context();
    auto b_context = server_b->security_context();
    EXPECT_TRUE(a_context.established);
    EXPECT_TRUE(b_context.established);
    EXPECT_TRUE(a_context.local_evidence_sent);
    EXPECT_TRUE(b_context.local_evidence_sent);
    EXPECT_TRUE(a_context.peer_attested);
    EXPECT_TRUE(b_context.peer_attested);
    EXPECT_EQ(a_context.peer_identity.security_domain_id, "domain-b");
    EXPECT_EQ(b_context.peer_identity.security_domain_id, "domain-a");
    EXPECT_EQ(a_context.backend_id, "fake");
    EXPECT_EQ(b_context.backend_id, "fake");
    EXPECT_EQ(a_context.session_id, b_context.session_id);
    EXPECT_EQ(server_a_service->session_count(), 1U);
    EXPECT_EQ(server_b_service->session_count(), 1U);
    EXPECT_TRUE(server_a_service->find_session(a_context.session_id).has_value());
    EXPECT_TRUE(server_b_service->find_session(b_context.session_id).has_value());

    auto received = coro::sync_wait(send_string(server_a, server_b, "attested server to server"));
    EXPECT_EQ(received, "attested server to server");

    coro::sync_wait(server_a->set_closed());
    coro::sync_wait(server_b->set_closed());
    scheduler->shutdown();
}

TEST(
    AttestationStream,
    UnattestedClientCanVerifyAttestedServer)
{
    auto scheduler = make_scheduler();
    auto pair = make_stream_pair(scheduler);

    auto client_service = make_service("client-process", "client-zone", false, true);
    auto server_service = make_service("server-domain", "server-zone", true, false, true);
    auto client = std::make_shared<streaming::attestation::stream>(pair.a, make_options(client_service));
    auto server = std::make_shared<streaming::attestation::stream>(pair.b, make_options(server_service));

    bool client_ok = false;
    bool server_ok = false;
    coro::sync_wait(
        coro::when_all(
            [&]() -> coro::task<void> { client_ok = co_await client->client_handshake(); }(),
            [&]() -> coro::task<void> { server_ok = co_await server->server_handshake(); }()));

    EXPECT_TRUE(client_ok);
    EXPECT_TRUE(server_ok);

    auto client_context = client->security_context();
    auto server_context = server->security_context();
    EXPECT_TRUE(client_context.established);
    EXPECT_TRUE(server_context.established);
    EXPECT_FALSE(client_context.local_evidence_sent);
    EXPECT_TRUE(server_context.local_evidence_sent);
    EXPECT_TRUE(client_context.peer_attested);
    EXPECT_FALSE(server_context.peer_attested);
    EXPECT_EQ(client_context.peer_identity.security_domain_id, "server-domain");
    EXPECT_EQ(server_context.peer_identity.security_domain_id, "client-process");
    EXPECT_EQ(client_context.backend_id, "fake");
    EXPECT_EQ(server_context.backend_id, "fake");
    EXPECT_EQ(client_service->session_count(), 1U);
    EXPECT_EQ(server_service->session_count(), 1U);
    EXPECT_TRUE(client_service->find_session(client_context.session_id).has_value());
    EXPECT_TRUE(server_service->find_session(server_context.session_id).has_value());

    auto received = coro::sync_wait(send_string(client, server, "unattested client request"));
    EXPECT_EQ(received, "unattested client request");

    coro::sync_wait(client->set_closed());
    coro::sync_wait(server->set_closed());
    scheduler->shutdown();
}

TEST(
    AttestationStream,
    UnattestedClientIsRejectedUnlessServerPolicyAllowsIt)
{
    auto scheduler = make_scheduler();
    auto pair = make_stream_pair(scheduler);

    auto client_service = make_service("client-process", "client-zone", false, true);
    auto server_service = make_service("server-domain", "server-zone", true, false);
    auto client = std::make_shared<streaming::attestation::stream>(pair.a, make_options(client_service));
    auto server = std::make_shared<streaming::attestation::stream>(pair.b, make_options(server_service));

    bool client_ok = true;
    bool server_ok = true;
    coro::sync_wait(
        coro::when_all(
            [&]() -> coro::task<void> { client_ok = co_await client->client_handshake(); }(),
            [&]() -> coro::task<void> { server_ok = co_await server->server_handshake(); }()));

    EXPECT_FALSE(client_ok);
    EXPECT_FALSE(server_ok);
    EXPECT_FALSE(client->handshake_complete());
    EXPECT_FALSE(server->handshake_complete());
    EXPECT_EQ(client_service->session_count(), 0U);
    EXPECT_EQ(server_service->session_count(), 0U);

    coro::sync_wait(client->set_closed());
    coro::sync_wait(server->set_closed());
    scheduler->shutdown();
}
