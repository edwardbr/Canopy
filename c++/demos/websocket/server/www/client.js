// WebSocket client logic with Echo and Calculator mode support using generated proxy/stub
let transport = null;
let calc = null;       // i_calculator_proxy (created after connect)
let eventStub = null;  // i_context_event_stub: LLM tokens AND stylized video frames

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
const videoModeRadio = document.getElementById('videoMode');
const echoPanel = document.getElementById('echoPanel');
const calculatorPanel = document.getElementById('calculatorPanel');
const chatPanel = document.getElementById('chatPanel');
const videoPanel = document.getElementById('videoPanel');

// Video elements
const videoLocal = document.getElementById('videoLocal');
const videoRemote = document.getElementById('videoRemote');
const videoRemoteCtx = videoRemote.getContext('2d');
const videoStartBtn = document.getElementById('videoStartBtn');
const videoStopBtn = document.getElementById('videoStopBtn');
const videoStatsEl = document.getElementById('videoStats');

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
    const timestamp = document.createElement('span');
    timestamp.className = 'timestamp';
    timestamp.textContent = `[${formatTime()}]`;
    messageDiv.appendChild(timestamp);
    messageDiv.appendChild(document.createTextNode(text));
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

    // Video controls
    videoStartBtn.disabled = !connected || videoState.active;
    videoStopBtn.disabled = !connected || !videoState.active;
}

// Mode switching
function switchMode(mode) {
    currentMode = mode;
    echoPanel.classList.add('hidden');
    calculatorPanel.classList.add('hidden');
    chatPanel.classList.add('hidden');
    videoPanel.classList.add('hidden');
    if (mode === 'echo') {
        echoPanel.classList.remove('hidden');
        addMessage('system', 'Switched to Echo mode');
    } else if (mode === 'calculator') {
        calculatorPanel.classList.remove('hidden');
        addMessage('system', 'Switched to Calculator mode');
    } else if (mode === 'chat') {
        chatPanel.classList.remove('hidden');
        addMessage('system', 'Switched to Chat mode');
    } else if (mode === 'video') {
        videoPanel.classList.remove('hidden');
        addMessage('system', 'Switched to Video mode');
    }
}

echoModeRadio.addEventListener('change', () => switchMode('echo'));
calculatorModeRadio.addEventListener('change', () => switchMode('calculator'));
chatModeRadio.addEventListener('change', () => switchMode('chat'));
videoModeRadio.addEventListener('change', () => switchMode('video'));

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

    // One stub for everything server-initiated. piece() handles streamed LLM
    // tokens; push_frame() handles stylized video frames. Sharing one stub
    // because the websocket transport binds a single typed sink per
    // connection — see the comment on i_context_event in the IDL.
    eventStub = new WebsocketDemo.i_context_event_stub({
        piece: function(text) {
            if (currentAssistantMessage) {
                currentAssistantMessage.textContent += text;
                chatHistory.scrollTop = chatHistory.scrollHeight;
                receivedCount++;
                updateStats();
            }
        },
        push_frame: function(seq, pts_us, flags, payload) {
            handleRemoteFrame(seq, pts_us, flags, payload);
        }
    });

    transport = new CanopyWebsocketTransport({
        url: wsUrl,
        proto: transportProto,
        appProto: appProto,
        encoding: (window.CanopyWebsocketDemoConfig && window.CanopyWebsocketDemoConfig.encoding)
            || CanopyWebsocketTransport.ENCODING_NANOPB,
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
        },
        onText: function(text) {
            addMessage('received', `← Echo: ${text}`);
            receivedCount++;
            updateStats();
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
    const message = messageInput.value;

    if (!message) {
        return;
    }

    if (!transport || !transport.isConnected()) {
        addMessage('error', 'Not connected');
        return;
    }

    try {
        transport.sendText(message);
        addMessage('sent', `→ Echo: ${message}`);
        sentCount++;
        updateStats();
        messageInput.value = '';
    } catch (err) {
        addMessage('error', `Echo send failed: ${err.message}`);
        console.error('Echo error:', err);
    }
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

// ---------------------------------------------------------------------------
// Video pipeline (browser side)
// ---------------------------------------------------------------------------
//
// Capture: getUserMedia → MediaStreamTrackProcessor → VideoEncoder (VP8).
//   Each encoded chunk fires calc.push_video_frame() — a [post] fire-and-forget
//   RPC into the enclave, no awaiting.
//
// Display: incoming [post] push_frame events arrive in videoStub, get fed to
//   a VideoDecoder, and decoded VideoFrames are drawn to a 2D canvas.
//
// The echo demo just bounces frames straight back; the same browser-side code
// will work unchanged once the enclave starts genuinely transforming frames.
//
// Frame payload layout (raw VP8 bitstream chunks; codec chosen because every
// modern Chromium ships an in-process software VP8 codec — no platform deps):
//   flags bit 0 = keyframe (codec-required keyframe signalling for the decoder)
//   flags bit 1 = end-of-stream (sender stopped)
const VIDEO_FLAG_KEYFRAME = 1;
const VIDEO_FLAG_EOS = 2;
const VIDEO_CODEC = 'vp8';      // browser-native, no extra mux/demux
const VIDEO_WIDTH = 640;
const VIDEO_HEIGHT = 480;
const VIDEO_FRAMERATE = 15;
const VIDEO_BITRATE = 1_500_000;

const videoState = {
    active: false,
    mediaStream: null,
    reader: null,
    encoder: null,
    decoder: null,
    frameSeq: 0,
    sentFrames: 0,
    receivedFrames: 0,
    droppedFrames: 0,
};

function updateVideoStats() {
    videoStatsEl.textContent =
        `sent ${videoState.sentFrames} / received ${videoState.receivedFrames} / dropped ${videoState.droppedFrames}`;
}

async function startVideo() {
    if (videoState.active) return;
    if (!calc) {
        addMessage('error', 'Connect before starting video');
        return;
    }
    if (typeof VideoEncoder === 'undefined' || typeof VideoDecoder === 'undefined'
        || typeof MediaStreamTrackProcessor === 'undefined') {
        addMessage('error', 'This browser lacks WebCodecs / MediaStreamTrackProcessor');
        return;
    }

    try {
        // No explicit sink registration — the server's existing chat-handshake
        // path (websocket_handler.cpp's set_callback wiring) hands the demo
        // class an i_context_event sink that doubles as the video egress.
        videoState.mediaStream = await navigator.mediaDevices.getUserMedia({
            video: { width: VIDEO_WIDTH, height: VIDEO_HEIGHT, frameRate: VIDEO_FRAMERATE },
            audio: false,
        });
        videoLocal.srcObject = videoState.mediaStream;

        // Decoder for inbound stylized frames.
        videoState.decoder = new VideoDecoder({
            output: function(frame) {
                videoRemoteCtx.drawImage(frame, 0, 0, videoRemote.width, videoRemote.height);
                frame.close();
            },
            error: function(err) {
                console.error('VideoDecoder error:', err);
            },
        });
        videoState.decoder.configure({
            codec: VIDEO_CODEC,
            codedWidth: VIDEO_WIDTH,
            codedHeight: VIDEO_HEIGHT,
        });

        // Encoder for outbound capture.
        videoState.encoder = new VideoEncoder({
            output: function(chunk, _metadata) {
                if (!videoState.active || !calc || !transport || !transport.isConnected()) {
                    // Encoder flushes can arrive after stop/disconnect — drop silently.
                    videoState.droppedFrames++;
                    updateVideoStats();
                    return;
                }
                const buf = new Uint8Array(chunk.byteLength);
                chunk.copyTo(buf);
                let flags = 0;
                if (chunk.type === 'key') flags |= VIDEO_FLAG_KEYFRAME;
                try {
                    calc.push_video_frame(
                        Long_fromNumber_safe(videoState.frameSeq++),
                        Long_fromNumber_safe(chunk.timestamp),
                        flags,
                        buf);
                    videoState.sentFrames++;
                } catch (err) {
                    videoState.droppedFrames++;
                    console.warn('push_video_frame failed:', err);
                }
                updateVideoStats();
            },
            error: function(err) {
                console.error('VideoEncoder error:', err);
            },
        });
        videoState.encoder.configure({
            codec: VIDEO_CODEC,
            width: VIDEO_WIDTH,
            height: VIDEO_HEIGHT,
            framerate: VIDEO_FRAMERATE,
            bitrate: VIDEO_BITRATE,
            latencyMode: 'realtime',
        });

        const track = videoState.mediaStream.getVideoTracks()[0];
        const processor = new MediaStreamTrackProcessor({ track: track });
        videoState.reader = processor.readable.getReader();

        videoState.active = true;
        videoStartBtn.disabled = true;
        videoStopBtn.disabled = false;
        addMessage('system', `Video started (${VIDEO_CODEC} ${VIDEO_WIDTH}x${VIDEO_HEIGHT}@${VIDEO_FRAMERATE})`);

        let frameCounter = 0;
        (async function pump() {
            while (videoState.active) {
                const { done, value } = await videoState.reader.read();
                if (done) break;
                // Keyframe every second to keep the receiver bounded.
                const keyFrame = (frameCounter % VIDEO_FRAMERATE) === 0;
                try {
                    videoState.encoder.encode(value, { keyFrame: keyFrame });
                } catch (err) {
                    videoState.droppedFrames++;
                    console.warn('encode failed:', err);
                }
                value.close();
                frameCounter++;
            }
        })();
    } catch (err) {
        addMessage('error', `Video start failed: ${err.message}`);
        console.error('startVideo error:', err);
        stopVideo();
    }
}

function stopVideo() {
    videoState.active = false;
    if (videoState.reader) {
        try { videoState.reader.cancel(); } catch (_) { /* ignore */ }
        videoState.reader = null;
    }
    if (videoState.encoder) {
        try { videoState.encoder.close(); } catch (_) { /* ignore */ }
        videoState.encoder = null;
    }
    if (videoState.decoder) {
        try { videoState.decoder.close(); } catch (_) { /* ignore */ }
        videoState.decoder = null;
    }
    if (videoState.mediaStream) {
        videoState.mediaStream.getTracks().forEach(function(t) { t.stop(); });
        videoState.mediaStream = null;
    }
    videoLocal.srcObject = null;
    videoStartBtn.disabled = !(transport && transport.isConnected());
    videoStopBtn.disabled = true;
    addMessage('system', 'Video stopped');
}

function handleRemoteFrame(seq, pts_us, flags, payload) {
    const dec = videoState.decoder;
    if (!dec || dec.state !== 'configured') {
        // Drop frames that arrive before the decoder is up.
        videoState.droppedFrames++;
        updateVideoStats();
        return;
    }
    try {
        const tsNum = (pts_us && pts_us.toNumber) ? pts_us.toNumber() : Number(pts_us || 0);
        const data = (payload instanceof Uint8Array) ? payload : new Uint8Array(payload);
        const chunk = new EncodedVideoChunk({
            type: (flags & VIDEO_FLAG_KEYFRAME) ? 'key' : 'delta',
            timestamp: tsNum,
            data: data,
        });
        dec.decode(chunk);
        videoState.receivedFrames++;
    } catch (err) {
        videoState.droppedFrames++;
        console.warn('remote frame decode failed:', err);
    }
    updateVideoStats();
}

// protobuf.js Long wrapper around large JS numbers; the generator uses
// Long.fromNumber for method ids, but for payload uint64 fields we need a
// safe conversion that doesn't lose precision under ~2^53.
function Long_fromNumber_safe(n) {
    if (typeof Long !== 'undefined' && Long.fromNumber) {
        return Long.fromNumber(n, true);
    }
    return n;
}

// Event listeners
connectBtn.addEventListener('click', connect);
disconnectBtn.addEventListener('click', disconnect);
sendBtn.addEventListener('click', sendMessage);
calculateBtn.addEventListener('click', calculate);
sendChatBtn.addEventListener('click', sendChatMessage);
clearBtn.addEventListener('click', clearMessages);
videoStartBtn.addEventListener('click', startVideo);
videoStopBtn.addEventListener('click', stopVideo);

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
