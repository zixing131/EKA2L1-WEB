/* ============================================================================
 * EKA2L1 Web — player page
 *
 * Boots the core with the visible canvas, activates the persisted device,
 * launches the app given by ?uid=, and provides pause/save/keypad controls.
 * ============================================================================ */

(function () {
    'use strict';

    var params = new URLSearchParams(location.search);
    var appUid = parseInt(params.get('uid'), 10);
    var appName = params.get('name') || '';

    var paused = false;
    var started = false;     // first emulated frame presented
    var autosaveTimer = null;

    document.getElementById('playerTitle').textContent = appName || 'EKA2L1';
    document.title = (appName ? appName + ' — ' : '') + 'EKA2L1';

    // ---- overlay -----------------------------------------------------------

    function overlay(title, text, pct) {
        document.getElementById('overlayTitle').textContent = title;
        document.getElementById('overlayText').textContent = text || '';
        if (typeof pct === 'number') {
            document.getElementById('overlayBar').style.width = pct + '%';
        }
    }

    function overlayError(title, text) {
        overlay(title, text);
        document.getElementById('overlaySpinner').style.display = 'none';
        document.querySelector('.overlay-progress').style.display = 'none';
        document.getElementById('overlayActions').style.display = '';
    }

    function hideOverlay() {
        document.getElementById('bootOverlay').classList.add('hidden');
    }

    // ---- top bar actions -----------------------------------------------------

    window.goBack = function () {
        // Flush saves before leaving; navigate regardless of the outcome.
        var go = function () { location.href = 'index.html'; };
        if (EKA2L1.ready) {
            EKA2L1.setPaused(true);
            EKA2L1.save().then(go, go);
            setTimeout(go, 2500); // failsafe if syncfs never calls back
        } else {
            go();
        }
    };

    window.togglePause = function () {
        if (!EKA2L1.ready) return;
        paused = !paused;
        EKA2L1.setPaused(paused);
        document.getElementById('iconPause').style.display = paused ? 'none' : '';
        document.getElementById('iconPlay').style.display = paused ? '' : 'none';
        EKA2L1.toast(paused ? '已暂停' : '继续运行');
        if (!paused) focusCanvas();
    };

    window.manualSave = function () {
        if (!EKA2L1.ready) return;
        EKA2L1.save().then(function () { EKA2L1.toast('已保存'); },
            function () { EKA2L1.toast('保存失败，详见控制台', 3500); });
    };

    window.toggleFullscreen = function () {
        var stage = document.getElementById('stage');
        if (document.fullscreenElement) document.exitFullscreen();
        else if (stage.requestFullscreen) stage.requestFullscreen();
    };

    // ---- keypad --------------------------------------------------------------

    var keypadEl = document.getElementById('keypad');

    function keypadDefaultVisible() {
        var pref = localStorage.getItem('eka2l1_keypad') || 'auto';
        if (pref === 'on') return true;
        if (pref === 'off') return false;
        return ('ontouchstart' in window) || navigator.maxTouchPoints > 0;
    }

    window.toggleKeypad = function () {
        keypadEl.classList.toggle('hidden');
        fitCanvas();
    };

    keypadEl.classList.toggle('hidden', !keypadDefaultVisible());

    // Pointer events so touch and mouse share one path; pointer capture keeps
    // the release reliable even when the finger slides off the button.
    keypadEl.querySelectorAll('.key').forEach(function (btn) {
        var code = EKA2L1.keys[btn.dataset.key];
        if (!code) return;

        var down = false;

        btn.addEventListener('pointerdown', function (e) {
            e.preventDefault();
            btn.setPointerCapture(e.pointerId);
            if (down || !EKA2L1.ready) return;
            down = true;
            btn.classList.add('pressed');
            EKA2L1.sendKey(code, 1);
            if (navigator.vibrate) navigator.vibrate(8);
        });

        var release = function (e) {
            if (!down) return;
            down = false;
            btn.classList.remove('pressed');
            if (EKA2L1.ready) EKA2L1.sendKey(code, 0);
        };

        btn.addEventListener('pointerup', release);
        btn.addEventListener('pointercancel', release);
        // No click handler: everything goes through pointer events.
        btn.addEventListener('contextmenu', function (e) { e.preventDefault(); });
    });

    // ---- canvas sizing ---------------------------------------------------------

    var canvas = document.getElementById('canvas');
    var BASE_W = 360, BASE_H = 640; // initial SDL window size owned by the emulator

    function fitCanvas() {
        var scalePref = localStorage.getItem('eka2l1_scale') || 'fit';
        var stage = document.getElementById('stage');
        var sw = stage.clientWidth, sh = stage.clientHeight;

        // The emulator resizes its drawing buffer on screen rotation; follow
        // the live canvas dimensions instead of the boot-time constants.
        var bw = canvas.width || BASE_W;
        var bh = canvas.height || BASE_H;
        var w, h;

        if (scalePref === 'fit') {
            var s = Math.min(sw / bw, sh / bh);
            w = Math.floor(bw * s);
            h = Math.floor(bh * s);
        } else {
            var mult = parseFloat(scalePref) || 1;
            w = bw * mult;
            h = bh * mult;
        }

        canvas.style.width = w + 'px';
        canvas.style.height = h + 'px';
    }

    window.addEventListener('resize', fitCanvas);

    function focusCanvas() {
        canvas.focus();
    }

    canvas.addEventListener('click', focusCanvas);

    // ---- screen rotation -------------------------------------------------------

    var rotKey = 'eka2l1_rot_' + appUid;
    var rotation = parseInt(localStorage.getItem(rotKey), 10) || 0;

    function applyRotation() {
        if (!EKA2L1.ready || !started) return;
        EKA2L1.setRotation(rotation);
        // SDL resizes the canvas synchronously; refit on the next tick.
        setTimeout(fitCanvas, 0);
        focusCanvas();
    }

    window.rotateScreen = function (delta) {
        if (!started) return;
        rotation = ((rotation + delta) % 360 + 360) % 360;
        localStorage.setItem(rotKey, String(rotation));
        applyRotation();
        EKA2L1.toast(rotation ? ('屏幕已旋转 ' + rotation + '°') : '屏幕已回正');
    };

    // ---- FPS ---------------------------------------------------------------

    setInterval(function () {
        if (!EKA2L1.ready || !started) return;
        document.getElementById('fpsLabel').textContent = EKA2L1.fps() + ' FPS';
    }, 1000);

    // ---- autosave ------------------------------------------------------------

    function startAutosave() {
        if (autosaveTimer) return;
        autosaveTimer = setInterval(function () {
            if (EKA2L1.ready && !paused) {
                EKA2L1.save().catch(function () {});
            }
        }, 60000);
    }

    document.addEventListener('visibilitychange', function () {
        if (document.visibilityState === 'hidden' && EKA2L1.ready) {
            EKA2L1.save().catch(function () {});
        }
    });

    // ---- boot & launch ---------------------------------------------------------

    if (!appUid || isNaN(appUid)) {
        overlayError('缺少应用参数', '请从游戏库选择一个游戏启动。');
        return;
    }

    overlay('正在加载模拟器…', appName, 5);
    fitCanvas();

    EKA2L1.boot({
        canvas: canvas,
        onProgress: function (pct, text) { overlay('正在加载模拟器…', text, pct); }
    }).then(function () {
        overlay('正在启动设备…', '', 94);

        var result = EKA2L1.initDevice('', '');
        if (result !== 0) {
            overlayError('未找到设备固件', '请先回到游戏库安装 ROM（错误：' +
                EKA2L1.decodeInstallError(result) + '）');
            return;
        }

        overlay('正在启动 ' + (appName || '应用') + '…', 'Symbian 系统引导中，首次启动较慢', 97);

        var launch = EKA2L1.launchApp(appUid);
        if (launch !== 0) {
            overlayError('启动失败', '应用未能启动（代码 ' + launch + '），可能已被卸载。');
            return;
        }

        EKA2L1.setPaused(false);

        // First composed frame -> reveal the canvas.
        var t0 = performance.now();
        var poll = setInterval(function () {
            if (EKA2L1.redrawCount() > 0) {
                clearInterval(poll);
                started = true;
                hideOverlay();
                focusCanvas();
                fitCanvas();
                if (rotation) applyRotation();
                startAutosave();
            } else if (performance.now() - t0 > 120000) {
                clearInterval(poll);
                overlayError('启动超时', '应用 2 分钟内没有输出画面，详见控制台日志。');
            } else {
                var secs = ((performance.now() - t0) / 1000) | 0;
                if (secs >= 3) {
                    overlay('正在启动 ' + (appName || '应用') + '…',
                        'Symbian 系统引导中（' + secs + 's），首次启动较慢', 99);
                }
            }
        }, 250);
    }).catch(function (err) {
        console.error('[EKA2L1] boot failed:', err);
        overlayError('模拟器加载失败', String(err && err.message || err));
    });
})();
