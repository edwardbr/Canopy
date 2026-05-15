# Video mode

A fourth mode in the websocket demo alongside Echo / Calculator / Chat.

**Phase 1 (complete):** `[post]` fire-and-forget pipe proven end-to-end with
real video bytes browser → enclave → browser.

**Phases 2–3 (complete):** the enclave genuinely processes frames. libvpx
(pinned submodule) VP8-decodes each chunk; `transform_frame` applies a
fixed-position procedural genie sprite and/or a luma invert, **toggled live
from the browser** via `set_video_effects`; then VP8 re-encodes and pushes
back. Includes the latency/smoothness work (all-keyframe, frame-drop,
latest-frame-wins worker, Release build). Next is Phase 4 (face detection +
SGX port) — see Roadmap.

Branch: `sgx_iouring_video_genie` (forked from `sgx_iouring`).

## Data flow

```
browser camera
   │  getUserMedia → MediaStreamTrackProcessor → VideoEncoder (VP8, all-key)
   │  drop frame if encoder backlog > 1
   ▼
calc.push_video_frame(seq, pts_us, flags, payload)        [post, MSG_POST]
   │  i_calculator (outbound proxy, no response awaited)
   ▼
demo::push_video_frame  →  video_session::forward_frame
   │  stash into 1-slot mailbox, spawn worker if idle, RETURN immediately
   │  (transport receive loop never blocks on codec work)
   ▼
video_session worker coroutine (on the scheduler)
   │  VP8 decode → invert luma → VP8 encode  (only the freshest slot value;
   │  frames superseded while busy are dropped by this app, not the transport)
   ▼
event_->push_frame(seq, pts_us, flags, payload)           [post, MSG_POST]
   │  i_context_event (the chat callback, reused)
   ▼
browser i_context_event_stub.push_frame
   │  EncodedVideoChunk → VideoDecoder → stash latest frame
   │  requestAnimationFrame loop paints latest → "From enclave" canvas
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

* `video_session.{h,cpp}` — the per-connection video engine. Holds the
  `i_context_event` sink, the libvpx decoder/encoder contexts, and a single-
  slot "latest frame wins" mailbox. `forward_frame` (the `[post]` handler)
  only stashes the newest frame and returns immediately — it never does codec
  work inline, so the transport receive loop is never blocked and no kernel-
  socket backlog forms. A detached worker coroutine drains the slot: VP8
  decode → `transform_frame` (luma invert) → VP8 encode → `co_await
  push_frame`. Frames superseded while the worker is busy are dropped **by
  this application**; transport `[post]` stays in-order and lossless. All
  future processing (sprite, face detection, genie) plugs into
  `transform_frame` on the decoded raw frame.
* **Lifetime:** `video_session` holds a `std::shared_ptr<coro::scheduler>`,
  *not* the `rpc::service`. The service owns `demo` which owns
  `video_session`, so a shared_ptr back to the service would form a reference
  cycle and leak the connection. The scheduler does not own `demo`, so a
  shared_ptr to it is cycle-free and outlives the connection. The
  `CALCULATOR_ONLY` enclave compile-fit build has no service/scheduler, so
  `forward_frame` there falls back to synchronous inline processing
  (correctness over latency).
* `demo.cpp` — non-`CALCULATOR_ONLY` ctor passes the scheduler to
  `video_session`; `set_callback` stores the chat callback **and** hands it to
  `video_session`; `push_video_frame` delegates to `forward_frame`.
* `websocket_handler.cpp` — unchanged; the existing `set_callback` handshake
  wiring serves video too.

### Codec (libvpx)

* libvpx is a **pinned, shallow git submodule** at
  `c++/submodules/libvpx`, commit `12f3a2ac` (tag v1.14.1) — `shallow = true`
  in `.gitmodules`, so `git submodule update --init` fetches only that commit
  (no full history). The pinned gitlink is what makes the build replicable;
  this is the foundation the SGX MRENCLAVE stability requirement builds on.
* `cmake/CanopyLibvpx.cmake` — libvpx ships its own configure+make build, so
  this drives it via a custom command and exposes `canopy_libvpx` as an
  imported static lib. VP8 enc+dec only. **Requires nasm ≥ 2.14** (x86
  assembly) — see the repository README prerequisites.
* Build is preset-isolated (`<binaryDir>/libvpx-build`). Linked only to the
  host `websocket_server`; **not** ported into the enclave yet (Roadmap
  phase 4 — needs an enclave toolchain build and a reproducible/deterministic
  build for stable MRENCLAVE).
* Encoder tuned for realtime: all-intra (`kf_max_dist=1`, forced KF), CBR,
  no lag, `VP8E_SET_CPUUSED=16` (the dominant speed lever), 1200 kbps.

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
  + Start/Stop + **Genie / Invert** effect checkboxes + a
  sent/received/dropped counter.
* `client.js` — WebCodecs VP8 pipeline. One `i_context_event_stub` handles
  both `piece` (chat) and `push_frame` (video). VP8 chosen because every
  modern Chromium ships a software VP8 codec with no platform dependency.
  Defaults: **320×240 @ 24 fps, all-keyframe, 1.2 Mbps**. Capture frames are
  dropped if the encoder backlog exceeds 1 (latency cap). Decoded frames are
  stashed and painted on a `requestAnimationFrame` loop (the most recent
  frame only) so the irregular enclave/network delivery cadence doesn't show
  as judder — no jitter buffer, so latency is unaffected.

## Running it (non-SGX)

Release is recommended — `-O0` Debug roughly halves the enclave's sustained
decode→invert→encode throughput, which shows as choppy motion. (libvpx itself
is always built optimized + SIMD regardless of the Canopy preset; Release only
affects the Canopy-side glue and the invert loop.)

```bash
cmake --preset Release_Coroutines
cmake --build build_release_coroutine --target websocket_server

./build_release_coroutine/output/websocket_server \
  --va-name server --va-type ipv4 \
  --listen 0.0.0.0:8000 \
  --cert c++/demos/websocket/server/certs/server.crt \
  --key  c++/demos/websocket/server/certs/server.key \
  --static-root c++/demos/websocket/server/www
```

(`Debug_Coroutine` / `build_debug_coroutine` works too for development.)

Open `https://localhost:8000`, accept the self-signed cert warning, click
**Connect**, wait for the green badge, switch to **Video**, **Start camera**,
and allow the camera permission. Left pane = local camera; right canvas =
the photo-negative produced inside the enclave.

Browser requirement: Chromium with WebCodecs + `MediaStreamTrackProcessor`
(Chrome/Edge 94+).

Rebuilding after an IDL change also needs the JS regenerated:

```bash
cmake --build build_debug_coroutine --target websocket_demo_www_js_generate
```

## Known limitations

* 256 KB hard cap per websocket message; no fragment reassembly.
* Application drops superseded frames to bound latency; under load the
  effective rate is whatever the enclave sustains (counter shows drops).
* Video egress rides the chat `i_context_event` callback rather than a
  dedicated interface (see the "why one shared callback" note above).
* libvpx is host-side only; not yet built for the SGX enclave (phase 4).
* Worker vs `video_session` teardown on abrupt disconnect is guarded by
  `stopping_` but a late in-flight frame is theoretically possible — benign
  for the demo; tighten with shared-ownership if it ever matters.

## Roadmap

Phase 1 — echo pipe **(done)**.

Phase 2 — **decode → transform → re-encode in the enclave (done).** libvpx
pinned submodule; VP8 decode → luma-invert → VP8 encode in `video_session`,
with the latest-frame-wins worker, all-keyframe streaming, and Release build
for throughput. Proven end-to-end at ~½ s, snappy.

Phase 3 — **static genie sprite at a fixed position (done).** Procedural
RGBA genie+lamp placeholder (`genie_sprite.{h,cpp}`), alpha-composited onto
the decoded I420 frame (RGB→YUV 4:2:0) in the `transform_frame` seam. Sprite
is drawn in code (no asset file) to stay deterministic for the SGX
reproducible build.

Phase 3.1 — **live effect toggles (done).** `i_calculator::set_video_effects(
uint32_t)` (regular RPC, scalar bitmask — no marshalling issues) drives a
`std::atomic<uint32_t>` in `video_session`, read by `transform_frame`.
Browser has **Genie** and **Invert** checkboxes; toggled live, synced on
connect. Invert is applied before the genie composite so the sprite always
renders with correct colours. (Future per-stream controls — sprite
position/scale, effect intensity — extend the same bitmask/channel.)

Phase 4 — face detection (BlazeFace or a small ONNX model; candidate runtime
`onnxruntime` minimal build or `ggml`), sprite anchored to detected
landmarks instead of the fixed corner — drops into the same
`transform_frame`/compositor seam. **Also the SGX/libvpx port**: build libvpx
with the enclave toolchain and make it a reproducible/deterministic build so
MRENCLAVE is stable (the pinned shallow submodule is the groundwork for
this).

Phase 5 — smoke-from-lamp particle animation resolving into the genie head at
the detected face position.

Cross-cutting cleanup (any time): teach the JS generator to construct a proxy
from an `[out]`/argument-marshalled interface ref so the clean
`i_video_session` / `i_video_stream` split can be restored and video can drop
off `i_context_event` and `i_calculator`.
