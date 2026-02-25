#!/usr/bin/env node

const WebSocket = require('ws');
const proto = require('./websocket_proto.js');
const Long = require('protobufjs/minimal').util.Long;

const WS_URL = process.env.WS_URL || 'ws://localhost:8888';

console.log('WebSocket Calculator RPC Test Suite');
console.log('====================================\n');

// Get the protobuf messages
const WebsocketProto = proto.protobuf.websocket_demo_v1;
const RpcProto = proto.protobuf.rpc;

// Interface and message type IDs (uint64 as Long strings)
const I_CALCULATOR_ID = Long.fromString('13570836582854900302', true);
const REQUEST_MESSAGE_TYPE = Long.fromString('3111821184188816472', true);

// Test runner
let testsPassed = 0;
let testsFailed = 0;
let messageCounter = 0;

function runCalculatorTest(operation, methodId, first, second, expected) {
    return new Promise((resolve) => {
        console.log(`Testing: ${first} ${operation} ${second} = ${expected}`);

        const ws = new WebSocket(WS_URL);
        ws.binaryType = 'nodebuffer';

        let handshakeComplete = false;
        let clientZoneId = null;
        let serverZoneId = null;
        let serverObjectId = null;

        const timeout = setTimeout(() => {
            console.log(`  ✗ FAILED: Timeout\n`);
            testsFailed++;
            ws.close();
            resolve();
        }, 5000);

        ws.on('open', () => {
            try {
                // Send connect_request handshake.
                // Zone is left as 0 — the server assigns it via generate_new_zone_id().
                // Object id 1 is used as the client's back-channel (i_context_event) stub.
                const connectReq = WebsocketProto.connect_request.create({
                    inboundRemoteObject: RpcProto.remote_object.create({
                        addr_: RpcProto.zone_address.create({ objectId: 1 })
                    })
                });
                ws.send(WebsocketProto.connect_request.encode(connectReq).finish());
            } catch (err) {
                clearTimeout(timeout);
                console.log(`  ✗ FAILED: Handshake encoding error: ${err.message}\n`);
                testsFailed++;
                ws.close();
            }
        });

        ws.on('message', (data) => {
            try {
                if (!handshakeComplete) {
                    // First binary message is connect_response
                    const connectResp = WebsocketProto.connect_response.decode(data);
                    clientZoneId = connectResp.callerZoneId && connectResp.callerZoneId.addr_
                        ? connectResp.callerZoneId.addr_.subnetId : 0;
                    serverZoneId = connectResp.outboundRemoteObject && connectResp.outboundRemoteObject.addr_
                        ? connectResp.outboundRemoteObject.addr_.subnetId : 0;
                    serverObjectId = connectResp.outboundRemoteObject && connectResp.outboundRemoteObject.addr_
                        ? connectResp.outboundRemoteObject.addr_.objectId : 0;
                    handshakeComplete = true;

                    // Now send the calculator request
                    messageCounter++;
                    const messageId = messageCounter;

                    // Encode the inner method request
                    let requestMessage;
                    switch (methodId) {
                        case 1:
                            requestMessage = WebsocketProto.i_calculator_addRequest.create({
                                firstVal: first, secondVal: second });
                            break;
                        case 2:
                            requestMessage = WebsocketProto.i_calculator_subtractRequest.create({
                                firstVal: first, secondVal: second });
                            break;
                        case 3:
                            requestMessage = WebsocketProto.i_calculator_multiplyRequest.create({
                                firstVal: first, secondVal: second });
                            break;
                        case 4:
                            requestMessage = WebsocketProto.i_calculator_divideRequest.create({
                                firstVal: first, secondVal: second });
                            break;
                    }

                    const requestBytes = Object.getPrototypeOf(requestMessage).constructor.encode(requestMessage).finish();

                    // Build the typed request
                    const wsRequest = WebsocketProto.request.create({
                        encoding: RpcProto.encoding.encoding_protocol_buffers,
                        tag: Long.fromNumber(messageId, true),
                        callerZoneId: RpcProto.caller_zone.create({
                            addr_: RpcProto.zone_address.create({ subnetId: clientZoneId })
                        }),
                        destinationZoneId: RpcProto.remote_object.create({
                            addr_: RpcProto.zone_address.create({ subnetId: serverZoneId, objectId: serverObjectId })
                        }),
                        interfaceId: RpcProto.interface_ordinal.create({ id: I_CALCULATOR_ID }),
                        methodId: RpcProto.method.create({ id: Long.fromNumber(methodId, true) }),
                        data: requestBytes,
                        backChannel: []
                    });

                    const wsRequestBytes = WebsocketProto.request.encode(wsRequest).finish();

                    const envelope = WebsocketProto.envelope.create({
                        messageId: Long.fromNumber(messageId, true),
                        messageType: REQUEST_MESSAGE_TYPE,
                        data: wsRequestBytes
                    });

                    ws.send(WebsocketProto.envelope.encode(envelope).finish());
                    return;
                }

                // Subsequent messages are responses wrapped in an envelope
                const envelope = WebsocketProto.envelope.decode(data);
                const response = WebsocketProto.response.decode(envelope.data);

                clearTimeout(timeout);

                if (response.error == 0 && response.data && response.data.length > 0) {
                    let resultValue = null;
                    switch (methodId) {
                        case 1: resultValue = WebsocketProto.i_calculator_addResponse.decode(response.data); break;
                        case 2: resultValue = WebsocketProto.i_calculator_subtractResponse.decode(response.data); break;
                        case 3: resultValue = WebsocketProto.i_calculator_multiplyResponse.decode(response.data); break;
                        case 4: resultValue = WebsocketProto.i_calculator_divideResponse.decode(response.data); break;
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
                console.log(`  ✗ FAILED: Decode error: ${err.message}\n`);
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
