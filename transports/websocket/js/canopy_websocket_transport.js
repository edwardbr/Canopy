/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 *
 *   Generic Canopy WebSocket transport — handles the handshake and RPC envelope
 *   protocol. Does NOT change when IDL changes. Works in Node.js and browser (UMD).
 *
 *   Usage:
 *     const transport = new CanopyWebsocketTransport({
 *         url,                   // ws:// or wss://
 *         proto,                 // websocket_protocol_v1 namespace from pbjs output
 *         appProto,              // app-specific namespace passed to stub.handlePost() (default: proto)
 *         inboundInterfaceId,    // Long — interface the client exposes (back-channel)
 *         outboundInterfaceId,   // Long — interface the client calls
 *         clientObjectId,        // Long — default Long(1, true)
 *         timeoutMs,             // default 10000
 *         onOpen(transport),
 *         onClose(code, reason),
 *         onError(err),
 *     });
 *     await transport.connect();
 *     transport.serverObject  // zone_address_args for RPC calls
 *     transport.clientObject  // zone_address_args assigned by server
 *
 *     transport.call(interfaceId, methodId, requestBytes) → Promise<responseBytes>
 *     transport.registerStub(stub)  // stub.handlePost(proto, ifaceId, methodId, data)
 *     transport.disconnect()
 *     transport.isConnected()
 */
(function(root, factory) {
    if (typeof module === 'object' && module.exports) {
        var WebSocket = require('ws');
        module.exports = factory(WebSocket);
    } else {
        root.CanopyWebsocketTransport = factory(root.WebSocket);
    }
})(typeof globalThis !== 'undefined' ? globalThis : this, function(WebSocket) {
    'use strict';

    // Envelope message_type enum values (from websocket_protocol.idl)
    var MSG_SEND             = 0;
    var MSG_POST             = 1;
    var MSG_RESPONSE         = 2;
    var MSG_HANDSHAKE        = 3;
    var MSG_HANDSHAKE_ACK    = 4;
    var MSG_HANDSHAKE_COMPLETE = 5;

    // Encoding constant — protocol_buffers = 16
    var ENCODING_PROTOCOL_BUFFERS = 16;

    function CanopyWebsocketTransport(opts) {
        this._url               = opts.url;
        this._proto             = opts.proto;       // websocket_protocol_v1 pbjs namespace (envelope/request/response)
        this._appProto          = opts.appProto || opts.proto;  // app-specific pbjs namespace passed to stub.handlePost()
        this._inboundIfaceId    = opts.inboundInterfaceId;
        this._outboundIfaceId   = opts.outboundInterfaceId;
        this._clientObjectId    = opts.clientObjectId || Long_fromNumber(1, true);
        this._timeoutMs         = opts.timeoutMs    || 10000;
        this._onOpen            = opts.onOpen       || function() {};
        this._onClose           = opts.onClose      || function() {};
        this._onError           = opts.onError      || function() {};

        this._ws                = null;
        this._connected         = false;
        this._msgCounter        = 0;
        this._pending           = {};  // id → {resolve, reject, timer}
        this._stubs             = [];  // registered stubs

        this.serverObject       = null;  // zone_address_args; populated after connect()
        this.clientObject       = null;  // zone_address_args; populated after handshake_ack
    }

    // ---------------------------------------------------------------------------
    // Long helpers — works with protobufjs's Long (browser global or require)
    // ---------------------------------------------------------------------------
    function Long_fromNumber(n, unsigned) {
        if (typeof Long !== 'undefined')
            return Long.fromNumber(n, unsigned);
        // fallback: plain number (loses precision for very large IDs, but safe for small counters)
        return n;
    }

    // ---------------------------------------------------------------------------
    // Connect: opens WebSocket, performs handshake, resolves when complete
    // ---------------------------------------------------------------------------
    CanopyWebsocketTransport.prototype.connect = function() {
        var self = this;
        return new Promise(function(resolve, reject) {
            var ws;
            try {
                ws = new WebSocket(self._url);
                ws.binaryType = 'arraybuffer';
            } catch(e) {
                reject(e);
                return;
            }
            self._ws = ws;

            var handshakeDone = false;

            ws.onopen = function() {
                // Send connect_request (Type 3 — handshake)
                try {
                    var connectReq = self._proto.connect_request.create({
                        inboundInterfaceId:  { id: self._inboundIfaceId },
                        outboundInterfaceId: { id: self._outboundIfaceId },
                        remoteObjectId: { objectId: self._clientObjectId }
                    });
                    var reqBytes = self._proto.connect_request.encode(connectReq).finish();
                    var env = self._proto.envelope.create({
                        id:   Long_fromNumber(0, true),
                        type: MSG_HANDSHAKE,
                        data: reqBytes
                    });
                    var envBytes = self._proto.envelope.encode(env).finish();
                    ws.send(envBytes);
                } catch(e) {
                    reject(e);
                }
            };

            ws.onmessage = function(event) {
                var bytes = new Uint8Array(event.data);
                var env;
                try {
                    env = self._proto.envelope.decode(bytes);
                } catch(e) {
                    console.error('[Canopy] envelope decode error:', e);
                    return;
                }

                var msgType = (env.type && env.type.toNumber) ? env.type.toNumber() : Number(env.type);

                if (!handshakeDone) {
                    // Handshake phase
                    if (msgType === MSG_HANDSHAKE_ACK) {
                        try {
                            var initialResp = self._proto.connect_initial_response.decode(env.data);
                            self.clientObject = initialResp.remoteObjectId || null;
                            // Notify stubs so they can populate getRemoteObject()
                            for (var i = 0; i < self._stubs.length; i++) {
                                if (self._stubs[i]._setRemoteObject)
                                    self._stubs[i]._setRemoteObject(self.clientObject);
                            }
                        } catch(e) {
                            reject(e);
                        }
                    } else if (msgType === MSG_HANDSHAKE_COMPLETE) {
                        try {
                            var connectResp = self._proto.connect_response.decode(env.data);
                            self.serverObject = connectResp.outboundRemoteObject || null;
                            handshakeDone = true;
                            self._connected = true;
                            // Replace onmessage with dispatch handler
                            ws.onmessage = function(ev) { self._onMessage(ev); };
                            self._onOpen(self);
                            resolve(self);
                        } catch(e) {
                            reject(e);
                        }
                    } else {
                        console.warn('[Canopy] Unexpected envelope type', msgType, 'during handshake');
                    }
                }
            };

            ws.onclose = function(event) {
                self._connected = false;
                // Reject all pending calls
                var pending = self._pending;
                self._pending = {};
                for (var id in pending) {
                    clearTimeout(pending[id].timer);
                    pending[id].reject(new Error('WebSocket closed (code ' + event.code + ')'));
                }
                if (!handshakeDone) {
                    reject(new Error('WebSocket closed before handshake complete'));
                }
                self._onClose(event.code, event.reason);
            };

            ws.onerror = function(err) {
                self._onError(err);
                if (!handshakeDone) {
                    reject(err);
                }
            };
        });
    };

    // ---------------------------------------------------------------------------
    // Inbound message dispatch (after handshake complete)
    // ---------------------------------------------------------------------------
    CanopyWebsocketTransport.prototype._onMessage = function(event) {
        var self = this;
        var bytes = new Uint8Array(event.data);
        var env;
        try {
            env = self._proto.envelope.decode(bytes);
        } catch(e) {
            console.error('[Canopy] envelope decode error:', e);
            return;
        }

        var msgType = (env.type && env.type.toNumber) ? env.type.toNumber() : Number(env.type);
        var msgId   = (env.id   && env.id.toNumber)   ? env.id.toNumber()   : Number(env.id);

        if (msgType === MSG_RESPONSE) {
            // Correlate to a pending call
            var entry = self._pending[msgId];
            if (entry) {
                clearTimeout(entry.timer);
                delete self._pending[msgId];
                try {
                    var resp = self._proto.response.decode(env.data);
                    var errCode = (resp.error && resp.error.toNumber) ? resp.error.toNumber() : Number(resp.error || 0);
                    if (errCode !== 0) {
                        entry.reject(new Error('[Canopy] RPC error code: ' + errCode));
                    } else {
                        entry.resolve(resp.data);
                    }
                } catch(e) {
                    entry.reject(e);
                }
            } else {
                console.warn('[Canopy] No pending request for id', msgId);
            }
        } else if (msgType === MSG_POST) {
            // Server-initiated event — dispatch to registered stubs
            try {
                var req = self._proto.request.decode(env.data);
                var ifaceId  = req.interfaceId  ? req.interfaceId.id  : null;
                var methodId = req.methodId     ? req.methodId.id     : null;
                for (var i = 0; i < self._stubs.length; i++) {
                    self._stubs[i].handlePost(self._appProto, ifaceId, methodId, req.data);
                }
            } catch(e) {
                console.error('[Canopy] post dispatch error:', e);
            }
        } else {
            console.warn('[Canopy] Unexpected message type after handshake:', msgType);
        }
    };

    // ---------------------------------------------------------------------------
    // call() — sends a type-0 envelope, returns Promise<responseBytes>
    // Used by generated proxy methods.
    // ---------------------------------------------------------------------------
    CanopyWebsocketTransport.prototype.call = function(interfaceId, methodId, requestBytes) {
        var self = this;
        if (!self._connected) {
            return Promise.reject(new Error('[Canopy] Not connected'));
        }

        self._msgCounter++;
        var id = self._msgCounter;

        return new Promise(function(resolve, reject) {
            var timer = setTimeout(function() {
                if (self._pending[id]) {
                    delete self._pending[id];
                    reject(new Error('[Canopy] RPC call timed out (id=' + id + ')'));
                }
            }, self._timeoutMs);

            self._pending[id] = { resolve: resolve, reject: reject, timer: timer };

            try {
                var wsReq = self._proto.request.create({
                    encoding:          ENCODING_PROTOCOL_BUFFERS,
                    tag:               Long_fromNumber(id, true),
                    callerZoneId:      self.clientObject || {},
                    destinationZoneId: self.serverObject || {},
                    interfaceId:       { id: interfaceId },
                    methodId:          { id: methodId },
                    data:              requestBytes,
                    backChannel:       []
                });
                var payload = self._proto.request.encode(wsReq).finish();

                var env = self._proto.envelope.create({
                    id:   Long_fromNumber(id, true),
                    type: MSG_SEND,
                    data: payload
                });
                var envBytes = self._proto.envelope.encode(env).finish();
                self._ws.send(envBytes);
            } catch(e) {
                clearTimeout(timer);
                delete self._pending[id];
                reject(e);
            }
        });
    };

    // ---------------------------------------------------------------------------
    // registerStub() — register a generated stub to receive [post] events
    // ---------------------------------------------------------------------------
    CanopyWebsocketTransport.prototype.registerStub = function(stub) {
        this._stubs.push(stub);
        // If already connected (clientObject known), populate immediately
        if (this.clientObject && stub._setRemoteObject) {
            stub._setRemoteObject(this.clientObject);
        }
    };

    // ---------------------------------------------------------------------------
    // disconnect() — close the WebSocket
    // ---------------------------------------------------------------------------
    CanopyWebsocketTransport.prototype.disconnect = function() {
        if (this._ws) {
            this._connected = false;
            this._ws.close();
            this._ws = null;
        }
    };

    // ---------------------------------------------------------------------------
    // isConnected()
    // ---------------------------------------------------------------------------
    CanopyWebsocketTransport.prototype.isConnected = function() {
        return this._connected;
    };

    return CanopyWebsocketTransport;
});
