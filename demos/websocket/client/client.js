#!/usr/bin/env node

const WebSocket = require('ws');

const WS_URL = process.env.WS_URL || 'ws://localhost:8888';

console.log(`Connecting to ${WS_URL}...`);

const ws = new WebSocket(WS_URL);

ws.on('open', function open() {
    console.log('✓ Connected to WebSocket server');

    // Send a test message
    const message = 'Hello from Node.js client!';
    console.log(`\nSending: "${message}"`);
    ws.send(message);

    // Send a few more messages with delays
    setTimeout(() => {
        const msg = 'Second message';
        console.log(`Sending: "${msg}"`);
        ws.send(msg);
    }, 1000);

    setTimeout(() => {
        const msg = 'Third message - closing after this';
        console.log(`Sending: "${msg}"`);
        ws.send(msg);

        // Close the connection after a short delay
        setTimeout(() => {
            console.log('\nClosing connection...');
            ws.close(1000, 'Normal closure');
        }, 500);
    }, 2000);
});

ws.on('message', function message(data) {
    console.log(`Received: "${data.toString()}"`);
});

ws.on('close', function close(code, reason) {
    console.log(`\n✓ Connection closed (code: ${code}, reason: ${reason || 'none'})`);
    process.exit(0);
});

ws.on('error', function error(err) {
    console.error('✗ WebSocket error:', err.message);
    process.exit(1);
});

// Handle Ctrl+C gracefully
process.on('SIGINT', () => {
    console.log('\n\nInterrupted, closing connection...');
    ws.close(1000, 'Client interrupted');
    setTimeout(() => process.exit(0), 500);
});
