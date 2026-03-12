#!/usr/bin/env node

const WebSocket = require('ws');
const api = require('./websocket_api.js');

const WS_URL = process.env.WS_URL || 'ws://localhost:8888';

console.log('WebSocket Calculator RPC Test Suite');
console.log('====================================\n');

let testsPassed = 0;
let testsFailed = 0;

async function runCalculatorTest(operation, first, second, expected) {
    console.log(`Testing: ${first} ${operation} ${second} = ${expected}`);

    const client = api.createClient({
        WebSocketImpl: WebSocket,
        websocketUrl: WS_URL
    });

    try {
        await client.connect({ callbackObjectId: 1 });
        const result = await client.call({
            interfaceName: 'i_calculator',
            methodName: operation,
            payloadType: `i_calculator_${operation}Request`,
            responseType: `i_calculator_${operation}Response`,
            payload: {
                firstVal: first,
                secondVal: second
            }
        });

        if (result.response.error !== 0) {
            console.log(`  ✗ FAILED: Error code ${result.response.error}\n`);
            testsFailed++;
            return;
        }

        if (!result.decoded) {
            console.log('  ✗ FAILED: Could not decode result\n');
            testsFailed++;
            return;
        }

        const match = Math.abs(result.decoded.response - expected) < 0.0001;
        if (!match) {
            console.log(`  ✗ FAILED: Expected ${expected}, got ${result.decoded.response}\n`);
            testsFailed++;
            return;
        }

        console.log(`  ✓ PASSED: Got ${result.decoded.response}\n`);
        testsPassed++;
    } catch (err) {
        console.log(`  ✗ FAILED: ${err.message}\n`);
        testsFailed++;
    } finally {
        client.disconnect();
    }
}

async function runAllTests() {
    await runCalculatorTest('add', 5, 3, 8);
    await runCalculatorTest('subtract', 10, 4, 6);
    await runCalculatorTest('multiply', 7, 6, 42);
    await runCalculatorTest('divide', 20, 4, 5);
    await runCalculatorTest('add', -5, 3, -2);
    await runCalculatorTest('multiply', 0, 100, 0);

    console.log('====================================');
    console.log(`Tests passed: ${testsPassed}`);
    console.log(`Tests failed: ${testsFailed}`);
    console.log(`Total: ${testsPassed + testsFailed}`);

    process.exit(testsFailed > 0 ? 1 : 0);
}

runAllTests().catch((err) => {
    console.error('Test suite failed:', err);
    process.exit(1);
});
