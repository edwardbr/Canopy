#!/usr/bin/env node

const proto = require('./generated/websocket_demo_proto.js');
const WebsocketDemo = require('./generated/websocket_demo.js');
const CanopyWebsocketTransport = require('./generated/canopy_websocket_transport.js');

const WS_URL = process.env.WS_URL || 'ws://localhost:8888';
const TransportProto = proto.protobuf.websocket_protocol_v1;
const AppProto = proto.protobuf.websocket_demo_v1;

console.log('WebSocket Calculator RPC Test Suite');
console.log('====================================\n');
console.log(`Connecting to ${WS_URL}\n`);

let testsPassed = 0;
let testsFailed = 0;

async function runCalculatorTest(operation, method, first, second, expected) {
    console.log(`Testing: ${first} ${operation} ${second} = ${expected}`);

    const transport = new CanopyWebsocketTransport({
        url: WS_URL,
        proto: TransportProto,
        appProto: AppProto,
        inboundInterfaceId: WebsocketDemo.interfaceIds.i_context_event,
        outboundInterfaceId: WebsocketDemo.interfaceIds.i_calculator,
        timeoutMs: 5000,
    });

    await transport.connect();
    try {
        const calc = new WebsocketDemo.i_calculator_proxy(transport, AppProto);
        const r = await calc[method](first, second);
        const match = Math.abs(r.response - expected) < 0.0001;
        if (match) {
            console.log(`  ✓ PASSED: Got ${r.response}\n`);
            testsPassed++;
        } else {
            console.log(`  ✗ FAILED: Expected ${expected}, got ${r.response}\n`);
            testsFailed++;
        }
    } finally {
        transport.disconnect();
    }
}

async function runAllTests() {
    // Test addition
    await runCalculatorTest('add', 'add', 5.5, 3.2, 8.7);
    await runCalculatorTest('add', 'add', -10, 5, -5);

    // Test subtraction
    await runCalculatorTest('subtract', 'subtract', 10, 3, 7);
    await runCalculatorTest('subtract', 'subtract', 5.5, 2.5, 3);

    // Test multiplication
    await runCalculatorTest('multiply', 'multiply', 6, 7, 42);
    await runCalculatorTest('multiply', 'multiply', 2.5, 4, 10);

    // Test division
    await runCalculatorTest('divide', 'divide', 10, 2, 5);
    await runCalculatorTest('divide', 'divide', 7, 2, 3.5);

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
