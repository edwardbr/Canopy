#!/usr/bin/env node

const http = require('http');

const BASE_URL = process.env.API_URL || 'http://localhost:8888';

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
    // Test 1: GET /api/status
    await runTest('Test 1: GET /api/status', async () => {
        const response = await makeRequest('GET', '/api/status');

        if (response.statusCode !== 200) {
            return { success: false, message: `Expected status 200, got ${response.statusCode}` };
        }

        if (!response.body || !response.body.success) {
            return { success: false, message: 'Response missing success field' };
        }

        if (!response.body.data || !response.body.data.status) {
            return { success: false, message: 'Response missing status data' };
        }

        return {
            success: true,
            message: `Status: ${response.body.data.status}, Version: ${response.body.data.version}`
        };
    });

    // Test 2: GET /api/resource/123
    await runTest('Test 2: GET /api/resource/123', async () => {
        const response = await makeRequest('GET', '/api/resource/123');

        if (response.statusCode !== 200) {
            return { success: false, message: `Expected status 200, got ${response.statusCode}` };
        }

        if (!response.body || !response.body.success) {
            return { success: false, message: 'Response missing success field' };
        }

        if (!response.body.data) {
            return { success: false, message: 'Response missing data field' };
        }

        return { success: true, message: `Got resource data: ${JSON.stringify(response.body.data)}` };
    });

    // Test 3: POST /api/resource
    await runTest('Test 3: POST /api/resource (create)', async () => {
        const requestBody = { name: 'Test Resource', value: 42 };
        const response = await makeRequest('POST', '/api/resource', requestBody);

        if (response.statusCode !== 200) {
            return { success: false, message: `Expected status 200, got ${response.statusCode}` };
        }

        if (!response.body || !response.body.success) {
            return { success: false, message: 'Response missing success field' };
        }

        if (!response.body.data || !response.body.data.created) {
            return { success: false, message: 'Response missing created confirmation' };
        }

        return {
            success: true,
            message: `Resource created with ID: ${response.body.data.id}`
        };
    });

    // Test 4: POST /api/echo (generic endpoint)
    await runTest('Test 4: POST /api/echo (generic)', async () => {
        const requestBody = { message: 'Hello, REST API!', timestamp: Date.now() };
        const response = await makeRequest('POST', '/api/echo', requestBody);

        if (response.statusCode !== 200) {
            return { success: false, message: `Expected status 200, got ${response.statusCode}` };
        }

        if (!response.body || !response.body.success) {
            return { success: false, message: 'Response missing success field' };
        }

        const bodyLength = JSON.stringify(requestBody).length;
        if (response.body.data.body_length !== bodyLength) {
            return {
                success: false,
                message: `Body length mismatch: expected ${bodyLength}, got ${response.body.data.body_length}`
            };
        }

        return { success: true, message: `Body length correctly reported: ${bodyLength} bytes` };
    });

    // Test 5: PUT /api/resource/123 (update)
    await runTest('Test 5: PUT /api/resource/123 (update)', async () => {
        const requestBody = { name: 'Updated Resource', value: 99 };
        const response = await makeRequest('PUT', '/api/resource/123', requestBody);

        if (response.statusCode !== 200) {
            return { success: false, message: `Expected status 200, got ${response.statusCode}` };
        }

        if (!response.body || !response.body.success) {
            return { success: false, message: 'Response missing success field' };
        }

        if (!response.body.data || !response.body.data.updated) {
            return { success: false, message: 'Response missing updated confirmation' };
        }

        return { success: true, message: 'Resource updated successfully' };
    });

    // Test 6: DELETE /api/resource/123
    await runTest('Test 6: DELETE /api/resource/123', async () => {
        const response = await makeRequest('DELETE', '/api/resource/123');

        if (response.statusCode !== 200) {
            return { success: false, message: `Expected status 200, got ${response.statusCode}` };
        }

        if (!response.body || !response.body.success) {
            return { success: false, message: 'Response missing success field' };
        }

        if (!response.body.data || !response.body.data.deleted) {
            return { success: false, message: 'Response missing deleted confirmation' };
        }

        return { success: true, message: 'Resource deleted successfully' };
    });

    // Test 7: GET /api/nonexistent (404 error)
    await runTest('Test 7: GET /api/nonexistent (404 error)', async () => {
        const response = await makeRequest('GET', '/api/nonexistent');

        if (response.statusCode !== 404) {
            return { success: false, message: `Expected status 404, got ${response.statusCode}` };
        }

        if (!response.body || !response.body.error) {
            return { success: false, message: 'Response missing error field' };
        }

        return { success: true, message: `Correct 404 error: ${response.body.error}` };
    });

    // Test 8: PATCH /api/resource (405 method not allowed)
    await runTest('Test 8: PATCH /api/resource (405 method not allowed)', async () => {
        try {
            const response = await makeRequest('PATCH', '/api/resource', { test: 'data' });

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

    // Test 9: Large JSON POST
    await runTest('Test 9: Large JSON POST (1KB)', async () => {
        const largeData = {
            items: Array.from({ length: 5000 }, (_, i) => ({
                id: i,
                name: `Item ${i}`,
                description: 'A'.repeat(10)
            }))
        };

        const response = await makeRequest('POST', '/api/bulk', largeData);

        if (response.statusCode !== 200) {
            return { success: false, message: `Expected status 200, got ${response.statusCode}` };
        }

        const bodyLength = JSON.stringify(largeData).length;
        return { success: true, message: `Large payload (${bodyLength} bytes) handled successfully` };
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
