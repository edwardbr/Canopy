// WebSocket client logic with Echo and Calculator mode support using generated proxy/stub
let transport = null;
let calc = null;       // i_calculator_proxy (created after connect)
let eventStub = null;  // i_context_event_stub (receives streaming events)

let sentCount = 0;
let receivedCount = 0;
let connectTime = null;
let uptimeInterval = null;
let currentMode = 'echo'; // 'echo' or 'calculator'
let currentAssistantMessage = null;

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
    if (transport && transport.isConnected()) {
        addMessage('system', 'Already connected');
        return;
    }

    updateStatus('Connecting...', 'connecting');
    addMessage('system', 'Connecting to WebSocket server...');

    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${protocol}//${window.location.host}`;
    const transportProto = $protobuf_websocket.protobuf.websocket_protocol_v1;
    const appProto = $protobuf_websocket.protobuf.websocket_demo_v1;

    // Create stub for server-initiated streaming events (i_context_event.piece)
    eventStub = new WebsocketDemo.i_context_event_stub({
        piece: function(text) {
            if (currentAssistantMessage) {
                currentAssistantMessage.textContent += text;
                chatHistory.scrollTop = chatHistory.scrollHeight;
                receivedCount++;
                updateStats();
            }
        }
    });

    transport = new CanopyWebsocketTransport({
        url: wsUrl,
        proto: transportProto,
        appProto: appProto,
        inboundInterfaceId: WebsocketDemo.interfaceIds.i_context_event,
        outboundInterfaceId: WebsocketDemo.interfaceIds.i_calculator,
        onOpen: function(t) {
            calc = new WebsocketDemo.i_calculator_proxy(t, appProto);
            updateStatus('Connected', 'connected');
            setUIConnected(true);
            connectTime = Date.now();
            uptimeInterval = setInterval(updateUptime, 1000);
            addMessage('system', '✓ Connected and ready');
        },
        onClose: function(code, reason) {
            calc = null;
            updateStatus('Disconnected', 'disconnected');
            addMessage('system', `Connection closed (code: ${code}, reason: ${reason || 'none'})`);
            setUIConnected(false);
            connectTime = null;
            if (uptimeInterval) {
                clearInterval(uptimeInterval);
                uptimeInterval = null;
            }
            updateUptime();
        },
        onError: function(err) {
            addMessage('error', 'WebSocket error occurred');
            console.error('WebSocket error:', err);
        }
    });

    transport.registerStub(eventStub);
    transport.connect().catch(function(err) {
        addMessage('error', `Connection failed: ${err.message}`);
        updateStatus('Disconnected', 'disconnected');
    });
}

function disconnect() {
    if (transport) {
        addMessage('system', 'Closing connection...');
        transport.disconnect();
    }
}

function sendMessage() {
    addMessage('error', 'Echo mode requires a raw WebSocket (not available with RPC transport)');
}

async function calculate() {
    const first = parseFloat(firstValueInput.value);
    const second = parseFloat(secondValueInput.value);
    const methodId = parseInt(operationSelect.value);

    if (isNaN(first) || isNaN(second)) {
        addMessage('error', 'Please enter valid numbers');
        return;
    }

    if (!calc) {
        addMessage('error', 'Not connected');
        return;
    }

    const opNames = { 1: 'add', 2: 'subtract', 3: 'multiply', 4: 'divide' };
    addMessage('sent', `→ Calculator: ${first} ${opNames[methodId]} ${second}`);
    resultDisplay.textContent = 'Calculating...';
    sentCount++;
    updateStats();

    try {
        let r;
        switch (methodId) {
            case 1: r = await calc.add(first, second);      break;
            case 2: r = await calc.subtract(first, second); break;
            case 3: r = await calc.multiply(first, second); break;
            case 4: r = await calc.divide(first, second);   break;
        }
        if (r.result !== 0) {
            resultDisplay.textContent = `Error: ${r.result}`;
            addMessage('received', `← Calculator error: ${r.result}`);
        } else {
            resultDisplay.textContent = `Result: ${r.response}`;
            addMessage('received', `← Calculator result: ${r.response}`);
        }
        receivedCount++;
        updateStats();
    } catch (err) {
        addMessage('error', `Calculator RPC failed: ${err.message}`);
        resultDisplay.textContent = `Error: ${err.message}`;
        console.error('Calculator error:', err);
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

async function sendChatMessage() {
    const prompt = chatInput.value.trim();

    if (!prompt) {
        return;
    }

    if (!calc) {
        addMessage('error', 'Not connected');
        return;
    }

    addChatMessage('user', prompt);
    addMessage('sent', `→ Chat: ${prompt.substring(0, 50)}${prompt.length > 50 ? '...' : ''}`);

    try {
        // Create a streaming assistant message that will be filled by piece events
        currentAssistantMessage = addChatMessage('assistant', '', true);
        chatInput.value = '';
        sentCount++;
        updateStats();

        const r = await calc.add_prompt(prompt);
        if (r.result !== 0) {
            addMessage('error', `Chat error: ${r.result}`);
            if (currentAssistantMessage) {
                currentAssistantMessage.textContent = `Error: ${r.result}`;
                currentAssistantMessage.classList.remove('streaming');
                currentAssistantMessage = null;
            }
        }
    } catch (err) {
        addMessage('error', `Chat RPC failed: ${err.message}`);
        if (currentAssistantMessage) {
            currentAssistantMessage.textContent = `Error: ${err.message}`;
            currentAssistantMessage.classList.remove('streaming');
            currentAssistantMessage = null;
        }
        console.error('Chat error:', err);
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
addMessage('system', 'Using generated Canopy proxy/stub from websocket_demo.js');
