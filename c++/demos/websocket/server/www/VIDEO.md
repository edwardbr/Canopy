# Websocket Video Demo

This demo streams real video bytes browser -> server -> browser over the
websocket transport. The server decodes VP8 frames, applies the selected video
effects, re-encodes the newest available frame, and posts it back to the
browser.

The application intentionally uses a latest-frame-wins mailbox. RPC delivery is
still ordered and lossless up to the demo handler, but the video worker drops
stale frames while codec work is in flight so end-to-end latency stays bounded.

## Files

- `index.html`, `client.js`, and `styles.css` implement the browser controls,
  local preview, processed output, and latency counters.
- `video_session.*` owns the per-connection codec worker and video parameters.
- `face_track.h` provides the lightweight skin-region tracker used by the genie
  overlay.
- `genie_sprite.*` draws the procedural overlay sprite.

## Running It

Use a release or coroutine release build for smooth playback. Debug builds are
useful for inspection but usually cannot sustain the same frame rate.

Start the websocket demo server, open the served page, switch to the video tab,
and grant camera permission. The page keeps the websocket connection open while
video capture starts and stops.

## Controls

- Brightness adjusts the luma offset applied to decoded frames.
- Quality maps to VP8 bitrate and encoder speed.
- The effects toggles control the genie overlay and invert-luma transform.

Resolution changes stop and restart capture while preserving the websocket
session. The server rebuilds the encoder when the frame size or video
parameters change.
