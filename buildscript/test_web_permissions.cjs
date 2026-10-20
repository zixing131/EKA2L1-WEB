// Run with: node --test buildscript/test_web_permissions.cjs
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const read = file => fs.readFileSync(path.join(__dirname, '..', file), 'utf8');
const turn = () => new Promise(resolve => setImmediate(resolve));

function setup() {
    const values = new Map();
    const events = {};
    const requests = [];
    const calls = [];
    const failures = [];
    const watches = [];
    const cleared = [];
    const rafs = new Map();
    let nextRaf = 1;
    const window = { addEventListener(name, callback) { events[name] = callback; } };
    const context = vm.createContext({
        window, console, performance, setTimeout, clearTimeout,
        localStorage: { getItem: key => values.get(key) ?? null, setItem: (key, value) => values.set(key, value) },
        navigator: {
            mediaDevices: { getUserMedia: () => new Promise((resolve, reject) => requests.push({ resolve, reject })) },
            geolocation: {
                watchPosition(success, failure) { watches.push({ success, failure }); return watches.length - 1; },
                clearWatch(id) { cleared.push(id); }
            }
        },
        document: {
            body: { appendChild() {} },
            createElement() { return { style: {}, play: () => Promise.resolve(), remove() {} }; }
        },
        Module: { _ek_cam_js_frame_ready(...args) { failures.push(args); } },
        requestAnimationFrame(callback) { const id = nextRaf++; rafs.set(id, callback); return id; },
        cancelAnimationFrame(id) { rafs.delete(id); }
    });
    vm.runInContext(read('src/emu/web/pages/js/boot.js'), context);
    const cameraSource = read('src/emu/drivers/src/camera/backend/web/camera_web.cpp');
    const cameraJS = cameraSource.match(/EM_JS\(void, ek_cam_js_init, \(\), \{([\s\S]*?)\n        \}\);/)[1];
    vm.runInContext('(function () {' + cameraJS + '\n})();', context);
    const api = window.EKA2L1;
    const camera = window.__ekCam;
    api.module = { ccall(name, ret, types, args) {
        calls.push({ name, args });
        return name === 'wasm_request_camera' ? camera.requestPermission() : 0;
    } };
    return { api, camera, requests, calls, failures, watches, cleared, values, events, rafs, context };
}
function stream() {
    const result = { stopped: 0 };
    result.getTracks = () => [{ stop() { result.stopped++; } }];
    return result;
}

test('default off blocks launch warmup, GPS and direct guest camera access', () => {
    const { api, camera, requests, watches, calls, failures, rafs } = setup();
    assert.equal(api.hostSensorsAllowed(), false);
    api.launchApp(1);
    api.launchMidlet(2);
    assert.equal(api.syncLocation(), 0);
    assert.equal(camera.requestPermission(), 0);
    camera.open(3, 0, 320, 240);
    camera.startRaf(3);
    assert.equal(requests.length, 0);
    assert.equal(watches.length, 0);
    assert.deepEqual(calls.map(c => c.name), ['wasm_launch_app', 'wasm_launch_midlet']);
    assert.deepEqual(failures, [[3, 0, 0]]);
    assert.equal(rafs.size, 0);
});

test('explicit opt-in permits requests; disabling clears watch zero and stops active camera', async () => {
    const { api, camera, requests, watches, cleared, calls, rafs } = setup();
    api.setHostSensorsAllowed(true);
    api.launchApp(1);
    api.launchApp(2);
    assert.equal(requests.length, 1); // pending warmup is not requested twice
    assert.equal(watches.length, 1);
    const warm = stream();
    requests[0].resolve(warm);
    await turn();
    camera.open(5, 0, 320, 240);
    camera.startRaf(5);
    assert.equal(warm.stopped, 1);
    const active = stream();
    requests[1].resolve(active);
    await turn();
    assert.equal(camera.cams[5].ready, 1);
    api.setHostSensorsAllowed(false);
    assert.equal(active.stopped, 1);
    assert.deepEqual(cleared, [0]);
    assert.equal(rafs.size, 0);
    watches[0].success({ coords: { latitude: 1, longitude: 2 } });
    assert.equal(calls.some(c => c.name === 'wasm_set_gps'), false);
    assert.equal(camera.requestPermission(), 0);
});

test('late camera promises cannot reactivate hardware after disabling and re-enabling', async () => {
    const { api, camera, requests } = setup();
    api.setHostSensorsAllowed(true);
    camera.requestPermission();
    camera.open(7, 0, 320, 240);
    api.setHostSensorsAllowed(false);
    api.setHostSensorsAllowed(true);
    const warm = stream(), capture = stream();
    requests[0].resolve(warm);
    requests[1].resolve(capture);
    await turn();
    assert.equal(warm.stopped, 1);
    assert.equal(capture.stopped, 1);
    assert.equal(camera.warmStream, null);
    assert.equal(camera.cams[7], undefined);
    camera.requestPermission();
    assert.equal(requests.length, 3);
});

test('closed camera handles reject stale streams and denied cameras notify guest failure', async () => {
    const { api, camera, requests, failures, rafs } = setup();
    api.setHostSensorsAllowed(true);
    camera.open(8, 0, 320, 240);
    camera.close(8);
    const closed = stream();
    requests[0].resolve(closed);
    await turn();
    assert.equal(closed.stopped, 1);
    camera.open(9, 0, 320, 240);
    camera.startRaf(9);
    requests[1].reject({ name: 'NotAllowedError' });
    await turn();
    assert.deepEqual(failures, [[9, 0, 0]]);
    assert.equal(rafs.size, 0);
});

test('preference changes in another tab stop active location and stale fixes stay discarded', () => {
    const { api, values, events, watches, cleared, calls } = setup();
    api.setHostSensorsAllowed(true);
    api.syncLocation();
    values.set('eka2l1_host_sensors', '0');
    events.storage({ key: 'eka2l1_host_sensors' });
    assert.deepEqual(cleared, [0]);
    api.setHostSensorsAllowed(true);
    api.syncLocation();
    watches[0].failure({ code: 1 });
    watches[0].success({ coords: { latitude: 1, longitude: 2 } });
    assert.equal(api._geoWatch, 1);
    assert.equal(calls.length, 0);
    watches[1].success({ coords: { latitude: 3, longitude: 4 } });
    assert.equal(calls[0].name, 'wasm_set_gps');
});

test('unavailable storage fails closed for browser and native camera wrappers', () => {
    const { api, camera, context, requests } = setup();
    context.localStorage.getItem = () => { throw new Error('blocked'); };
    assert.equal(api.requestCamera(), 0);
    assert.equal(api.syncLocation(), 0);
    camera.open(1, 0, 320, 240);
    assert.equal(requests.length, 0);
});
