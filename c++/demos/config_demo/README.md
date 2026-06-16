# config_demo

`config_demo` is a JSON-driven calculator RPC demo for the connection factory.
It is intended as a readable starting point for future transport demos.

The JSON file uses the generated `config_demo::v1::demo_settings` schema:

- `execution`: executable-owned demo settings, including calculator
  iterations, executor thread count, and the named client/server connections
  this demo should run.
- `spsc_queues`: named in-process SPSC queue pairs that the connection factory
  can place into connection contexts.
- `attestation_services`: generic `null_backend` or `fake_backend`
  attestation services that attestation stream layers can reference by name.
- `connections`: named connector or acceptor definitions. Each item owns the
  generated transport and stream-layer settings for that connection.

Top-level `spsc_queues` and `attestation_services` are host-side resources that
the connection factory runtime creates once for the loaded config. TLS
identities, trust anchors, and certificate/key file references belong to the `tls`
object in each connection's
`stream_layers[].settings`, so different links can use different TLS material.
`config_demo` parses its own generated IDL type from JSON, then passes only
these host-side resources and `connections` into the connection factory runtime.

The sample files include a relative `$schema` reference to
`../schemas/config_demo_config.schema.json`. The build emits that schema with
`CanopyGenerateConnectionConfigSchema(config_demo_config_idl, <output-dir>)`,
which combines the app config IDL schema with the built connection factory
transport and stream-layer schemas.

Build and run:

```bash
cmake --preset Debug
cmake --build build_debug --target config_demo
./build_debug/output/config_demo \
  c++/demos/config_demo/samples/tcp_blocking_websocket.json

cmake --preset Debug_Coroutine
cmake --build build_debug_coroutine --target config_demo
./build_debug_coroutine/output/config_demo \
  c++/demos/config_demo/samples/tcp_websocket.json
```

The build also copies samples and fixture certificates into:

```text
build_debug/c++/demos/config_demo/
build_debug_coroutine/c++/demos/config_demo/
```

The composed schema is written beside the source samples and into the build
tree:

```text
c++/demos/config_demo/schemas/config_demo_config.schema.json
build_debug_coroutine/c++/demos/config_demo/schemas/config_demo_config.schema.json
```

Runnable local samples:

- `tcp_blocking_websocket.json` in blocking builds
- `tcp_websocket.json` in coroutine builds
- `spsc_websocket.json`
- `local_child.json`
- `tcp_tls_websocket_attestation.json` when TLS and attestation support are built

Reference samples:

- `tcp_all_layers.json` when compression is built
- `external_ipc_spsc_sidecar.json`
- `external_blocking_dll.json`
- `external_shared_scheduler_dll.json`
- `external_unshared_scheduler_dll.json`

`spsc_buffered_stream` is intentionally not used by the runnable config demo
samples. It is a local buffering layer around an already-created stream; it does
not pair two endpoints by sharing a queue. Use `spsc_queue` for configured
in-process queue-pair transport.

Each `transport.settings` and `stream_layers[].settings` object is passed to the
owning generated IDL config type. That keeps the demo generic: adding or editing
fields in the JSON exercises the same materialisation path as application code.
