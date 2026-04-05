<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# WebSocket Client for Canopy WebSocket Server

Node.js WebSocket client for testing the C++ WebSocket server with Echo and Calculator RPC modes.

## Installation

```bash
cd demos/websocket/client
npm install
```

## Usage

### 1. Simple Echo Client (Automated)
Connects, sends a few test messages, and disconnects:

```bash
npm start
# or
node client.js
```

### 2. Interactive Echo Client
Interactive console for manual testing:

```bash
npm run interactive
# or
node interactive.js
```

Type messages and press Enter to send them. Type `quit` or press Ctrl+C to exit.

### 3. Echo Test Suite
Runs a comprehensive WebSocket echo test suite:

```bash
npm test
# or
node test.js
```

Tests include:
- Basic connection
- Text message echo
- Multiple sequential messages
- Binary data echo
- Large messages (10KB)
- Unicode character support
- Graceful connection close

### 4. Calculator RPC Test Suite
Runs calculator operations using binary WebSocket + Protobuf:

```bash
node test_calculator.js
```

Tests include:
- Addition operations (positive, negative, decimal)
- Subtraction operations
- Multiplication operations
- Division operations
- Protobuf encoding/decoding validation
- RPC framework integration

### 5. REST API Test Suite
Runs a comprehensive REST API test suite:

```bash
npm run test-rest
# or
node test_rest.js
```

Tests include:
- GET /api/status (server status)
- GET /api/resource/:id (retrieve resource)
- POST /api/resource (create resource)
- POST /api/* (generic POST with body)
- PUT /api/resource/:id (update resource)
- DELETE /api/resource/:id (delete resource)
- Error handling (404, 405)
- Large JSON payloads

## Configuration

### WebSocket Server URL
Set the WebSocket server URL via environment variable:

```bash
WS_URL=ws://192.168.1.100:8888 npm start
```

Default: `ws://localhost:8888`

### REST API Server URL
Set the REST API server URL via environment variable:

```bash
API_URL=http://192.168.1.100:8888 npm run test-rest
```

Default: `http://localhost:8888`

## Examples

### Start the C++ server
```bash
./build/output/debug/websocket_server
```

### Run the simple client
```bash
cd demos/websocket/client
npm start
```

Expected output:
```
Connecting to ws://localhost:8888...
✓ Connected to WebSocket server

Sending: "Hello from Node.js client!"
Received: "Hello from Node.js client!"
Sending: "Second message"
Received: "Second message"
Sending: "Third message - closing after this"
Received: "Third message - closing after this"

Closing connection...
✓ Connection closed (code: 1000, reason: Normal closure)
```

### Run the WebSocket test suite
```bash
npm test
```

Expected output:
```
WebSocket Echo Server Test Suite
=================================

Connecting to ws://localhost:8888

Running: Test 1: Basic connection
  ✓ PASSED: Connection established

Running: Test 2: Echo text message
  ✓ PASSED: Correctly echoed: "Hello, WebSocket!"

[...]

=================================
Test Summary:
  Total: 7
  Passed: 7
  Failed: 0
```

### Run the REST API test suite
```bash
npm run test-rest
```

Expected output:
```
REST API Test Suite
===================

Testing API at http://localhost:8888

Running: Test 1: GET /api/status
  ✓ PASSED: Status: running, Version: 1.0

Running: Test 2: GET /api/resource/123
  ✓ PASSED: Got resource data: {"id":123,"name":"example","value":"data"}

Running: Test 3: POST /api/resource (create)
  ✓ PASSED: Resource created with ID: 456

[...]

===================
Test Summary:
  Total: 9
  Passed: 9
  Failed: 0
```

## Requirements

- Node.js 14+ (uses ES6 features)
- `ws` package (WebSocket library)

## Modes

### Echo Mode (Text WebSocket)
- Simple text message echoing
- Uses `WSLAY_TEXT_FRAME` for transport
- Tests: `test.js`, clients: `client.js`, `interactive.js`

### Calculator Mode (Binary WebSocket + Protobuf)
- RPC-based calculator service
- Operations: add, subtract, multiply, divide
- Uses `WSLAY_BINARY_FRAME` with protobuf encoding
- **Generated protobuf code** from IDL compiler `.proto` files
- Full Canopy framework integration
- Method IDs: 1=add, 2=subtract, 3=multiply, 4=divide
- Tests: `test_calculator.js`

## Protobuf Code Generation

The calculator test uses generated protobuf JavaScript from the Canopy IDL compiler output.

### Automatic Generation

The protobuf JavaScript files are **automatically generated** when you build the Canopy project:

```bash
# Build the project (generates proto JS files automatically)
cd /rpc
cmake --build build

# The websocket_proto.js file is automatically created in demos/websocket/client/
```

### Manual Generation

If you need to regenerate manually:

```bash
# Install protobufjs-cli (only needed once)
npm install --save-dev protobufjs-cli

# Generate Node.js protobuf code
./node_modules/.bin/pbjs -t static-module -w commonjs \
  --path /rpc/build/generated/src \
  -o ./websocket_proto.js \
  websocket_demo/protobuf/websocket_demo_all.proto
```

### Generated Content

The generated `websocket_proto.js` file contains:
- `websocket.envelope` - Message envelope wrapper
- `websocket.request` - RPC request structure
- `websocket.response` - RPC response structure
- `websocket.add_Request` / `add_Response` - Calculator operation messages
- `websocket.subtract_Request` / `subtract_Response`
- `websocket.multiply_Request` / `multiply_Response`
- `websocket.divide_Request` / `divide_Response`
- `rpc.encoding` - RPC encoding enumeration

## Files

- `client.js` - Simple automated echo client
- `interactive.js` - Interactive echo console client
- `test.js` - Comprehensive echo test suite
- `test_calculator.js` - Calculator RPC test suite (protobuf + binary WebSocket)
- `test_rest.js` - Comprehensive REST API test suite
- `package.json` - NPM package configuration
- `README.md` - This file
