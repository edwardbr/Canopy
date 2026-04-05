#!/usr/bin/env node

const WebSocket = require('ws');

const WS_URL = process.env.WS_URL || 'ws://localhost:8888';

console.log('WebSocket Echo Server Test Suite');
console.log('=================================\n');

let testsPassed = 0;
let testsFailed = 0;

function runTest(name, testFn) {
    return new Promise((resolve) => {
        console.log(`Running: ${name}`);
        const ws = new WebSocket(WS_URL);

        const timeout = setTimeout(() => {
            console.log(`  ✗ FAILED: Timeout\n`);
            testsFailed++;
            ws.close();
            resolve();
        }, 5000);

        ws.on('open', () => {
            testFn(ws, (success, message) => {
                clearTimeout(timeout);
                if (success) {
                    console.log(`  ✓ PASSED${message ? ': ' + message : ''}\n`);
                    testsPassed++;
                } else {
                    console.log(`  ✗ FAILED${message ? ': ' + message : ''}\n`);
                    testsFailed++;
                }
                ws.close();
            });
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
    // Test 1: Basic connection
    await runTest('Test 1: Basic connection', (ws, done) => {
        done(true, 'Connection established');
    });
    await new Promise(resolve => setTimeout(resolve, 100)); // Small delay between tests

    // Test 2: Echo text message
    await runTest('Test 2: Echo text message', (ws, done) => {
        const testMessage = 'Hello, WebSocket!';
        ws.on('message', (data) => {
            const received = data.toString();
            if (received === testMessage) {
                done(true, `Correctly echoed: "${received}"`);
            } else {
                done(false, `Expected "${testMessage}", got "${received}"`);
            }
        });
        ws.send(testMessage);
    });
    await new Promise(resolve => setTimeout(resolve, 100)); // Small delay between tests

    // Test 3: Multiple messages in sequence
    await runTest('Test 3: Multiple messages in sequence', (ws, done) => {
        const messages = ['First', 'Second', 'Third'];
        let received = [];

        ws.on('message', (data) => {
            received.push(data.toString());
            if (received.length === messages.length) {
                const allMatch = messages.every((msg, i) => msg === received[i]);
                if (allMatch) {
                    done(true, `All ${messages.length} messages echoed correctly`);
                } else {
                    done(false, `Mismatch: sent ${JSON.stringify(messages)}, got ${JSON.stringify(received)}`);
                }
            }
        });

        messages.forEach((msg, i) => {
            setTimeout(() => ws.send(msg), i * 100);
        });
    });
    await new Promise(resolve => setTimeout(resolve, 100)); // Small delay between tests

    // Test 4: Binary data
    // await runTest('Test 4: Binary data echo', (ws, done) => {
    //     const buffer = Buffer.from([0x01, 0x02, 0x03, 0x04, 0x05]);
    //     ws.on('message', (data) => {
    //         if (Buffer.isBuffer(data) && data.equals(buffer)) {
    //             done(true, `Binary data echoed correctly (${buffer.length} bytes)`);
    //         } else {
    //             done(false, `Binary data mismatch`);
    //         }
    //     });
    //     ws.send(buffer);
    // });
    // await new Promise(resolve => setTimeout(resolve, 100)); // Small delay between tests

    // Test 5: Large text message (10KB)
    await runTest('Test 5: Large text message (10KB)', (ws, done) => {
        const largeMessage = 'A'.repeat(10 * 1024);
        ws.on('message', (data) => {
            const received = data.toString();
            if (received === largeMessage) {
                done(true, `Large message (${largeMessage.length} bytes) echoed correctly`);
            } else {
                done(false, `Size mismatch: sent ${largeMessage.length}, got ${received.length}`);
            }
        });
        ws.send(largeMessage);
    });
    await new Promise(resolve => setTimeout(resolve, 100)); // Small delay between tests

    // Test 6: Unicode characters
    await runTest('Test 6: Unicode characters', (ws, done) => {
        const unicodeMessage = '你好世界 🌍 Привет мир';
        ws.on('message', (data) => {
            const received = data.toString();
            if (received === unicodeMessage) {
                done(true, 'Unicode message echoed correctly');
            } else {
                done(false, `Expected "${unicodeMessage}", got "${received}"`);
            }
        });
        ws.send(unicodeMessage);
    });
    await new Promise(resolve => setTimeout(resolve, 100)); // Small delay between tests

    // Test 7: Graceful close
    await runTest('Test 7: Graceful connection close', (ws, done) => {
        ws.on('close', (code, reason) => {
            if (code === 1000) {
                done(true, `Clean close with code ${code}`);
            } else {
                done(false, `Unexpected close code: ${code}`);
            }
        });
        ws.close(1000, 'Normal closure');
    });

    // Print summary
    console.log('=================================');
    console.log('Test Summary:');
    console.log(`  Total: ${testsPassed + testsFailed}`);
    console.log(`  Passed: ${testsPassed}`);
    console.log(`  Failed: ${testsFailed}`);

    process.exit(testsFailed > 0 ? 1 : 0);
}

console.log(`Connecting to ${WS_URL}\n`);
runAllTests().catch(err => {
    console.error('Test suite error:', err);
    process.exit(1);
});
