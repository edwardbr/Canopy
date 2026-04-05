<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# WebSocket Demo for Canopy

Complete WebSocket server and client demonstration using libcoro and wslay.

## Architecture

```
┌─────────────────────────────────────┐
│   C++ WebSocket Server              │
│   (libcoro + wslay)                 │
│                                     │
│   - HTTP static file server         │
│   - WebSocket echo server           │
│   - Port 8888                       │
└─────────────┬───────────────────────┘
              │
      ┌───────┴────────┐
      │                │
┌─────▼─────┐   ┌─────▼──────┐
│  Browser  │   │  Node.js   │
│  Client   │   │  Client    │
└───────────┘   └────────────┘
```

## Quick Start

### 1. Build and Start the Server

```bash
# Build the server (automatically generates protobuf JavaScript)
cmake --build build --target websocket_server

# Run the server
./build/output/debug/websocket_server
```

**Note**: The build process automatically generates JavaScript protobuf files from the IDL definitions:
- Browser version: `server/www/websocket_proto.js`
- Node.js version: `client/websocket_proto.js`

Requirements: Node.js and npm (for protobuf JS generation)

Expected output:
```
WebSocket server listening on port 8888
```

### 2. Option A: Use the Browser Client

Open your web browser and navigate to:
```
http://localhost:8888
```

**What you'll see:**
- Beautiful web interface with gradient purple design
- "Connect" button to establish WebSocket connection
- Message input field for sending messages
- Real-time message history with color-coded entries
- Statistics: messages sent/received, connection uptime

**How to use:**
1. Click "Connect"
2. Type a message in the input field
3. Press Enter or click "Send"
4. See your message echoed back immediately
5. Click "Disconnect" when done

### 3. Option B: Use the Node.js Client

```bash
# Install dependencies (first time only)
cd demos/websocket/client
npm install

# Run simple client (automated test)
npm start

# OR run interactive client
npm run interactive

# OR run comprehensive test suite
npm test
```

## Components

### Server (`server/`)
- **`server.cpp`** - Main server with:
  - HTTP static file server
  - WebSocket handshake handler
  - WebSocket echo functionality
  - Multi-client support via coroutines
- **`websocket_handshake.h`** - WebSocket handshake utilities
- **`www/`** - Web client files:
  - `index.html` - Web UI
  - `client.js` - WebSocket client logic

### Node.js Client (`client/`)
- **`client.js`** - Simple automated test client
- **`interactive.js`** - Interactive console client
- **`test.js`** - Comprehensive test suite (7 tests)

## Features

### Server Features
✓ **Dual mode support**: Echo (text) and Calculator (RPC with protobuf)
✓ WebSocket protocol support (RFC 6455)
✓ HTTP/1.1 static file serving
✓ Proper handshake with `Sec-WebSocket-Accept`
✓ Automatic PING/PONG handling
✓ Binary and text message support
✓ Multiple concurrent connections
✓ Coroutine-based async I/O
✓ Multi-threaded (uses hardware concurrency)
✓ Full Canopy framework integration

### Client Features
✓ **Browser and Node.js** clients with identical functionality
✓ Real-time bidirectional communication
✓ **Calculator mode**: Binary RPC calls using protobuf
✓ Message echo verification
✓ Connection statistics
✓ Graceful connection handling
✓ Error handling and reporting

### Build Features
✓ **Automatic protobuf JS generation** from IDL files via CMake
✓ Generates browser-compatible static modules
✓ Generates Node.js CommonJS modules
✓ Regenerates on IDL/proto file changes
✓ Uses `protobufjs-cli` via npm (auto-installed)
✓ Unicode support
✓ Binary data support

## Testing

### Browser Testing
1. Open `http://localhost:8888` in browser
2. Click "Connect"
3. Send messages and verify echo
4. Check statistics update correctly

### Node.js Testing
```bash
cd demos/websocket/client

# Quick test
npm start

# Interactive testing
npm run interactive

# Full test suite
npm test
```

**Test suite includes:**
- Basic connection test
- Text message echo
- Multiple sequential messages
- Binary data echo
- Large message (10KB)
- Unicode character support
- Graceful connection close

## Protocol Flow

### HTTP Request → WebSocket Upgrade
```
Client → Server: GET / HTTP/1.1
Server → Client: HTTP/1.1 200 OK (HTML page)

Client → Server: GET / HTTP/1.1
                  Upgrade: websocket
                  Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==

Server → Client: HTTP/1.1 101 Switching Protocols
                  Upgrade: websocket
                  Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
```

### WebSocket Message Exchange
```
Client → Server: TEXT frame: "Hello"
Server → Client: TEXT frame: "Hello" (echo)

Client → Server: CLOSE frame
Server → Client: CLOSE frame (response)
```

## Directory Structure

```
demos/websocket/
├── README.md                    # This file
├── server/
│   ├── server.cpp              # C++ WebSocket server
│   ├── websocket_handshake.h   # Handshake utilities
│   ├── CMakeLists.txt          # Build configuration
│   └── www/
│       ├── index.html          # Web UI
│       ├── client.js           # Browser client logic
│       └── README.md           # Web client docs
└── client/
    ├── client.js               # Node.js simple client
    ├── interactive.js          # Node.js interactive client
    ├── test.js                 # Test suite
    ├── package.json            # NPM configuration
    └── README.md               # Node.js client docs
```

## Technical Details

### Server Technology
- **Framework**: libcoro (C++20 coroutines)
- **WebSocket Library**: wslay (transport-agnostic)
- **Handshake**: OpenSSL (SHA-1, Base64)
- **File I/O**: C++17 filesystem
- **Concurrency**: io_scheduler with hardware concurrency

### Client Technology
- **Browser**: Vanilla JavaScript (no frameworks)
- **Node.js**: ws library (WebSocket implementation)
- **UI**: Modern CSS with gradients and animations

## Configuration

### Server Port
Default: `8888`

To change, edit `server.cpp`:
```cpp
coro::net::tcp::server server{scheduler,
    coro::net::tcp::server::options{.port = 8888}};
```

### Client URL
Browser client auto-detects from current host.

Node.js client can be configured:
```bash
WS_URL=ws://192.168.1.100:8888 npm start
```

## Troubleshooting

### "Connection refused"
- Ensure server is running: `./build/output/debug/websocket_server`
- Check firewall settings
- Verify port 8888 is not in use: `netstat -tuln | grep 8888`

### "File not found" in browser
- Ensure `www/` directory exists in server directory
- Check file permissions
- Server uses `__FILE__` macro to find www directory relative to source

### npm install fails
- Ensure Node.js is installed: `node --version`
- Try: `npm install --legacy-peer-deps`

## Examples

### Example 1: Browser Chat Session
```
1. Open http://localhost:8888
2. Click "Connect" → Status shows "Connected"
3. Type "Hello WebSocket!" → Press Enter
4. See echo: "← Hello WebSocket!"
5. Stats show: Sent: 1, Received: 1
```

### Example 2: Node.js Interactive
```bash
$ npm run interactive
ws> Hello from Node!
→ Sending: "Hello from Node!"
← Received: "Hello from Node!"
ws> quit
Connection closed
```

### Example 3: Automated Testing
```bash
$ npm test
Running: Test 1: Basic connection
  ✓ PASSED: Connection established

Running: Test 2: Echo text message
  ✓ PASSED: Correctly echoed: "Hello, WebSocket!"

[... 5 more tests ...]

Test Summary:
  Total: 7
  Passed: 7
  Failed: 0
```

## Next Steps

- Add message queueing and persistence
- Implement broadcast to all connected clients
- Add authentication/authorization
- Support for WebSocket subprotocols
- Add TLS/SSL support (wss://)
- Implement rate limiting
- Add compression (permessage-deflate)

## License

Part of the Canopy project.
