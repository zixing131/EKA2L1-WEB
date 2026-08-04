/**
 * EKA2L1 Service Worker
 *
 * 策略：
 *  - HTML 页面：始终 no-store 拉网络最新版，仅离线才降级 SW 缓存
 *  - 带版本号的资源（?v=…）   : 缓存优先（版本号变化时缓存名同步更新）
 *  - 大型二进制（.wasm/.data）  : 缓存优先，按请求完整 URL 缓存（含 ?v=）
 *  - 其他同源资源               : 过时时重新验证（stale-while-revalidate）
 *
 * 版本更新：
 *  - BUILD_ID 变化 → 新 CACHE_NAME → 删除旧缓存 → skipWaiting → 刷新已开标签页
 *  - HTML 中 sw.js 引用带 ?v=BUILD_ID，确保浏览器检测到 SW 脚本变更
 *
 * COOP/COEP 头注入：
 *  SharedArrayBuffer / Atomics 需要跨源隔离（cross-origin isolation）。
 *  SW 对所有同源响应自动注入这两个头，确保离线状态下 WASM 多线程仍可工作。
 *
 * 缓存版本由构建系统注入（stamp_pages.cmake 替换 BUILD_ID_PLACEHOLDER）。
 */

const CACHE_VERSION = 'BUILD_ID_PLACEHOLDER';
const CACHE_NAME    = 'eka2l1-' + CACHE_VERSION;
// Must match the ?v=… query string stamped into HTML by stamp_pages.cmake.
const ASSET_V       = '?v=' + CACHE_VERSION;

/** 应用外壳：安装时预缓存（JS/CSS 用带版本号的 URL，与 HTML 引用一致） */
const APP_SHELL = [
    './index.html',
    './run.html',
    './manifest.json' + ASSET_V,
    './css/app.css' + ASSET_V,
    './js/boot.js' + ASSET_V,
    './js/build_id.js' + ASSET_V,
    './js/i18n.js' + ASSET_V,
    './js/i18n/zh-CN.js' + ASSET_V,
    './js/i18n/en-US.js' + ASSET_V,
    './js/index.js' + ASSET_V,
    './js/run.js' + ASSET_V,
    './icons/icon.svg',
    './icons/icon-maskable.svg',
    './icons/icon-192.png',
    './icons/icon-512.png',
];

function cacheShell(cache) {
    // Prefer allSettled so one missing optional asset cannot abort install
    // (addAll rejects the whole install on the first failure).
    return Promise.all(
        APP_SHELL.map((url) =>
            cache.add(url).catch((err) => {
                console.warn('[SW] precache skip', url, err && err.message);
            })
        )
    );
}

// ─── 安装：预缓存应用外壳 ─────────────────────────────────────────────────────
self.addEventListener('install', (event) => {
    event.waitUntil(
        caches.open(CACHE_NAME)
            .then((cache) => cacheShell(cache))
            .then(() => self.skipWaiting())
    );
});

// ─── 激活：清理旧版本缓存，并刷新已打开的标签页 ─────────────────────────────
self.addEventListener('activate', (event) => {
    event.waitUntil(
        caches.keys()
            .then((keys) => Promise.all(
                keys
                    .filter((k) => k.startsWith('eka2l1-') && k !== CACHE_NAME)
                    .map((k) => caches.delete(k))
            ))
            .then(() => self.clients.claim())
            // New build: ask every open tab to reload so it picks up the
            // stamped HTML instead of a stale shell. Do NOT use
            // Clients.navigate() here — with COOP same-origin it rejects
            // ("Cannot navigate to URL") and aborts this activate handler,
            // which leaves the page mid-boot and makes app launch look dead.
            .then(() => self.clients.matchAll({ type: 'window', includeUncontrolled: true }))
            .then((clients) => Promise.all(
                clients.map((client) => {
                    try {
                        client.postMessage({ type: 'eka2l1-sw-updated', version: CACHE_VERSION });
                    } catch (_) { /* ignore closed clients */ }
                })
            ))
    );
});

// ─── 工具函数 ─────────────────────────────────────────────────────────────────

/**
 * 为响应注入 COOP / COEP / CORP / Permissions-Policy，使 SharedArrayBuffer
 * 与摄像头权限策略在离线状态下仍可用。仅对可读的同源响应执行注入。
 */
function injectCrossOriginHeaders(response) {
    if (!response || response.type === 'opaque') return response;

    const headers = new Headers(response.headers);
    headers.set('Cross-Origin-Opener-Policy',   'same-origin');
    headers.set('Cross-Origin-Embedder-Policy',  'require-corp');
    headers.set('Cross-Origin-Resource-Policy',  'same-origin');
    headers.set('Permissions-Policy', 'camera=(self), microphone=()');

    return new Response(response.body, {
        status:     response.status,
        statusText: response.statusText,
        headers,
    });
}

/** 将网络响应存入缓存（仅成功响应）*/
function storeInCache(request, response) {
    if (!response || !response.ok) return;
    const copy = response.clone();
    caches.open(CACHE_NAME).then((cache) => cache.put(request, copy)).catch(() => {});
}

/** 网络优先，失败时回落缓存；永远不把 TypeError 抛给控制台。 */
function networkThenCache(request) {
    return fetch(request)
        .then((r) => {
            storeInCache(request, r);
            return injectCrossOriginHeaders(r);
        })
        .catch(() =>
            caches.match(request).then((cached) =>
                cached ? injectCrossOriginHeaders(cached) : Response.error()
            )
        );
}

/** 缓存优先，未命中再走网络；网络失败时返回 Response.error()。 */
function cacheThenNetwork(request) {
    return caches.match(request).then((cached) => {
        if (cached) return injectCrossOriginHeaders(cached);
        return networkThenCache(request);
    });
}

// ─── 请求拦截 ─────────────────────────────────────────────────────────────────
self.addEventListener('fetch', (event) => {
    const { request } = event;

    if (request.method !== 'GET') return;

    let url;
    try { url = new URL(request.url); } catch { return; }

    if (url.origin !== self.location.origin) return;

    // Never intercept the SW script itself — lets updates bypass a broken
    // controlling worker (otherwise "Failed to update a ServiceWorker" loops).
    if (url.pathname.endsWith('/sw.js')) return;

    const path = url.pathname;
    const isHTML = request.mode === 'navigate';
    const isVersioned = url.searchParams.has('v');
    const isBinary = path.endsWith('.wasm') || path.endsWith('.data');

    if (isHTML) {
        event.respondWith(networkThenCache(request));
    } else if (isVersioned || isBinary) {
        event.respondWith(cacheThenNetwork(request));
    } else {
        // stale-while-revalidate：有缓存先回；后台刷新失败也不抛错
        event.respondWith(
            caches.match(request).then((cached) => {
                const refreshing = fetch(request)
                    .then((r) => {
                        storeInCache(request, r);
                        return injectCrossOriginHeaders(r);
                    })
                    .catch(() => null);

                if (cached) {
                    refreshing.catch(() => {});
                    return injectCrossOriginHeaders(cached);
                }
                return refreshing.then((r) => r || Response.error());
            })
        );
    }
});
