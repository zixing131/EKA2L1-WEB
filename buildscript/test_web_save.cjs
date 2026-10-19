// Run with: node --test buildscript/test_web_save.cjs
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function bootstrap() {
    const context = vm.createContext({ window: {}, console, performance, setTimeout, clearTimeout });
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../src/emu/web/pages/js/boot.js'), 'utf8'), context);
    const callbacks = [];
    const api = context.window.EKA2L1;
    api.module = { FS: { syncfs(populate, callback) {
        assert.equal(populate, false);
        callbacks.push(callback);
    } } };
    return { api, callbacks };
}

const turn = () => new Promise(resolve => setImmediate(resolve));

test('a second save waits for a fresh snapshot after the first transaction', async () => {
    const { api, callbacks } = bootstrap();
    const first = api.save();
    const second = api.save();
    let secondDone = false;
    second.then(() => { secondDone = true; });
    await turn();
    assert.equal(callbacks.length, 1);
    assert.equal(secondDone, false);
    callbacks[0](null);
    await first;
    await turn();
    assert.equal(callbacks.length, 2);
    assert.equal(secondDone, false);
    callbacks[1](null);
    await second;
    assert.equal(secondDone, true);
});

test('a failed transaction rejects its caller and allows the next save to retry', async () => {
    const { api, callbacks } = bootstrap();
    const first = api.save();
    const failed = assert.rejects(first, /storage failed/);
    const retry = api.save();
    await turn();
    callbacks[0](new Error('storage failed'));
    await failed;
    await turn();
    assert.equal(callbacks.length, 2);
    callbacks[1](null);
    await retry;
});
