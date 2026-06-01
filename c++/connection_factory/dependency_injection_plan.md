<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Connection Factory Dependency Injection Plan

This note captures the intended direction for `c++/connection_factory`.
The live source and IDL remain authoritative; this document is a working
implementation plan so the refactor can continue without relying on chat
history.

## Goal

The connection factory should become Canopy's dependency injection boundary for
connections, streams, transports, and configuration-owned runtime services.

The public API should describe topology:

```json
{
  "service": { "type": "service", "settings": { "name": "client_service" } },
  "transport": { "type": "stream_rpc", "settings": { "encoding": "nanopb" } },
  "stream_layers": [
    { "type": "tcp_blocking", "settings": { "host": "127.0.0.1", "port": 8080 } },
    { "type": "tls", "settings": { "client": { "verify_peer": false } } },
    { "type": "websocket", "settings": { "keep_alive": { "enabled": true } } },
    { "type": "compression", "settings": { "algorithm": "zstd", "level": 3 } }
  ]
}
```

The connection factory should not understand the private JSON schema for every
component. It should route the typed settings envelope's `type` to the
registered component factory. `connection_settings` uses
`rpc::stream_layers::stream_layer_settings` for stream layers so the same
descriptor can be consumed by the host connection factory, SGX runtimes, future
TEE runtimes, and in-runtime application services. The selected factory owns
its generated IDL config type, materialises sparse
JSON into the complete C++ settings object, embellishes it with runtime
dependencies, and calls the local typed construction function.

## Target Data Flow

The desired flow is:

```text
connection_settings JSON overlay
  -> materialise connection_factory::connection_settings
  -> for each typed settings entry:
       registry lookup by type
       component factory materialises its own generated IDL type
       component factory validates/embellishes runtime dependencies
       component factory calls local typed create/connect/accept/wrap function
  -> RPC transport adapter connects or accepts the generated interface pair
```

Raw JSON should only be visible at the configuration boundary and inside the
small factory adapter that owns the corresponding IDL type. Stream, transport,
filesystem, attestation, and other implementation code should receive typed
settings and runtime dependencies, not generic JSON blobs.

## Component Categories

The registry should distinguish these roles:

- Base stream factories: create the first byte stream in a sequence.
  Examples: TCP, SPSC queue, io_uring loopback, future LoRa.
- Stream layer factories: wrap an existing stream.
  Examples: TLS, WebSocket, compression, attestation, SPSC wrapping.
- RPC transport factories: adapt an established stream or native child-zone
  mechanism into `rpc::transport`.
  Examples: stream RPC, local child-zone transport. Local child zones are a
  special ultra-thin case: the parent and child transports are tightly coupled,
  share the same configuration intent, and only need names plus default
  encoding. They should not be routed through stream services or stream-layer
  machinery.
- Runtime dependency providers: create or provide objects that are not streams
  but are needed by factories.
  Examples: `rpc::service`, executor/scheduler, io_uring controller,
  attestation service, TLS contexts, filesystem services, queue pairs.

The same mechanism should later support non-stream systems, such as filesystem
configuration backed by io_uring, without forcing those settings into a stream
options type.

## Private Factory Shape

Most concrete factories should be private implementation details in `.cpp`
files. Public headers should expose the connection factory API, context hooks,
and the small number of templates that genuinely need interface types.

A private stream component adapter can have this conceptual shape:

```cpp
class stream_component_factory
{
public:
    virtual ~stream_component_factory() = default;

    virtual CORO_TASK(stream_result) connect_base(
        const json::v1::object& settings,
        std::shared_ptr<rpc::service> service,
        const context& factory_context) const;

    virtual CORO_TASK(stream_acceptor_result) accept_base(
        const json::v1::object& settings,
        std::shared_ptr<rpc::service> service,
        const context& factory_context) const;

    virtual CORO_TASK(stream_result) accept_single_base(
        const json::v1::object& settings,
        std::shared_ptr<rpc::service> service,
        const context& factory_context) const;

    virtual CORO_TASK(stream_result) wrap(
        std::shared_ptr<streaming::stream> stream,
        const json::v1::object& settings,
        layer_direction direction,
        const context& factory_context) const;
};
```

Each implementation then owns its materialisation:

```cpp
class tcp_stream_factory final : public stream_component_factory
{
    CORO_TASK(stream_result) connect_base(
        const json::v1::object& json,
        std::shared_ptr<rpc::service> service,
        const context&) const override
    {
        auto settings = materialise_settings<streaming::blocking::tcp::endpoint>(json);
        if (settings.error_code != rpc::error::OK())
            CO_RETURN stream_result{settings.error_code, {}};

        CO_RETURN CO_AWAIT rpc::tcp_blocking::connect_stream(settings.settings, std::move(service));
    }
};
```

The central connection factory should only know that `tcp` maps to a factory
with a `connect_base` implementation. It should not know the TCP schema fields.

## Dependency Injection Responsibilities

The connection factory context should evolve from a bag of optional special
cases into a scoped dependency container.

It needs to resolve:

- named and unnamed services
- executors and schedulers
- io_uring controllers and controller options
- TLS client/server contexts or credentials used to construct them
- attestation services and policy/provider/verifier dependencies
- SPSC queue pairs
- filesystem services and other non-stream runtime systems
- application-provided factories for future types such as LoRa or compression

Factories should be able to ask the context for dependencies by type and
optional name. If a dependency is not registered, the factory may construct it
from its IDL settings only when that is safe and local to that component.

## IDL Ownership Rules

Every concrete stream, stream layer, transport, and configurable runtime system
should own an IDL file for its settings.

Rules:

- Do not put implementation-specific fields into `connection_settings`.
- Do not revive a generic `stream_factory_options` style struct for all
  components.
- Use a typed settings envelope only; stream layers use
  `rpc::stream_layers::stream_layer_settings`.
- The `type` string selects the factory.
- The `settings` object belongs to the selected factory.
- Sparse JSON must be materialised through that factory's generated IDL schema
  and defaults before reaching implementation logic.

Examples:

- TCP settings live with TCP.
- TLS settings live with the selected TLS backend.
- io_uring host controller settings stay usable by non-stream users.
- io_uring stream settings live with the io_uring stream implementation.
- SGX and attestation settings should not be folded into generic stream
  settings; they should have their own IDL and resolve shared attestation
  dependencies through the context.

## Public API Direction

The intended public header is `connection_factory/connection_factory.h`.

It should expose:

- `connection_settings` based connect/accept entry points
- explicit typed overloads that already take generated IDL settings
- minimal context registration APIs
- extension registration APIs for application-specific component factories

It should avoid exposing:

- private factory classes
- implementation registries
- backend-specific schema includes
- non-template helper mechanics
- JSON-processing overloads below the configuration boundary
- generic native-factory maps that make the caller bind transport names to
  construction lambdas outside the configuration object

Existing typed helpers such as `rpc::tcp_blocking::connect_rpc` should remain when they
are initialised with generated IDL types rather than raw JSON.

Local child-zone helpers should stay explicit and narrow. `connect_local_child_rpc`
is the configuration-based entry point for local child zones: it uses the same
typed settings envelope as other configured connections, but the implementation
should remain a direct parent/child transport setup and must reject stream
layers.

## Migration Steps

1. Audit the current construction paths and classify each as base stream,
   stream layer, RPC transport, native/local transport, or runtime dependency.

2. Add private factory interfaces and a private registry implementation under
   `connection_factory/src`.

3. Move the built-in stream construction logic out of the central
   `if (layer.type == "...")` switch and into implementation-owned factories.
   TCP and SPSC queue currently remain as base-stream adapters inside the host
   connection factory. WebSocket, TLS, SPSC wrapping, and attestation stream
   wrappers are now created by `streaming/layer_factory`, which is the
   SGX/TEE-compatible layer construction path reused by the host connection
   factory.

4. Keep the old public registration hooks working by adapting them into the
   new registry. Application-specific types should not need edits in the
   central connection factory.

5. Move non-template mechanics from public headers into `.cpp` files. Keep only
   templates that require generated interface types in headers.

6. Split transport construction from stream construction in the registry.
   `stream_rpc` and `local` are transport choices, not stream layers. Keep the
   local transport path intentionally direct: it should materialise only its
   small generated IDL settings type and then construct the paired local
   parent/child transport objects without introducing stream abstractions.

7. Expand the context into a scoped dependency resolver. Initially preserve the
   current explicit setters, then layer typed/name-based lookup underneath.

8. Add or finish IDL files for every configurable stream, transport, and
   runtime system. Do this close to the implementation that owns the settings.

9. Add tests proving sparse JSON is materialised by the correct component
   factory before construction:
   one base stream, one stream layer, one transport, and one runtime dependency.

10. Verify both blocking and coroutine builds when shared logic changes.
    Coroutine-only implementations should still leave their IDL/config schema
    available where non-coroutine users need to parse topology.

## Short-Term Refactor Boundary

The first safe refactor should not attempt to solve the full DI container.
That slice now exists in source form:

- `connection_factory` has a private registry for base streams and transports.
- `streaming/layer_factory` owns reusable stream-wrapper materialisation for
  WebSocket, TLS, SPSC wrapping, and attestation.
- `connection_factory` still preserves public context setters and custom
  stream-layer registration, but built-in wrappers no longer need individual
  `streaming/*/src/connection_factory_adapter.cpp` files.
- Compression is still pending because there is no compression stream
  implementation yet.

Once that shape is stable, the context can be expanded into the real dependency
injection mechanism without another topology rewrite.
