<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# WebSocket Calculator Mode Documentation

## Overview

The WebSocket demo now supports two modes:
1. **Echo Mode**: Simple text-based WebSocket echoing using `WSLAY_TEXT_FRAME`
2. **Calculator Mode**: Binary RPC using protobuf-encoded messages with `WSLAY_BINARY_FRAME`

## Calculator Mode

### Protocol

Calculator mode uses the Canopy framework over WebSockets with protobuf encoding. Messages are wrapped in two layers:

1. **Outer Layer**: `websocket::envelope`
   ```cpp
   struct envelope {
       uint64_t message_type;
       std::vector<char> data;
   };
   ```

2. **Inner Layer**: `websocket::request`
   ```cpp
   struct request {
       rpc::encoding encoding;        // Set to 0 (default)
       uint64_t tag;                   // Set to 0 (not used)
       uint64_t caller_zone_id;        // Set to 0 (not used)
       uint64_t destination_zone_id;   // Set to 0 (not used)
       uint64_t object_id;             // Set to 0 (not used)
       uint64_t interface_id;          // Set to 0 (not used)
       uint64_t method_id;             // 1=add, 2=subtract, 3=multiply, 4=divide
       std::vector<char> data;         // Protobuf-encoded parameters
       std::vector<rpc::back_channel_entry> back_channel; // Empty
   };
   ```

### Method IDs

The calculator interface (`i_calculator`) has four methods:

| Method ID | Operation | Signature |
|-----------|-----------|-----------|
| 1 | Add | `int add(double first_val, double second_val, [out] double& response)` |
| 2 | Subtract | `int subtract(double first_val, double second_val, [out] double& response)` |
| 3 | Multiply | `int multiply(double first_val, double second_val, [out] double& response)` |
| 4 | Divide | `int divide(double first_val, double second_val, [out] double& response)` |

### Message Format

#### Request

1. **Encode Parameters**: Two doubles encoded as protobuf fields:
   - Field 1: `first_val` (tag=1, wire_type=1, fixed64)
   - Field 2: `second_val` (tag=2, wire_type=1, fixed64)

2. **Create Request**: Wrap parameters in `websocket::request` structure:
   - Set all zone/object/interface IDs to 0
   - Set `method_id` to 1-4 based on operation
   - Put encoded parameters in `data` field

3. **Create Envelope**: Wrap request in `websocket::envelope`:
   - Set `message_type` to request type ID
   - Put encoded request in `data` field

4. **Send**: Send the envelope as a binary WebSocket message

#### Response

1. **Receive Binary Message**: Server sends back protobuf-encoded response

2. **Decode Envelope**: Extract `websocket::envelope`:
   - Extract `message_type` and `data` fields

3. **Decode Response**: Extract `websocket::response` from envelope data:
   ```cpp
   struct response {
       uint64_t error;              // 0 = success
       std::vector<char> data;      // Protobuf-encoded result
       std::vector<rpc::back_channel_entry> back_channel;
   };
   ```

4. **Extract Result**: Decode response data to get the result:
   - Field 3: `response` (tag=3, wire_type=1, fixed64) - the calculated result

### Example Usage

#### Browser (JavaScript)

**Important**: The fingerprint ID `12812964479505592837` exceeds JavaScript's `Number.MAX_SAFE_INTEGER` (2^53-1), so it must be represented as a `Long` using protobuf.js's Long type to maintain precision.

```javascript
// Switch to calculator mode
document.getElementById('calculatorMode').checked = true;

// Connect to WebSocket
ws = new WebSocket('ws://localhost:8888');
ws.binaryType = 'arraybuffer';

// Perform calculation: 5.5 + 3.2
const first = 5.5;
const second = 3.2;
const methodId = 1; // add

// Encode and send request
const paramsData = encodeCalculatorParams(first, second);
const requestData = createWebSocketRequest(methodId, paramsData);
// Use Long for message_type to preserve precision
const REQUEST_MESSAGE_TYPE = Long.fromString("12812964479505592837", true);
const envelopeData = createWebSocketEnvelope(REQUEST_MESSAGE_TYPE, requestData);
ws.send(envelopeData);

// Handle response
ws.onmessage = (event) => {
    const envelope = decodeEnvelope(event.data);
    const response = decodeResponse(envelope.data);
    const result = extractResultFromResponseData(response.data);
    console.log(`Result: ${result}`); // 8.7
};
```

#### Node.js

```bash
cd /rpc/demos/websocket/client
node test_calculator.js
```

The test suite will run calculations and verify results.

## Generated Protobuf Code

The WebSocket demo uses **generated protobuf code** from the IDL compiler output located in:
- **Source Proto Files**: `/build/generated/src/websocket_demo/protobuf/*.proto`
- **Browser JS**: `/demos/websocket/server/www/websocket_proto.js` (static module)
- **Node.js JS**: `/demos/websocket/client/websocket_proto.js` (CommonJS module)

### Automatic Generation via CMake

The protobuf JavaScript files are **automatically generated** during the build process:

```bash
# Build the project (generates proto JS files automatically)
cmake --build build

# Or specifically build just the JS proto files
cmake --build build --target websocket_proto_js
```

**Requirements**:
- Node.js and npm must be installed
- CMake will automatically run `npm install` to get `protobufjs-cli`
- Files regenerate when `.idl` or `.proto` files change

**CMake Integration** (`demos/websocket/idl/CMakeLists.txt`):
- Detects `npm` and `node` executables
- Installs `protobufjs-cli` via npm if needed
- Generates browser version (static module)
- Generates Node.js version (CommonJS module)
- Sets up proper dependencies on IDL generation

### Manual Generation

If you need to regenerate manually:

```bash
# Browser version (static module)
cd /rpc/demos/websocket/client
./node_modules/.bin/pbjs -t static-module \
  --path /rpc/build/generated/src \
  -o /rpc/demos/websocket/server/www/websocket_proto.js \
  websocket_demo/protobuf/websocket_demo_all.proto

# Node.js version (CommonJS)
./node_modules/.bin/pbjs -t static-module -w commonjs \
  --path /rpc/build/generated/src \
  -o ./websocket_proto.js \
  websocket_demo/protobuf/websocket_demo_all.proto
```

### Using Generated Protobuf

**Browser**:
```javascript
// Access protobuf messages
const WebsocketProto = protobuf.roots.default.websocket;
const RpcProto = roots.default.rpc;

// Create and encode request
const requestMessage = WebsocketProto.add_Request.create({
    firstVal: 5.5,
    secondVal: 3.2
});
const bytes = WebsocketProto.add_Request.encode(requestMessage).finish();
```

**Node.js**:
```javascript
const proto = require('./websocket_proto.js');
const WebsocketProto = proto.websocket;

// Same usage as browser
```

## Protobuf Wire Format

The implementation uses protobuf wire format for encoding:

### Wire Types

- **0 (Varint)**: Used for uint64 fields (method_id, error, message_type, etc.)
- **1 (Fixed64)**: Used for double fields (parameters and results)
- **2 (Length-delimited)**: Used for bytes/vector fields (data, back_channel)

### Encoding Example

For `add(5.5, 3.2)`:

1. **Parameters** (17 bytes):
   ```
   09                    // Tag 1, wire type 1 (fixed64)
   66 66 66 66 66 66 16 40  // 5.5 as little-endian double
   11                    // Tag 2, wire type 1 (fixed64)
   CD CC CC CC CC CC 09 40  // 3.2 as little-endian double
   ```

2. **Request** (28 bytes):
   ```
   08 00     // encoding = 0
   10 00     // tag = 0
   18 00     // caller_zone_id = 0
   20 00     // destination_zone_id = 0
   28 00     // object_id = 0
   30 00     // interface_id = 0
   38 01     // method_id = 1 (add)
   42 11     // data field, length = 17
   [17 bytes of encoded parameters]
   ```

3. **Envelope**:
   ```
   08 B6 CC C7 DA C6 8B AA 90 01  // message_type = 12812964479505592837 (varint encoded)
   12 1C                          // data field, length = 28
   [28 bytes of encoded request]
   ```

## Implementation Notes

### Browser Client

- Uses manual protobuf encoding (no external library required)
- Supports both text (echo) and binary (calculator) modes
- Automatically switches UI based on selected mode
- Displays results in real-time

### Node.js Client

- Includes comprehensive test suite with 8 test cases
- Tests all four operations (add, subtract, multiply, divide)
- Uses Buffer API for efficient binary encoding
- Validates results with floating-point tolerance

### Server

- Handles both text and binary messages
- Text messages: Echoed back directly
- Binary messages: Decoded, processed through RPC framework, response sent back
- Supports full Canopy infrastructure with zones and services

## Debugging

### Browser Console

Open browser developer tools and check console for:
- Protobuf encoding/decoding logs
- WebSocket message details
- Error messages

### Network Tab

In browser dev tools, inspect WebSocket frames:
- Text frames: Echo mode messages
- Binary frames: Calculator mode messages (shown as binary data)

### Server Logs

Check server console output for:
- Connection events
- Message processing logs
- RPC call details

## Troubleshooting

### "Failed to decode response"

- Check that server is running and supports calculator interface
- Verify `message_type` ID matches server expectations (12812964479505592837)
- **Ensure message_type uses Long type** - JavaScript numbers lose precision above 2^53-1
- Ensure protobuf encoding matches server's protobuf library

### "Calculator error: X"

- Error code from RPC layer
- Check method_id (must be 1-4)
- Verify parameters are valid doubles

### No response received

- Check WebSocket connection is established
- Ensure binary message was sent (not text)
- Verify server has `i_calculator` implementation

## Future Enhancements

- Add support for more complex types (structs, arrays)
- Implement request-response correlation with tag field
- Add authentication using zone/object IDs
- Support for streaming/multiple responses
