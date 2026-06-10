/* ============================================================================
 * EKA2L1 Web — library page (install / storage / app list)
 *
 * The emulator core boots in the background purely for management duties
 * (installs, registry scan) and is kept paused — actual gameplay happens on
 * run.html, which boots its own instance with the canvas visible.
 * ============================================================================ */

(function () {
    'use strict';

    var APPS_CACHE_KEY = 'eka2l1_apps_cache';
    var HAS_DEVICE_KEY = 'eka2l1_has_device';

    var coreReady = false;
    var deviceReady = false;
    var hasDevice = false; // assigned in the boot section below
    var apps = [];

    // ---- tabs --------------------------------------------------------------

    var tabTitles = { games: '游戏', settings: '设置' };

    window.switchTab = function (name) {
        document.querySelectorAll('.tab-view').forEach(function (t) { t.classList.remove('active'); });
        document.querySelectorAll('.bottom-nav-item, .rail-item').forEach(function (b) {
            b.classList.toggle('active', b.dataset.tab === name);
        });
        document.getElementById('tab-' + name).classList.add('active');
        document.getElementById('topBarLargeTitle').textContent = tabTitles[name];
        document.getElementById('topBarSmallTitle').textContent = tabTitles[name];
        document.getElementById('fab').classList.toggle('hidden', name !== 'games');
        window.scrollTo({ top: 0 });
    };

    var scrolled = false;
    window.addEventListener('scroll', function () {
        var y = window.pageYOffset;
        var bar = document.getElementById('topAppBar');
        if (!scrolled && y > 24) { scrolled = true; bar.classList.add('scrolled'); }
        else if (scrolled && y < 4) { scrolled = false; bar.classList.remove('scrolled'); }
    }, { passive: true });

    // ---- status pill -------------------------------------------------------

    function setStatus(color, text) {
        document.getElementById('coreStatusDot').className = 'status-dot ' + (color || '');
        document.getElementById('coreStatusText').textContent = text;
    }

    function setDeviceStatusText(text) {
        document.getElementById('deviceStatusText').textContent = text;
    }

    // On-device visible error (iOS Safari has no console). Pass null to hide.
    function showError(msg) {
        var el = document.getElementById('bootError');
        if (!el) return;
        if (!msg) { el.style.display = 'none'; el.textContent = ''; return; }
        el.textContent = msg;
        el.style.display = '';
    }

    // ---- icons ----------------------------------------------------------------

    var ICONS_CACHE_KEY = 'eka2l1_icons_cache';
    var ICONS_CACHE_LIMIT = 3.5 * 1024 * 1024; // stay well under the LS quota

    var iconCache = {}; // uid -> dataURL ('' = known to have no icon)
    try { iconCache = JSON.parse(localStorage.getItem(ICONS_CACHE_KEY)) || {}; } catch (e) {}

    function persistIconCache() {
        try {
            var json = JSON.stringify(iconCache);
            if (json.length <= ICONS_CACHE_LIMIT) localStorage.setItem(ICONS_CACHE_KEY, json);
        } catch (e) { /* quota — keep in-memory only */ }
    }

    // {type:'svg'|'rgba',...} -> data URL the <img> tag can show
    function iconToDataURL(icon) {
        if (!icon || !icon.type) return null;
        if (icon.type === 'svg') {
            return 'data:image/svg+xml;base64,' + icon.data;
        }
        if (icon.type === 'rgba' && icon.w > 0 && icon.h > 0) {
            try {
                var bin = atob(icon.data);
                var px = new Uint8ClampedArray(bin.length);
                for (var i = 0; i < bin.length; i++) px[i] = bin.charCodeAt(i);
                var cnv = document.createElement('canvas');
                cnv.width = icon.w;
                cnv.height = icon.h;
                cnv.getContext('2d').putImageData(new ImageData(px, icon.w, icon.h), 0, 0);
                return cnv.toDataURL('image/png');
            } catch (e) {
                console.warn('[EKA2L1] icon decode failed:', e);
            }
        }
        return null;
    }

    function applyIcon(avatarEl, dataURL) {
        avatarEl.classList.add('has-icon');
        avatarEl.textContent = '';
        var img = document.createElement('img');
        img.className = 'game-icon-img';
        img.src = dataURL;
        img.alt = '';
        avatarEl.appendChild(img);
    }

    var iconPumpRunning = false;

    // Fetch missing icons one at a time so the wasm calls never jank the UI.
    function pumpIcons() {
        if (iconPumpRunning || !coreReady || !deviceReady) return;
        var pending = apps.filter(function (a) { return !(a.uid in iconCache); });
        if (pending.length === 0) return;
        iconPumpRunning = true;

        var idx = 0;
        (function step() {
            if (idx >= pending.length) {
                iconPumpRunning = false;
                persistIconCache();
                return;
            }
            var app = pending[idx++];
            var url = null;
            try { url = iconToDataURL(EKA2L1.appIcon(app.uid)); } catch (e) {}
            iconCache[app.uid] = url || '';

            if (url) {
                var avatarEl = document.querySelector('.game-avatar[data-icon-uid="' + app.uid + '"]');
                if (avatarEl) applyIcon(avatarEl, url);
            }
            setTimeout(step, 16);
        })();
    }

    // ---- game list ---------------------------------------------------------

    var VIEW_KEY = 'eka2l1_view';
    var viewMode = localStorage.getItem(VIEW_KEY) || 'grid';

    function applyViewIcon() {
        document.getElementById('viewIconGrid').style.display = (viewMode === 'list') ? '' : 'none';
        document.getElementById('viewIconList').style.display = (viewMode === 'grid') ? '' : 'none';
    }

    window.toggleView = function () {
        viewMode = (viewMode === 'grid') ? 'list' : 'grid';
        localStorage.setItem(VIEW_KEY, viewMode);
        applyViewIcon();
        renderGameList();
    };

    var avatarPalette = ['#e94560', '#0a84ff', '#34c759', '#ff9f0a', '#bf5af2',
        '#5e5ce6', '#ff375f', '#64d2ff', '#30d158', '#ffd60a'];

    function avatarColor(name) {
        var h = 0;
        for (var i = 0; i < name.length; i++) h = ((h << 5) - h + name.charCodeAt(i)) | 0;
        return avatarPalette[Math.abs(h) % avatarPalette.length];
    }

    window.renderGameList = function () {
        var listEl = document.getElementById('gameList');
        var filter = (document.getElementById('gameSearch').value || '').toLowerCase();
        var shown = apps.filter(function (a) {
            return !filter || (a.name || '').toLowerCase().indexOf(filter) !== -1;
        });

        listEl.className = (viewMode === 'grid') ? 'game-grid' : 'game-list';
        listEl.innerHTML = '';

        if (shown.length === 0) {
            listEl.className = 'game-list';
            var empty = document.createElement('div');
            empty.className = 'empty-state';
            empty.innerHTML = apps.length === 0
                ? '<span class="big">🎮</span>暂无已安装的游戏。<br>点击右下角 + 安装 SIS 游戏包。'
                : '没有匹配「' + filter + '」的游戏';
            listEl.appendChild(empty);
            return;
        }

        shown.forEach(function (app) {
            var name = app.name || ('App 0x' + (app.uid >>> 0).toString(16));
            var item = document.createElement('div');
            item.className = 'game-item';

            var avatar = document.createElement('div');
            avatar.className = 'game-avatar';
            avatar.dataset.iconUid = app.uid;
            if (iconCache[app.uid]) {
                applyIcon(avatar, iconCache[app.uid]);
            } else {
                avatar.style.background = avatarColor(name);
                avatar.textContent = name.charAt(0).toUpperCase();
            }

            var info = document.createElement('div');
            info.className = 'game-item-info';
            var nm = document.createElement('div');
            nm.className = 'game-item-name';
            nm.textContent = name;
            var sub = document.createElement('div');
            sub.className = 'game-item-sub';
            sub.textContent = 'UID 0x' + (app.uid >>> 0).toString(16).toUpperCase();
            info.appendChild(nm);
            info.appendChild(sub);

            var go = document.createElement('div');
            go.className = 'game-item-go';
            go.innerHTML = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><polyline points="9 18 15 12 9 6"/></svg>';

            item.appendChild(avatar);
            item.appendChild(info);
            item.appendChild(go);
            item.addEventListener('click', function () { playApp(app); });
            listEl.appendChild(item);
        });

        pumpIcons();
    };

    function playApp(app) {
        location.href = 'run.html?uid=' + app.uid + '&name=' + encodeURIComponent(app.name || '');
    }

    function refreshApps() {
        if (!coreReady || !deviceReady) return;
        apps = EKA2L1.appList();
        apps.sort(function (a, b) { return (a.name || '').localeCompare(b.name || '', 'zh'); });
        try { localStorage.setItem(APPS_CACHE_KEY, JSON.stringify(apps)); } catch (e) {}
        renderGameList();
    }

    // Render instantly from the last session's cache while the core boots.
    try {
        apps = JSON.parse(localStorage.getItem(APPS_CACHE_KEY)) || [];
    } catch (e) { apps = []; }
    applyViewIcon();
    renderGameList();

    // ---- sheets ------------------------------------------------------------

    window.openInstallSheet = function () {
        document.getElementById('installSheetOverlay').classList.add('visible');
    };

    window.openDeviceSheet = function () {
        document.getElementById('deviceSheetOverlay').classList.add('visible');
    };

    window.closeSheets = function () {
        document.querySelectorAll('.sheet-overlay').forEach(function (s) {
            s.classList.remove('visible');
        });
    };

    // ---- device (ROM/RPKG) install ------------------------------------------

    var romFile = null;
    var rpkgFile = null;

    document.getElementById('romInput').addEventListener('change', function () {
        romFile = this.files[0] || null;
        var zone = document.getElementById('romZone');
        zone.textContent = romFile ? '✓ ' + romFile.name : '📁 点击选择 ROM 文件';
        zone.classList.toggle('picked', !!romFile);
        // Not gated on coreReady: on a fresh visit the core boots lazily
        // inside installDevice(), after the files are already picked.
        document.getElementById('deviceInstallBtn').disabled = !romFile;
    });

    document.getElementById('rpkgInput').addEventListener('change', function () {
        rpkgFile = this.files[0] || null;
        var zone = document.getElementById('rpkgZone');
        zone.textContent = rpkgFile ? '✓ ' + rpkgFile.name : '📦 点击选择 RPKG 文件（可选）';
        zone.classList.toggle('picked', !!rpkgFile);
    });

    // Breadcrumb for silent deaths: iOS jetsam reloads the page without any
    // error event, so persist the current install stage and report it on the
    // next load. Cleared on success and on handled (on-screen) errors.
    var STAGE_KEY = 'eka2l1_install_stage';

    function setInstallStage(stage) {
        try {
            if (stage === null) localStorage.removeItem(STAGE_KEY);
            else localStorage.setItem(STAGE_KEY, stage);
        } catch (e) {}
    }

    window.installDevice = function () {
        if (!romFile) return;

        var btn = document.getElementById('deviceInstallBtn');
        btn.disabled = true;
        btn.textContent = '正在安装…';

        var romPath = '/eka2l1/rom_upload.rom';
        var rpkgPath = '/eka2l1/rpkg_upload.rpkg';

        // Boot the core only now (fresh visits don't boot at page load): the
        // file picking is already done, so the high-memory phase no longer
        // overlaps with the iOS Files picker backgrounding the tab.
        setInstallStage('启动核心');
        ensureCore()
            .then(function () {
                if (rpkgFile && EKA2L1.canStreamRpkg()) {
                    // EKA2 streaming path: the RPKG is parsed/extracted as the
                    // chunks arrive and never lands in MEMFS, and the ROM goes
                    // straight to its final location — no temp copies at all.
                    // This cuts the iOS install peak by the full package size.
                    setInstallStage('流式安装 RPKG');
                    return EKA2L1.streamInstallRpkg(rpkgFile, function (done, total) {
                        var pct = Math.floor(done * 100 / total);
                        setStatus('yellow', '安装 RPKG ' + pct + '%…');
                        setInstallStage('流式安装 RPKG ' + pct + '%');
                    }).then(function (romTarget) {
                        setStatus('yellow', '写入 ROM…');
                        setInstallStage('写入 ROM');
                        return EKA2L1.writeFileToVFS(romFile, romTarget);
                    }).then(function () {
                        setStatus('yellow', '激活设备…');
                        setInstallStage('激活设备');
                        var result = EKA2L1.initDevice('', '');
                        if (result !== 0) {
                            throw new Error('安装失败：' + EKA2L1.decodeInstallError(result));
                        }
                    });
                }

                // Classic path (EKA1 ROM-only, or streaming unavailable):
                // stage the uploads in MEMFS, then install.
                setStatus('yellow', '写入 ROM…');
                setInstallStage('写入 ROM');
                return EKA2L1.writeFileToVFS(romFile, romPath)
                    .then(function () {
                        if (!rpkgFile) return null;
                        setStatus('yellow', '写入 RPKG…');
                        setInstallStage('写入 RPKG');
                        return EKA2L1.writeFileToVFS(rpkgFile, rpkgPath);
                    })
                    .then(function () {
                        // Heavy synchronous call; yield a frame so the UI paints first.
                        return new Promise(function (resolve) { setTimeout(resolve, 60); });
                    })
                    .then(function () {
                        setStatus('yellow', '解压安装…');
                        setInstallStage('解压安装');
                        var result = EKA2L1.initDevice(romPath, rpkgFile ? rpkgPath : '');
                        if (result !== 0) {
                            throw new Error('安装失败：' + EKA2L1.decodeInstallError(result));
                        }
                        // The installer copied the ROM into roms/<firmcode>/; drop
                        // the uploads so they don't bloat IndexedDB on every sync.
                        try { EKA2L1.module.FS.unlink(romPath); } catch (e) {}
                        try { EKA2L1.module.FS.unlink(rpkgPath); } catch (e) {}
                    });
            })
            .then(function () {
                deviceReady = true;
                setStatus('yellow', '保存到浏览器…');
                setInstallStage('保存到浏览器');
                // Persist FIRST: if the browser kills the tab during the
                // post-install work (iOS memory pressure), the device must
                // already be safe in IndexedDB or it comes back half-broken.
                return EKA2L1.save();
            })
            .then(function () {
                setInstallStage(null);
                try { localStorage.setItem(HAS_DEVICE_KEY, '1'); } catch (e) {}
                refreshApps();
                EKA2L1.setPaused(true);
                showError(null);
                setStatus('green', '设备就绪');
                setDeviceStatusText('已安装，数据已持久化');
                document.getElementById('onboardCard').style.display = 'none';
                closeSheets();
                EKA2L1.toast('设备固件安装完成');
            })
            .catch(function (err) {
                setInstallStage(null); // surfaced on screen, breadcrumb not needed
                console.error('[EKA2L1] device install failed:', err);
                setStatus('red', '安装失败');
                showError('设备固件安装失败：\n' + (err.message || err));
                EKA2L1.toast(err.message || '安装失败，详见上方提示', 4000);
            })
            .then(function () {
                btn.disabled = !romFile;
                btn.textContent = '开始安装';
            });
    };

    // ---- SIS install ---------------------------------------------------------

    window.pickSis = function () {
        if (!coreReady) {
            if (!hasDevice) { EKA2L1.toast('请先安装设备 ROM'); openDeviceSheet(); return; }
            EKA2L1.toast('核心还在启动中，请稍候');
            return;
        }
        if (!deviceReady) { EKA2L1.toast('请先安装设备 ROM'); openDeviceSheet(); return; }
        document.getElementById('sisInput').click();
    };

    document.getElementById('sisInput').addEventListener('change', function () {
        var file = this.files[0];
        this.value = '';
        if (!file) return;

        setStatus('yellow', '安装 ' + file.name + '…');
        var target = '/eka2l1/temp/' + file.name;

        EKA2L1.writeFileToVFS(file, target)
            .then(function () {
                return new Promise(function (resolve) { setTimeout(resolve, 60); });
            })
            .then(function () {
                var result = EKA2L1.installPackage(target);
                try { EKA2L1.module.FS.unlink(target); } catch (e) {}
                if (result !== 0) throw new Error('安装失败（代码 ' + result + '）');
                refreshApps();
                return EKA2L1.save();
            })
            .then(function () {
                setStatus('green', '就绪');
                EKA2L1.toast('已安装 ' + file.name);
            })
            .catch(function (err) {
                console.error('[EKA2L1] SIS install failed:', err);
                setStatus('red', '安装失败');
                showError('SIS 安装失败：\n' + (err.message || err));
                EKA2L1.toast(err.message || '安装失败，详见上方提示', 4000);
            });
    });

    // ---- settings ------------------------------------------------------------

    var prefScale = document.getElementById('prefScale');
    var prefKeypad = document.getElementById('prefKeypad');
    prefScale.value = localStorage.getItem('eka2l1_scale') || 'fit';
    prefKeypad.value = localStorage.getItem('eka2l1_keypad') || 'auto';
    prefScale.addEventListener('change', function () { localStorage.setItem('eka2l1_scale', this.value); });
    prefKeypad.addEventListener('change', function () { localStorage.setItem('eka2l1_keypad', this.value); });

    window.manualSave = function () {
        if (!coreReady) { EKA2L1.toast('核心未就绪'); return; }
        EKA2L1.save().then(function () { EKA2L1.toast('已保存'); })
            .catch(function () { EKA2L1.toast('保存失败，详见控制台', 3500); });
    };

    window.wipeAll = function () {
        if (!confirm('确定要清除全部数据吗？\n设备固件、已装游戏和所有存档都会被删除，且无法恢复。')) return;
        try {
            localStorage.removeItem(APPS_CACHE_KEY);
            localStorage.removeItem(ICONS_CACHE_KEY);
            localStorage.removeItem(HAS_DEVICE_KEY);
        } catch (e) {}
        var req = indexedDB.deleteDatabase('/eka2l1');
        req.onsuccess = req.onerror = req.onblocked = function () { location.reload(); };
    };

    // ---- boot ----------------------------------------------------------------

    var progressEl = document.getElementById('bootProgress');
    var progressBar = document.getElementById('bootProgressBar');

    function onProgress(pct, text) {
        progressBar.style.width = pct + '%';
        if (text) setStatus('yellow', text);
    }

    var bootPromise = null;

    /**
     * Boot the core on demand (idempotent). iOS Safari evicts high-memory
     * tabs while the Files document picker has the page backgrounded — with
     * the core booted that was enough to get the tab killed and reloaded
     * ("立即刷新"), losing the picked files. So on a fresh visit nothing is
     * booted until the user actually taps install; only when a device is
     * known to exist does the page boot at load (needed for the app list).
     */
    function ensureCore() {
        if (bootPromise) return bootPromise;
        bootPromise = EKA2L1.boot({
            canvas: document.getElementById('canvas'),
            onProgress: onProgress
        }).then(function () {
            coreReady = true;
            progressBar.style.width = '100%';
            progressEl.classList.add('done');

            // Activate the persisted device if one exists (no install).
            var result = EKA2L1.initDevice('', '');
            if (result === 0) {
                deviceReady = true;
                try { localStorage.setItem(HAS_DEVICE_KEY, '1'); } catch (e) {}
                setStatus('green', '就绪');
                setDeviceStatusText('已安装，数据已持久化');
                refreshApps();
            } else {
                setStatus('yellow', '未安装设备');
                setDeviceStatusText('未安装 — 需要 ROM（可选 RPKG）');
                document.getElementById('onboardCard').style.display = '';
            }

            // The library page never runs guest code: keep the core paused so
            // the RAF loop stays cheap while the user browses.
            EKA2L1.setPaused(true);
        });
        bootPromise.catch(function (err) {
            bootPromise = null; // allow a retry on the next install attempt
            console.error('[EKA2L1] boot failed:', err);
            setStatus('red', '启动失败');
            showError('模拟器核心启动失败：\n' + (err && err.message ? err.message : err));
        });
        return bootPromise;
    }

    // A leftover stage marker means the last install died silently (iOS
    // jetsam kills + reloads the page with no error event). Tell the user
    // exactly where it stopped — that is otherwise invisible on iPhone.
    try {
        var lastStage = localStorage.getItem(STAGE_KEY);
        if (lastStage) {
            localStorage.removeItem(STAGE_KEY);
            showError('上次安装在「' + lastStage + '」阶段中断（页面被系统终止后自动刷新）。\n' +
                'iPhone 上多为内存不足：请先关闭其它标签页和后台 App 再重试，' +
                '安装期间保持本页在前台、不要锁屏。若反复中断在同一阶段，请截图反馈本提示。');
        }
    } catch (e) {}

    try {
        // The explicit flag is written on install/activation; the app cache
        // doubles as evidence for devices installed before the flag existed.
        hasDevice = (localStorage.getItem(HAS_DEVICE_KEY) === '1') || (apps.length > 0);
    } catch (e) {}

    if (hasDevice) {
        ensureCore();
    } else {
        // Fresh visit: stay near-zero-footprint until an install starts.
        setStatus('yellow', '未安装设备');
        setDeviceStatusText('未安装 — 需要 ROM（可选 RPKG）');
        document.getElementById('onboardCard').style.display = '';
    }
})();
