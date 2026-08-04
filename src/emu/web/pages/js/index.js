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

    function tabTitle(name) {
        return name === 'settings' ? EKA2L1.t('nav.settings') : EKA2L1.t('nav.games');
    }

    window.switchTab = function (name) {
        document.querySelectorAll('.tab-view').forEach(function (t) { t.classList.remove('active'); });
        document.querySelectorAll('.bottom-nav-item, .rail-item').forEach(function (b) {
            b.classList.toggle('active', b.dataset.tab === name);
        });
        document.getElementById('tab-' + name).classList.add('active');
        document.getElementById('topBarLargeTitle').textContent = tabTitle(name);
        document.getElementById('topBarSmallTitle').textContent = tabTitle(name);
        document.getElementById('fab').classList.toggle('hidden', name !== 'games');
        // Reset scroll position AND large-title state synchronously so the
        // top bar never shows the wrong (scrolled) appearance after switching.
        window.scrollTo({ top: 0 });
        if (scrolled) {
            scrolled = false;
            document.getElementById('topAppBar').classList.remove('scrolled');
        }
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

    var ICONS_CACHE_KEY = 'eka2l1_icons_cache_v2'; // v2: SVG namespace fix
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

    // miuix/HyperOS-ish avatar palette (blue-led, saturated but soft)
    var avatarPalette = ['#3482ff', '#36d167', '#ff9f0a', '#bf5af2', '#f23030',
        '#5e5ce6', '#00b8c9', '#64d2ff', '#f5c400', '#ff6d3f'];

    function avatarColor(name) {
        var h = 0;
        for (var i = 0; i < name.length; i++) h = ((h << 5) - h + name.charCodeAt(i)) | 0;
        return avatarPalette[Math.abs(h) % avatarPalette.length];
    }

    // Debounced search: rebuilding the whole list DOM on every keystroke
    // janks low-end phones once the library grows; 120ms coalesces bursts.
    var searchDebounce = null;
    window.onGameSearchInput = function () {
        if (searchDebounce) clearTimeout(searchDebounce);
        searchDebounce = setTimeout(function () {
            searchDebounce = null;
            renderGameList();
        }, 120);
    };

    window.renderGameList = function () {
        var listEl = document.getElementById('gameList');
        var filter = (document.getElementById('gameSearch').value || '').toLowerCase();
        var showSys = localStorage.getItem('eka2l1_show_sys') === '1';
        var shown = apps.filter(function (a) {
            if (!showSys && a.sys) return false;
            return !filter || (a.name || '').toLowerCase().indexOf(filter) !== -1;
        });

        listEl.className = (viewMode === 'grid') ? 'game-grid' : 'game-list';
        listEl.innerHTML = '';

        if (shown.length === 0) {
            listEl.className = 'game-list';
            var empty = document.createElement('div');
            empty.className = 'empty-state';
            empty.innerHTML = apps.length === 0
                ? EKA2L1.t('games.empty.none')
                : (filter
                    ? EKA2L1.t('games.empty.noMatch', { query: filter })
                    : EKA2L1.t('games.empty.hiddenSys'));
            listEl.appendChild(empty);
            return;
        }

        // Batch into a fragment: one DOM insertion = one reflow instead of
        // one per app (matters on weak devices with large libraries).
        var frag = document.createDocumentFragment();

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
            sub.textContent = EKA2L1.t('games.item.uid', { uid: '0x' + (app.uid >>> 0).toString(16).toUpperCase() });
            info.appendChild(nm);
            info.appendChild(sub);

            var go = document.createElement('div');
            go.className = 'game-item-go';
            go.innerHTML = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><polyline points="9 18 15 12 9 6"/></svg>';

            item.appendChild(avatar);
            item.appendChild(info);
            item.appendChild(go);
            item.addEventListener('click', function () { playApp(app); });
            frag.appendChild(item);
        });

        listEl.appendChild(frag);
        pumpIcons();
    };

    function playApp(app) {
        location.href = 'run.html?uid=' + app.uid + '&name=' + encodeURIComponent(app.name || '');
    }

    function refreshApps() {
        if (!coreReady || !deviceReady) return;
        apps = EKA2L1.appList();
        var collationLocale = EKA2L1.getLocale ? EKA2L1.getLocale() : 'zh-CN';
        apps.sort(function (a, b) { return (a.name || '').localeCompare(b.name || '', collationLocale); });
        try { localStorage.setItem(APPS_CACHE_KEY, JSON.stringify(apps)); } catch (e) {}
        renderGameList();
        if (window.renderDeviceList) renderDeviceList();
        if (window.renderPkgList) renderPkgList();
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
        zone.textContent = romFile ? EKA2L1.t('dropzone.picked', { name: romFile.name }) : EKA2L1.t('dropzone.rom');
        zone.classList.toggle('picked', !!romFile);
        // Not gated on coreReady: on a fresh visit the core boots lazily
        // inside installDevice(), after the files are already picked.
        document.getElementById('deviceInstallBtn').disabled = !romFile;
    });

    document.getElementById('rpkgInput').addEventListener('change', function () {
        rpkgFile = this.files[0] || null;
        var zone = document.getElementById('rpkgZone');
        zone.textContent = rpkgFile ? EKA2L1.t('dropzone.picked', { name: rpkgFile.name }) : EKA2L1.t('dropzone.rpkg');
        zone.classList.toggle('picked', !!rpkgFile);
    });

    // Breadcrumb for silent deaths: iOS jetsam reloads the page without any
    // error event, so persist the current install stage and report it on the
    // next load. Cleared on success and on handled (on-screen) errors.
    var STAGE_KEY = 'eka2l1_install_stage';
    var INFO_KEY = 'eka2l1_install_info'; // file sizes/quota: shown next to the stage

    function setInstallStage(stage) {
        try {
            if (stage === null) localStorage.removeItem(STAGE_KEY);
            else localStorage.setItem(STAGE_KEY, stage);
        } catch (e) {}
    }

    function appendInstallInfo(extra) {
        try {
            var cur = localStorage.getItem(INFO_KEY) || '';
            localStorage.setItem(INFO_KEY, cur ? (cur + '；' + extra) : extra);
        } catch (e) {}
    }

    // iOS Safari's private browsing keeps IndexedDB in MEMORY: installing a
    // firmware there both fails to persist and doubles the RAM bill (which is
    // exactly the "保存到浏览器" jetsam signature, even on 12GB devices). The
    // quota estimate exposes it — private windows report a tiny quota.
    function checkStorageHealth() {
        if (!navigator.storage || !navigator.storage.estimate) {
            return Promise.resolve(null);
        }
        return navigator.storage.estimate().then(function (est) {
            var quotaMB = Math.floor((est.quota || 0) / 1048576);
            if (quotaMB && quotaMB < 300) {
                return EKA2L1.t('error.privateBrowsing', { quota: quotaMB });
            }
            return null;
        }).catch(function () { return null; });
    }

    // Batched IndexedDB persist with stage markers. Replaces FS.syncfs, whose
    // single whole-mount transaction is itself enough to get iOS tabs killed.
    function stagedSaveWithMarkers() {
        setStatus('yellow', EKA2L1.t('status.savingToBrowser'));
        setInstallStage(EKA2L1.t('stage.save.prepare'));
        return EKA2L1.saveInitialStaged(function (done, total, bytesDone, bytesTotal) {
            var pct = total ? Math.floor(done * 100 / total) : 0;
            setStatus('yellow', EKA2L1.t('status.savingToBrowserPct', { pct: pct }));
            setInstallStage(EKA2L1.t('stage.save.batch', {
                done: done, total: total,
                doneMB: Math.floor(bytesDone / 1048576), totalMB: Math.floor(bytesTotal / 1048576)
            }));
        }, function (sub) {
            setInstallStage(EKA2L1.t('stage.save.sub', { sub: sub }));
        }).then(function (stats) {
            appendInstallInfo(EKA2L1.t('info.image', {
                mb: Math.floor((stats.bytesTotal || 0) / 1048576), entries: stats.entries
            }));
        }).catch(function (err) {
            console.warn('[EKA2L1] staged save failed:', err);
            // The one-shot syncfs clones EVERYTHING into a single IndexedDB
            // transaction — exactly the spike that kills iOS tabs. Only fall
            // back when the image is small enough to plausibly survive it.
            var bt = err ? err.bytesTotal : undefined;
            if (typeof bt === 'number' && bt > 200 * 1048576) {
                throw new Error(EKA2L1.t('error.stagedSaveFailed', {
                    msg: err.message || err, mb: Math.floor(bt / 1048576)
                }));
            }
            setInstallStage(EKA2L1.t('stage.save.fallbackSyncfs'));
            return EKA2L1.save();
        });
    }

    window.installDevice = function () {
        if (!romFile) return;

        var btn = document.getElementById('deviceInstallBtn');
        btn.disabled = true;
        btn.textContent = EKA2L1.t('action.installing');

        var romPath = '/eka2l1/rom_upload.rom';
        var rpkgPath = '/eka2l1/rpkg_upload.rpkg';

        // Boot the core only now (fresh visits don't boot at page load): the
        // file picking is already done, so the high-memory phase no longer
        // overlaps with the iOS Files picker backgrounding the tab.
        try { localStorage.removeItem(INFO_KEY); } catch (e) {}
        appendInstallInfo(EKA2L1.t('info.rom', { mb: Math.floor(romFile.size / 1048576) })
            + (rpkgFile ? EKA2L1.t('info.rpkgAppend', { mb: Math.floor(rpkgFile.size / 1048576) }) : ''));
        if (navigator.storage && navigator.storage.estimate) {
            navigator.storage.estimate().then(function (est) {
                appendInstallInfo(EKA2L1.t('info.quota', {
                    quota: Math.floor((est.quota || 0) / 1048576), usage: Math.floor((est.usage || 0) / 1048576)
                }));
            }).catch(function () {});
        }
        setInstallStage(EKA2L1.t('stage.bootCore'));
        checkStorageHealth()
            .then(function (problem) {
                if (problem) {
                    throw new Error(problem);
                }
                return ensureCore();
            })
            .then(function () {
                if (rpkgFile && EKA2L1.canStreamRpkg()) {
                    // EKA2 streaming path: the RPKG is parsed/extracted as the
                    // chunks arrive and never lands in MEMFS, and the ROM goes
                    // straight to its final location — no temp copies at all.
                    //
                    // Order matters for iOS: PERSIST BEFORE ACTIVATING. The
                    // activation (initDevice) is a full emulator boot — it
                    // loads the 60MB+ ROM and grows the wasm heap by hundreds
                    // of MB. Doing the IndexedDB save while that is resident
                    // was what pushed iPhones over the jetsam limit at the
                    // first write batch. Saving first runs at the memory
                    // floor, and once the flag is set even a kill during
                    // activation self-heals on the next page load.
                    setInstallStage(EKA2L1.t('stage.streamRpkg'));
                    return EKA2L1.streamInstallRpkg(rpkgFile, function (done, total) {
                        var pct = Math.floor(done * 100 / total);
                        setStatus('yellow', EKA2L1.t('status.installingRpkgPct', { pct: pct }));
                        setInstallStage(EKA2L1.t('stage.streamRpkgPct', { pct: pct }));
                    }).then(function (romTarget) {
                        setStatus('yellow', EKA2L1.t('status.writingRom'));
                        setInstallStage(EKA2L1.t('stage.writeRom'));
                        return EKA2L1.writeFileToVFS(romFile, romTarget);
                    }).then(function () {
                        return stagedSaveWithMarkers();
                    }).then(function () {
                        // Durably persisted: from here a reload always recovers.
                        try { localStorage.setItem(HAS_DEVICE_KEY, '1'); } catch (e) {}
                        setStatus('yellow', EKA2L1.t('status.activatingDevice'));
                        setInstallStage(EKA2L1.t('stage.activateDevice'));
                        return new Promise(function (resolve) { setTimeout(resolve, 60); });
                    }).then(function () {
                        var result = EKA2L1.initDevice('', '');
                        if (result !== 0) {
                            throw new Error(EKA2L1.t('error.installFailed', { reason: EKA2L1.decodeInstallError(result) }));
                        }
                        deviceReady = true;
                    });
                }

                // Classic path (EKA1 ROM-only, or streaming unavailable):
                // stage the uploads in MEMFS, then install+activate in one C
                // call (cannot be split), then persist.
                setStatus('yellow', EKA2L1.t('status.writingRom'));
                setInstallStage(EKA2L1.t('stage.writeRom'));
                return EKA2L1.writeFileToVFS(romFile, romPath)
                    .then(function () {
                        if (!rpkgFile) return null;
                        setStatus('yellow', EKA2L1.t('status.writingRpkg'));
                        setInstallStage(EKA2L1.t('stage.writeRpkg'));
                        return EKA2L1.writeFileToVFS(rpkgFile, rpkgPath);
                    })
                    .then(function () {
                        // Heavy synchronous call; yield a frame so the UI paints first.
                        return new Promise(function (resolve) { setTimeout(resolve, 60); });
                    })
                    .then(function () {
                        setStatus('yellow', EKA2L1.t('status.extracting'));
                        setInstallStage(EKA2L1.t('stage.extract'));
                        var result = EKA2L1.initDevice(romPath, rpkgFile ? rpkgPath : '');
                        if (result !== 0) {
                            throw new Error(EKA2L1.t('error.installFailed', { reason: EKA2L1.decodeInstallError(result) }));
                        }
                        deviceReady = true;
                        // The installer copied the ROM into roms/<firmcode>/; drop
                        // the uploads so they don't bloat IndexedDB on every sync.
                        try { EKA2L1.module.FS.unlink(romPath); } catch (e) {}
                        try { EKA2L1.module.FS.unlink(rpkgPath); } catch (e) {}
                    })
                    .then(function () {
                        return stagedSaveWithMarkers();
                    })
                    .then(function () {
                        try { localStorage.setItem(HAS_DEVICE_KEY, '1'); } catch (e) {}
                    });
            })
            .then(function () {
                setInstallStage(null);
                refreshApps();
                EKA2L1.setPaused(true);
                showError(null);
                setStatus('green', EKA2L1.t('status.deviceReady'));
                setDeviceStatusText(EKA2L1.t('device.status.installed'));
                document.getElementById('onboardCard').style.display = 'none';
                closeSheets();
                EKA2L1.toast(EKA2L1.t('toast.deviceInstallComplete'));
            })
            .catch(function (err) {
                setInstallStage(null); // surfaced on screen, breadcrumb not needed
                console.error('[EKA2L1] device install failed:', err);
                setStatus('red', EKA2L1.t('status.installFailed'));
                showError(EKA2L1.t('error.deviceInstallFailed', { msg: err.message || err }));
                EKA2L1.toast(err.message || EKA2L1.t('toast.installFailedSeeAbove'), 4000);
            })
            .then(function () {
                btn.disabled = !romFile;
                btn.textContent = EKA2L1.t('action.startInstall');
            });
    };

    // ---- SIS install ---------------------------------------------------------

    window.pickSis = function () {
        if (!coreReady) {
            if (!hasDevice) { EKA2L1.toast(EKA2L1.t('toast.installRomFirst')); openDeviceSheet(); return; }
            EKA2L1.toast(EKA2L1.t('toast.coreBooting'));
            return;
        }
        if (!deviceReady) { EKA2L1.toast(EKA2L1.t('toast.installRomFirst')); openDeviceSheet(); return; }
        document.getElementById('sisInput').click();
    };

    document.getElementById('sisInput').addEventListener('change', function () {
        var file = this.files[0];
        this.value = '';
        if (!file) return;

        setStatus('yellow', EKA2L1.t('status.installingNamed', { name: file.name }));
        var target = '/eka2l1/temp/' + file.name;

        EKA2L1.writeFileToVFS(file, target)
            .then(function () {
                return new Promise(function (resolve) { setTimeout(resolve, 60); });
            })
            .then(function () {
                var result = EKA2L1.installPackage(target);
                try { EKA2L1.module.FS.unlink(target); } catch (e) {}
                if (result !== 0) throw new Error(EKA2L1.t('error.installFailedCode', { code: result }));
                refreshApps();
                return EKA2L1.save();
            })
            .then(function () {
                setStatus('green', EKA2L1.t('status.ready'));
                EKA2L1.toast(EKA2L1.t('toast.installedNamed', { name: file.name }));
            })
            .catch(function (err) {
                console.error('[EKA2L1] SIS install failed:', err);
                setStatus('red', EKA2L1.t('status.installFailed'));
                showError(EKA2L1.t('error.sisInstallFailed', { msg: err.message || err }));
                EKA2L1.toast(err.message || EKA2L1.t('toast.installFailedSeeAbove'), 4000);
            });
    });

    // ---- device list (multi-ROM) ---------------------------------------------

    window.renderDeviceList = function () {
        var box = document.getElementById('deviceList');
        if (!box) return;
        if (!coreReady) { box.innerHTML = ''; return; }

        var devices = [];
        try {
            devices = JSON.parse(EKA2L1.module.ccall('wasm_get_devices', 'string', [], [])) || [];
        } catch (e) { devices = []; }

        box.innerHTML = '';
        devices.forEach(function (d) {
            var row = document.createElement('div');
            row.className = 'pref-item';
            var info = document.createElement('div');
            info.style.flex = '1';
            info.style.minWidth = '0';
            info.innerHTML = '<div class="pref-label">' + (d.name || EKA2L1.t('device.unnamed', { index: d.index })) + '</div>'
                + '<div class="pref-sub">' + (d.firmware || '') + (d.current ? EKA2L1.t('device.currentSuffix') : '') + '</div>';
            row.appendChild(info);
            if (!d.current) {
                var btn = document.createElement('button');
                btn.className = 'btn';
                btn.textContent = EKA2L1.t('action.switch');
                btn.addEventListener('click', function () {
                    if (!confirm(EKA2L1.t('confirm.switchDevice', { name: d.name || d.index }))) return;
                    var r = EKA2L1.module.ccall('wasm_set_device', 'number', ['number'], [d.index]);
                    if (r !== 0) { EKA2L1.toast(EKA2L1.t('toast.switchFailed', { code: r })); return; }
                    EKA2L1.save().catch(function () {}).then(function () { location.reload(); });
                });
                row.appendChild(btn);
            }
            var delBtn = document.createElement('button');
            delBtn.className = 'btn btn-danger';
            delBtn.textContent = EKA2L1.t('action.delete');
            delBtn.addEventListener('click', function () {
                var warn = EKA2L1.t('confirm.deleteDevice', { name: d.name || d.index });
                if (d.current) {
                    warn += EKA2L1.t('confirm.deleteDeviceCurrentSuffix');
                }
                if (!confirm(warn)) return;
                delBtn.disabled = true;
                delBtn.textContent = EKA2L1.t('action.deleting');
                setTimeout(function () {
                    var r = EKA2L1.deleteDevice(d.index);
                    if (r !== 0) {
                        EKA2L1.toast(EKA2L1.t('toast.deleteFailed', { code: r }));
                        delBtn.disabled = false;
                        delBtn.textContent = EKA2L1.t('action.delete');
                        return;
                    }
                    // devices.yml/config.yml are rewritten (in MEMFS) by the
                    // C++ side above; persist that edit with the same
                    // small-write path used for uploads, then directly
                    // delete the firmware payload's IndexedDB records —
                    // NOT a full-mount EKA2L1.save(). A full syncfs reconcile
                    // is oriented around discovering new/changed files and
                    // isn't guaranteed to also propagate a whole subtree's
                    // removal, which left ghost "device already exists"
                    // registrations behind after delete + reinstall.
                    var firmcodeLow = (d.firmware || '').toLowerCase();
                    var persist = (EKA2L1.savePaths
                        ? EKA2L1.savePaths(['/eka2l1/devices.yml', '/eka2l1/config.yml'])
                        : EKA2L1.save())
                        .then(function () {
                            if (!EKA2L1.deletePathPrefixes || !firmcodeLow) return;
                            return EKA2L1.deletePathPrefixes([
                                '/eka2l1/roms/' + firmcodeLow,
                                '/eka2l1/drives/z/' + firmcodeLow
                            ]);
                        });
                    persist.catch(function (e) { console.warn('[EKA2L1] device delete persist failed:', e); }).then(function () {
                        if (d.current) {
                            // The deleted device may still be mapped by the
                            // kernel; only a fresh boot is safe.
                            try {
                                var devsLeft = JSON.parse(EKA2L1.module.ccall('wasm_get_devices', 'string', [], [])) || [];
                                if (!devsLeft.length) {
                                    localStorage.removeItem(HAS_DEVICE_KEY);
                                    localStorage.removeItem(APPS_CACHE_KEY);
                                    localStorage.removeItem(ICONS_CACHE_KEY);
                                }
                            } catch (e) {}
                            location.reload();
                            return;
                        }
                        EKA2L1.toast(EKA2L1.t('toast.deviceDeleted', { name: d.name || d.index }));
                        renderDeviceList();
                    });
                }, 30);
            });
            row.appendChild(delBtn);
            box.appendChild(row);
        });
    };

    // ---- package manager (uninstall) ------------------------------------------

    window.renderPkgList = function () {
        var box = document.getElementById('pkgList');
        if (!box) return;
        if (!coreReady || !deviceReady) {
            box.innerHTML = '<div class="pref-item"><div class="pref-sub">' + EKA2L1.t('settings.appManage.hint') + '</div></div>';
            return;
        }

        var pkgs = [];
        try {
            pkgs = JSON.parse(EKA2L1.module.ccall('wasm_get_packages', 'string', [], [])) || [];
        } catch (e) { pkgs = []; }

        if (!pkgs.length) {
            box.innerHTML = '<div class="pref-item"><div class="pref-sub">' + EKA2L1.t('pkg.empty') + '</div></div>';
            return;
        }

        box.innerHTML = '';
        pkgs.forEach(function (p) {
            var row = document.createElement('div');
            row.className = 'pref-item';
            var info = document.createElement('div');
            info.style.flex = '1';
            info.style.minWidth = '0';
            info.innerHTML = '<div class="pref-label" style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap">' + (p.name || ('0x' + (p.uid >>> 0).toString(16))) + '</div>'
                + '<div class="pref-sub">' + (p.vendor || '') + '</div>';
            row.appendChild(info);
            var btn = document.createElement('button');
            btn.className = 'btn btn-danger';
            btn.textContent = EKA2L1.t('action.uninstall');
            btn.addEventListener('click', function () {
                if (!confirm(EKA2L1.t('confirm.uninstall', { name: p.name }))) return;
                btn.disabled = true;
                btn.textContent = EKA2L1.t('action.uninstalling');
                setTimeout(function () {
                    var r = EKA2L1.module.ccall('wasm_uninstall_package', 'number', ['number'], [p.uid >>> 0]);
                    if (r !== 0) {
                        EKA2L1.toast(EKA2L1.t('toast.uninstallFailed', { code: r }));
                        btn.disabled = false;
                        btn.textContent = EKA2L1.t('action.uninstall');
                        return;
                    }
                    EKA2L1.save().catch(function () {}).then(function () {
                        refreshApps();
                        renderPkgList();
                        EKA2L1.toast(EKA2L1.t('toast.uninstalled', { name: p.name }));
                    });
                }, 30);
            });
            row.appendChild(btn);
            box.appendChild(row);
        });
    };

    // ---- file upload to virtual disk ------------------------------------------

    window.onFileTargetChange = function () {
        var sel = document.getElementById('fileUpTarget');
        document.getElementById('fileUpCustomRow').style.display
            = (sel.value === 'custom') ? '' : 'none';
    };

    function uploadTargetGuestPath() {
        var sel = document.getElementById('fileUpTarget');
        var raw = (sel.value === 'custom')
            ? (document.getElementById('fileUpCustom').value || '')
            : sel.value;
        raw = raw.trim();
        if (!raw) return null;
        if (!/^[a-zA-Z]:[\\/]/.test(raw)) return null;
        if (raw.indexOf('..') !== -1) return null;
        return raw;
    }

    // e:\n-gage\ -> /eka2l1/drives/e/n-gage/   (drive letters c/d/e supported)
    function guestPathToFs(guest) {
        var drive = guest[0].toLowerCase();
        if ('cde'.indexOf(drive) === -1) return null;
        var rest = guest.substring(2).replace(/\\/g, '/').toLowerCase();
        if (rest[0] !== '/') rest = '/' + rest;
        if (rest[rest.length - 1] !== '/') rest += '/';
        return '/eka2l1/drives/' + drive + rest;
    }

    window.pickUpload = function () {
        if (!coreReady || !deviceReady) { EKA2L1.toast(EKA2L1.t('toast.installRomFirst')); return; }
        var guest = uploadTargetGuestPath();
        if (!guest) { EKA2L1.toast(EKA2L1.t('toast.invalidPathExample')); return; }
        document.getElementById('fileUpInput').click();
    };

    document.getElementById('fileUpInput').addEventListener('change', function () {
        var files = Array.prototype.slice.call(this.files || []);
        this.value = '';
        if (!files.length) return;

        var guest = uploadTargetGuestPath();
        var fsDir = guest && guestPathToFs(guest);
        if (!fsDir) { EKA2L1.toast(EKA2L1.t('toast.invalidPath')); return; }

        var totalBytes = files.reduce(function (acc, f) { return acc + f.size; }, 0);
        var totalMB = Math.ceil(totalBytes / 1048576);

        var writtenPaths = [];
        var stageName = EKA2L1.t('stage.scanFiles');
        setStatus('yellow', EKA2L1.t('status.checkingStorage'));

        // Pre-flight: persisting needs the uploaded bytes free in the IndexedDB
        // quota, plus a working copy. The device-install path already checks
        // this; uploads used to skip it and just die mid-write with a cryptic
        // error. Refuse early with the actual numbers instead.
        var preflight = (navigator.storage && navigator.storage.estimate)
            ? navigator.storage.estimate()
            : Promise.resolve(null);

        preflight.then(function (est) {
            if (est) {
                var freeBytes = (est.quota || 0) - (est.usage || 0);
                // Headroom: the put() transaction transiently holds another copy
                // of the largest file on top of the persisted bytes.
                var largest = files.reduce(function (m, f) { return Math.max(m, f.size); }, 0);
                var needBytes = totalBytes + largest;
                if (est.quota && freeBytes < needBytes) {
                    var lowQuota = Math.floor((est.quota || 0) / 1048576) < 300;
                    var err = new Error(EKA2L1.t('error.quotaInsufficient', {
                        need: Math.ceil(needBytes / 1048576),
                        free: Math.floor(freeBytes / 1048576),
                        quota: Math.floor((est.quota || 0) / 1048576)
                    }) + (lowQuota ? EKA2L1.t('error.quotaInsufficientSuffixLow') : EKA2L1.t('error.quotaInsufficientSuffixNormal')));
                    err.name = 'QuotaPreflightError';
                    throw err;
                }
            }

            stageName = EKA2L1.t('stage.writeMemfs');
            var chain = Promise.resolve();
            files.forEach(function (f, i) {
                chain = chain.then(function () {
                    var label = EKA2L1.t('stage.uploadingFile', { i: i + 1, n: files.length, name: f.name });
                    setStatus('yellow', label);
                    // Lowercase the filename: on the emulator's case-sensitive
                    // host filesystem, guest path lookups map to all-lowercase
                    // physical names - a file stored with upper-case letters is
                    // invisible to Symbian apps (the classic "uploaded but the
                    // file manager doesn't see it").
                    return EKA2L1.writeFileToVFS(f, fsDir + f.name.toLowerCase(), function (done, total) {
                        if (total > 4 * 1048576) {
                            setStatus('yellow', EKA2L1.t('stage.uploadingFilePct', { label: label, pct: Math.floor(done * 100 / total) }));
                        }
                    }).then(function (p) { writtenPaths.push(p); });
                });
            });
            return chain;
        }).then(function () {
            stageName = EKA2L1.t('stage.writeBrowserStorage');
            setStatus('yellow', EKA2L1.t('status.writingBrowserStorage'));
            // Persist just the uploaded files. A full-mount syncfs reconciles the
            // whole device tree (300MB+), which is exactly what OOM-kills the tab
            // for big uploads - so on failure we report it, NOT fall back to that
            // heavier path (which would only fail harder). The bytes are already
            // in MEMFS, so a later autosave still has a chance to persist them.
            if (!EKA2L1.savePaths) {
                return EKA2L1.save();
            }
            return EKA2L1.savePaths(writtenPaths, function (done, total, bytes) {
                setStatus('yellow', EKA2L1.t('status.writingBrowserStorageMB', { mb: Math.floor(bytes / 1048576) }));
            });
        }).then(function () {
            setStatus('green', EKA2L1.t('status.ready'));
            var hint = (guest.toLowerCase().indexOf('n-gage') !== -1)
                ? EKA2L1.t('hint.ngageReinstall')
                : '';
            EKA2L1.toast(EKA2L1.t('toast.uploaded', { count: files.length, mb: totalMB, path: guest, hint: hint }), 5000);
        }).catch(function (err) {
            console.error('[EKA2L1] upload failed:', err);
            setStatus('red', EKA2L1.t('status.uploadFailed'));
            var detail = (err && err.name ? err.name + ': ' : '') + ((err && err.message) || err);
            if (err && (err.name === 'QuotaExceededError' || /quota/i.test(detail))) {
                detail = EKA2L1.t('error.uploadQuotaExceeded', { mb: totalMB, detail: detail });
            }
            showError(EKA2L1.t('error.uploadFailed', { stage: stageName, detail: detail }));
        });
    });

    // ---- settings ------------------------------------------------------------

    var prefScale = document.getElementById('prefScale');
    var prefKeypad = document.getElementById('prefKeypad');
    var prefShowSys = document.getElementById('prefShowSys');
    var prefPerf = document.getElementById('prefPerf');
    var prefFilter = document.getElementById('prefFilter');
    var prefLang = document.getElementById('prefLang');
    prefScale.value = localStorage.getItem('eka2l1_scale') || 'fit';
    prefKeypad.value = localStorage.getItem('eka2l1_keypad') || 'auto';
    prefShowSys.value = localStorage.getItem('eka2l1_show_sys') || '0';
    prefPerf.value = localStorage.getItem('eka2l1_perf') || 'auto';
    prefFilter.value = localStorage.getItem('eka2l1_filter') || 'auto';
    if (prefLang) prefLang.value = EKA2L1.getLocalePref();
    prefScale.addEventListener('change', function () { localStorage.setItem('eka2l1_scale', this.value); });
    prefKeypad.addEventListener('change', function () { localStorage.setItem('eka2l1_keypad', this.value); });
    prefShowSys.addEventListener('change', function () {
        localStorage.setItem('eka2l1_show_sys', this.value);
        renderGameList();
    });
    prefPerf.addEventListener('change', function () { localStorage.setItem('eka2l1_perf', this.value); });
    prefFilter.addEventListener('change', function () { localStorage.setItem('eka2l1_filter', this.value); });
    if (prefLang) {
        prefLang.addEventListener('change', function () {
            EKA2L1.setLocale(this.value);
            // Re-render dynamic (JS-built) strings that [data-i18n] can't reach.
            document.title = EKA2L1.t('page.library.title');
            var activeTab = document.querySelector('.tab-view.active');
            var activeName = (activeTab && activeTab.id === 'tab-settings') ? 'settings' : 'games';
            document.getElementById('topBarLargeTitle').textContent = tabTitle(activeName);
            document.getElementById('topBarSmallTitle').textContent = tabTitle(activeName);
            renderGameList();
            if (window.renderDeviceList) renderDeviceList();
            if (window.renderPkgList) renderPkgList();
        });
    }

    window.manualSave = function () {
        if (!coreReady) { EKA2L1.toast(EKA2L1.t('toast.coreNotReady')); return; }
        EKA2L1.save().then(function () { EKA2L1.toast(EKA2L1.t('toast.saved')); })
            .catch(function () { EKA2L1.toast(EKA2L1.t('toast.saveFailedSeeConsole'), 3500); });
    };

    window.wipeAll = function () {
        if (!confirm(EKA2L1.t('confirm.wipeAll'))) return;
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
                setStatus('green', EKA2L1.t('status.ready'));
                setDeviceStatusText(EKA2L1.t('device.status.installed'));
                refreshApps();
            } else {
                // Stale eka2l1_has_device after delete/corrupt IDBFS used to
                // force a boot and hit "ROM file not found: " with empty path.
                try { localStorage.removeItem(HAS_DEVICE_KEY); } catch (e) {}
                hasDevice = false;
                setStatus('yellow', EKA2L1.t('status.noDevice'));
                setDeviceStatusText(EKA2L1.t('device.status.notInstalled'));
                document.getElementById('onboardCard').style.display = '';
            }

            // The library page never runs guest code: keep the core paused so
            // the RAF loop stays cheap while the user browses.
            EKA2L1.setPaused(true);
        });
        bootPromise.catch(function (err) {
            bootPromise = null; // allow a retry on the next install attempt
            console.error('[EKA2L1] boot failed:', err);
            setStatus('red', EKA2L1.t('status.bootFailed'));
            showError(EKA2L1.t('error.bootFailed', { msg: err && err.message ? err.message : err }));
        });
        return bootPromise;
    }

    // A leftover stage marker means the last install died silently (iOS
    // jetsam kills + reloads the page with no error event). Tell the user
    // exactly where it stopped — that is otherwise invisible on iPhone.
    try {
        var lastStage = localStorage.getItem(STAGE_KEY);
        if (lastStage) {
            var lastInfo = '';
            try { lastInfo = localStorage.getItem(INFO_KEY) || ''; } catch (e2) {}
            localStorage.removeItem(STAGE_KEY);
            showError(EKA2L1.t('error.installInterrupted', {
                stage: lastStage,
                infoLine: lastInfo ? EKA2L1.t('error.installInterruptedInfoLine', { info: lastInfo }) : ''
            }));
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
        setStatus('yellow', EKA2L1.t('status.noDevice'));
        setDeviceStatusText(EKA2L1.t('device.status.notInstalled'));
        document.getElementById('onboardCard').style.display = '';
    }
})();
