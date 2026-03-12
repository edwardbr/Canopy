// WebSocket demo browser client using generated transport helpers
let rpcClient = null;
let sentCount = 0;
let receivedCount = 0;
let connectTime = null;
let uptimeInterval = null;
let currentMode = 'echo';

const WebsocketProto = $protobuf_websocket.protobuf.websocket_demo_v1;
const RpcProto = $protobuf_websocket.protobuf.rpc;

let currentAssistantMessage = null;

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
const echoModeRadio = document.getElementById('echoMode');
const calculatorModeRadio = document.getElementById('calculatorMode');
const chatModeRadio = document.getElementById('chatMode');
const echoPanel = document.getElementById('echoPanel');
const calculatorPanel = document.getElementById('calculatorPanel');
const chatPanel = document.getElementById('chatPanel');
const firstValueInput = document.getElementById('firstValue');
const secondValueInput = document.getElementById('secondValue');
const operationSelect = document.getElementById('operation');
const calculateBtn = document.getElementById('calculateBtn');
const resultDisplay = document.getElementById('resultDisplay');
const chatHistory = document.getElementById('chatHistory');
const chatInput = document.getElementById('chatInput');
const sendChatBtn = document.getElementById('sendChatBtn');

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
    if (!connectTime) {
        uptimeEl.textContent = '0s';
        return;
    }

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
}

function setUIConnected(connected) {
    messageInput.disabled = !connected;
    sendBtn.disabled = !connected;
    connectBtn.disabled = connected;
    disconnectBtn.disabled = !connected;
    firstValueInput.disabled = !connected;
    secondValueInput.disabled = !connected;
    operationSelect.disabled = !connected;
    calculateBtn.disabled = !connected;
    chatInput.disabled = !connected;
    sendChatBtn.disabled = !connected;
}

function switchMode(mode) {
    currentMode = mode;
    echoPanel.classList.toggle('hidden', mode !== 'echo');
    calculatorPanel.classList.toggle('hidden', mode !== 'calculator');
    chatPanel.classList.toggle('hidden', mode !== 'chat');
    addMessage('system', `Switched to ${mode.charAt(0).toUpperCase()}${mode.slice(1)} mode`);
}

function ensureClientReady() {
    if (!rpcClient || !rpcClient.isReady()) {
        addMessage('error', 'Handshake not complete yet, please wait');
        return false;
    }
    return true;
}

function resetConnectionState() {
    connectTime = null;
    if (uptimeInterval) {
        clearInterval(uptimeInterval);
        uptimeInterval = null;
    }
    updateUptime();
    resultDisplay.textContent = 'Result will appear here';
}

function addChatMessage(role, content, streaming) {
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

function handleTransportEvent(eventInfo) {
    if (eventInfo.interfaceName !== 'i_context_event' || eventInfo.methodName !== 'piece') {
        addMessage('system', `Unhandled event: ${eventInfo.interfaceName}.${eventInfo.methodName}`);
        return;
    }

    const pieceEvent = eventInfo.decoded;
    if (currentAssistantMessage && pieceEvent && pieceEvent.piece) {
        currentAssistantMessage.textContent += pieceEvent.piece;
        chatHistory.scrollTop = chatHistory.scrollHeight;
    } else if (pieceEvent && pieceEvent.piece) {
        addMessage('system', `Event piece: ${pieceEvent.piece}`);
    }

    receivedCount++;
    updateStats();
}

function createRpcClient() {
    return CanopyWebsocketDemo.createClient({
        protoModule: $protobuf_websocket,
        onOpen: function() {
            updateStatus('Connecting...', 'connecting');
            addMessage('system', '✓ TCP/WS open — sending connect_request handshake...');
        },
        onHandshake: function(state) {
            updateStatus('Connected', 'connected');
            setUIConnected(true);
            connectTime = Date.now();
            uptimeInterval = setInterval(updateUptime, 1000);
            addMessage(
                'system',
                `✓ Handshake complete — client_zone=${state.clientZoneId}, server_zone=${state.serverZoneId}, server_object=${state.serverObjectId}`);
        },
        onTextMessage: function(text) {
            addMessage('received', `← ${text}`);
            receivedCount++;
            updateStats();
        },
        onEvent: handleTransportEvent,
        onClose: function(event) {
            updateStatus('Disconnected', 'disconnected');
            addMessage('system', `Connection closed (code: ${event.code}, reason: ${event.reason || 'none'})`);
            setUIConnected(false);
            resetConnectionState();
        },
        onError: function(error) {
            addMessage('error', 'WebSocket error occurred');
            console.error('WebSocket error:', error);
        }
    });
}

async function connect() {
    if (rpcClient && rpcClient.isOpen()) {
        addMessage('system', 'Already connected');
        return;
    }

    updateStatus('Connecting...', 'connecting');
    addMessage('system', 'Connecting to WebSocket server...');
    rpcClient = createRpcClient();

    try {
        await rpcClient.connect({ callbackObjectId: 1 });
    } catch (err) {
        updateStatus('Disconnected', 'disconnected');
        setUIConnected(false);
        addMessage('error', `Failed to connect: ${err.message}`);
    }
}

function disconnect() {
    if (rpcClient && rpcClient.isOpen()) {
        addMessage('system', 'Closing connection...');
        rpcClient.disconnect(1000, 'User requested disconnect');
    }
}

function sendMessage() {
    const message = messageInput.value.trim();
    if (!message) {
        return;
    }
    if (!rpcClient || !rpcClient.isOpen()) {
        addMessage('error', 'Not connected to server');
        return;
    }

    rpcClient.sendText(message);
    addMessage('sent', `→ ${message}`);
    sentCount++;
    updateStats();
    messageInput.value = '';
}

async function calculate() {
    const first = parseFloat(firstValueInput.value);
    const second = parseFloat(secondValueInput.value);
    const methodId = parseInt(operationSelect.value, 10);

    if (isNaN(first) || isNaN(second)) {
        addMessage('error', 'Please enter valid numbers');
        return;
    }
    if (!ensureClientReady()) {
        return;
    }

    const opNames = { 1: 'add', 2: 'subtract', 3: 'multiply', 4: 'divide' };
    const responseTypes = {
        1: WebsocketProto.i_calculator_addResponse,
        2: WebsocketProto.i_calculator_subtractResponse,
        3: WebsocketProto.i_calculator_multiplyResponse,
        4: WebsocketProto.i_calculator_divideResponse
    };
    const requestTypes = {
        1: WebsocketProto.i_calculator_addRequest,
        2: WebsocketProto.i_calculator_subtractRequest,
        3: WebsocketProto.i_calculator_multiplyRequest,
        4: WebsocketProto.i_calculator_divideRequest
    };

    addMessage('sent', `→ Calculator: ${first} ${opNames[methodId]} ${second}`);
    sentCount++;
    updateStats();
    resultDisplay.textContent = 'Calculating...';

    try {
        const rpcResult = await rpcClient.call({
            interfaceName: 'i_calculator',
            methodName: opNames[methodId],
            payloadType: requestTypes[methodId],
            responseType: responseTypes[methodId],
            payload: {
                firstVal: first,
                secondVal: second
            },
            encoding: RpcProto.encoding.encoding_protocol_buffers
        });

        receivedCount++;
        updateStats();

        if (rpcResult.response.error !== 0) {
            addMessage('error', `Calculator error: ${rpcResult.response.error}`);
            resultDisplay.textContent = `Error: ${rpcResult.response.error}`;
            return;
        }

        const decoded = rpcResult.decoded;
        if (!decoded) {
            addMessage('error', 'Could not decode calculator response');
            resultDisplay.textContent = 'Error decoding response';
            return;
        }

        if (decoded.result !== 0) {
            addMessage('received', `← Calculator error: ${decoded.result}`);
            resultDisplay.textContent = `Error: ${decoded.result}`;
            return;
        }

        addMessage('received', `← Calculator result: ${decoded.response}`);
        resultDisplay.textContent = `Result: ${decoded.response}`;
    } catch (err) {
        addMessage('error', `Failed to calculate: ${err.message}`);
        resultDisplay.textContent = `Error: ${err.message}`;
    }
}

async function sendChatMessage() {
    const prompt = chatInput.value.trim();
    if (!prompt) {
        return;
    }
    if (!ensureClientReady()) {
        return;
    }
    if (!SecretLlamaProto) {
        addMessage('error', 'Secret Llama protobuf not loaded yet, please wait...');
        return;
    }

    const addPromptRequest = SecretLlamaProto.lookupType('i_context_add_promptRequest');

    addChatMessage('user', prompt, false);
    currentAssistantMessage = addChatMessage('assistant', '', true);
    addMessage('sent', `→ Chat: ${prompt.substring(0, 50)}${prompt.length > 50 ? '...' : ''}`);
    sentCount++;
    updateStats();
    chatInput.value = '';

    try {
        const rpcResult = await rpcClient.call({
            interfaceName: 'i_calculator',
            methodName: 'add_prompt',
            payloadType: addPromptRequest,
            responseType: WebsocketProto.i_calculator_add_promptResponse,
            payload: { prompt: prompt },
            encoding: RpcProto.encoding.encoding_protocol_buffers
        });

        receivedCount++;
        updateStats();

        if (rpcResult.response.error !== 0) {
            addMessage('error', `Chat RPC error: ${rpcResult.response.error}`);
            currentAssistantMessage.textContent = `Error: ${rpcResult.response.error}`;
            currentAssistantMessage.classList.remove('streaming');
            currentAssistantMessage = null;
            return;
        }

        if (rpcResult.decoded && rpcResult.decoded.result !== 0) {
            addMessage('error', `Chat error: ${rpcResult.decoded.result}`);
            currentAssistantMessage.textContent = `Error: ${rpcResult.decoded.result}`;
            currentAssistantMessage.classList.remove('streaming');
            currentAssistantMessage = null;
            return;
        }
    } catch (err) {
        addMessage('error', `Failed to send chat message: ${err.message}`);
        if (currentAssistantMessage) {
            currentAssistantMessage.textContent = `Error: ${err.message}`;
            currentAssistantMessage.classList.remove('streaming');
            currentAssistantMessage = null;
        }
    }
}

function clearMessages() {
    messagesEl.innerHTML = '';
    sentCount = 0;
    receivedCount = 0;
    updateStats();
    addMessage('system', 'Messages cleared');
}

echoModeRadio.addEventListener('change', function() { switchMode('echo'); });
calculatorModeRadio.addEventListener('change', function() { switchMode('calculator'); });
chatModeRadio.addEventListener('change', function() { switchMode('chat'); });
connectBtn.addEventListener('click', connect);
disconnectBtn.addEventListener('click', disconnect);
sendBtn.addEventListener('click', sendMessage);
calculateBtn.addEventListener('click', calculate);
sendChatBtn.addEventListener('click', sendChatMessage);
clearBtn.addEventListener('click', clearMessages);

messageInput.addEventListener('keypress', function(e) {
    if (e.key === 'Enter') {
        sendMessage();
    }
});

chatInput.addEventListener('keypress', function(e) {
    if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault();
        sendChatMessage();
    }
});

setUIConnected(false);
addMessage('system', 'WebSocket client ready. Click "Connect" to start.');
addMessage('system', 'Mode: Echo - Switch to Calculator or Chat mode to use other features');
addMessage('system', 'Using generated websocket transport helpers from websocket_api.js');
