// Exercise the actual EM_JS installer with native WebAssembly in Node.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const cpp = fs.readFileSync(path.join(__dirname,
    '../src/emu/cpu/src/dyncom/arm_dyncom_wasmjit.cpp'), 'utf8');
const body = cpp.match(/EM_JS\(int, dyncom_wasmjit_install,[^\n]*\{([\s\S]*?)\n\}\);/)[1];

// A real module exporting f() -> i32.const value (0..63).
function binary(value) {
    return Uint8Array.from([
        0,97,115,109,1,0,0,0, 1,5,1,96,0,1,127, 3,2,1,0,
        7,5,1,1,102,0,0, 10,6,1,4,0,65,value,11
    ]);
}

function setup({ collision = false, maxEntries = 8192, maxBytes = 16 * 1024 * 1024 } = {}) {
    let compiles = 0;
    const table = new WebAssembly.Table({ initial: 1, element: 'anyfunc' });
    const context = vm.createContext({
        Module: {}, HEAPU8: new Uint8Array(1024), wasmTable: table,
        wasmMemory: new WebAssembly.Memory({ initial: 1 }),
        Math: collision ? { imul: () => 0 } : Math,
        console: { warn() {} },
        WebAssembly: {
            Module: new Proxy(WebAssembly.Module, { construct(target, args) {
                compiles++;
                return Reflect.construct(target, args);
            } }),
            Instance: WebAssembly.Instance
        }
    });
    // Small budgets exercise the same eviction paths without thousands of modules.
    const source = body.replace('const maxEntries = 8192;', `const maxEntries = ${maxEntries};`)
        .replace('const maxBytes = 16 * 1024 * 1024;', `const maxBytes = ${maxBytes};`);
    const install = vm.runInContext(`(function(bytes, len, reuse_idx) {${source}})`, context);
    return {
        context, table, compiles: () => compiles,
        install(bytes, slot = -1) {
            context.HEAPU8.set(bytes);
            return install(0, bytes.length, slot);
        },
        cache: () => context.Module._ekaJitModuleCache
    };
}

test('identical bytes reuse a module but create a fresh instance and honor slot reuse', () => {
    const s = setup();
    const first = s.install(binary(7));
    const oldFunction = s.table.get(first);
    const second = s.install(binary(7), first);
    assert.equal(first, second);
    assert.equal(s.table.length, 2);
    assert.equal(s.table.get(second)(), 7);
    assert.notEqual(s.table.get(second), oldFunction);
    assert.equal(s.compiles(), 1);
    assert.equal(s.cache().hits, 1);
    s.install(binary(8), first);
    s.install(binary(7), first); // Snapshot was not a view into the reused heap buffer.
    assert.equal(s.compiles(), 2);
    assert.equal(s.table.get(first)(), 7);
});

test('a hash collision cannot execute a different module', () => {
    const s = setup({ collision: true });
    const slot = s.install(binary(1));
    s.install(binary(2), slot);
    assert.equal(s.table.get(slot)(), 2);
    assert.equal(s.compiles(), 2);
    assert.equal(s.cache().hits, 0);
    assert.equal(s.cache().bytes, binary(2).length);
});

test('entry and byte budgets evict the least recently used module', () => {
    for (const limits of [{ maxEntries: 2 }, { maxBytes: binary(0).length * 2 }]) {
        const s = setup(limits);
        const slot = s.install(binary(1));
        s.install(binary(2), slot);
        s.install(binary(1), slot); // 2 is now oldest.
        s.install(binary(3), slot);
        assert.equal(s.cache().entries.size, 2);
        assert.equal(s.cache().bytes, binary(0).length * 2);
        s.install(binary(1), slot);
        assert.equal(s.compiles(), 3);
        s.install(binary(2), slot);
        assert.equal(s.compiles(), 4);
    }
});

test('oversized modules are usable without retention and failed compilation is not cached', () => {
    const s = setup({ maxBytes: 1 });
    const slot = s.install(binary(3));
    assert.equal(s.table.get(slot)(), 3);
    assert.equal(s.cache().entries.size, 0);
    assert.equal(s.cache().bytes, 0);
    const bad = Uint8Array.of(0, 1, 2, 3);
    assert.equal(s.install(bad, slot), -1);
    assert.equal(s.table.get(slot)(), 3);
    assert.equal(s.cache().entries.size, 0);
});
