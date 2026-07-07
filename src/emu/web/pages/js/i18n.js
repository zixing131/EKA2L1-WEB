/* ============================================================================
 * EKA2L1 Web — i18n engine
 *
 * Loaded AFTER the per-locale dictionaries (js/i18n/*.js, which each register
 * themselves into window.EKA2L1_LOCALES) and BEFORE boot.js/index.js/run.js.
 * Exposes translation on window.EKA2L1 so the rest of the app can call
 * EKA2L1.t(key, params) without caring about module load order — boot.js
 * merges into this same object instead of replacing it (see its header).
 *
 * Language selection:
 *   - 'auto' (default): resolved from navigator.language(s) — Chinese
 *     variants map to zh-CN, everything else falls back to en-US.
 *   - Explicit locale: persisted in localStorage and used verbatim.
 * ============================================================================ */

(function () {
    'use strict';

    var EKA2L1 = window.EKA2L1 = window.EKA2L1 || {};

    var LANG_KEY = 'eka2l1_lang';
    var FALLBACK_LOCALE = 'zh-CN';
    var SUPPORTED_LOCALES = ['zh-CN', 'en-US'];

    function detectBrowserLocale() {
        var langs = (navigator.languages && navigator.languages.length)
            ? navigator.languages
            : [navigator.language || FALLBACK_LOCALE];
        for (var i = 0; i < langs.length; i++) {
            var l = (langs[i] || '').toLowerCase();
            if (l.indexOf('zh') === 0) return 'zh-CN';
            if (l.indexOf('en') === 0) return 'en-US';
        }
        return FALLBACK_LOCALE;
    }

    function getLocalePref() {
        try { return localStorage.getItem(LANG_KEY) || 'auto'; } catch (e) { return 'auto'; }
    }

    function resolveLocale(pref) {
        if (pref !== 'auto' && SUPPORTED_LOCALES.indexOf(pref) !== -1) return pref;
        return detectBrowserLocale();
    }

    var currentLocale = resolveLocale(getLocalePref());

    function dict(locale) {
        return (window.EKA2L1_LOCALES && window.EKA2L1_LOCALES[locale]) || {};
    }

    function lookup(key) {
        var d = dict(currentLocale);
        if (Object.prototype.hasOwnProperty.call(d, key)) return d[key];
        var fb = dict(FALLBACK_LOCALE);
        if (Object.prototype.hasOwnProperty.call(fb, key)) return fb[key];
        return key;
    }

    function format(str, params) {
        if (!params) return str;
        return str.replace(/\{(\w+)\}/g, function (m, name) {
            return (params[name] !== undefined && params[name] !== null) ? params[name] : m;
        });
    }

    /** Translate `key`, optionally interpolating `{placeholder}` tokens from `params`. */
    EKA2L1.t = function (key, params) {
        return format(lookup(key), params);
    };

    /** Currently active locale (e.g. 'zh-CN'), after auto-detection is resolved. */
    EKA2L1.getLocale = function () {
        return currentLocale;
    };

    /** Raw preference as stored ('auto' | 'zh-CN' | 'en-US' | …). */
    EKA2L1.getLocalePref = getLocalePref;

    EKA2L1.supportedLocales = SUPPORTED_LOCALES.slice();

    /**
     * Change the active language. `pref` is 'auto' or an explicit locale;
     * persisted so it survives reloads. Re-applies static [data-i18n]
     * bindings immediately; callers that also render dynamic strings (toast
     * text, lists, …) should re-render those themselves after calling this.
     */
    EKA2L1.setLocale = function (pref) {
        try { localStorage.setItem(LANG_KEY, pref); } catch (e) {}
        currentLocale = resolveLocale(pref);
        EKA2L1.applyI18n();
        document.documentElement.setAttribute('lang', currentLocale);
    };

    /**
     * Apply translations to every element under `root` (default: whole
     * document) carrying a data-i18n* attribute:
     *   data-i18n            -> element.textContent
     *   data-i18n-html        -> element.innerHTML (only for trusted,
     *                            developer-authored markup like <br>)
     *   data-i18n-title       -> element.title
     *   data-i18n-placeholder -> element.placeholder
     */
    EKA2L1.applyI18n = function (root) {
        var scope = root || document;

        var textEls = scope.querySelectorAll('[data-i18n]');
        for (var i = 0; i < textEls.length; i++) {
            textEls[i].textContent = EKA2L1.t(textEls[i].getAttribute('data-i18n'));
        }

        var htmlEls = scope.querySelectorAll('[data-i18n-html]');
        for (var j = 0; j < htmlEls.length; j++) {
            htmlEls[j].innerHTML = EKA2L1.t(htmlEls[j].getAttribute('data-i18n-html'));
        }

        var titleEls = scope.querySelectorAll('[data-i18n-title]');
        for (var k = 0; k < titleEls.length; k++) {
            titleEls[k].title = EKA2L1.t(titleEls[k].getAttribute('data-i18n-title'));
        }

        var placeholderEls = scope.querySelectorAll('[data-i18n-placeholder]');
        for (var m = 0; m < placeholderEls.length; m++) {
            placeholderEls[m].placeholder = EKA2L1.t(placeholderEls[m].getAttribute('data-i18n-placeholder'));
        }
    };

    document.documentElement.setAttribute('lang', currentLocale);

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', function () { EKA2L1.applyI18n(); });
    } else {
        EKA2L1.applyI18n();
    }
})();
