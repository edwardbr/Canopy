#!/usr/bin/env node

const { chromium } = require('playwright');

async function main() {
    const baseUrl = process.env.WS_DEMO_URL || 'https://localhost:8889/';
    const browser = await chromium.launch({ headless: true });
    const context = await browser.newContext({ ignoreHTTPSErrors: true });
    const page = await context.newPage();
    const consoleMessages = [];
    const pageErrors = [];

    page.on('console', (msg) => {
        const entry = `[${msg.type()}] ${msg.text()}`;
        consoleMessages.push(entry);
        console.log('browser-console', entry);
    });

    page.on('pageerror', (err) => {
        const entry = err && err.stack ? err.stack : String(err);
        pageErrors.push(entry);
        console.log('browser-pageerror', entry);
    });

    try {
        await page.goto(baseUrl, { waitUntil: 'networkidle', timeout: 30000 });

        await page.waitForSelector('#status');
        await page.waitForSelector('#connectBtn');

        const initialStatus = await page.locator('#status').textContent();
        console.log('initial-status', initialStatus.trim());

        await page.click('#connectBtn');

        try {
            await page.waitForFunction(() => {
                const status = document.querySelector('#status');
                const messages = document.querySelector('#messages');
                return status
                    && status.textContent.includes('Connected')
                    && messages
                    && messages.textContent.includes('Handshake complete');
            }, null, { timeout: 15000 });
        } catch (err) {
            const currentStatus = await page.locator('#status').textContent();
            const currentMessages = await page.locator('#messages').textContent();
            console.log('connect-timeout-status', currentStatus.trim());
            console.log('connect-timeout-messages', currentMessages.trim());
            if (consoleMessages.length) {
                console.log('connect-timeout-console', consoleMessages.join('\n'));
            }
            if (pageErrors.length) {
                console.log('connect-timeout-pageerrors', pageErrors.join('\n'));
            }
            throw err;
        }

        const connectedStatus = await page.locator('#status').textContent();
        const messagesText = await page.locator('#messages').textContent();
        console.log('connected-status', connectedStatus.trim());

        await page.fill('#firstValue', '2');
        await page.fill('#secondValue', '3');
        await page.selectOption('#operation', '1');
        await page.click('#calculateBtn');

        await page.waitForFunction(() => {
            const result = document.querySelector('#resultDisplay');
            return result && result.textContent.includes('Result: 5');
        }, null, { timeout: 15000 });

        const resultText = await page.locator('#resultDisplay').textContent();
        console.log('calculator-result', resultText.trim());
        console.log('messages-tail', messagesText.trim().slice(-300));
    } finally {
        await context.close();
        await browser.close();
    }
}

main().catch((err) => {
    console.error(err && err.stack ? err.stack : err);
    process.exit(1);
});
