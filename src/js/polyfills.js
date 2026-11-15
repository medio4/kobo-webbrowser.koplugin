// Fill-ins for browser features newer than the engine (run in every page before its own
// scripts). Sites test for few of these and just call them, and one missing function stops
// a whole script. Plain ES5, so the firmware's 2013 engine can read it too; every piece is
// added only where it is missing.
(function () {
    'use strict';
    var w = window;
    function def(obj, name, fn) {
        if (!obj || obj[name]) return;
        try { Object.defineProperty(obj, name, { value: fn, writable: true, configurable: true }); }
        catch (e) { try { obj[name] = fn; } catch (e2) {} }
    }

    // --- collections and strings
    if (w.NodeList) def(NodeList.prototype, 'forEach', Array.prototype.forEach);
    if (w.DOMTokenList) def(DOMTokenList.prototype, 'forEach', Array.prototype.forEach);
    def(Object, 'values', function (o) { return Object.keys(o).map(function (k) { return o[k]; }); });
    def(Object, 'entries', function (o) { return Object.keys(o).map(function (k) { return [k, o[k]]; }); });
    def(Object, 'fromEntries', function (list) {
        var o = {};
        Array.prototype.forEach.call(list, function (p) { o[p[0]] = p[1]; });
        return o;
    });
    def(Object, 'assign', function (t) {
        for (var i = 1; i < arguments.length; i++) {
            var s = arguments[i];
            if (s != null) for (var k in s) if (Object.prototype.hasOwnProperty.call(s, k)) t[k] = s[k];
        }
        return t;
    });
    def(Array.prototype, 'includes', function (x, from) { return this.indexOf(x, from) !== -1; });
    def(Array.prototype, 'find', function (f, self) {
        for (var i = 0; i < this.length; i++) if (f.call(self, this[i], i, this)) return this[i];
    });
    def(Array.prototype, 'findIndex', function (f, self) {
        for (var i = 0; i < this.length; i++) if (f.call(self, this[i], i, this)) return i;
        return -1;
    });
    // Generic on purpose: libraries call it on array-likes (flat.call(arguments)).
    def(Array.prototype, 'flat', function (depth) {
        var out = [];
        (function walk(list, d) {
            for (var i = 0; i < list.length; i++) {
                if (!(i in list)) continue;
                if (Array.isArray(list[i]) && d > 0) walk(list[i], d - 1); else out.push(list[i]);
            }
        })(this, depth === undefined ? 1 : depth);
        return out;
    });
    def(Array.prototype, 'flatMap', function (f, self) {
        return Array.prototype.flat.call(Array.prototype.map.call(this, f, self), 1);
    });
    def(Array.prototype, 'at', function (i) { i = i | 0; return this[i < 0 ? this.length + i : i]; });
    def(Array, 'from', function (list, map, self) {
        var out = [], i;
        for (i = 0; i < list.length; i++) out.push(map ? map.call(self, list[i], i) : list[i]);
        return out;
    });
    def(String.prototype, 'includes', function (s, from) { return this.indexOf(s, from) !== -1; });
    def(String.prototype, 'startsWith', function (s, at) { at = at || 0; return this.substr(at, s.length) === s; });
    def(String.prototype, 'endsWith', function (s, end) {
        end = end === undefined ? this.length : end;
        return this.substring(end - s.length, end) === s;
    });
    def(String.prototype, 'repeat', function (n) { var out = ''; while (n-- > 0) out += this; return out; });
    def(String.prototype, 'padStart', function (n, pad) {
        var s = String(this); pad = pad === undefined ? ' ' : String(pad);
        while (s.length < n && pad) s = pad.substr(0, n - s.length) + s;
        return s;
    });
    def(String.prototype, 'padEnd', function (n, pad) {
        var s = String(this); pad = pad === undefined ? ' ' : String(pad);
        while (s.length < n && pad) s += pad.substr(0, n - s.length);
        return s;
    });
    def(String.prototype, 'trimStart', function () { return this.replace(/^\s+/, ''); });
    def(String.prototype, 'trimEnd', function () { return this.replace(/\s+$/, ''); });
    def(String.prototype, 'replaceAll', function (from, to) {
        if (from instanceof RegExp) return this.replace(from, to);
        return this.split(String(from)).join(typeof to === 'function' ? to(from) : to);
    });
    def(String.prototype, 'at', function (i) { i = i | 0; return this.charAt(i < 0 ? this.length + i : i); });
    def(Number, 'isInteger', function (v) { return typeof v === 'number' && isFinite(v) && Math.floor(v) === v; });
    def(Number, 'isNaN', function (v) { return v !== v; });
    def(Number, 'isFinite', function (v) { return typeof v === 'number' && isFinite(v); });
    if (!w.globalThis) { try { w.globalThis = w; } catch (e) {} }

    // --- DOM
    var EP = w.Element && Element.prototype;
    if (EP) {
        def(EP, 'matches', EP.webkitMatchesSelector || function (sel) {
            var all = (this.ownerDocument || document).querySelectorAll(sel);
            for (var i = 0; i < all.length; i++) if (all[i] === this) return true;
            return false;
        });
        def(EP, 'closest', function (sel) {
            for (var el = this; el && el.nodeType === 1; el = el.parentNode) if (el.matches(sel)) return el;
            return null;
        });
        def(EP, 'remove', function () { if (this.parentNode) this.parentNode.removeChild(this); });
        def(EP, 'toggleAttribute', function (name, force) {
            var on = force === undefined ? !this.hasAttribute(name) : !!force;
            if (on) this.setAttribute(name, ''); else this.removeAttribute(name);
            return on;
        });
        def(EP, 'getAttributeNames', function () {
            return Array.prototype.map.call(this.attributes, function (a) { return a.name; });
        });
        var frag = function (nodes) {
            var f = document.createDocumentFragment();
            Array.prototype.forEach.call(nodes, function (n) {
                f.appendChild(typeof n === 'string' ? document.createTextNode(n) : n);
            });
            return f;
        };
        def(EP, 'append', function () { this.appendChild(frag(arguments)); });
        def(EP, 'prepend', function () { this.insertBefore(frag(arguments), this.firstChild); });
        def(EP, 'before', function () { if (this.parentNode) this.parentNode.insertBefore(frag(arguments), this); });
        def(EP, 'after', function () { if (this.parentNode) this.parentNode.insertBefore(frag(arguments), this.nextSibling); });
        def(EP, 'replaceWith', function () { if (this.parentNode) this.parentNode.replaceChild(frag(arguments), this); });
    }
    if (typeof w.CustomEvent !== 'function') {
        var CE = function (type, p) {
            p = p || {};
            var e = document.createEvent('CustomEvent');
            e.initCustomEvent(type, !!p.bubbles, !!p.cancelable, p.detail);
            return e;
        };
        if (w.Event) CE.prototype = w.Event.prototype;
        w.CustomEvent = CE;
    }

    // --- timing
    var perf = w.performance;
    if (perf) {
        def(perf, 'now', function () { return Date.now(); });
        def(perf, 'getEntries', function () { return []; });
        def(perf, 'getEntriesByType', function () { return []; });
        def(perf, 'getEntriesByName', function () { return []; });
        def(perf, 'mark', function () {});
        def(perf, 'measure', function () {});
        def(perf, 'clearMarks', function () {});
        def(perf, 'clearMeasures', function () {});
    }
    def(w, 'requestAnimationFrame', w.webkitRequestAnimationFrame || function (f) {
        return setTimeout(function () { f(Date.now()); }, 16);
    });
    def(w, 'cancelAnimationFrame', w.webkitCancelAnimationFrame || clearTimeout);
    def(w, 'requestIdleCallback', function (f) {
        return setTimeout(function () {
            f({ didTimeout: false, timeRemaining: function () { return 20; } });
        }, 1);
    });
    def(w, 'cancelIdleCallback', clearTimeout);
    def(w, 'queueMicrotask', function (f) {
        if (w.Promise) Promise.resolve().then(f); else setTimeout(f, 0);
    });

    // --- promises
    if (w.Promise) {
        def(Promise.prototype, 'finally', function (f) {
            return this.then(function (v) { return Promise.resolve(f()).then(function () { return v; }); },
                             function (e) { return Promise.resolve(f()).then(function () { throw e; }); });
        });
        def(Promise, 'allSettled', function (list) {
            return Promise.all(Array.prototype.map.call(list, function (p) {
                return Promise.resolve(p).then(
                    function (v) { return { status: 'fulfilled', value: v }; },
                    function (e) { return { status: 'rejected', reason: e }; });
            }));
        });
    }

    // --- observers: no real watching, but pages that wait for "it is on screen now" (lazy
    // pictures, lazy sections) get told so at once and load their content.
    if (!w.IntersectionObserver) {
        var IO = function (cb) { this._cb = cb; };
        IO.prototype.observe = function (el) {
            var self = this;
            setTimeout(function () {
                var r = el.getBoundingClientRect ? el.getBoundingClientRect() : null;
                try {
                    self._cb([{ target: el, isIntersecting: true, intersectionRatio: 1, boundingClientRect: r,
                                intersectionRect: r, rootBounds: null, time: Date.now() }], self);
                } catch (e) {}
            }, 0);
        };
        IO.prototype.unobserve = function () {};
        IO.prototype.disconnect = function () {};
        IO.prototype.takeRecords = function () { return []; };
        w.IntersectionObserver = IO;
    }
    if (!w.ResizeObserver) {
        var RO = function (cb) { this._cb = cb; };
        RO.prototype.observe = function (el) {
            var self = this;
            setTimeout(function () {
                var r = el.getBoundingClientRect ? el.getBoundingClientRect() : { width: 0, height: 0 };
                try { self._cb([{ target: el, contentRect: r }], self); } catch (e) {}
            }, 0);
        };
        RO.prototype.unobserve = function () {};
        RO.prototype.disconnect = function () {};
        w.ResizeObserver = RO;
    }
    if (w.matchMedia) {
        try {
            var mq = w.matchMedia('all'), MP = Object.getPrototypeOf(mq);
            def(MP, 'addEventListener', function (type, f) { if (this.addListener) this.addListener(f); });
            def(MP, 'removeEventListener', function (type, f) { if (this.removeListener) this.removeListener(f); });
        } catch (e) {}
    }

    // --- URLSearchParams (enough for reading and building query strings)
    if (!w.URLSearchParams) {
        var dec = function (s) { try { return decodeURIComponent(s.replace(/\+/g, ' ')); } catch (e) { return s; } };
        var USP = function (init) {
            this._l = [];
            var self = this;
            if (typeof init === 'string') {
                init.replace(/^\?/, '').split('&').forEach(function (p) {
                    if (!p) return;
                    var i = p.indexOf('=');
                    self._l.push([dec(i < 0 ? p : p.substr(0, i)), i < 0 ? '' : dec(p.substr(i + 1))]);
                });
            } else if (init && typeof init === 'object') {
                if (init._l) init._l.forEach(function (p) { self._l.push([p[0], p[1]]); });
                else Object.keys(init).forEach(function (k) { self._l.push([k, String(init[k])]); });
            }
        };
        USP.prototype.get = function (k) { for (var i = 0; i < this._l.length; i++) if (this._l[i][0] === k) return this._l[i][1]; return null; };
        USP.prototype.getAll = function (k) { return this._l.filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; }); };
        USP.prototype.has = function (k) { return this.get(k) !== null; };
        USP.prototype.append = function (k, v) { this._l.push([String(k), String(v)]); };
        USP.prototype['delete'] = function (k) { this._l = this._l.filter(function (p) { return p[0] !== k; }); };
        USP.prototype.set = function (k, v) { this['delete'](k); this.append(k, v); };
        USP.prototype.forEach = function (f, self) { this._l.forEach(function (p) { f.call(self, p[1], p[0]); }); };
        USP.prototype.toString = function () {
            return this._l.map(function (p) { return encodeURIComponent(p[0]) + '=' + encodeURIComponent(p[1]); }).join('&');
        };
        w.URLSearchParams = USP;
    }

    // --- fetch, on top of XMLHttpRequest (text, JSON; needs Promise, so the newer engine)
    if (!w.fetch && w.Promise && w.XMLHttpRequest) {
        var Hdrs = function (raw) {
            this._m = {};
            var self = this;
            (raw || '').split(/\r?\n/).forEach(function (line) {
                var i = line.indexOf(':');
                if (i > 0) self._m[line.substr(0, i).trim().toLowerCase()] = line.substr(i + 1).trim();
            });
        };
        Hdrs.prototype.get = function (k) { var v = this._m[String(k).toLowerCase()]; return v === undefined ? null : v; };
        Hdrs.prototype.has = function (k) { return String(k).toLowerCase() in this._m; };
        Hdrs.prototype.forEach = function (f, self) { for (var k in this._m) f.call(self, this._m[k], k); };
        w.fetch = function (input, init) {
            init = init || {};
            var url = typeof input === 'string' ? input : (input && input.url) || String(input);
            return new Promise(function (resolve, reject) {
                var x = new XMLHttpRequest();
                x.open((init.method || 'GET').toUpperCase(), url, true);
                if (init.credentials === 'include') x.withCredentials = true;
                var h = init.headers || {};
                if (h._m) h = h._m;
                if (typeof h.forEach === 'function' && !Array.isArray(h)) h.forEach(function (v, k) { x.setRequestHeader(k, v); });
                else for (var k in h) if (Object.prototype.hasOwnProperty.call(h, k)) x.setRequestHeader(k, h[k]);
                x.onload = function () {
                    var text = x.responseText;
                    resolve({
                        ok: x.status >= 200 && x.status < 300, status: x.status, statusText: x.statusText,
                        url: x.responseURL || url, redirected: false, type: 'basic',
                        headers: new Hdrs(x.getAllResponseHeaders()),
                        text: function () { return Promise.resolve(text); },
                        json: function () { return new Promise(function (res) { res(JSON.parse(text)); }); },
                        clone: function () { return this; }
                    });
                };
                x.onerror = x.ontimeout = function () { reject(new TypeError('Network request failed')); };
                x.onabort = function () { reject(new Error('Aborted')); };
                x.send(init.body === undefined ? null : init.body);
            });
        };
    }
})();
