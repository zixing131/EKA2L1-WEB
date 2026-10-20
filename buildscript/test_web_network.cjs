const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(new URL('../src/emu/web/src/web_network.js', `file://${__filename}`), 'utf8');
const flush = () => new Promise(resolve => setImmediate(resolve));
function fixture() {
    const events = [], timers = new Map(), heap = new Uint8Array(1024 * 1024);
    let timerId = 0;
    class WS {
        static instances = [];
        constructor(url) { this.url = url; this.readyState = 0; this.bufferedAmount = 0; this.sent = []; WS.instances.push(this); }
        send(message) { assert.equal(this.readyState, 1); this.sent.push(JSON.parse(message)); }
        open() { this.readyState = 1; this.onopen(); }
        close() { this.readyState = 3; this.onclose(); }
        event(message, binary = false) {
            const text = JSON.stringify(message);
            this.onmessage({data: binary ? new TextEncoder().encode(text) : text});
        }
    }
    const Module = {_eka_web_network_event: (id, type, pointer, length, error) => {
        events.push({id, type, bytes: [...heap.slice(pointer, pointer + length)], error});
    }};
    vm.runInNewContext(source, {Module, WebSocket: WS, console, TextDecoder, Uint8Array, Map, atob, btoa,
        queueMicrotask, HEAPU8: heap, _malloc: () => 64, _free: () => {},
        setTimeout: fn => {timers.set(++timerId, fn); return timerId;}, clearTimeout: id => timers.delete(id),
        setInterval: () => 0, clearInterval: () => {}});
    return {net: Module.ekaNetwork, events, WS, timers};
}
test('multiplexed TCP preserves binary bytes, event order and send acceptance', async () => {
    const {net, WS, events} = fixture();
    assert.equal(WS.instances.length, 0);
    net.connect(1, 'music.163.com', 80); net.connect(2, 'example.org', 443);
    const ws = WS.instances[0];
    assert.equal(ws.url, 'wss://qq.zixing.fun/ws/tcp');
    ws.open(); await flush();
    assert.deepEqual(ws.sent.map(m => m.id), ['1', '2']);
    ws.event({type:'event', id:'1', event:'connect'});
    assert.equal(events.length, 0, 'callbacks must not reenter the kernel');
    await flush();
    assert.equal(net.send(1, new Uint8Array([0, 255, 128, 13, 10])), 0);
    assert.equal(ws.sent.at(-1).data, 'AP+ADQo=');
    ws.event({type:'event', id:'1', event:'data', data:'AP+A'}, true);
    ws.event({type:'event', id:'1', event:'data', data:'DQo='});
    ws.event({type:'event', id:'1', event:'close'});
    await flush();
    assert.deepEqual(events.map(e => e.type), [1, 2, 2, 3]);
    assert.deepEqual(events.flatMap(e => e.bytes), [0, 255, 128, 13, 10]);
    assert.equal(net.send(1, new Uint8Array(1)), -36);
});
test('cancelled connects and late messages cannot complete destroyed sockets', async () => {
    const {net, WS, events} = fixture();
    net.connect(1, 'example.org', 80);
    net.close(1);
    WS.instances[0].open(); await flush();
    assert.equal(WS.instances[0].sent.length, 0);
    WS.instances[0].event({type:'event', id:'1', event:'connect'});
    net.connect(2, 'example.org', 80); await flush();
    assert.equal(WS.instances.length, 1);
    assert.equal(WS.instances[0].sent[0].id, '2');
    await flush(); assert.equal(events.length, 0);
    WS.instances[0].close(); await flush();
    assert.deepEqual(events.map(e => [e.id, e.type, e.error]), [[2, 4, -36]]);
    net.connect(3, 'example.org', 80); assert.equal(WS.instances.length, 2);
});
test('bounded sends, connection errors and timeouts complete once', async () => {
    const {net, WS, events, timers} = fixture();
    net.connect(1, 'example.org', 80); const ws = WS.instances[0]; ws.open(); await flush();
    ws.event({type:'event', id:'1', event:'connect'}); await flush();
    assert.equal(net.send(1, new Uint8Array(4 * 1024 * 1024 + 1)), -9);
    ws.bufferedAmount = 4 * 1024 * 1024 + 1;
    assert.equal(net.send(1, new Uint8Array(1)), -9);
    net.connect(2, 'invalid.example', 80); await flush();
    ws.event({type:'event', id:'2', event:'error', message:'Connection refused'});
    ws.event({type:'event', id:'2', event:'close'}); await flush();
    assert.equal(events.filter(e => e.id === 2).length, 1);
    assert.equal(events.at(-1).error, -34);
    net.connect(3, 'example.org', 80); await flush();
    for (const fn of [...timers.values()]) fn();
    await flush(); assert.equal(events.at(-1).error, -33);
});
