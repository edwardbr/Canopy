#!/usr/bin/env node

const WebSocket = require('ws');
const proto = require('./websocket_proto.js');

const WS_URL = process.env.WS_URL || 'ws://localhost:8888';

console.log('WebSocket Calculator RPC Test Suite');
console.log('====================================\n');

// Get the protobuf messages
const WebsocketProto = proto.protobuf.websocket_demo_v1;
const RpcProto = proto.protobuf.rpc;

// Test runner
let testsPassed = 0;
let testsFailed = 0;
let messageCounter = 0;
let pendingRequests = new Map(); // messageId -> {methodId}

function runCalculatorTest(operation, methodId, first, second, expected) {
    return new Promise((resolve) => {
        const opName = operation.toUpperCase();
        console.log(`Testing: ${first} ${operation} ${second} = ${expected}`);

        const ws = new WebSocket(WS_URL);

        const timeout = setTimeout(() => {
            console.log(`  ✗ FAILED: Timeout\n`);
            testsFailed++;
            ws.close();
            resolve();
        }, 5000);

        ws.on('open', () => {
            try {
                // Increment message counter for unique ID
                messageCounter++;
                const messageId = messageCounter;

                // Create the request message based on the method
                let requestMessage;
                switch (methodId) {
                    case 1:
                        requestMessage = WebsocketProto.i_calculator_addRequest.create({
                            firstVal: first,
                            secondVal: second
                        });
                        break;
                    case 2:
                        requestMessage = WebsocketProto.i_calculator_subtractRequest.create({
                            firstVal: first,
                            secondVal: second
                        });
                        break;
                    case 3:
                        requestMessage = WebsocketProto.i_calculator_multiplyRequest.create({
                            firstVal: first,
                            secondVal: second
                        });
                        break;
                    case 4:
                        requestMessage = WebsocketProto.i_calculator_divideRequest.create({
                            firstVal: first,
                            secondVal: second
                        });
                        break;
                }

                // Encode the request message
                const requestBytes = Object.getPrototypeOf(requestMessage).constructor.encode(requestMessage).finish();

                // Create the websocket::request wrapper
                const wsRequest = WebsocketProto.request.create({
                    encoding: RpcProto.encoding.encoding_UNSPECIFIED,
                    tag: 0,
                    callerZoneId: 0,
                    destinationZoneId: 0,
                    objectId: 0,
                    interfaceId: 0,
                    methodId: methodId,
                    data: requestBytes,
                    backChannel: []
                });

                // Encode the websocket::request
                const wsRequestBytes = WebsocketProto.request.encode(wsRequest).finish();

                // Create the envelope
                // Use the fingerprint ID for websocket::request as a Long (uint64)
                // JavaScript numbers lose precision above 2^53-1, so use protobuf Long
                const protobuf = require('protobufjs/minimal');
                const REQUEST_MESSAGE_TYPE = protobuf.util.Long.fromString("12812964479505592837", true);
                const envelope = WebsocketProto.envelope.create({
                    messageId: protobuf.util.Long.fromNumber(messageId, true),
                    messageType: REQUEST_MESSAGE_TYPE,
                    data: wsRequestBytes
                });

                // Store the request info for response matching
                pendingRequests.set(messageId, { methodId });

                // Encode and send the envelope
                const envelopeBytes = WebsocketProto.envelope.encode(envelope).finish();
                ws.send(envelopeBytes);
            } catch (err) {
                clearTimeout(timeout);
                console.log(`  ✗ FAILED: Encoding error: ${err.message}\n`);
                testsFailed++;
                ws.close();
            }
        });

        ws.on('message', (data) => {
            try {
                clearTimeout(timeout);

                // Decode the envelope
                const envelope = WebsocketProto.envelope.decode(data);

                // Get the message ID and match to pending request
                const messageId = envelope.messageId.toNumber();
                const requestInfo = pendingRequests.get(messageId);
                if (!requestInfo) {
                    console.log(`  ✗ FAILED: Received response for unknown request ID: ${messageId}\n`);
                    testsFailed++;
                    ws.close();
                    return;
                }
                pendingRequests.delete(messageId);

                // Decode the response from the envelope data
                const response = WebsocketProto.response.decode(envelope.data);

                if (response.error == 0 && response.data && response.data.length > 0) {
                    // Decode the response data to get the result
                    // Use protobufjs Reader to extract field 3 (the out parameter)
                    const protobuf = require('protobufjs/minimal');
                    const reader = protobuf.Reader.create(response.data);
                    let resultValue = null;
                    switch (requestInfo.methodId) {
                        case 1:

                            resultValue = WebsocketProto.i_calculator_addResponse.decode(response.data);
                            break;
                        case 2:
                            resultValue = WebsocketProto.i_calculator_subtractResponse.decode(response.data);
                            break;
                        case 3:
                            resultValue = WebsocketProto.i_calculator_multiplyResponse.decode(response.data);
                            break;
                        case 4:
                            resultValue = WebsocketProto.i_calculator_divideResponse.decode(response.data);
                            break;
                    }

                    if (resultValue !== null) {
                        const match = Math.abs(resultValue.response - expected) < 0.0001;
                        if (match) {
                            console.log(`  ✓ PASSED: Got ${resultValue.response}\n`);
                            testsPassed++;
                        } else {
                            console.log(`  ✗ FAILED: Expected ${expected}, got ${resultValue.response}\n`);
                            testsFailed++;
                        }
                    } else {
                        console.log(`  ✗ FAILED: Could not extract result from response\n`);
                        testsFailed++;
                    }
                } else {
                    console.log(`  ✗ FAILED: Error code ${response.error}\n`);
                    testsFailed++;
                }

                ws.close();
            } catch (err) {
                clearTimeout(timeout);
                console.log(`  ✗ FAILED: Decoding error: ${err.message}\n`);
                testsFailed++;
                ws.close();
            }
        });

        ws.on('close', () => {
            resolve();
        });

        ws.on('error', (err) => {
            clearTimeout(timeout);
            console.log(`  ✗ FAILED: ${err.message}\n`);
            testsFailed++;
            resolve();
        });
    });
}

async function runAllTests() {
    console.log(`Connecting to ${WS_URL}\n`);

    // Test addition (method_id = 1)
    await runCalculatorTest('add', 1, 5.5, 3.2, 8.7);
    await new Promise(resolve => setTimeout(resolve, 100));

    await runCalculatorTest('add', 1, -10, 5, -5);
    await new Promise(resolve => setTimeout(resolve, 100));

    // Test subtraction (method_id = 2)
    await runCalculatorTest('subtract', 2, 10, 3, 7);
    await new Promise(resolve => setTimeout(resolve, 100));

    await runCalculatorTest('subtract', 2, 5.5, 2.5, 3);
    await new Promise(resolve => setTimeout(resolve, 100));

    // Test multiplication (method_id = 3)
    await runCalculatorTest('multiply', 3, 6, 7, 42);
    await new Promise(resolve => setTimeout(resolve, 100));

    await runCalculatorTest('multiply', 3, 2.5, 4, 10);
    await new Promise(resolve => setTimeout(resolve, 100));

    // Test division (method_id = 4)
    await runCalculatorTest('divide', 4, 10, 2, 5);
    await new Promise(resolve => setTimeout(resolve, 100));

    await runCalculatorTest('divide', 4, 7, 2, 3.5);
    await new Promise(resolve => setTimeout(resolve, 100));

    // Print summary
    console.log('====================================');
    console.log('Test Summary:');
    console.log(`  Total: ${testsPassed + testsFailed}`);
    console.log(`  Passed: ${testsPassed}`);
    console.log(`  Failed: ${testsFailed}`);

    process.exit(testsFailed > 0 ? 1 : 0);
}

runAllTests().catch(err => {
    console.error('Test suite error:', err);
    process.exit(1);
});
