const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function bootstrap(states) {
    let now = 0;
    let calls = 0;
    const timers = [];
    const context = vm.createContext({
        window: {}, console, performance,
        Date: { now: () => now },
        setTimeout: fn => timers.push(fn),
        clearTimeout() {}
    });
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../src/emu/web/pages/js/boot.js'), 'utf8'), context);
    const api = context.window.EKA2L1;
    api.module = { ccall(name) {
        assert.equal(name, 'wasm_prepare_midlet_install');
        return states[Math.min(calls++, states.length - 1)];
    } };
    return { api, timers, calls: () => calls, advance: ms => { now += ms; } };
}

test('cold AMS initialization yields between polls and waits for readiness', async () => {
    const fixture = bootstrap([1, 1, 0]);
    const pending = fixture.api.prepareMidletInstall();
    let ready = false;
    pending.then(() => { ready = true; });
    assert.equal(fixture.calls(), 1);
    assert.equal(ready, false);
    fixture.timers.shift()();
    await Promise.resolve();
    assert.equal(fixture.calls(), 2);
    assert.equal(ready, false);
    fixture.timers.shift()();
    await pending;
    assert.equal(ready, true);
    assert.equal(fixture.timers.length, 0);
});

test('AMS startup failure rejects without scheduling another poll', async () => {
    const fixture = bootstrap([-1]);
    await assert.rejects(fixture.api.prepareMidletInstall(), /initialization failed/);
    assert.equal(fixture.timers.length, 0);
});

test('AMS startup timeout rejects instead of proceeding with installation', async () => {
    const fixture = bootstrap([1]);
    const pending = fixture.api.prepareMidletInstall();
    const rejected = assert.rejects(pending, /initialization timed out/);
    fixture.advance(180001);
    fixture.timers.shift()();
    await rejected;
    assert.equal(fixture.timers.length, 0);
});
