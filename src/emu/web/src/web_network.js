/* EKA2L1 browser TCP transport. Wire-compatible with JavaQQServer /ws/tcp.
 * Loaded as Emscripten pre-js; no network activity before a guest connects.
 * All notifications are asynchronous, including immediate local failures.
 */
Module['ekaNetwork'] = (() => {
    const sockets = new Map();
    let ws = null;
    let ping = null;
    let opening = null;
    const MAX_SEND = 4 * 1024 * 1024;
    const stats = { connects: 0, sent: 0, received: 0, errors: 0 };
    function notify(id, event, bytes, error) {
        // Never reenter the guest kernel from an EM_JS call holding its lock.
        queueMicrotask(() => {
            if (!sockets.has(id)) return;
            let pointer = 0;
            try {
                if (bytes && bytes.length) {
                    pointer = _malloc(bytes.length);
                    if (!pointer) { fail(id, -4); return; }
                    HEAPU8.set(bytes, pointer);
                }
                Module['_eka_web_network_event'](id, event, pointer, bytes ? bytes.length : 0, error || 0);
            } finally {
                if (pointer) _free(pointer);
            }
        });
    }
    function fail(id, code) {
        const socket = sockets.get(id);
        if (!socket || socket.closed) return;
        socket.closed = true;
        clearTimeout(socket.timer);
        stats.errors++;
        notify(id, 4, null, code);
    }
    function endpoint() {
        return Module['networkRelayUrl'] || (typeof window !== 'undefined' && window.EKA2L1_NETWORK_RELAY_URL)
            || 'wss://qq.zixing.fun/ws/tcp';
    }
    function ensure() {
        if (ws && ws.readyState === 1) return Promise.resolve(ws);
        if (opening) return opening;
        opening = new Promise((resolve, reject) => {
            let transport;
            try { transport = new WebSocket(endpoint()); }
            catch (e) { reject(e); return; }
            ws = transport;
            transport.binaryType = 'arraybuffer';
            const timer = setTimeout(() => { reject(new Error('relay timeout')); transport.close(); }, 15000);
            transport.onopen = () => {
                clearTimeout(timer);
                resolve(transport);
                ping = setInterval(() => {
                    if (transport.readyState === 1) transport.send(JSON.stringify({ type: 'ping' }));
                }, 25000);
            };
            transport.onmessage = event => {
                // arraybuffer avoids asynchronous Blob conversion reordering TCP chunks.
                let message;
                try {
                    const text = typeof event.data === 'string' ? event.data : new TextDecoder().decode(event.data);
                    message = JSON.parse(text);
                } catch (_) { transport.close(); return; }
                if (message.type !== 'event') return;
                const id = Number(message.id);
                const socket = sockets.get(id);
                if (!socket || socket.closed) return;
                if (message.event === 'connect') {
                    clearTimeout(socket.timer);
                    socket.connected = true;
                    stats.connects++;
                    notify(id, 1);
                } else if (message.event === 'data') {
                    try {
                        const binary = atob(message.data);
                        const bytes = Uint8Array.from(binary, c => c.charCodeAt(0));
                        stats.received += bytes.length;
                        notify(id, 2, bytes);
                    } catch (_) { fail(id, -20); }
                } else if (message.event === 'error') {
                    console.warn('[WebNet]', message.message || 'relay error');
                    fail(id, /tim(e|ed).*out|cancel/i.test(message.message || '') ? -33 : -34);
                } else if (message.event === 'close' || message.event === 'end') {
                    socket.closed = true;
                    clearTimeout(socket.timer);
                    notify(id, 3);
                }
            };
            transport.onerror = () => reject(new Error('relay connection failed'));
            transport.onclose = () => {
                clearTimeout(timer);
                clearInterval(ping);
                if (ws !== transport) return;
                ws = null;
                opening = null;
                reject(new Error('relay disconnected'));
                for (const id of sockets.keys()) fail(id, -36);
            };
        });
        // A synchronous constructor failure also needs to allow a later retry.
        opening.catch(() => { if (!ws) opening = null; });
        return opening;
    }
    return {
        stats,
        connect(id, host, port) {
            const socket = { connected: false, closed: false, timer: null };
            sockets.set(id, socket);
            socket.timer = setTimeout(() => {
                fail(id, -33);
                if (ws && ws.readyState === 1) ws.send(JSON.stringify({ type: 'destroy', id: String(id) }));
            }, 30000);
            ensure().then(transport => {
                if (sockets.get(id) !== socket || socket.closed) return;
                transport.send(JSON.stringify({ type: 'connect', id: String(id), host, port, timeout: 15000 }));
            }).catch(() => fail(id, -34));
        },
        send(id, data) {
            const socket = sockets.get(id);
            if (!socket || !socket.connected || socket.closed || !ws || ws.readyState !== 1) return -36;
            if (data.length > MAX_SEND || ws.bufferedAmount > MAX_SEND) return -9;
            try {
                // Never spread a full file onto the JS argument stack.
                const parts = [];
                for (let offset = 0; offset < data.length; offset += 16384)
                    parts.push(String.fromCharCode.apply(null, data.subarray(offset, offset + 16384)));
                ws.send(JSON.stringify({ type: 'write', id: String(id), data: btoa(parts.join('')) }));
                stats.sent += data.length;
                // Relay v1 has no per-write ACK: completion means the transport
                // accepted the bytes, like a local TCP send buffer.
                return 0;
            } catch (_) { return -36; }
        },
        close(id) {
            const socket = sockets.get(id);
            if (!socket) return;
            clearTimeout(socket.timer);
            sockets.delete(id);
            if (ws && ws.readyState === 1)
                ws.send(JSON.stringify({ type: 'destroy', id: String(id) }));
            // Keep the multiplexed transport for subsequent sockets. Closing
            // it here races a new connect against the old WebSocket's close.

        }
    };
})();
