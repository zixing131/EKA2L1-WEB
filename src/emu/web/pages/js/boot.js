/* ============================================================================
 * EKA2L1 Web — shared module bootstrap
 *
 * Loads the Emscripten module (eka2l1.js must be included before this file's
 * boot() is called), mounts the persistent IDBFS at /eka2l1, restores it from
 * IndexedDB, runs main(), and exposes a thin wrapper around the exported
 * C API for the two pages (index = library/installer, run = player).
 * ============================================================================ */

(function () {
    'use strict';

    var EKA2L1 = {
        module: null,
        ready: false
    };

    // ---- IDBFS patch -------------------------------------------------------
    // Stock IDBFS caches its IndexedDB connection in IDBFS.dbs[] and reuses it
    // across transactions. After a long wasm task the browser can move the
    // cached handle into "closing" state, making the next syncfs fail with
    // InvalidStateError. Always opening a fresh connection avoids that.
    function patchIDBFS(mod) {
        var IDBFS = mod.FS.filesystems && mod.FS.filesystems.IDBFS;
        if (!IDBFS) {
            console.warn('[EKA2L1] IDBFS not found, persistence disabled');
            return;
        }
        IDBFS.getDB = function (name, callback) {
            var req;
            try {
                req = IDBFS.indexedDB().open(name, IDBFS.DB_VERSION);
            } catch (e) {
                return callback(e);
            }
            req.onupgradeneeded = function (e) {
                var db = e.target.result;
                var tx = e.target.transaction;
                var store = db.objectStoreNames.contains(IDBFS.DB_STORE_NAME)
                    ? tx.objectStore(IDBFS.DB_STORE_NAME)
                    : db.createObjectStore(IDBFS.DB_STORE_NAME);
                if (!store.indexNames.contains('timestamp')) {
                    store.createIndex('timestamp', 'timestamp', { unique: false });
                }
            };
            req.onsuccess = function () {
                var db = req.result;
                db.onversionchange = function () { db.close(); };
                callback(null, db);
            };
            req.onerror = function (e) {
                callback(e.target.error);
                e.preventDefault();
            };
        };
        IDBFS.dbs = {};
    }

    // SDL's Emscripten port creates the WebGL context with default attributes
    // (default GPU, alpha-composited canvas). Wrap getContext on our canvas so
    // every WebGL request runs on the discrete/high-performance GPU and skips
    // features the emulator never uses (alpha compositing, antialiasing of a
    // plain textured quad) — cheaper compositing, smoother presentation.
    function boostWebGLContext(canvas) {
        var original = canvas.getContext.bind(canvas);
        canvas.getContext = function (type, attrs) {
            if (type === 'webgl' || type === 'webgl2' || type === 'experimental-webgl') {
                attrs = attrs || {};
                attrs.powerPreference = 'high-performance';
                attrs.alpha = false;
                attrs.antialias = false;
                attrs.desynchronized = true;
            }
            return original(type, attrs);
        };
    }

    /**
     * The wasm core is a pthreads build: it needs SharedArrayBuffer, which the
     * browser only exposes in a cross-origin-isolated *secure* context (https
     * or localhost + COOP/COEP headers). The classic "works on desktop, fails
     * on iPhone" case is opening the dev server from the phone over a LAN IP
     * (http://192.168.x.x:8080) — not a secure context, so SharedArrayBuffer is
     * undefined and the module aborts during instantiation. Detect this up front
     * and return a human-readable reason instead of a cryptic console error.
     * Returns a problem string, or null when the environment is fine.
     */
    EKA2L1.environmentProblem = function () {
        var crossIsolated = (typeof self !== 'undefined') && self.crossOriginIsolated;
        if (typeof SharedArrayBuffer === 'undefined' || !crossIsolated) {
            var ctx = (location.protocol === 'https:' || location.hostname === 'localhost' ||
                       location.hostname === '127.0.0.1');
            var hint = ctx
                ? '服务器缺少 COOP/COEP 响应头（serve.py 已带，换其它服务器要自行加）。'
                : '当前以 ' + location.protocol + '//' + location.hostname +
                  ' 打开，不是安全上下文。iPhone 请改用 https:// 访问（或本机 localhost），' +
                  '用「http://局域网IP」会导致多线程不可用。';
            return '无法启用多线程（SharedArrayBuffer 不可用），模拟器核心无法启动。\n' + hint;
        }
        return null;
    };

    /**
     * Boot the emulator core.
     * opts: { canvas: HTMLCanvasElement, onProgress(pct, text) }
     * Resolves once main() has run (logger, SDL, services ready).
     */
    EKA2L1.boot = function (opts) {
        var onProgress = opts.onProgress || function () {};

        return new Promise(function (resolve, reject) {
            if (typeof createEKA2L1Module === 'undefined') {
                reject(new Error('eka2l1.js 未加载'));
                return;
            }

            var envProblem = EKA2L1.environmentProblem();
            if (envProblem) {
                reject(new Error(envProblem));
                return;
            }

            boostWebGLContext(opts.canvas);

            onProgress(8, '加载 WebAssembly 模块…');

            var totalDeps = 0;
            createEKA2L1Module({
                canvas: opts.canvas,
                noInitialRun: true, // callMain() manually after IDBFS restore
                print: function (t) { console.log('[EKA2L1]', t); },
                printErr: function (t) { console.error('[EKA2L1]', t); },
                monitorRunDependencies: function (left) {
                    totalDeps = Math.max(totalDeps, left);
                    if (left && totalDeps) {
                        onProgress(8 + Math.round((1 - left / totalDeps) * 50), '加载资源…');
                    }
                }
            }).then(function (mod) {
                EKA2L1.module = mod;
                patchIDBFS(mod);

                try { mod.FS.mkdir('/eka2l1'); } catch (e) { /* exists */ }
                mod.FS.mount(mod.FS.filesystems.IDBFS, {}, '/eka2l1');

                onProgress(62, '恢复存档数据…');
                mod.FS.syncfs(true, function (err) {
                    if (err) console.warn('[EKA2L1] initial restore error (OK on first run):', err);

                    onProgress(82, '启动模拟器核心…');
                    // main() registers the RAF loop then throws "unwind" by
                    // design (simulate_infinite_loop); treat that as success.
                    try {
                        mod.callMain([]);
                    } catch (e) {
                        if (!(e === 'unwind' || (e && e.name === 'ExitStatus'))) {
                            reject(e);
                            return;
                        }
                    }

                    EKA2L1.ready = true;
                    onProgress(92, '核心已启动');
                    resolve(mod);
                });
            }).catch(reject);
        });
    };

    // ---- persistence -------------------------------------------------------

    var syncInFlight = false;

    /** Flush /eka2l1 (MEMFS) to IndexedDB. Returns a Promise. */
    EKA2L1.save = function () {
        return new Promise(function (resolve, reject) {
            var mod = EKA2L1.module;
            if (!mod || !mod.FS) { resolve(); return; }
            if (syncInFlight) { resolve(); return; }
            syncInFlight = true;
            var t0 = performance.now();
            mod.FS.syncfs(false, function (err) {
                syncInFlight = false;
                if (err) {
                    console.warn('[EKA2L1] syncfs failed:', err);
                    reject(err);
                } else {
                    console.log('[EKA2L1] saved to IndexedDB in ' +
                        (performance.now() - t0).toFixed(0) + 'ms');
                    resolve();
                }
            });
        });
    };

    // ---- C API wrappers ----------------------------------------------------

    function ccall(name, ret, argTypes, args) {
        return EKA2L1.module.ccall(name, ret, argTypes || [], args || []);
    }

    /**
     * Activate the installed device, or install one when rom/rpkg given.
     * Returns 0 on success; -3 when no device exists yet and no ROM given.
     */
    EKA2L1.initDevice = function (romPath, rpkgPath) {
        return ccall('wasm_init_with_rom', 'number', ['string', 'string'],
            [romPath || '', rpkgPath || '']);
    };

    EKA2L1.installPackage = function (vfsPath) {
        return ccall('wasm_install_package', 'number', ['string'], [vfsPath]);
    };

    EKA2L1.appList = function () {
        try {
            return JSON.parse(ccall('wasm_get_app_list', 'string')) || [];
        } catch (e) {
            return [];
        }
    };

    EKA2L1.launchApp = function (uid) {
        return ccall('wasm_launch_app', 'number', ['number'], [uid]);
    };

    /**
     * Decoded icon for an app, or null.
     * {type:'svg', data:<b64>} | {type:'rgba', w, h, data:<b64>}
     */
    EKA2L1.appIcon = function (uid) {
        try {
            return JSON.parse(ccall('wasm_get_app_icon', 'string', ['number'], [uid]));
        } catch (e) {
            return null;
        }
    };

    EKA2L1.setPaused = function (paused) {
        ccall('wasm_set_paused', null, ['number'], [paused ? 1 : 0]);
    };

    EKA2L1.setVolume = function (vol) {
        ccall('wasm_set_volume', null, ['number'], [vol | 0]);
    };

    EKA2L1.sendKey = function (scancode, pressed) {
        ccall('wasm_send_key', null, ['number', 'number'], [scancode, pressed ? 1 : 0]);
    };

    /** Rotate the presented screen clockwise by 0/90/180/270 degrees. */
    EKA2L1.setRotation = function (degrees) {
        ccall('wasm_set_screen_rotation', null, ['number'], [degrees | 0]);
    };

    EKA2L1.fps = function () {
        return ccall('wasm_get_fps', 'number');
    };

    EKA2L1.redrawCount = function () {
        return ccall('wasm_get_redraw_count', 'number');
    };

    /**
     * Print a full guest state dump (threads, PCs, wait objects, progress
     * counters) to the console. When something hangs, call this twice a few
     * seconds apart from devtools: `EKA2L1.debugDump()`.
     * If even this call never returns, the browser main thread itself is
     * stuck inside the wasm — report that, it pinpoints a different bug class.
     */
    EKA2L1.debugDump = function () {
        ccall('wasm_debug_dump', null, [], []);
    };

    /**
     * Write a File/Blob into the wasm VFS in 8MB slices. Streaming matters on
     * iOS Safari: loading a 100MB+ ROM as one ArrayBuffer (on top of the MEMFS
     * copy) pushed the tab over the memory limit and got it killed mid-install.
     * Resolves with the target path.
     */
    EKA2L1.writeFileToVFS = function (file, targetPath) {
        var FS = EKA2L1.module.FS;
        var CHUNK = 8 * 1024 * 1024;

        var dir = targetPath.substring(0, targetPath.lastIndexOf('/'));
        FS.mkdirTree(dir);

        var stream = FS.open(targetPath, 'w');
        var offset = 0;

        function writeNext() {
            if (offset >= file.size) {
                FS.close(stream);
                return Promise.resolve(targetPath);
            }
            var slice = file.slice(offset, Math.min(offset + CHUNK, file.size));
            return slice.arrayBuffer().then(function (buf) {
                var data = new Uint8Array(buf);
                FS.write(stream, data, 0, data.length, offset);
                offset += data.length;
                return writeNext();
            });
        }

        return writeNext().catch(function (err) {
            try { FS.close(stream); } catch (e) {}
            throw err;
        });
    };

    // ---- error decoding ----------------------------------------------------

    var installErrNames = [
        'none', '文件不存在', '空间不足', 'RPKG 损坏',
        '无法确定机型', '设备已存在', '一般错误',
        'ROM 复制失败', 'VPL 文件无效', 'ROFS 损坏',
        'ROM 文件损坏', 'FPSX 损坏'
    ];

    EKA2L1.decodeInstallError = function (result) {
        if (result === 0) return null;
        if (result <= -1000) {
            var idx = -(result + 1000);
            return installErrNames[idx] || ('错误 ' + idx);
        }
        if (result === -3) return '未找到 ROM 文件';
        if (result === -4) return '设备激活失败';
        if (result === -5) return 'ROM 复制失败';
        return '错误码 ' + result;
    };

    // ---- shared UI helpers -------------------------------------------------

    var toastTimer = null;
    EKA2L1.toast = function (text, ms) {
        var el = document.getElementById('toast');
        if (!el) return;
        el.textContent = text;
        el.classList.add('visible');
        if (toastTimer) clearTimeout(toastTimer);
        toastTimer = setTimeout(function () { el.classList.remove('visible'); }, ms || 2200);
    };

    // Symbian scancodes for the on-screen keypad (epoc::std_scan_code).
    EKA2L1.keys = {
        UP: 0x10, DOWN: 0x11, LEFT: 0x0e, RIGHT: 0x0f,
        OK: 0xa7, LSK: 0xa4, RSK: 0xa5,
        CALL: 0xc4, END: 0xc5, CLEAR: 0x01,
        STAR: 0x85, HASH: 0x7f,
        // digits use their ASCII codes
        D0: 0x30, D1: 0x31, D2: 0x32, D3: 0x33, D4: 0x34,
        D5: 0x35, D6: 0x36, D7: 0x37, D8: 0x38, D9: 0x39
    };

    window.EKA2L1 = EKA2L1;
})();
