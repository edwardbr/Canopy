// WebSocket client logic with Echo and Calculator mode support using generated protobuf
let ws = null;
let sentCount = 0;
let receivedCount = 0;
let connectTime = null;
let uptimeInterval = null;
let currentMode = 'echo'; // 'echo' or 'calculator'
let messageCounter = 0;
let pendingRequests = new Map(); // messageId -> {methodId}

// Protobuf messages - loaded from generated websocket_proto.js via module shim
const WebsocketProto = $protobuf_websocket.protobuf.websocket_demo_v1;
const RpcProto = $protobuf_websocket.protobuf.rpc;
// SecretLlamaProto is loaded dynamically in index.html

// DOM elements
const statusEl = document.getElementById('status');
const messageInput = document.getElementById('messageInput');
const sendBtn = document.getElementById('sendBtn');
const connectBtn = document.getElementById('connectBtn');
const disconnectBtn = document.getElementById('disconnectBtn');
const clearBtn = document.getElementById('clearBtn');
const messagesEl = document.getElementById('messages');
const sentCountEl = document.getElementById('sentCount');
const receivedCountEl = document.getElementById('receivedCount');
const uptimeEl = document.getElementById('uptime');

// Mode switching elements
const echoModeRadio = document.getElementById('echoMode');
const calculatorModeRadio = document.getElementById('calculatorMode');
const chatModeRadio = document.getElementById('chatMode');
const echoPanel = document.getElementById('echoPanel');
const calculatorPanel = document.getElementById('calculatorPanel');
const chatPanel = document.getElementById('chatPanel');

// Calculator elements
const firstValueInput = document.getElementById('firstValue');
const secondValueInput = document.getElementById('secondValue');
const operationSelect = document.getElementById('operation');
const calculateBtn = document.getElementById('calculateBtn');
const resultDisplay = document.getElementById('resultDisplay');

// Chat elements
const chatHistory = document.getElementById('chatHistory');
const chatInput = document.getElementById('chatInput');
const sendChatBtn = document.getElementById('sendChatBtn');

// Chat state
let currentAssistantMessage = null;

// Helper functions
function formatTime() {
    const now = new Date();
    return now.toLocaleTimeString('en-US', { hour12: false });
}

function addMessage(type, text) {
    const messageDiv = document.createElement('div');
    messageDiv.className = `message ${type}`;
    messageDiv.innerHTML = `<span class="timestamp">[${formatTime()}]</span>${text}`;
    messagesEl.appendChild(messageDiv);
    messagesEl.scrollTop = messagesEl.scrollHeight;
}

function updateStatus(status, className) {
    statusEl.textContent = status;
    statusEl.className = `status ${className}`;
}

function updateStats() {
    sentCountEl.textContent = sentCount;
    receivedCountEl.textContent = receivedCount;
}

function updateUptime() {
    if (connectTime) {
        const elapsed = Math.floor((Date.now() - connectTime) / 1000);
        const hours = Math.floor(elapsed / 3600);
        const minutes = Math.floor((elapsed % 3600) / 60);
        const seconds = elapsed % 60;

        if (hours > 0) {
            uptimeEl.textContent = `${hours}h ${minutes}m ${seconds}s`;
        } else if (minutes > 0) {
            uptimeEl.textContent = `${minutes}m ${seconds}s`;
        } else {
            uptimeEl.textContent = `${seconds}s`;
        }
    } else {
        uptimeEl.textContent = '0s';
    }
}

function setUIConnected(connected) {
    messageInput.disabled = !connected;
    sendBtn.disabled = !connected;
    connectBtn.disabled = connected;
    disconnectBtn.disabled = !connected;

    // Calculator controls
    firstValueInput.disabled = !connected;
    secondValueInput.disabled = !connected;
    operationSelect.disabled = !connected;
    calculateBtn.disabled = !connected;

    // Chat controls
    chatInput.disabled = !connected;
    sendChatBtn.disabled = !connected;
}

// Mode switching
function switchMode(mode) {
    currentMode = mode;
    if (mode === 'echo') {
        echoPanel.classList.remove('hidden');
        calculatorPanel.classList.add('hidden');
        chatPanel.classList.add('hidden');
        addMessage('system', 'Switched to Echo mode');
    } else if (mode === 'calculator') {
        echoPanel.classList.add('hidden');
        calculatorPanel.classList.remove('hidden');
        chatPanel.classList.add('hidden');
        addMessage('system', 'Switched to Calculator mode');
    } else if (mode === 'chat') {
        echoPanel.classList.add('hidden');
        calculatorPanel.classList.add('hidden');
        chatPanel.classList.remove('hidden');
        addMessage('system', 'Switched to Chat mode');
    }
}

echoModeRadio.addEventListener('change', () => switchMode('echo'));
calculatorModeRadio.addEventListener('change', () => switchMode('calculator'));
chatModeRadio.addEventListener('change', () => switchMode('chat'));

// WebSocket functions
function connect() {
    if (ws && ws.readyState === WebSocket.OPEN) {
        addMessage('system', 'Already connected');
        return;
    }

    updateStatus('Connecting...', 'connecting');
    addMessage('system', 'Connecting to WebSocket server...');

    // Use current host and port for WebSocket URL
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${protocol}//${window.location.host}`;

    ws = new WebSocket(wsUrl);
    ws.binaryType = 'arraybuffer'; // Important for binary data

    ws.onopen = function () {
        updateStatus('Connected', 'connected');
        addMessage('system', '✓ Connected to WebSocket server');
        setUIConnected(true);
        connectTime = Date.now();
        uptimeInterval = setInterval(updateUptime, 1000);

        // Send a welcome message in echo mode
        if (currentMode === 'echo') {
            const welcomeMsg = 'Hello from browser client!';
            ws.send(welcomeMsg);
            addMessage('sent', `→ ${welcomeMsg}`);
            sentCount++;
            updateStats();
        }
    };

    ws.onmessage = function (event) {
        if (typeof event.data === 'string') {
            // Text message (echo mode)
            addMessage('received', `← ${event.data}`);
            receivedCount++;
            updateStats();
        } else {
            // Binary message (calculator/chat mode response or event)
            try {
                // Decode the envelope
                const envelopeBytes = new Uint8Array(event.data);
                const envelope = WebsocketProto.envelope.decode(envelopeBytes);

                // Check if this is an event (no matching request) or a response
                const messageId = envelope.messageId.toNumber();
                const requestInfo = pendingRequests.get(messageId);

                // Try to determine if this is an event by checking the message type
                // Events typically have different message type fingerprints
                const messageType = envelope.messageType.toString();

                // If no matching request, this might be an event
                if (!requestInfo) {
                    // Try to decode as an event from i_context_event
                    try {
                        // First decode as websocket::request (events come through the same envelope)
                        const eventRequest = WebsocketProto.request.decode(envelope.data);

                        console.log('[' + new Date().toLocaleTimeString() + '] Event received:', {
                            messageId: messageId,
                            interfaceId: eventRequest.interfaceId.toString(),
                            methodId: eventRequest.methodId,
                            dataLength: eventRequest.data.length
                        });

                        // Decode the i_context_event_pieceRequest from websocket_demo namespace
                        const pieceEvent = WebsocketProto.i_context_event_pieceRequest.decode(eventRequest.data);

                        // Update the streaming assistant message with the piece
                        if (currentAssistantMessage && pieceEvent.piece) {
                            currentAssistantMessage.textContent += pieceEvent.piece;
                            chatHistory.scrollTop = chatHistory.scrollHeight;
                            receivedCount++;
                            updateStats();
                        } else {
                            console.log('[' + new Date().toLocaleTimeString() + '] Event piece received but no current message:', pieceEvent.piece);
                            addMessage('system', `Event piece: ${pieceEvent.piece}`);
                            receivedCount++;
                            updateStats();
                        }

                        return;
                    } catch (eventErr) {
                        addMessage('error', `Failed to decode event: ${eventErr.message}`);
                        console.error('Event decode error:', eventErr);
                        console.error('Envelope data:', envelope);
                        return;
                    }
                }

                pendingRequests.delete(messageId);

                // Decode the response from the envelope data
                const response = WebsocketProto.response.decode(envelope.data);

                // Handle based on request type
                if (requestInfo.type === 'chat') {
                    console.log('[' + new Date().toLocaleTimeString() + '] Chat response received:', {
                        messageId: messageId,
                        error: response.error,
                        dataLength: response.data ? response.data.length : 0
                    });

                    // Chat response handling - check RPC error first
                    if (response.error == 0) {
                        // Decode i_calculator_add_promptResponse from websocket_demo
                        if (response.data && response.data.length > 0) {
                            const chatResponse = WebsocketProto.i_calculator_add_promptResponse.decode(response.data);
                            if (chatResponse.result === 0) {
                                console.log('[' + new Date().toLocaleTimeString() + '] Chat prompt accepted, awaiting streaming events...');
                                // The actual content comes through events (i_context_event.piece)
                            } else {
                                addMessage('error', `Chat error: ${chatResponse.result}`);
                                if (currentAssistantMessage) {
                                    currentAssistantMessage.textContent = `Error: ${chatResponse.result}`;
                                    currentAssistantMessage.classList.remove('streaming');
                                    currentAssistantMessage = null;
                                }
                            }
                        } else {
                            // Empty response data is OK for chat - content comes via events
                            console.log('[' + new Date().toLocaleTimeString() + '] Chat prompt accepted (empty response), awaiting streaming events...');
                        }
                    } else {
                        addMessage('error', `Chat RPC error: ${response.error}`);
                        if (currentAssistantMessage) {
                            currentAssistantMessage.textContent = `Error: ${response.error}`;
                            currentAssistantMessage.classList.remove('streaming');
                            currentAssistantMessage = null;
                        }
                    }
                    receivedCount++;
                    updateStats();
                } else {
                    // Calculator response handling
                    if (response.error == 0 && response.data && response.data.length > 0) {
                        // Decode the response based on the method that was called
                        // For calculator methods, all return a double result in field 3

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

                        if (resultValue === null) {
                            addMessage('error', 'Could not extract result from response');
                        } else if (resultValue.result !== 0) {
                            resultDisplay.textContent = `Result: ${resultValue.result}`;
                            addMessage('received', `← Calculator error: ${resultValue.result}`);
                            receivedCount++;
                            updateStats();
                        } else {
                            resultDisplay.textContent = `Result: ${resultValue.response}`;
                            addMessage('received', `← Calculator result: ${resultValue.response}`);
                            receivedCount++;
                            updateStats();
                        }
                    } else {
                        addMessage('error', `Calculator error: ${response.error}`);
                        resultDisplay.textContent = `Error: ${response.error}`;
                    }
                }
            } catch (err) {
                addMessage('error', `Failed to decode response: ${err.message}`);
                console.error('Decode error:', err);
            }
        }
    };

    ws.onclose = function (event) {
        updateStatus('Disconnected', 'disconnected');
        addMessage('system', `Connection closed (code: ${event.code}, reason: ${event.reason || 'none'})`);
        setUIConnected(false);
        connectTime = null;
        if (uptimeInterval) {
            clearInterval(uptimeInterval);
            uptimeInterval = null;
        }
        updateUptime();
    };

    ws.onerror = function (error) {
        addMessage('error', `WebSocket error occurred`);
        console.error('WebSocket error:', error);
    };
}

function disconnect() {
    if (ws && ws.readyState === WebSocket.OPEN) {
        addMessage('system', 'Closing connection...');
        ws.close(1000, 'User requested disconnect');
    }
}

function sendMessage() {
    const message = messageInput.value.trim();

    if (!message) {
        return;
    }

    if (!ws || ws.readyState !== WebSocket.OPEN) {
        addMessage('error', 'Not connected to server');
        return;
    }

    ws.send(message);
    addMessage('sent', `→ ${message}`);
    sentCount++;
    updateStats();
    messageInput.value = '';
}

function calculate() {
    const first = parseFloat(firstValueInput.value);
    const second = parseFloat(secondValueInput.value);
    const methodId = parseInt(operationSelect.value);

    if (isNaN(first) || isNaN(second)) {
        addMessage('error', 'Please enter valid numbers');
        return;
    }

    if (!ws || ws.readyState !== WebSocket.OPEN) {
        addMessage('error', 'Not connected to server');
        return;
    }

    const opNames = { 1: 'add', 2: 'subtract', 3: 'multiply', 4: 'divide' };
    addMessage('sent', `→ Calculator: ${first} ${opNames[methodId]} ${second}`);

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
            callerZoneId: 2,
            destinationZoneId: 1,
            objectId: 0,
            interfaceId: Long.fromString("2180915978302953945", true), //i_calculator
            methodId: methodId,
            data: requestBytes,
            backChannel: []
        });

        // Encode the websocket::request
        const wsRequestBytes = WebsocketProto.request.encode(wsRequest).finish();

        // Create the envelope
        // Use the fingerprint ID for websocket::request as a Long (uint64)
        // JavaScript numbers lose precision above 2^53-1, so use protobuf.Long
        const REQUEST_MESSAGE_TYPE = Long.fromString("12812964479505592837", true);
        const envelope = WebsocketProto.envelope.create({
            messageId: Long.fromNumber(messageId, true),
            messageType: REQUEST_MESSAGE_TYPE,
            data: wsRequestBytes
        });

        // Store the request info for response matching
        pendingRequests.set(messageId, { methodId });

        // Encode and send the envelope
        const envelopeBytes = WebsocketProto.envelope.encode(envelope).finish();
        ws.send(envelopeBytes);

        sentCount++;
        updateStats();
        resultDisplay.textContent = 'Calculating...';
    } catch (err) {
        addMessage('error', `Failed to encode request: ${err.message}`);
        console.error('Encode error:', err);
    }
}

function addChatMessage(role, content, streaming = false) {
    const messageDiv = document.createElement('div');
    messageDiv.className = `chat-message ${role}`;
    if (streaming) {
        messageDiv.classList.add('streaming');
    }
    messageDiv.textContent = content;
    chatHistory.appendChild(messageDiv);
    chatHistory.scrollTop = chatHistory.scrollHeight;
    return messageDiv;
}

function sendChatMessage() {
    const prompt = chatInput.value.trim();

    if (!prompt) {
        return;
    }

    if (!ws || ws.readyState !== WebSocket.OPEN) {
        addMessage('error', 'Not connected to server');
        return;
    }

    if (!SecretLlamaProto) {
        addMessage('error', 'Secret Llama protobuf not loaded yet, please wait...');
        return;
    }

    // Add user message to chat
    addChatMessage('user', prompt);
    addMessage('sent', `→ Chat: ${prompt.substring(0, 50)}${prompt.length > 50 ? '...' : ''}`);

    try {
        // Increment message counter for unique ID
        messageCounter++;
        const messageId = messageCounter;

        // Create the i_context_add_promptRequest using protobuf
        const AddPromptRequest = SecretLlamaProto.lookupType("i_context_add_promptRequest");
        const requestMessage = AddPromptRequest.create({
            prompt: prompt
        });

        // Encode the request message
        const requestBytes = AddPromptRequest.encode(requestMessage).finish();

        // Create the websocket::request wrapper
        const wsRequest = WebsocketProto.request.create({
            encoding: RpcProto.encoding.encoding_protocol_buffers,
            tag: 0,
            callerZoneId: 2,
            destinationZoneId: 1,
            objectId: 0,
            interfaceId: Long.fromString("2180915978302953945", true), // i_context
            methodId: 5, // add_prompt
            data: requestBytes,
            backChannel: []
        });

        // Encode the websocket::request
        const wsRequestBytes = WebsocketProto.request.encode(wsRequest).finish();

        // Create the envelope
        const REQUEST_MESSAGE_TYPE = Long.fromString("12812964479505592837", true);
        const envelope = WebsocketProto.envelope.create({
            messageId: Long.fromNumber(messageId, true),
            messageType: REQUEST_MESSAGE_TYPE,
            data: wsRequestBytes
        });

        // Store the request info for response matching
        pendingRequests.set(messageId, { methodId: 1, type: 'chat' });

        console.log('[' + new Date().toLocaleTimeString() + '] Sending chat message:', {
            messageId: messageId,
            promptLength: prompt.length,
            interfaceId: wsRequest.interfaceId.toString(),
            methodId: wsRequest.methodId
        });

        // Encode and send the envelope
        const envelopeBytes = WebsocketProto.envelope.encode(envelope).finish();
        ws.send(envelopeBytes);

        // Create a new streaming assistant message
        currentAssistantMessage = addChatMessage('assistant', '', true);

        sentCount++;
        updateStats();
        chatInput.value = '';
    } catch (err) {
        addMessage('error', `Failed to send chat message: ${err.message}`);
        console.error('Chat send error:', err);
    }
}

function clearMessages() {
    messagesEl.innerHTML = '';
    sentCount = 0;
    receivedCount = 0;
    updateStats();
    addMessage('system', 'Messages cleared');
}

// Event listeners
connectBtn.addEventListener('click', connect);
disconnectBtn.addEventListener('click', disconnect);
sendBtn.addEventListener('click', sendMessage);
calculateBtn.addEventListener('click', calculate);
sendChatBtn.addEventListener('click', sendChatMessage);
clearBtn.addEventListener('click', clearMessages);

messageInput.addEventListener('keypress', function (e) {
    if (e.key === 'Enter') {
        sendMessage();
    }
});

chatInput.addEventListener('keypress', function (e) {
    if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault();
        sendChatMessage();
    }
});

// Initial message
addMessage('system', 'WebSocket client ready. Click "Connect" to start.');
addMessage('system', 'Mode: Echo - Switch to Calculator or Chat mode to use other features');
addMessage('system', 'Using generated protobuf definitions from websocket_proto.js');
