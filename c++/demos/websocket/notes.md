# WebSocket Transport Protocol

All messages are serialised with protobuf. Every message in both directions — including the handshake — is wrapped in an `envelope`. There are two phases: a handshake and a dispatch loop, both using the same envelope framing.

## Message type enum

```cpp
enum message_type : uint8_t {
    call               = 0;  // client → server RPC call
    event              = 1;  // server → client fire-and-forget
    reply              = 2;  // server → client RPC response
    handshake          = 3;  // client → server initial handshake
    handshake_ack      = 4;  // server → client pre-handler ack (zone_id + remote_object_id)
    handshake_complete = 5;  // server → client handshake done (outbound_remote_object)
};
```

The enum values intentionally avoid names that match struct names (`connect_request`, `connect_response`, etc.) as the IDL parser resolves names before code generation and would hang on the ambiguity.

## Envelope (all messages)

Every message in both directions is wrapped in an `envelope`:

```cpp
struct envelope {
    uint64_t      id;    // client-assigned request counter, starting at 1;
                         // server echoes it in responses; 0 for server-initiated messages
    message_type  type;  // discriminates the inner struct
    vector<char>  data;  // protobuf-encoded inner struct
};
```

`id` is a monotonically increasing counter assigned by the originating side:
- Client starts at 1 and increments for each message it sends.
- Server uses 0 for all server-initiated messages (`post`, `connect_initial_response`).
- Server echoes the client's `id` in `response` and `connect_response`.

---

## Phase 1 — Handshake

### Client → Server: `connect_request` (envelope type 3 — `handshake`)

`envelope.id = 1` (first message), `envelope.type = connect_request`.

`envelope.data` contains a protobuf-encoded `connect_request`:

```cpp
struct connect_request {
    interface_ordinal      inbound_interface_id;   // interface the client exposes (its back-channel stub)
    interface_ordinal      outbound_interface_id;  // interface the client wants to call on the server
    rpc::zone_address_args remote_object_id;       // client's back-channel object;
                                                   //   remote_object_id.object_id set by client (e.g. 1);
                                                   //   subnet/routing_prefix left 0 — server assigns them
};
```

`connect_request` mirrors `rpc::connection_settings` structurally. The client declares both the interface it provides (back-channel) and the interface it wishes to call.

### Server → Client: `connect_initial_response` (envelope type 4 — `handshake_ack`, id 0)

Sent immediately upon receiving `connect_request`, **before** `connection_handler` is invoked. Gives the client both the server's `zone_id` and its own fully-populated `remote_object_id` (with server-assigned subnet/routing_prefix). This is necessary because `connection_handler` may trigger back-channel calls to the client before `connect_response` is sent — the client must already know its own address to handle them.

```cpp
struct connect_initial_response {
    rpc::zone              zone_id;         // server's root zone_id
    rpc::zone_address_args remote_object_id; // client's back-channel object, now fully populated:
                                             //   routing_prefix and subnet filled in by the server
};
```

### Server → Client: `connect_response` (envelope type 5 — `handshake_complete`, id echoed)

`envelope.id` echoes the `connect_request` id. Sent after `connection_handler` completes successfully.

```cpp
struct connect_response {
    rpc::zone_address_args outbound_remote_object;  // server-side callable object the client should use
                                                     //   as destination_zone_id in every subsequent request
};
```

After receiving `connect_response` the handshake is complete. The client has already had `remote_object_id` since `connect_initial_response`; it now also holds `outbound_remote_object` (the server-side callable object).

---

## Phase 2 — Dispatch loop

### Client → Server: RPC call (envelope type 0 — `call`)

`envelope.id` = next counter value. `envelope.data` contains a protobuf-encoded `request`:

```cpp
struct request {
    rpc::encoding          encoding;           // always protocol_buffers (16)
    uint64_t               tag;                // 0 for websocket
    rpc::zone_address_args caller_zone_id;     // client's zone — server ignores this,
                                               //   it uses its own adjacent_zone record; send default/zero
    rpc::zone_address_args destination_zone_id; // copy of connect_response.outbound_remote_object
    rpc::interface_ordinal interface_id;       // hash identifying the interface, e.g. i_calculator::get_id()
    rpc::method            method_id;         // 1-based ordinal within that interface
    vector<char>           data;              // protobuf-encoded method arguments
    vector<rpc::back_channel_entry> back_channel; // interface marshalling (see below)
};
```

Method ordinals for `i_calculator` (as generated):

| ordinal | method                                              |
|---------|-----------------------------------------------------|
| 1       | `add(first_val, second_val) → response`             |
| 2       | `subtract(first_val, second_val) → response`        |
| 3       | `multiply(first_val, second_val) → response`        |
| 4       | `divide(first_val, second_val) → response`          |
| 5       | `add_prompt(prompt)`                                |
| 6       | `set_callback(event)` — passes interface ref via `back_channel` |

### Server → Client: RPC response (envelope type 2 — `reply`)

`envelope.id` echoes the request's `id`. `envelope.data` contains a protobuf-encoded `response`:

```cpp
struct response {
    uint64_t               error;        // rpc error code; 0 = OK
    vector<char>           data;         // protobuf-encoded return values (may be empty)
    vector<rpc::back_channel_entry> back_channel; // interface refs in return values
};
```

### Server → Client: server-initiated event (envelope type 1 — `event`)

`envelope.id = 0` (no correlation — fire and forget). `envelope.data` contains a protobuf-encoded `request`:

```cpp
envelope {
    id   = 0;
    type = event;  // 1
    data = protobuf-encoded request {
        destination_zone_id = remote_object_id from connect_request (client's back-channel address)
        interface_id        = i_context_event::get_id()
        method_id           = 1  // piece()
        data                = protobuf-encoded i_context_event_pieceRequest { piece: "..." }
    };
};
```

No reply is expected. The client distinguishes posts from responses by `envelope.type == 1`.

---

## Back-channel entries

`back_channel` carries marshalled interface references — used when a method argument or return value is itself an interface pointer (e.g. `set_callback(rpc::shared_ptr<i_context_event>)`). Each entry is:

```cpp
struct back_channel_entry {
    uint64_t             type_id;  // fingerprint of the interface being marshalled
    vector<uint8_t>      payload;  // serialised zone_address of the object
};
```

The receiver uses `type_id` to identify which interface the payload describes, then reconstructs a proxy to it.

---

## Key observations for a generic connector

1. Every message is enveloped — there are no bare protobuf structs at any phase.
2. The client assigns `id` starting at 1, incrementing per message sent. The server echoes `id` in `response` and `connect_response`; uses 0 for all server-initiated messages.
3. `envelope.type` is the primary discriminator — check it before inspecting `id`.
4. `connect_request` mirrors `rpc::connection_settings`: the client declares both the interface it exposes (back-channel) and the interface it wants to call.
5. `connect_initial_response` arrives before `connect_response` and carries the server's `zone_id`. A client must handle it without treating it as an error.
6. `destination_zone_id` is opaque — it is exactly `connect_response.outbound_remote_object`, round-tripped as a `zone_address_args` blob. A generic client never needs to interpret it.
7. `caller_zone_id` in requests is ignored server-side — the server substitutes its own `get_adjacent_zone_id()`, so the client can send a zero/default `zone_address_args`.
8. Interface and method IDs are computed hashes — they change whenever the IDL signature changes. A generic connector needs a way to be told these values (e.g. from a generated JSON manifest or by querying a discovery endpoint).
9. All argument and return payloads are protobuf — the `data` field in both `request` and `response` is always a protobuf-encoded struct named after the method (e.g. `i_calculator_addRequest` / `i_calculator_addResponse`).
