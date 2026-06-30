<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# WebSocket Demo for Canopy

This demo is a small browser and Node.js application built on Canopy's HTTP,
REST, WebSocket, and generated JavaScript bindings. It is no longer just a text
echo sample: the default coroutine build serves a browser test UI, exposes a
generated REST echo endpoint, and upgrades WebSocket connections into Canopy
RPC.

## What It Demonstrates

- Static HTTP file serving for the browser UI.
- Generated REST handler dispatch through `POST /api/echo`.
- Browser and Node.js generated JavaScript bindings for `websocket_demo.idl`.
- Canopy RPC over WebSocket using `transport_untrusted_web`.
- Calculator RPC calls over binary WebSocket frames.
- Optional WebSocket `permessage-deflate` when `CANOPY_BUILD_ZLIB=ON`.
- Optional TLS with `--cert` and `--key`.
- Optional video mode in coroutine builds.
- Optional LLM chat support when the demo is not built in calculator-only mode.

## Build And Run

The normal host demo is built from the coroutine preset:

```bash
cmake --preset Debug_Coroutine
cmake --build build_debug_coroutine --target websocket_server
./build_debug_coroutine/output/websocket_server
```

Then open:

```text
http://127.0.0.1:8080
```

The server defaults are injected through the shared network-config CLI layer.
By default the demo listens on `127.0.0.1:8080`. Override it with `--listen`:

```bash
./build_debug_coroutine/output/websocket_server --listen=server:0.0.0.0:8080
```

To serve a different static directory:

```bash
./build_debug_coroutine/output/websocket_server --static-root=/path/to/www
```

To run with TLS:

```bash
./build_debug_coroutine/output/websocket_server \
  --cert c++/demos/websocket/server/certs/server.crt \
  --key c++/demos/websocket/server/certs/server.key
```

With TLS enabled, open `https://127.0.0.1:8080`. The browser client derives
`wss://` from the page URL automatically.

## Browser Client

The served UI has four modes:

- **Echo**: sends HTTP JSON to `POST /api/echo` and displays the generated REST
  response. This does not require an open WebSocket connection.
- **Calculator**: connects over WebSocket and calls the generated
  `i_calculator` proxy.
- **Chat**: streams LLM output through the `i_context_event` callback when LLM
  support is present. Calculator-only builds disable this mode.
- **Video**: sends VP8/WebCodecs frames through `i_calculator::push_video_frame`
  and receives processed frames through `i_context_event::push_frame`.

The browser client uses:

- `server/www/index.html`
- `server/www/client.js`
- `server/www/generated/websocket_demo.js`
- `server/www/generated/websocket_demo_proto.js`
- `server/www/generated/untrusted_web_transport.js`
- `server/www/generated/websocket_demo_config.js`

The `ws` query parameter can override the WebSocket URL used by the browser:

```text
http://127.0.0.1:8080/?ws=ws://127.0.0.1:8080
```

## Node.js Clients

Install the client dependencies once:

```bash
cd c++/demos/websocket/client
npm install
```

Run calculator RPC tests using the generated JavaScript transport and proxy:

```bash
node test_calculator.js
```

Run REST echo tests:

```bash
npm run test-rest
```

The Node clients default to `ws://localhost:8080` and `http://localhost:8080`.
Override them with:

```bash
WS_URL=ws://192.168.1.100:8080 node test_calculator.js
API_URL=http://192.168.1.100:8080 npm run test-rest
```

If the demo was configured with `CANOPY_WEBSOCKET_DEMO_ENCODING=PROTOCOL_BUFFERS`,
set the same encoding for the generated Node transport:

```bash
CANOPY_WEBSOCKET_DEMO_ENCODING=PROTOCOL_BUFFERS node test_calculator.js
```

`client.js`, `interactive.js`, and `test.js` are older raw text WebSocket echo
helpers. They do not exercise the current Canopy RPC handshake path used by the
browser calculator and `test_calculator.js`.

## Generated Files

CMake generates browser and Node.js JavaScript bindings from the IDL files:

- `idl/websocket_demo/websocket_demo.idl` -> WebSocket/RPC demo bindings.
- `idl/websocket_rest/websocket_rest.idl` -> REST echo interface.
- `idl/websocket_rest/websocket_rest.rest.json` -> REST binding metadata.
- `idl/websocket_rest/websocket_rest.openapi.json` -> generated OpenAPI output.

Generated JavaScript is written to:

- `client/generated/` for CommonJS Node.js clients.
- `server/www/generated/` for the browser UI.

The generated files must match the server binary. If the browser reports that
the Canopy handshake was rejected, rebuild the server target so the generated
JavaScript bundle and C++ IDL code are regenerated from the same sources.

## Build Options

- `CANOPY_BUILD_WEBSOCKET=ON` is required for this demo.
- `CANOPY_BUILD_COROUTINE=ON` enables the full host demo, TLS stream support,
  and video mode.
- Non-coroutine builds force `CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY=ON`.
- `CANOPY_WEBSOCKET_DEMO_ENCODING=AUTO` chooses Nanopb when available, otherwise
  full Protocol Buffers. It can be set to `NANOPB` or `PROTOCOL_BUFFERS`.
- `CANOPY_BUILD_ZLIB=ON` enables HTTP gzip responses and WebSocket
  `permessage-deflate`.

## Source Layout

```text
c++/demos/websocket/
|-- CMakeLists.txt
|-- README.md
|-- client/
|   |-- README.md
|   |-- client.js               # legacy raw text WebSocket helper
|   |-- interactive.js          # legacy raw text WebSocket helper
|   |-- test.js                 # legacy raw text WebSocket tests
|   |-- test_calculator.js
|   |-- test_rest.js
|   `-- generated/
|-- idl/
|   |-- CMakeLists.txt
|   |-- websocket_demo/
|   `-- websocket_rest/
|-- server/
|   |-- CMakeLists.txt
|   |-- main.cpp
|   |-- demo.cpp
|   |-- demo_zone.cpp
|   |-- http_client_connection.cpp
|   |-- rest_echo_service.cpp
|   |-- websocket_handler.cpp
|   |-- video_session.cpp
|   |-- llama_server_engine.cpp
|   `-- www/
`-- tests/
    `-- websocket_rest_echo_test.cpp
```

## Tests

The JavaScript tests require a running server:

```bash
./build_debug_coroutine/output/websocket_server
```

Then, from `c++/demos/websocket/client`:

```bash
node test_calculator.js
npm run test-rest
```

The C++ REST test is registered with CTest when `CANOPY_BUILD_TEST=ON` and the
`websocket_server_rest` target is available:

```bash
cmake --build build_debug_coroutine --target websocket_rest_echo_test
ctest --test-dir build_debug_coroutine -R websocket_rest_echo_test
```

## Troubleshooting

**Connection refused**

Check that the server is running and listening on the endpoint your client uses.
The default is `127.0.0.1:8080`.

**Browser handshake rejected**

Rebuild the relevant server target. This usually means the generated JavaScript
bundle and the server binary were built from different IDL output.

**Chat mode disabled**

The build was configured with `CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY=ON`, which
is the default for non-coroutine builds.

**Camera or video mode unavailable**

Video mode depends on browser WebCodecs/camera APIs and the coroutine video
pipeline. Run from a secure origin for camera access when required by the
browser.

**TLS only works in coroutine builds**

The plain server target is dual-mode, but the secure stream path used by
`--cert/--key` is currently wired for coroutine builds.

## Related Docs

- `server/www/README.md` - browser client details.
- `server/www/CALCULATOR.md` - calculator RPC protocol notes.
- `server/www/VIDEO.md` - video mode design and limitations.
- `client/README.md` - Node.js client commands and tests.

## License

Part of the Canopy project.
