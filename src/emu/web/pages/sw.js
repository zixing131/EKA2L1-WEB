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

// ─── 安装：预缓存应用外壳 ─────────────────────────────────────────────────────
self.addEventListener('install', (event) => {
    event.waitUntil(
        caches.open(CACHE_NAME)
            .then((cache) => cache.addAll(APP_SHELL))
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
 * 为响应注入 COOP / COEP / CORP 头，使 SharedArrayBuffer 在离线状态下可用。
 * 仅对可读的同源响应（非 opaque）执行注入。
 */
function injectCrossOriginHeaders(response) {
    // opaque 响应（type === 'opaque'）无法读取，跳过
    if (!response || response.type === 'opaque') return response;

    const headers = new Headers(response.headers);
    headers.set('Cross-Origin-Opener-Policy',   'same-origin');
    headers.set('Cross-Origin-Embedder-Policy',  'require-corp');
    headers.set('Cross-Origin-Resource-Policy',  'same-origin');

    return new Response(response.body, {
        status:     response.status,
        statusText: response.statusText,
        headers,
    });
}

/** 将网络响应存入缓存（仅成功响应）*/
function storeInCache(request, response) {
    if (!response || !response.ok) return;
    // Clone SYNCHRONOUSLY: the caller consumes response.body right after this
    // returns (injectCrossOriginHeaders does `new Response(response.body,…)`),
    // so cloning inside the async caches.open().then() would fail with
    // "Response body is already used".
    const copy = response.clone();
    caches.open(CACHE_NAME).then((cache) => cache.put(request, copy));
}

// ─── 请求拦截 ─────────────────────────────────────────────────────────────────
self.addEventListener('fetch', (event) => {
    const { request } = event;

    // 只处理 GET 请求
    if (request.method !== 'GET') return;

    let url;
    try { url = new URL(request.url); } catch { return; }

    // 只处理同源请求（WASM 加载器也只请求同源资源）
    if (url.origin !== self.location.origin) return;

    const path      = url.pathname;
    const isHTML    = request.mode === 'navigate';
    const isVersioned = url.searchParams.has('v');
    const isBinary  = path.endsWith('.wasm') || path.endsWith('.data');

    if (isHTML) {
        // 页面导航：始终绕过 HTTP 缓存拉最新 HTML；仅离线时才降级到 SW 缓存。
        // 避免「新 run.js + 旧 run.html（缺 i18n 引用）」这类外壳/脚本不匹配。
        event.respondWith(
            fetch(request, { cache: 'no-store' })
                .then((r) => {
                    storeInCache(request, r);
                    return injectCrossOriginHeaders(r);
                })
                .catch(() =>
                    caches.match(request).then((cached) =>
                        cached ? injectCrossOriginHeaders(cached) : Response.error()
                    )
                )
        );

    } else if (isVersioned || isBinary) {
        // 带版本号的 JS/CSS 及大型二进制：缓存优先（版本号确保永久可用）
        event.respondWith(
            caches.match(request).then((cached) => {
                if (cached) return injectCrossOriginHeaders(cached);
                return fetch(request).then((r) => {
                    storeInCache(request, r);
                    return injectCrossOriginHeaders(r);
                });
            })
        );

    } else {
        // 其他静态资源：过时时重新验证
        event.respondWith(
            caches.match(request).then((cached) => {
                const networkFetch = fetch(request).then((r) => {
                    storeInCache(request, r);
                    return injectCrossOriginHeaders(r);
                });
                return cached
                    ? (networkFetch.catch(() => {}), injectCrossOriginHeaders(cached))
                    : networkFetch;
            })
        );
    }
});
