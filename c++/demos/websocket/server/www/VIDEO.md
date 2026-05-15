# Video mode

A fourth mode added to the websocket demo alongside Echo / Calculator / Chat.
Proves the `[post]` fire-and-forget pipe end-to-end with real video bytes
travelling browser → enclave → browser.

Today the enclave-side handler is a pure echo: every frame received via
`i_calculator::push_video_frame` is bounced straight back to the browser via
the registered `i_video_stream` sink. The interesting work — face detection,
sprite compositing, the "Aladdin lamp genie" stylisation — is left as the
next stage; only the data path is implemented here.

## What's in the box

### IDL (`c++/demos/websocket/idl/websocket_demo/websocket_demo.idl`)

A new pure data-plane interface:

```idl
interface i_video_stream
{
    [post] int push_frame(uint64_t seq, uint64_t pts_us, uint32_t flags,
                          const std::string& payload);
};
```

Two bootstrap methods grafted onto `i_calculator` (the demo's existing
session interface):

```idl
int set_video_sink(const rpc::shared_ptr<i_video_stream>& sink);
[post] int push_video_frame(uint64_t seq, uint64_t pts_us, uint32_t flags,
                            const std::string& payload);
```

`push_video_frame` is browser → enclave ingress. The enclave egress goes back
through `i_video_stream::push_frame` on the sink the browser registered.

### Server (`c++/demos/websocket/server/`)

* `video_session.{h,cpp}` — owns the per-connection sink and forwards inbound
  frames to it. Kept out of `demo.cpp` so future processing (ffmpeg decode,
  face detection, genie overlay, re-encode) has somewhere to live without
  re-polluting the calculator/chat class.
* `demo.cpp` gains two thin overrides that delegate straight to the
  `video_session` member.

The non-enclave `websocket_server` executable picks up these sources
unconditionally. The enclave variant is gated as before by
`CANOPY_BUILD_ENCLAVE`.

### Browser (`c++/demos/websocket/server/www/`)

* `index.html` — Video radio + a two-pane panel (local preview / remote
  display) and Start/Stop controls.
* `client.js` — WebCodecs pipeline:
  * `getUserMedia` → `MediaStreamTrackProcessor` → `VideoEncoder` (VP8).
  * Encoded chunks fire `calc.push_video_frame(...)` — a `[post]` outbound
    that does not block on a response.
  * Stylised frames returning from the enclave land in
    `i_video_stream_stub.push_frame`, get decoded by a `VideoDecoder`, and
    are drawn to a 2D canvas.

Codec is VP8 because every Chromium ships a software VP8 codec — no platform
codec dependency. Resolution defaults to 640×480 @ 15 fps, ~1.5 Mbps; well
within the documented 100+ Mbps async io_uring envelope.

### Generator and transport additions

To make the browser ingress work, two small changes were needed:

* `c++/transports/websocket/js/canopy_websocket_transport.js` — new
  `callPost(interfaceId, methodId, bytes)` that wraps the existing
  `MSG_POST=1` envelope and does not register a pending response.
* `generator/src/javascript_generator.cpp` — `[post]` methods now also emit a
  proxy method (synchronous fire-and-forget, no `Promise`) that invokes
  `callPost` instead of `call`. Previously `[post]` rendered only on the stub
  side, which was sufficient for chat (only enclave → browser) but not for
  video (both directions).

## Running it (non-SGX)

```bash
cmake --preset Debug_Coroutine
cmake --build build_debug_coroutine --target websocket_server
./build_debug_coroutine/output/websocket_server
```

Then open the browser at the configured endpoint, click **Connect**, switch
to the **Video** radio, and click **Start camera**. Permission prompts for
the camera will appear; allow them and you should see the local preview on
the left and the echo-returned stream on the right.

Browser requirements: any modern Chromium with WebCodecs and
`MediaStreamTrackProcessor` (Chrome 94+ / Edge 94+).

## Limitations

* Echo only. No in-enclave processing yet.
* No backpressure beyond the encoder's own queue — at sustained overload the
  encoder will drop frames and `videoState.droppedFrames` will climb.
* The bootstrap call lives on `i_calculator` rather than a dedicated
  `i_video_session` interface. The cleaner split is described below.

## Next steps

### Cleaner interface boundary

The minimum viable IDL keeps `set_video_sink` on `i_calculator`. The proper
shape is:

```idl
interface i_video_session
{
    int open([in] const rpc::shared_ptr<i_video_stream>& browser_sink,
             [out] rpc::shared_ptr<i_video_stream>& enclave_pipe);
    int close();
};
```

This needs the JavaScript generator to construct a proxy on receipt of an
`[out]` interface ref (today it just hands back the raw remote ref). Once
that's done, video can drop off `i_calculator` entirely.

### Actual processing

The `video_session` class is where ffmpeg/OpenCV/face-detection plug in.
A reasonable staged plan:

1. Bypass — pure echo (what we have today).
2. Color-pass — invert / sepia a decoded frame, re-encode. Verifies the
   decode → process → encode loop works inside the enclave.
3. Static sprite overlay — composite a genie sprite at a fixed canvas
   position; no face detection yet.
4. Face detection — BlazeFace or similar lightweight ONNX model in the
   enclave; anchor the sprite to the detected landmarks.
5. Smoke-from-lamp animation — particle effect emerging from a stylized
   lamp into the genie head at the detected face position.

Submodules that would land in `c++/submodules/` for stage 2+ (subject to
SGX-build constraints): `libvpx` (decode/encode VP8), a small inference
runtime (`onnxruntime` minimal build or `ggml`), and a face detector model.
