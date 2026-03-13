#!/usr/bin/env node

const fs = require('fs');

function getArg(name) {
    const prefix = `${name}=`;
    const match = process.argv.find((arg) => arg.startsWith(prefix));
    if (!match) {
        throw new Error(`Missing required argument ${name}=...`);
    }
    return match.slice(prefix.length);
}

function parseInterfaceIds(headerText) {
    const result = {};
    const regex
        = /class\s+(\w+)\s*:\s*public rpc::casting_interface[\s\S]*?static rpc::interface_ordinal get_id\(uint64_t rpc_version\)[\s\S]*?return \{(\d+)ull\};/g;

    let match = null;
    while ((match = regex.exec(headerText)) !== null) {
        result[match[1]] = match[2];
    }

    return result;
}

function parseMessageTypeIds(headerText) {
    const result = {};
    const regex = /template<>\s*class id<::websocket_demo::v1::(\w+)>\s*\{[\s\S]*?auto id = (\d+)ull;/g;

    let match = null;
    while ((match = regex.exec(headerText)) !== null) {
        result[match[1]] = match[2];
    }

    return result;
}

function parseServiceMethods(protoText) {
    const services = {};
    const serviceRegex = /service\s+(\w+)\s*\{([\s\S]*?)\}/g;

    let serviceMatch = null;
    while ((serviceMatch = serviceRegex.exec(protoText)) !== null) {
        const [, serviceName, body] = serviceMatch;
        const methods = {};
        const rpcRegex = /rpc\s+(\w+)\s*\(/g;
        let rpcMatch = null;
        let ordinal = 1;

        while ((rpcMatch = rpcRegex.exec(body)) !== null) {
            methods[rpcMatch[1]] = ordinal++;
        }

        services[serviceName] = methods;
    }

    return services;
}

function generateHelperSource(metadata) {
    const metadataJson = JSON.stringify(metadata, null, 4);

    return `/* Auto-generated from websocket_demo IDL. Do not edit by hand. */
(function(root, factory) {
    if (typeof module === 'object' && module.exports) {
        module.exports = factory(require('./websocket_proto.js'), require('protobufjs/minimal'));
        return;
    }

    root.CanopyWebsocketDemo = factory(root.$protobuf_websocket, root.protobuf);
})(typeof globalThis !== 'undefined' ? globalThis : this, function(protoModule, protobufLib) {
    'use strict';

    if (!protoModule || !protoModule.protobuf) {
        throw new Error('websocket_proto.js must be loaded before websocket_api.js');
    }

    const WebsocketProto = protoModule.protobuf.websocket_demo_v1;
    const RpcProto = protoModule.protobuf.rpc;
    const LongCtor = (protobufLib && protobufLib.util && protobufLib.util.Long)
        || (typeof Long !== 'undefined' ? Long : null);
    const metadata = ${metadataJson};
    const zoneProto = RpcProto.zone || RpcProto.caller_zone || RpcProto.destination_zone;
    const callerZoneProto = RpcProto.caller_zone || zoneProto;
    const objectProto = RpcProto.object;

    function toUnsigned(value) {
        if (LongCtor && typeof LongCtor.fromString === 'function') {
            if (typeof value === 'string') {
                return LongCtor.fromString(value, true);
            }
            return LongCtor.fromNumber(Number(value), true);
        }
        return typeof value === 'string' ? Number(value) : value;
    }

    function longToString(value) {
        if (value === undefined || value === null) {
            return '0';
        }
        if (typeof value === 'string') {
            return value;
        }
        if (typeof value === 'number') {
            return String(value);
        }
        if (typeof value.toString === 'function') {
            return value.toString();
        }
        return String(value);
    }

    function toUint8Array(data) {
        if (data instanceof Uint8Array) {
            return data;
        }
        if (typeof Buffer !== 'undefined' && Buffer.isBuffer(data)) {
            return new Uint8Array(data);
        }
        if (data instanceof ArrayBuffer) {
            return new Uint8Array(data);
        }
        if (ArrayBuffer.isView(data)) {
            return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
        }
        return new Uint8Array(data);
    }

    function cloneMetadata() {
        return JSON.parse(JSON.stringify(metadata));
    }

    function lookupInterface(interfaceName) {
        const entry = metadata.interfaces[interfaceName];
        if (!entry) {
            throw new Error('Unknown interface: ' + interfaceName);
        }
        return entry;
    }

    function lookupMethod(interfaceName, methodName) {
        const iface = lookupInterface(interfaceName);
        const ordinal = iface.methods[methodName];
        if (!ordinal) {
            throw new Error('Unknown method ' + interfaceName + '.' + methodName);
        }
        return ordinal;
    }

    function zoneTextToDisplayId(zoneText) {
        if (!zoneText) {
            return 0;
        }

        const text = String(zoneText);
        const slash = text.indexOf('/');
        const zoneOnly = slash >= 0 ? text.slice(0, slash) : text;
        const colon = zoneOnly.indexOf(':');
        const subnetText = colon >= 0 ? zoneOnly.slice(colon + 1) : zoneOnly;
        const parsed = Number(subnetText);
        return Number.isFinite(parsed) ? parsed : 0;
    }

    function isHandshakeResponse(connectResponse) {
        if (!connectResponse || typeof connectResponse !== 'object') {
            return false;
        }

        if (connectResponse.clientZoneIdText) {
            return true;
        }

        const serverRemoteText = connectResponse.serverRemoteObjectText;
        if (serverRemoteText && (serverRemoteText.zoneId || serverRemoteText.objectId)) {
            return true;
        }

        return false;
    }

    function createObjectId(objectId) {
        if (objectProto && typeof objectProto.create === 'function') {
            return objectProto.create({ id: toUnsigned(objectId) });
        }
        return { id: toUnsigned(objectId) };
    }

    function resolveProtoType(typeRef) {
        if (!typeRef) {
            return null;
        }
        if (typeof typeRef === 'string') {
            const direct = WebsocketProto[typeRef];
            if (direct) {
                return direct;
            }
            throw new Error('Unknown protobuf type: ' + typeRef);
        }
        return typeRef;
    }

    function encodeMessage(typeRef, payload) {
        const type = resolveProtoType(typeRef);
        if (!type) {
            return new Uint8Array();
        }
        const message = typeof type.create === 'function' ? type.create(payload) : payload;
        return type.encode(message).finish();
    }

    function decodeMessage(typeRef, data) {
        const type = resolveProtoType(typeRef);
        if (!type) {
            return null;
        }
        return type.decode(toUint8Array(data));
    }

    class DemoWebsocketClient {
        constructor(options) {
            const opts = options || {};
            this.WebSocketImpl = opts.WebSocketImpl || (typeof WebSocket !== 'undefined' ? WebSocket : null);
            this.websocketUrl = opts.websocketUrl || null;
            this.onOpen = opts.onOpen || null;
            this.onHandshake = opts.onHandshake || null;
            this.onTextMessage = opts.onTextMessage || null;
            this.onEvent = opts.onEvent || null;
            this.onClose = opts.onClose || null;
            this.onError = opts.onError || null;
            this.ws = null;
            this.handshakeComplete = false;
            this.clientZoneId = 0;
            this.serverZoneId = 0;
            this.serverObjectId = 0;
            this.clientZoneText = '';
            this.serverZoneText = '';
            this.messageCounter = 0;
            this.pendingConnect = null;
            this.pendingRequests = new Map();
        }

        isOpen() {
            return !!this.ws && this.ws.readyState === this.WebSocketImpl.OPEN;
        }

        isReady() {
            return this.isOpen() && this.handshakeComplete;
        }

        getState() {
            return {
                handshakeComplete: this.handshakeComplete,
                clientZoneId: this.clientZoneId,
                serverZoneId: this.serverZoneId,
                serverObjectId: this.serverObjectId
            };
        }

        getWebSocketUrl() {
            if (this.websocketUrl) {
                return this.websocketUrl;
            }
            if (typeof window !== 'undefined' && window.location) {
                const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
                return protocol + '//' + window.location.host;
            }
            throw new Error('websocketUrl must be provided outside the browser');
        }

        async connect(options) {
            if (!this.WebSocketImpl) {
                throw new Error('No WebSocket implementation available');
            }
            if (this.isReady()) {
                return this.getState();
            }
            if (this.pendingConnect) {
                return this.pendingConnect.promise;
            }

            const connectOptions = options || {};
            this.handshakeComplete = false;
            this.clientZoneId = 0;
            this.serverZoneId = 0;
            this.serverObjectId = 0;

            this.ws = new this.WebSocketImpl(this.getWebSocketUrl());
            if ('binaryType' in this.ws) {
                this.ws.binaryType = typeof Buffer === 'undefined' ? 'arraybuffer' : 'nodebuffer';
            }

            this.pendingConnect = {};
            this.pendingConnect.promise = new Promise((resolve, reject) => {
                this.pendingConnect.resolve = resolve;
                this.pendingConnect.reject = reject;
            });

            this.ws.onopen = () => {
                if (this.onOpen) {
                    this.onOpen();
                }
                this.sendHandshake(connectOptions.callbackObjectId || 1);
            };

            this.ws.onmessage = (event) => {
                const data = event && Object.prototype.hasOwnProperty.call(event, 'data') ? event.data : event;
                this.handleIncoming(data);
            };

            this.ws.onclose = (event) => {
                this.rejectAllPending(new Error('WebSocket closed'));
                if (this.pendingConnect) {
                    this.pendingConnect.reject(new Error('WebSocket closed before handshake completed'));
                    this.pendingConnect = null;
                }
                if (this.onClose) {
                    this.onClose(event);
                }
            };

            this.ws.onerror = (error) => {
                if (this.onError) {
                    this.onError(error);
                }
            };

            return this.pendingConnect.promise;
        }

        disconnect(code, reason) {
            if (this.ws) {
                this.ws.close(code || 1000, reason || 'Client disconnect');
            }
        }

        sendText(message) {
            if (!this.isOpen()) {
                throw new Error('WebSocket is not connected');
            }
            this.ws.send(message);
        }

        sendHandshake(callbackObjectId) {
            const connectRequest = WebsocketProto.connect_request.create({
                inboundRemoteObject: RpcProto.remote_object.create({
                    addr_: RpcProto.zone_address.create({})
                }),
                inboundCallbackObjectId: createObjectId(callbackObjectId)
            });

            this.ws.send(WebsocketProto.connect_request.encode(connectRequest).finish());
        }

        async call(options) {
            if (!this.isReady()) {
                throw new Error('Handshake not complete');
            }

            const interfaceName = options.interfaceName;
            const methodName = options.methodName;
            const payloadType = options.payloadType;
            const responseType = options.responseType || null;
            const encoding = options.encoding !== undefined
                ? options.encoding
                : (RpcProto.encoding.encoding_protocol_buffers || 16);

            const methodId = lookupMethod(interfaceName, methodName);
            const interfaceId = lookupInterface(interfaceName).id;
            const requestBytes = encodeMessage(payloadType, options.payload || {});

            this.messageCounter += 1;
            const messageId = this.messageCounter;

            const wsRequest = WebsocketProto.request.create({
                encoding: encoding,
                tag: toUnsigned(messageId),
                callerZoneId: callerZoneProto.create({}),
                destinationZoneId: RpcProto.remote_object.create({}),
                interfaceId: RpcProto.interface_ordinal.create({
                    id: toUnsigned(interfaceId)
                }),
                methodId: RpcProto.method.create({
                    id: toUnsigned(methodId)
                }),
                data: requestBytes,
                backChannel: [],
                callerZoneIdText: this.clientZoneText,
                destinationZoneIdText: this.serverZoneText,
                destinationObjectId: toUnsigned(this.serverObjectId)
            });

            const envelope = WebsocketProto.envelope.create({
                messageId: toUnsigned(messageId),
                messageType: toUnsigned(metadata.messageTypes.request),
                data: WebsocketProto.request.encode(wsRequest).finish()
            });

            const promise = new Promise((resolve, reject) => {
                this.pendingRequests.set(String(messageId), {
                    resolve: resolve,
                    reject: reject,
                    responseType: responseType,
                    interfaceName: interfaceName,
                    methodName: methodName
                });
            });

            this.ws.send(WebsocketProto.envelope.encode(envelope).finish());
            return promise;
        }

        rejectAllPending(error) {
            for (const pending of this.pendingRequests.values()) {
                pending.reject(error);
            }
            this.pendingRequests.clear();
        }

        handleIncoming(data) {
            if (typeof data === 'string') {
                if (this.onTextMessage) {
                    this.onTextMessage(data);
                }
                return;
            }

            const bytes = toUint8Array(data);

            if (!this.handshakeComplete) {
                const connectResponse = WebsocketProto.connect_response.decode(bytes);
                if (isHandshakeResponse(connectResponse)) {
                    const clientZoneText = connectResponse.clientZoneIdText || '';
                    const serverRemoteText = connectResponse.serverRemoteObjectText || null;

                    this.clientZoneText = clientZoneText;
                    this.serverZoneText = serverRemoteText && serverRemoteText.zoneId ? serverRemoteText.zoneId : '';
                    this.clientZoneId = zoneTextToDisplayId(this.clientZoneText);
                    this.serverZoneId = zoneTextToDisplayId(this.serverZoneText);
                    this.serverObjectId = serverRemoteText ? Number(longToString(serverRemoteText.objectId || 0)) : 0;
                    this.handshakeComplete = true;

                    if (this.pendingConnect) {
                        this.pendingConnect.resolve(this.getState());
                        this.pendingConnect = null;
                    }
                    if (this.onHandshake) {
                        this.onHandshake(this.getState(), connectResponse);
                    }
                    return;
                }
            }

            const envelope = WebsocketProto.envelope.decode(bytes);
            const messageId = longToString(envelope.messageId);
            const pending = this.pendingRequests.get(messageId);

            if (pending) {
                this.pendingRequests.delete(messageId);
                const response = WebsocketProto.response.decode(envelope.data);
                const decoded = response.data && response.data.length
                    ? decodeMessage(pending.responseType, response.data)
                    : null;
                pending.resolve({
                    envelope: envelope,
                    response: response,
                    decoded: decoded,
                    interfaceName: pending.interfaceName,
                    methodName: pending.methodName,
                    messageId: messageId
                });
                return;
            }

            const event = this.decodeEvent(envelope);
            if (event && this.onEvent) {
                this.onEvent(event);
            }
        }

        decodeEvent(envelope) {
            const request = WebsocketProto.request.decode(envelope.data);
            const interfaceId = longToString(request.interfaceId && request.interfaceId.id);
            const methodId = longToString(request.methodId && request.methodId.id);

            for (const [interfaceName, iface] of Object.entries(metadata.interfaces)) {
                if (iface.id !== interfaceId) {
                    continue;
                }

                for (const [methodName, ordinal] of Object.entries(iface.methods)) {
                    if (String(ordinal) !== methodId) {
                        continue;
                    }

                    const payloadTypeName = interfaceName + '_' + methodName + 'Request';
                    return {
                        interfaceName: interfaceName,
                        methodName: methodName,
                        request: request,
                        decoded: decodeMessage(payloadTypeName, request.data),
                        envelope: envelope
                    };
                }
            }

            return null;
        }
    }

    return {
        metadata: cloneMetadata(),
        createClient: function(options) {
            return new DemoWebsocketClient(options);
        },
        DemoWebsocketClient: DemoWebsocketClient,
        toUnsigned: toUnsigned
    };
});
`;
}

function main() {
    const headerPath = getArg('--header');
    const protoPath = getArg('--proto');
    const browserOutPath = getArg('--out-browser');
    const nodeOutPath = getArg('--out-node');

    const headerText = fs.readFileSync(headerPath, 'utf8');
    const protoText = fs.readFileSync(protoPath, 'utf8');
    const interfaceIds = parseInterfaceIds(headerText);
    const messageTypeIds = parseMessageTypeIds(headerText);
    const methods = parseServiceMethods(protoText);

    const metadata = {
        interfaces: {
            i_context_event: {
                id: interfaceIds.i_context_event,
                methods: methods.i_context_event || {}
            },
            i_calculator: {
                id: interfaceIds.i_calculator,
                methods: methods.i_calculator || {}
            }
        },
        messageTypes: {
            connect_request: messageTypeIds.connect_request,
            connect_response: messageTypeIds.connect_response,
            envelope: messageTypeIds.envelope,
            request: messageTypeIds.request,
            response: messageTypeIds.response
        }
    };

    const source = generateHelperSource(metadata);
    fs.writeFileSync(browserOutPath, source);
    fs.writeFileSync(nodeOutPath, source);
}

main();
