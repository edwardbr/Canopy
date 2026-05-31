/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <streaming/attestation/stream.h>

#include <array>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <rpc/rpc.h>

namespace streaming::attestation
{
    namespace
    {
        using canopy::security::attestation::cmw;
        using canopy::security::attestation::establish_session_params;
        using canopy::security::attestation::evidence_binding;
        using canopy::security::attestation::identity;

        constexpr uint32_t frame_magic = 0x31544143U; // "CAT1", little-endian
        constexpr uint16_t frame_version = 1;
        constexpr size_t frame_header_size
            = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint32_t);
        constexpr size_t max_frame_payload = 1024 * 1024;

        enum class frame_kind : uint16_t
        {
            client_hello_attest = 1,
            server_hello_attest = 2,
            evidence = 3,
            evidence_verdict = 4
        };

        struct frame
        {
            frame_kind kind{frame_kind::client_hello_attest};
            uint64_t transcript_id{0};
            cmw body;
        };

        struct hello_payload
        {
            identity local_identity;
            std::string backend_id;
            bool will_send_evidence{false};
            bool requires_peer_evidence{false};
        };

        struct verdict_payload
        {
            bool accepted{false};
            std::string reason;
        };

        auto ok_status() noexcept -> coro::net::io_status
        {
            return coro::net::io_status{.type = coro::net::io_status::kind::ok};
        }

        auto closed_status() noexcept -> coro::net::io_status
        {
            return coro::net::io_status{.type = coro::net::io_status::kind::closed};
        }

        auto protocol_status() noexcept -> coro::net::io_status
        {
            return coro::net::io_status{
                .type = coro::net::io_status::kind::native, .native_code = rpc::error::PROTOCOL_ERROR()};
        }

        auto can_append(
            const std::vector<uint8_t>& out,
            size_t size) noexcept -> bool
        {
            return size <= out.max_size() - out.size();
        }

        auto has_remaining(
            const std::vector<uint8_t>& in,
            size_t offset,
            size_t size) noexcept -> bool
        {
            return offset <= in.size() && size <= in.size() - offset;
        }

        auto fits_u32(size_t value) noexcept -> bool
        {
            return value <= static_cast<size_t>(std::numeric_limits<uint32_t>::max());
        }

        auto append_u16(
            std::vector<uint8_t>& out,
            uint16_t value) -> bool
        {
            if (!can_append(out, sizeof(value)))
                return false;
            for (unsigned shift = 0; shift < 16; shift += 8)
                out.push_back(static_cast<uint8_t>((value >> shift) & 0xffU));
            return true;
        }

        auto append_u32(
            std::vector<uint8_t>& out,
            uint32_t value) -> bool
        {
            if (!can_append(out, sizeof(value)))
                return false;
            for (unsigned shift = 0; shift < 32; shift += 8)
                out.push_back(static_cast<uint8_t>((value >> shift) & 0xffU));
            return true;
        }

        auto append_u64(
            std::vector<uint8_t>& out,
            uint64_t value) -> bool
        {
            if (!can_append(out, sizeof(value)))
                return false;
            for (unsigned shift = 0; shift < 64; shift += 8)
                out.push_back(static_cast<uint8_t>((value >> shift) & 0xffU));
            return true;
        }

        auto append_bool(
            std::vector<uint8_t>& out,
            bool value) -> bool
        {
            if (!can_append(out, sizeof(uint8_t)))
                return false;
            out.push_back(value ? 1 : 0);
            return true;
        }

        auto append_bytes(
            std::vector<uint8_t>& out,
            const std::vector<uint8_t>& bytes) -> bool
        {
            if (bytes.size() > max_frame_payload || !fits_u32(bytes.size()))
                return false;
            if (!can_append(out, sizeof(uint32_t) + bytes.size()))
                return false;
            if (!append_u32(out, static_cast<uint32_t>(bytes.size())))
                return false;
            out.insert(out.end(), bytes.begin(), bytes.end());
            return true;
        }

        auto append_string(
            std::vector<uint8_t>& out,
            const std::string& value) -> bool
        {
            if (value.size() > max_frame_payload || !fits_u32(value.size()))
                return false;
            if (!can_append(out, sizeof(uint32_t) + value.size()))
                return false;
            if (!append_u32(out, static_cast<uint32_t>(value.size())))
                return false;
            out.insert(out.end(), value.begin(), value.end());
            return true;
        }

        auto read_u16(
            const std::vector<uint8_t>& in,
            size_t& offset,
            uint16_t& value) -> bool
        {
            if (!has_remaining(in, offset, sizeof(uint16_t)))
                return false;
            value = 0;
            for (unsigned shift = 0; shift < 16; shift += 8)
                value |= static_cast<uint16_t>(in[offset++]) << shift;
            return true;
        }

        auto read_u32(
            const std::vector<uint8_t>& in,
            size_t& offset,
            uint32_t& value) -> bool
        {
            if (!has_remaining(in, offset, sizeof(uint32_t)))
                return false;
            value = 0;
            for (unsigned shift = 0; shift < 32; shift += 8)
                value |= static_cast<uint32_t>(in[offset++]) << shift;
            return true;
        }

        auto read_u64(
            const std::vector<uint8_t>& in,
            size_t& offset,
            uint64_t& value) -> bool
        {
            if (!has_remaining(in, offset, sizeof(uint64_t)))
                return false;
            value = 0;
            for (unsigned shift = 0; shift < 64; shift += 8)
                value |= static_cast<uint64_t>(in[offset++]) << shift;
            return true;
        }

        auto read_bool(
            const std::vector<uint8_t>& in,
            size_t& offset,
            bool& value) -> bool
        {
            if (!has_remaining(in, offset, sizeof(uint8_t)))
                return false;
            const auto raw = in[offset++];
            if (raw > 1)
                return false;
            value = raw != 0;
            return true;
        }

        auto read_bytes(
            const std::vector<uint8_t>& in,
            size_t& offset,
            std::vector<uint8_t>& value) -> bool
        {
            uint32_t size = 0;
            if (!read_u32(in, offset, size))
                return false;
            if (size > max_frame_payload || !has_remaining(in, offset, size))
                return false;
            value.assign(
                in.begin() + static_cast<std::ptrdiff_t>(offset), in.begin() + static_cast<std::ptrdiff_t>(offset + size));
            offset += size;
            return true;
        }

        auto read_string(
            const std::vector<uint8_t>& in,
            size_t& offset,
            std::string& value) -> bool
        {
            std::vector<uint8_t> bytes;
            if (!read_bytes(in, offset, bytes))
                return false;
            value.assign(bytes.begin(), bytes.end());
            return true;
        }

        auto serialise_cmw(const cmw& value) -> std::optional<std::vector<uint8_t>>
        {
            std::vector<uint8_t> out;
            if (!append_string(out, value.media_type))
                return std::nullopt;
            if (!append_string(out, value.content_format))
                return std::nullopt;
            if (!append_bytes(out, value.payload))
                return std::nullopt;
            if (out.size() > max_frame_payload)
                return std::nullopt;
            return out;
        }

        auto parse_cmw(
            const std::vector<uint8_t>& payload,
            cmw& value) -> bool
        {
            size_t offset = 0;
            if (!read_string(payload, offset, value.media_type))
                return false;
            if (!read_string(payload, offset, value.content_format))
                return false;
            if (!read_bytes(payload, offset, value.payload))
                return false;
            return offset == payload.size();
        }

        auto serialise_hello(const hello_payload& hello) -> std::optional<std::vector<uint8_t>>
        {
            std::vector<uint8_t> out;
            if (!append_string(out, hello.local_identity.enclave_id))
                return std::nullopt;
            if (!append_string(out, hello.local_identity.zone_id))
                return std::nullopt;
            if (!append_string(out, hello.backend_id))
                return std::nullopt;
            if (!append_bool(out, hello.will_send_evidence))
                return std::nullopt;
            if (!append_bool(out, hello.requires_peer_evidence))
                return std::nullopt;
            return out;
        }

        auto parse_hello(
            const std::vector<uint8_t>& payload,
            hello_payload& hello) -> bool
        {
            size_t offset = 0;
            if (!read_string(payload, offset, hello.local_identity.enclave_id))
                return false;
            if (!read_string(payload, offset, hello.local_identity.zone_id))
                return false;
            if (!read_string(payload, offset, hello.backend_id))
                return false;
            if (!read_bool(payload, offset, hello.will_send_evidence))
                return false;
            if (!read_bool(payload, offset, hello.requires_peer_evidence))
                return false;
            return offset == payload.size();
        }

        auto serialise_verdict(const verdict_payload& verdict) -> std::optional<std::vector<uint8_t>>
        {
            std::vector<uint8_t> out;
            if (!append_bool(out, verdict.accepted))
                return std::nullopt;
            if (!append_string(out, verdict.reason))
                return std::nullopt;
            return out;
        }

        auto parse_verdict(
            const std::vector<uint8_t>& payload,
            verdict_payload& verdict) -> bool
        {
            size_t offset = 0;
            if (!read_bool(payload, offset, verdict.accepted))
                return false;
            if (!read_string(payload, offset, verdict.reason))
                return false;
            return offset == payload.size();
        }

        auto make_nonce(
            uint64_t transcript_id,
            const identity& subject,
            handshake_role role) -> std::optional<std::vector<uint8_t>>
        {
            std::vector<uint8_t> nonce;
            if (!append_u64(nonce, transcript_id))
                return std::nullopt;
            if (!append_string(nonce, subject.enclave_id))
                return std::nullopt;
            if (!append_string(nonce, subject.zone_id))
                return std::nullopt;
            if (!append_string(nonce, role == handshake_role::client ? "client" : "server"))
                return std::nullopt;
            return nonce;
        }

        auto make_cmw(
            std::string media_type,
            std::string content_format,
            std::vector<uint8_t> payload) -> cmw
        {
            cmw value;
            value.media_type = std::move(media_type);
            value.content_format = std::move(content_format);
            value.payload = std::move(payload);
            return value;
        }

        auto serialise_frame(const frame& value) -> std::optional<std::vector<uint8_t>>
        {
            auto payload = serialise_cmw(value.body);
            if (!payload || payload->size() > max_frame_payload || !fits_u32(payload->size()))
                return std::nullopt;
            std::vector<uint8_t> out;
            if (!can_append(out, frame_header_size + payload->size()))
                return std::nullopt;
            out.reserve(frame_header_size + payload->size());
            if (!append_u32(out, frame_magic))
                return std::nullopt;
            if (!append_u16(out, frame_version))
                return std::nullopt;
            if (!append_u16(out, static_cast<uint16_t>(value.kind)))
                return std::nullopt;
            if (!append_u64(out, value.transcript_id))
                return std::nullopt;
            if (!append_u32(out, static_cast<uint32_t>(payload->size())))
                return std::nullopt;
            out.insert(out.end(), payload->begin(), payload->end());
            return out;
        }

        auto parse_frame_header(
            const std::vector<uint8_t>& header,
            frame_kind& kind,
            uint64_t& transcript_id,
            uint32_t& payload_size) -> bool
        {
            size_t offset = 0;
            uint32_t magic = 0;
            uint16_t version = 0;
            uint16_t raw_kind = 0;
            if (!read_u32(header, offset, magic))
                return false;
            if (!read_u16(header, offset, version))
                return false;
            if (!read_u16(header, offset, raw_kind))
                return false;
            if (!read_u64(header, offset, transcript_id))
                return false;
            if (!read_u32(header, offset, payload_size))
                return false;
            if (offset != header.size() || magic != frame_magic || version != frame_version)
                return false;
            if (payload_size > max_frame_payload)
                return false;
            switch (static_cast<frame_kind>(raw_kind))
            {
            case frame_kind::client_hello_attest:
            case frame_kind::server_hello_attest:
            case frame_kind::evidence:
            case frame_kind::evidence_verdict:
                kind = static_cast<frame_kind>(raw_kind);
                return true;
            }
            return false;
        }

        auto hello_media_type() -> std::string
        {
            return "application/canopy-attestation-hello";
        }

        auto verdict_media_type() -> std::string
        {
            return "application/canopy-attestation-verdict";
        }

        auto read_exact(
            const std::shared_ptr<::streaming::stream>& underlying,
            std::vector<uint8_t>& buffer,
            std::chrono::milliseconds timeout) -> coro::task<coro::net::io_status>
        {
            size_t offset = 0;
            while (offset < buffer.size())
            {
                auto target = rpc::mutable_byte_span{buffer.data() + offset, buffer.size() - offset};
                auto [status, bytes] = co_await underlying->receive(target, timeout);
                if (!status.is_ok())
                    co_return status;
                if (bytes.empty())
                    co_return coro::net::io_status{.type = coro::net::io_status::kind::timeout};
                offset += bytes.size();
            }
            co_return ok_status();
        }
    } // namespace

    struct stream::impl
    {
        impl(
            std::shared_ptr<::streaming::stream> raw_stream,
            stream_options stream_opts)
            : underlying(std::move(raw_stream))
            , options(std::move(stream_opts))
        {
        }

        auto send_frame(const frame& value) -> coro::task<coro::net::io_status>
        {
            if (!underlying)
                co_return closed_status();
            auto bytes = serialise_frame(value);
            if (!bytes)
                co_return protocol_status();
            co_return co_await underlying->send(rpc::byte_span{*bytes});
        }

        auto receive_frame() -> coro::task<std::pair<
            coro::net::io_status,
            frame>>
        {
            frame result;
            if (!underlying)
                co_return std::pair{closed_status(), result};

            std::vector<uint8_t> header(frame_header_size);
            auto status = co_await read_exact(underlying, header, options.handshake_timeout);
            if (!status.is_ok())
                co_return std::pair{status, result};

            frame_kind kind = frame_kind::client_hello_attest;
            uint64_t transcript_id = 0;
            uint32_t payload_size = 0;
            if (!parse_frame_header(header, kind, transcript_id, payload_size))
                co_return std::pair{protocol_status(), result};

            std::vector<uint8_t> payload(payload_size);
            status = co_await read_exact(underlying, payload, options.handshake_timeout);
            if (!status.is_ok())
                co_return std::pair{status, result};

            cmw body;
            if (!parse_cmw(payload, body))
                co_return std::pair{protocol_status(), result};

            result.kind = kind;
            result.transcript_id = transcript_id;
            result.body = std::move(body);
            co_return std::pair{ok_status(), std::move(result)};
        }

        auto send_hello(handshake_role role) -> coro::task<coro::net::io_status>
        {
            if (!options.service)
                co_return protocol_status();

            hello_payload hello;
            hello.local_identity = options.service->local_identity();
            hello.backend_id = options.service->backend_id();
            hello.will_send_evidence = options.service->should_send_local_evidence();
            hello.requires_peer_evidence = options.service->requires_peer_evidence();

            frame value;
            value.kind = role == handshake_role::client ? frame_kind::client_hello_attest : frame_kind::server_hello_attest;
            value.transcript_id = options.transcript_id;
            auto hello_payload_bytes = serialise_hello(hello);
            if (!hello_payload_bytes)
                co_return protocol_status();
            value.body = make_cmw(hello_media_type(), "canopy.attestation.hello.v1", std::move(*hello_payload_bytes));
            co_return co_await send_frame(value);
        }

        auto send_evidence(handshake_role role) -> coro::task<coro::net::io_status>
        {
            if (!options.service)
                co_return protocol_status();
            if (!options.service->should_send_local_evidence())
                co_return ok_status();

            auto nonce = make_nonce(options.transcript_id, options.service->local_identity(), role);
            if (!nonce)
                co_return protocol_status();

            auto evidence = options.service->produce_evidence(options.transcript_id, std::move(*nonce));
            if (!evidence.accepted)
            {
                RPC_WARNING("Attestation service failed to produce local evidence: {}", evidence.reason);
                co_return protocol_status();
            }

            frame value;
            value.kind = frame_kind::evidence;
            value.transcript_id = options.transcript_id;
            value.body = std::move(evidence.evidence);
            context.local_evidence_sent = true;
            co_return co_await send_frame(value);
        }

        auto send_verdict(
            bool accepted,
            const std::string& reason) -> coro::task<coro::net::io_status>
        {
            verdict_payload verdict;
            verdict.accepted = accepted;
            verdict.reason = reason;

            frame value;
            value.kind = frame_kind::evidence_verdict;
            value.transcript_id = options.transcript_id;
            auto verdict_payload_bytes = serialise_verdict(verdict);
            if (!verdict_payload_bytes)
                co_return protocol_status();
            value.body
                = make_cmw(verdict_media_type(), "canopy.attestation.verdict.v1", std::move(*verdict_payload_bytes));
            co_return co_await send_frame(value);
        }

        auto receive_hello(
            handshake_role local_role,
            hello_payload& hello) -> coro::task<bool>
        {
            auto [status, value] = co_await receive_frame();
            if (!status.is_ok())
                co_return false;

            const auto expected_kind = local_role == handshake_role::client ? frame_kind::server_hello_attest
                                                                            : frame_kind::client_hello_attest;
            if (value.kind != expected_kind || value.body.media_type != hello_media_type())
                co_return false;
            if (value.transcript_id != options.transcript_id)
                co_return false;
            co_return parse_hello(value.body.payload, hello);
        }

        auto receive_and_verify_evidence(
            const hello_payload& peer_hello,
            handshake_role local_role,
            std::string& reason) -> coro::task<bool>
        {
            context.peer_identity = peer_hello.local_identity;
            if (!peer_hello.will_send_evidence)
            {
                if (!options.service || options.service->requires_peer_evidence()
                    || !options.service->allows_unattested_peer())
                {
                    reason = "peer did not send attestation evidence and local policy does not allow unattested peers";
                    co_return false;
                }
                reason = "peer attestation explicitly allowed by policy";
                co_return true;
            }

            if (!options.service)
            {
                reason = "no attestation service configured";
                co_return false;
            }

            auto [status, value] = co_await receive_frame();
            if (!status.is_ok())
            {
                reason = "failed to receive peer evidence";
                co_return false;
            }
            if (value.kind != frame_kind::evidence || value.transcript_id != options.transcript_id)
            {
                reason = "peer evidence frame is invalid";
                co_return false;
            }

            evidence_binding binding;
            binding.subject = peer_hello.local_identity;
            binding.transcript_id = options.transcript_id;
            auto nonce = make_nonce(
                options.transcript_id,
                peer_hello.local_identity,
                local_role == handshake_role::client ? handshake_role::server : handshake_role::client);
            if (!nonce)
            {
                reason = "failed to encode peer evidence binding";
                co_return false;
            }
            binding.nonce = std::move(*nonce);

            auto verdict = options.service->verify_peer_evidence(value.body, std::move(binding));
            if (!verdict.accepted)
            {
                reason = verdict.reason;
                co_return false;
            }

            context.peer_attested = true;
            context.peer_identity = verdict.peer_identity;
            context.backend_id = verdict.backend_id;
            context.level = verdict.level;
            reason = verdict.reason;
            co_return true;
        }

        auto receive_verdict(std::string& reason) -> coro::task<bool>
        {
            auto [status, value] = co_await receive_frame();
            if (!status.is_ok())
            {
                reason = "failed to receive peer attestation verdict";
                co_return false;
            }
            if (value.kind != frame_kind::evidence_verdict || value.body.media_type != verdict_media_type())
            {
                reason = "peer attestation verdict frame is invalid";
                co_return false;
            }
            if (value.transcript_id != options.transcript_id)
            {
                reason = "peer attestation verdict transcript mismatch";
                co_return false;
            }

            verdict_payload verdict;
            if (!parse_verdict(value.body.payload, verdict))
            {
                reason = "peer attestation verdict payload is malformed";
                co_return false;
            }
            reason = verdict.reason;
            co_return verdict.accepted;
        }

        auto perform_handshake(handshake_role role) -> coro::task<bool>
        {
            if (complete)
                co_return true;
            if (!underlying || underlying->is_closed() || !options.service)
                co_return false;

            context = {};
            context.local_identity = options.service->local_identity();
            context.transcript_id = options.transcript_id;

            auto status = co_await send_hello(role);
            if (!status.is_ok())
                co_return false;

            status = co_await send_evidence(role);
            if (!status.is_ok())
                co_return false;

            hello_payload peer_hello;
            if (!co_await receive_hello(role, peer_hello))
            {
                co_await send_verdict(false, "peer hello was invalid");
                co_return false;
            }

            std::string local_reason;
            const bool local_accept = co_await receive_and_verify_evidence(peer_hello, role, local_reason);
            status = co_await send_verdict(local_accept, local_reason);
            if (!status.is_ok() || !local_accept)
                co_return false;

            std::string peer_reason;
            if (!co_await receive_verdict(peer_reason))
            {
                RPC_WARNING("Peer rejected attestation handshake: {}", peer_reason);
                co_return false;
            }

            establish_session_params session_params;
            session_params.peer_identity = context.peer_identity;
            session_params.transcript_id = context.transcript_id;
            session_params.local_evidence_sent = context.local_evidence_sent;
            session_params.peer_attested = context.peer_attested;
            session_params.verified_backend_id = context.backend_id;
            session_params.verified_level = context.level;
            context = options.service->establish_session(session_params);
            complete = true;
            co_return true;
        }

        const std::shared_ptr<::streaming::stream> underlying;
        const stream_options options;
        canopy::security::attestation::security_context context;
        bool complete{false};
    };

    stream::stream(
        std::shared_ptr<::streaming::stream> underlying,
        stream_options options)
        : impl_(
              std::make_unique<impl>(
                  std::move(underlying),
                  std::move(options)))
    {
    }

    stream::~stream() = default;

    auto stream::client_handshake() -> coro::task<bool>
    {
        co_return co_await handshake(handshake_role::client);
    }

    auto stream::server_handshake() -> coro::task<bool>
    {
        co_return co_await handshake(handshake_role::server);
    }

    auto stream::handshake(handshake_role role) -> coro::task<bool>
    {
        if (!impl_)
            co_return false;
        co_return co_await impl_->perform_handshake(role);
    }

    auto stream::security_context() const -> canopy::security::attestation::security_context
    {
        return impl_ ? impl_->context : canopy::security::attestation::security_context{};
    }

    auto stream::handshake_complete() const -> bool
    {
        return impl_ && impl_->complete;
    }

    auto stream::receive(
        rpc::mutable_byte_span buffer,
        std::chrono::milliseconds timeout)
        -> coro::task<std::pair<
            coro::net::io_status,
            rpc::mutable_byte_span>>
    {
        if (!impl_ || !impl_->complete || !impl_->underlying)
            co_return std::pair{closed_status(), rpc::mutable_byte_span{}};
        co_return co_await impl_->underlying->receive(buffer, timeout);
    }

    auto stream::send(rpc::byte_span buffer) -> coro::task<coro::net::io_status>
    {
        if (!impl_ || !impl_->complete || !impl_->underlying)
            co_return closed_status();
        co_return co_await impl_->underlying->send(buffer);
    }

    bool stream::is_closed() const
    {
        return !impl_ || !impl_->underlying || impl_->underlying->is_closed();
    }

    auto stream::set_closed() -> coro::task<void>
    {
        if (!impl_ || !impl_->underlying)
            co_return;
        co_await impl_->underlying->set_closed();
    }

    auto stream::get_peer_info() const -> peer_info
    {
        return impl_ && impl_->underlying ? impl_->underlying->get_peer_info() : peer_info{};
    }
} // namespace streaming::attestation
