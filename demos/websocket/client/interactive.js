#!/usr/bin/env node

const WebSocket = require('ws');
const readline = require('readline');

const WS_URL = process.env.WS_URL || 'ws://localhost:8888';

console.log(`Connecting to ${WS_URL}...`);

const ws = new WebSocket(WS_URL);
let connected = false;

const rl = readline.createInterface({
    input: process.stdin,
    output: process.stdout,
    prompt: 'ws> '
});

ws.on('open', function open() {
    console.log('✓ Connected to WebSocket server');
    console.log('Type messages to send (Ctrl+C or "quit" to exit)\n');
    connected = true;
    rl.prompt();
});

ws.on('message', function message(data) {
    // Clear the current line and move cursor up
    readline.clearLine(process.stdout, 0);
    readline.cursorTo(process.stdout, 0);

    console.log(`← Received: "${data.toString()}"`);
    rl.prompt();
});

ws.on('close', function close(code, reason) {
    console.log(`\n✓ Connection closed (code: ${code}, reason: ${reason || 'none'})`);
    rl.close();
    process.exit(0);
});

ws.on('error', function error(err) {
    console.error('✗ WebSocket error:', err.message);
    rl.close();
    process.exit(1);
});

rl.on('line', (line) => {
    const message = line.trim();

    if (message === 'quit' || message === 'exit') {
        console.log('Closing connection...');
        ws.close(1000, 'Normal closure');
        return;
    }

    if (message === '') {
        rl.prompt();
        return;
    }

    if (!connected) {
        console.log('Not connected yet...');
        rl.prompt();
        return;
    }

    console.log(`→ Sending: "${message}"`);
    ws.send(message);
    rl.prompt();
});

rl.on('close', () => {
    console.log('\nGoodbye!');
    if (ws.readyState === WebSocket.OPEN) {
        ws.close(1000, 'Client exiting');
    }
    process.exit(0);
});

// Handle Ctrl+C gracefully
process.on('SIGINT', () => {
    console.log('\n\nInterrupted, closing connection...');
    ws.close(1000, 'Client interrupted');
    rl.close();
    setTimeout(() => process.exit(0), 500);
});
