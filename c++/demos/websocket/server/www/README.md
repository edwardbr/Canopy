<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# WebSocket Browser Client

Browser-based WebSocket test client with dual-mode support: Echo and Calculator.

## Usage

1. **Start the WebSocket server:**
   ```bash
   ./build/output/debug/websocket_server
   ```

2. **Open your browser:**
   Navigate to: `http://localhost:8888`

3. **Connect and Test:**
   - Click the "Connect" button
   - Choose between Echo or Calculator mode
   - **Echo Mode**: Type messages and see them echoed back
   - **Calculator Mode**: Perform RPC calculations using the calculator service

## Modes

### Echo Mode (Text WebSocket)
- Simple text-based message echoing
- Type any message and receive it back
- Uses `WSLAY_TEXT_FRAME` for transport

### Calculator Mode (Binary WebSocket + Protobuf)
- Remote procedure calls to calculator service
- Operations: Add, Subtract, Multiply, Divide
- Uses `WSLAY_BINARY_FRAME` with protobuf encoding
- Full Canopy framework integration
- See [CALCULATOR.md](CALCULATOR.md) for protocol details

## Features

- **Dual Mode Support**: Switch between Echo and Calculator modes
- **Modern UI**: Beautiful gradient design with color-coded messages
- **Real-time Stats**: Track sent/received messages and connection uptime
- **Message History**: Scrollable message log with timestamps
- **Calculator UI**: Dedicated interface with operation selector and result display
- **Color Coding**:
  - 🔵 Blue: Sent messages
  - 🟢 Green: Received messages
  - ⚪ Gray: System messages
  - 🔴 Red: Error messages

## Files

- `index.html` - Main HTML structure, styling, and mode switcher UI
- `client.js` - WebSocket client logic, protobuf encoding, and UI controls
- `CALCULATOR.md` - Detailed protocol documentation for calculator mode

## Server Endpoints

- `GET /` or `GET /index.html` - Main web interface
- `GET /client.js` - JavaScript client code
- `WebSocket ws://localhost:8888` - WebSocket endpoint (supports both text and binary)

## Technical Details

- **Protobuf Encoding**: Uses **generated protobuf JavaScript** from IDL compiler output
- **Protobuf.js Library**: Uses protobufjs minimal runtime for encoding/decoding
- **Generated Code**: `websocket_proto.js` compiled from `.proto` files in `/build/generated/`
- **Binary Support**: WebSocket `binaryType = 'arraybuffer'` for calculator mode
- **Automatic WebSocket URL Detection**: Uses current host
- **Responsive Design**: Works on desktop and mobile
- **Pure Vanilla JavaScript**: No frameworks required (except protobuf.js for binary encoding)

## Protocol

### Echo Mode
- **Transport**: Text WebSocket frames (`WSLAY_TEXT_FRAME`)
- **Format**: Plain UTF-8 strings
- **Flow**: Client sends text → Server echoes back same text

### Calculator Mode
- **Transport**: Binary WebSocket frames (`WSLAY_BINARY_FRAME`)
- **Format**: Protobuf-encoded RPC messages
- **Flow**:
  1. Client encodes parameters as protobuf
  2. Wraps in `websocket::request` structure
  3. Wraps in `websocket::envelope` structure
  4. Sends as binary message
  5. Server processes through Canopy framework
  6. Returns result in `websocket::response` wrapped in `websocket::envelope`
  7. Client decodes and displays result

See [CALCULATOR.md](CALCULATOR.md) for complete protocol specification.
