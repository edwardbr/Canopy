# Video mode

A fourth mode in the websocket demo alongside Echo / Calculator / Chat.
Phase 1 (complete): proves the `[post]` fire-and-forget pipe end-to-end with
real video bytes travelling browser → enclave → browser.

The enclave-side handler is currently a pure echo: every frame received via
`i_calculator::push_video_frame` is bounced straight back to the browser via
the `i_context_event` callback the browser already registered for chat. The
interesting work — face detection, sprite compositing, the "Aladdin lamp
genie" stylisation — is phase 2 onward (see Roadmap).

Branch: `sgx_iouring_video_genie` (forked from `sgx_iouring`).

## Data flow

```
browser camera
   │  getUserMedia → MediaStreamTrackProcessor → VideoEncoder (VP8)
   ▼
calc.push_video_frame(seq, pts_us, flags, payload)        [post, MSG_POST]
   │  i_calculator (outbound proxy, no response awaited)
   ▼
demo::push_video_frame  →  video_session::forward_frame
   │  (echo: no processing yet)
   ▼
event_->push_frame(seq, pts_us, flags, payload)           [post, MSG_POST]
   │  i_context_event (the chat callback, reused)
   ▼
browser i_context_event_stub.push_frame
   │  EncodedVideoChunk → VideoDecoder → canvas.drawImage
   ▼
"From enclave" canvas
```

## What's in the box

### IDL (`idl/websocket_demo/websocket_demo.idl`)

`i_context_event` gained a second `[post]` method so the single per-connection
callback carries both LLM tokens and video egress:

```idl
interface i_context_event
{
    [post] int piece(const std::string& piece);
    [post] int push_frame(uint64_t seq, uint64_t pts_us, uint32_t flags,
                          const std::vector<uint8_t>& payload);
};
```

`i_calculator` gained the ingress method:

```idl
[post] int push_video_frame(uint64_t seq, uint64_t pts_us, uint32_t flags,
                            const std::vector<uint8_t>& payload);
```

`payload` is `std::vector<uint8_t>` (protobuf `bytes`, JS `Uint8Array`) — a raw
encoded VP8 chunk. `flags` bit 0 = keyframe, bit 1 = end-of-stream.

**Why one shared callback instead of a dedicated `i_video_stream`:** the
websocket transport binds a single typed inbound sink per connection from its
`create<Remote, Local>` template, and the JS proxy cannot yet marshal an
interface-reference *argument* into the bit-packed `zone_address` wire format
the C++ nanopb stub expects (this is why a JS-initiated `set_callback` /
`set_video_sink` fails to deserialise server-side; chat only works because the
server auto-binds the sink during the handshake). Reusing the
handshake-bound `i_context_event` sidesteps that entirely. Restoring a clean
separate `i_video_stream` is a generator/transport task tracked in Roadmap.

### Server (`server/`)

* `video_session.{h,cpp}` — owns the per-connection sink (`i_context_event`)
  and forwards inbound frames to it. Isolated from the calculator/chat code so
  phase 2+ processing (decode, detection, compositing, encode) has a home.
* `demo.cpp` — `set_callback` stores the chat callback **and** hands it to
  `video_session`; `push_video_frame` delegates to `video_session::forward_frame`.
* `websocket_handler.cpp` — unchanged; the existing `set_callback` handshake
  wiring now serves video too.

The non-enclave `websocket_server` executable picks these up unconditionally;
the enclave variant is gated as before by `CANOPY_BUILD_ENCLAVE`.

### Transport (`c++/transports/websocket/`)

Two fixes were required and both are general improvements, not video-specific:

1. **Receive buffer 4 KB → 256 KB** (`src/transport.cpp`). The old 4 KB buffer
   truncated anything larger than a chat token; an encoded VP8 frame is
   ~30–60 KB. Anything bigger than 256 KB would still truncate — proper
   streaming reassembly is a future hardening item.
2. **`MSG_POST` is now truly one-way** (`src/transport.cpp`,
   `stub_handle_send`). The server previously sent a `response` envelope for
   every inbound message including `[post]`; the client never registers a
   pending entry for `[post]`, so each one logged a spurious
   "No pending request for id N". `[post]` envelopes now return after dispatch
   with no response.

### Generator + JS transport

* `generator/src/javascript_generator.cpp` — `[post]` methods now emit a
  synchronous fire-and-forget **proxy** method (calling `callPost`) in
  addition to the existing stub-side dispatch. Previously `[post]` rendered
  stub-only, which was fine for chat (enclave → browser only) but not for
  video ingress (browser → enclave).
* `c++/transports/websocket/js/canopy_websocket_transport.js` — new
  `callPost(interfaceId, methodId, bytes)` wrapping a `MSG_POST` envelope with
  no pending-response bookkeeping.

### Browser (`server/www/`)

* `index.html` — Video radio + two-pane panel (local preview / enclave output)
  + Start/Stop + a sent/received/dropped counter.
* `client.js` — WebCodecs VP8 pipeline. One `i_context_event_stub` handles
  both `piece` (chat) and `push_frame` (video). VP8 chosen because every
  modern Chromium ships a software VP8 codec with no platform dependency.
  Defaults: 640×480 @ 15 fps, ~1.5 Mbps, keyframe every second.

## Running it (non-SGX)

```bash
cmake --preset Debug_Coroutine
cmake --build build_debug_coroutine --target websocket_server

./build_debug_coroutine/output/websocket_server \
  --va-name server --va-type ipv4 \
  --listen 0.0.0.0:8000 \
  --cert c++/demos/websocket/server/certs/server.crt \
  --key  c++/demos/websocket/server/certs/server.key \
  --static-root c++/demos/websocket/server/www
```

Open `https://localhost:8000`, accept the self-signed cert warning, click
**Connect**, wait for the green badge, switch to **Video**, **Start camera**,
and allow the camera permission. Left pane = local camera; right canvas =
echo returned from the enclave.

Browser requirement: Chromium with WebCodecs + `MediaStreamTrackProcessor`
(Chrome/Edge 94+).

Rebuilding after an IDL change also needs the JS regenerated:

```bash
cmake --build build_debug_coroutine --target websocket_demo_www_js_generate
```

## Known limitations

* Echo only — no in-enclave processing yet.
* 256 KB hard cap per websocket message; no fragment reassembly.
* No backpressure beyond the encoder's own queue; sustained overload drops
  frames (visible in the counter).
* Video egress rides the chat `i_context_event` callback rather than a
  dedicated interface (see the "why one shared callback" note above).

## Roadmap

Phase 1 — echo pipe **(done)**.

Phase 2 — **decode → trivial transform → re-encode in the enclave.** Invert
or sepia a decoded frame and re-encode it. This is the real proof that a
codec runs inside the enclave and validates the decode/encode loop before any
detection work. Needs a VP8 codec usable in the build (and later, inside SGX):
candidate is `libvpx` vendored under `c++/submodules/`. This is the next
session's starting point.

Phase 3 — static genie sprite composited at a fixed canvas position; still no
detection.

Phase 4 — face detection (BlazeFace or a small ONNX model; candidate runtime
`onnxruntime` minimal build or `ggml`), sprite anchored to detected
landmarks.

Phase 5 — smoke-from-lamp particle animation resolving into the genie head at
the detected face position.

Cross-cutting cleanup (any time): teach the JS generator to construct a proxy
from an `[out]`/argument-marshalled interface ref so the clean
`i_video_session` / `i_video_stream` split can be restored and video can drop
off `i_context_event` and `i_calculator`.
