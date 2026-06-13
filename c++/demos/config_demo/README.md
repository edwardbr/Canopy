# config_demo

`config_demo` is a JSON-driven calculator RPC demo for the connection factory.
It is intended as a readable starting point for future transport demos.

The JSON file uses the generated `config_demo::v1::demo_settings` schema:

- `iterations`, `scheduler_threads`, `client_connection`, and
  `server_connection`: executable-owned demo settings.
- `rpc_runtime`: host-side resources the connection factory can place into
  connection contexts, including SPSC queue pairs and generic `null_backend`,
  `fake_backend`, or `sgx_sim_backend` attestation services.
- `connections`: named connector or acceptor definitions. Each item owns the
  generated transport and stream-layer settings for that connection.

`rpc_runtime` is host-side RPC runtime setup. SGX transport and enclave-specific
settings remain inside the SGX transport's own `transport.settings` object.
TLS identities, trust anchors, and certificate/key file references belong to
the `tls` object in each connection's `stream_layers[].settings`, so different
links can use different TLS material.
`config_demo` parses its own generated IDL type from JSON, then passes only
`rpc_runtime` and `connections` into the connection factory runtime.

The sample files include a relative `$schema` reference to
`../schemas/config_demo_config.schema.json`. The build emits that schema with
`CanopyGenerateConnectionConfigSchema(config_demo_config_idl, <output-dir>)`,
which combines the app config IDL schema with the built connection factory
transport and stream-layer schemas.

Build and run:

```bash
cmake --preset Debug_Coroutine
cmake --build build_debug_coroutine --target config_demo
./build_debug_coroutine/output/config_demo \
  c++/demos/config_demo/samples/tcp_websocket.json
```

The build also copies samples and fixture certificates into:

```text
build_debug_coroutine/c++/demos/config_demo/
```

The composed schema is written beside the source samples and into the build
tree:

```text
c++/demos/config_demo/schemas/config_demo_config.schema.json
build_debug_coroutine/c++/demos/config_demo/schemas/config_demo_config.schema.json
```

Runnable local samples:

- `tcp_websocket.json`
- `spsc_websocket.json`
- `local_child.json`
- `tcp_tls_websocket_attestation.json` when TLS and attestation support are built

Reference samples:

- `tcp_all_layers.json` when compression is built
- `external_sgx_coroutine.json`
- `external_sgx_blocking.json`
- `external_ipc_spsc_sidecar.json`
- `external_blocking_dll.json`
- `external_shared_scheduler_dll.json`
- `external_unshared_scheduler_dll.json`

Each `transport.settings` and `stream_layers[].settings` object is passed to the
owning generated IDL config type. That keeps the demo generic: adding or editing
fields in the JSON exercises the same materialisation path as application code.
