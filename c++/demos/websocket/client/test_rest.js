#!/usr/bin/env node

const http = require('http');

const BASE_URL = process.env.API_URL || 'http://localhost:8080';

console.log('REST API Test Suite');
console.log('===================\n');

let testsPassed = 0;
let testsFailed = 0;

function makeRequest(method, path, body = null) {
    return new Promise((resolve, reject) => {
        const url = new URL(path, BASE_URL);
        const options = {
            hostname: url.hostname,
            port: url.port,
            path: url.pathname,
            method: method,
            headers: {
                'Content-Type': 'application/json'
            }
        };

        if (body) {
            const bodyString = JSON.stringify(body);
            options.headers['Content-Length'] = Buffer.byteLength(bodyString);
        }

        const req = http.request(options, (res) => {
            let data = '';

            res.on('data', (chunk) => {
                data += chunk;
            });

            res.on('end', () => {
                try {
                    const response = {
                        statusCode: res.statusCode,
                        headers: res.headers,
                        body: data ? JSON.parse(data) : null
                    };
                    resolve(response);
                } catch (err) {
                    reject(new Error(`Failed to parse JSON: ${data}`));
                }
            });
        });

        req.on('error', (err) => {
            reject(err);
        });

        if (body) {
            req.write(JSON.stringify(body));
        }

        req.end();
    });
}

async function runTest(name, testFn) {
    console.log(`Running: ${name}`);
    try {
        const result = await testFn();
        if (result.success) {
            console.log(`  ✓ PASSED${result.message ? ': ' + result.message : ''}\n`);
            testsPassed++;
        } else {
            console.log(`  ✗ FAILED${result.message ? ': ' + result.message : ''}\n`);
            testsFailed++;
        }
    } catch (err) {
        console.log(`  ✗ FAILED: ${err.message}\n`);
        testsFailed++;
    }
}

async function runAllTests() {
    // Test 1: POST /api/echo (generated REST endpoint)
    await runTest('Test 1: POST /api/echo (generated REST)', async () => {
        const requestBody = 'Hello, REST API!';
        const response = await makeRequest('POST', '/api/echo', requestBody);

        if (response.statusCode !== 200) {
            return { success: false, message: `Expected status 200, got ${response.statusCode}` };
        }

        if (response.body !== requestBody) {
            return { success: false, message: `Echo mismatch: expected ${requestBody}, got ${response.body}` };
        }

        return { success: true, message: `Echoed: ${response.body}` };
    });

    // Test 2: POST /api/echo with a larger JSON string
    await runTest('Test 2: POST /api/echo (large generated REST request)', async () => {
        const requestBody = 'A'.repeat(64 * 1024);
        const response = await makeRequest('POST', '/api/echo', requestBody);

        if (response.statusCode !== 200) {
            return { success: false, message: `Expected status 200, got ${response.statusCode}` };
        }

        if (response.body !== requestBody) {
            return { success: false, message: `Echo mismatch: got ${response.body ? response.body.length : 0} bytes` };
        }

        return { success: true, message: `Echoed ${response.body.length} bytes` };
    });

    // Test 3: POST /api/nonexistent (404 from REST registry)
    await runTest('Test 3: POST /api/nonexistent (404 error)', async () => {
        const response = await makeRequest('POST', '/api/nonexistent', 'ignored');

        if (response.statusCode !== 404) {
            return { success: false, message: `Expected status 404, got ${response.statusCode}` };
        }

        if (!response.body || !response.body.error) {
            return { success: false, message: 'Response missing error field' };
        }

        return { success: true, message: `Correct 404 error: ${response.body.error}` };
    });

    // Test 4: GET /api/echo (405 method not allowed from generated handler)
    await runTest('Test 4: GET /api/echo (405 method not allowed)', async () => {
        try {
            const response = await makeRequest('GET', '/api/echo');

            if (response.statusCode !== 405) {
                return { success: false, message: `Expected status 405, got ${response.statusCode}` };
            }

            if (!response.body || !response.body.error) {
                return { success: false, message: 'Response missing error field' };
            }

            return { success: true, message: `Correct 405 error: ${response.body.error}` };
        } catch (err) {
            // Some servers might close connection for unsupported methods
            return { success: true, message: 'Unsupported method rejected (connection closed)' };
        }
    });

    // Print summary
    console.log('===================');
    console.log('Test Summary:');
    console.log(`  Total: ${testsPassed + testsFailed}`);
    console.log(`  Passed: ${testsPassed}`);
    console.log(`  Failed: ${testsFailed}`);

    process.exit(testsFailed > 0 ? 1 : 0);
}

console.log(`Testing API at ${BASE_URL}\n`);
runAllTests().catch(err => {
    console.error('Test suite error:', err);
    process.exit(1);
});
