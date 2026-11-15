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
    var perfStubs = {
        getEntries: function () { return []; },
        getEntriesByType: function () { return []; },
        getEntriesByName: function () { return []; },
        mark: function () {}, measure: function () {}, clearMarks: function () {}, clearMeasures: function () {}
    };
    if (perf) {
        // On the prototype first: in some builds every look at window.performance hands out
        // a fresh object, and what was added to the last one is gone.
        var perfProto = (w.Performance && w.Performance.prototype) || Object.getPrototypeOf(perf);
        if (perfProto && perfProto !== Object.prototype) {
            def(perfProto, 'now', function () { return Date.now(); });
            for (var pp in perfStubs) def(perfProto, pp, perfStubs[pp]);
        }
        def(perf, 'now', function () { return Date.now(); });
        for (var pk in perfStubs) def(perf, pk, perfStubs[pk]);
    }
    // Some builds of the engine don't let anything be added to window.performance (or have
    // none): put a plain object in its place that passes on what the real one has.
    if (!perf || typeof w.performance.getEntriesByType !== 'function') {
        var t0 = Date.now(), real = perf, px = {
            now: function () { return real && real.now ? real.now() : Date.now() - t0; },
            timing: real ? real.timing : { navigationStart: t0 },
            navigation: real ? real.navigation : { type: 0, redirectCount: 0 },
            timeOrigin: t0
        };
        for (var sk in perfStubs) px[sk] = perfStubs[sk];
        try { Object.defineProperty(w, 'performance', { value: px, writable: true, configurable: true }); }
        catch (e) { try { w.performance = px; } catch (e2) {} }
    }
    if (!w.performance || typeof w.performance.getEntriesByType !== 'function')
        try { console.log('Error: fill-ins could not patch window.performance'); } catch (e) {}

    // --- dates and numbers in the user's language. The engine leans on the system's ICU
    // data for these, and where that is incomplete "new Intl.DateTimeFormat()" throws and
    // takes the page's script with it. Plain English stand-ins are better than that.
    var pad2 = function (n) { return (n < 10 ? '0' : '') + n; };
    var MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September',
                  'October', 'November', 'December'];
    var DAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
    var plainDate = function (d, o) {
        o = o || {};
        var wantDate = o.year || o.month || o.day || o.weekday || o.dateStyle, out = [];
        var wantTime = o.hour || o.minute || o.second || o.timeStyle;
        if (!wantDate && !wantTime) wantDate = true;
        if (wantDate) {
            var m = o.month === 'long' ? MONTHS[d.getMonth()] : o.month === 'short' ? MONTHS[d.getMonth()].substr(0, 3) : null;
            var s = '';
            if (o.weekday) s += (o.weekday === 'long' ? DAYS[d.getDay()] : DAYS[d.getDay()].substr(0, 3)) + ', ';
            if (m) s += m + ' ' + d.getDate() + (o.year === undefined && o.dateStyle === undefined && !o.day ? '' : ', ' + d.getFullYear());
            else s += (d.getMonth() + 1) + '/' + d.getDate() + '/' + d.getFullYear();
            out.push(s);
        }
        if (wantTime) {
            var h = d.getHours(), t = (h % 12 || 12) + ':' + pad2(d.getMinutes());
            if (o.second) t += ':' + pad2(d.getSeconds());
            out.push(t + (h < 12 ? ' AM' : ' PM'));
        }
        return out.join(', ');
    };
    var works = function (f) { try { f(); return true; } catch (e) { return false; } };
    if (!w.Intl) { try { w.Intl = {}; } catch (e) {} }
    if (w.Intl && !works(function () { new Intl.DateTimeFormat('en-US').format(new Date()); })) {
        var DTF = function (locales, options) {
            if (!(this instanceof DTF)) return new DTF(locales, options);
            this._o = options || {};
        };
        DTF.prototype.format = function (d) { return plainDate(d === undefined ? new Date() : new Date(d), this._o); };
        DTF.prototype.formatToParts = function (d) { return [{ type: 'literal', value: this.format(d) }]; };
        DTF.prototype.resolvedOptions = function () {
            return { locale: 'en-US', calendar: 'gregory', numberingSystem: 'latn', timeZone: 'UTC' };
        };
        DTF.supportedLocalesOf = function () { return ['en-US']; };
        Intl.DateTimeFormat = DTF;
        Date.prototype.toLocaleString = function (l, o) {
            return plainDate(this, o || { year: 'numeric', month: 'numeric', day: 'numeric', hour: 'numeric', minute: 'numeric', second: 'numeric' });
        };
        Date.prototype.toLocaleDateString = function (l, o) { return plainDate(this, o || { year: 'numeric', month: 'numeric', day: 'numeric' }); };
        Date.prototype.toLocaleTimeString = function (l, o) { return plainDate(this, o || { hour: 'numeric', minute: 'numeric', second: 'numeric' }); };
    }
    if (w.Intl && !works(function () { new Intl.NumberFormat('en-US').format(1234.5); })) {
        var plainNumber = function (n, o) {
            o = o || {};
            if (typeof n !== 'number' || !isFinite(n)) return String(n);
            var max = o.maximumFractionDigits === undefined ? 3 : o.maximumFractionDigits;
            var min = o.minimumFractionDigits || 0;
            if (o.style === 'percent') n *= 100;
            var s = Math.abs(n).toFixed(Math.max(min, Math.min(max, 20)));
            if (s.indexOf('.') >= 0) {
                var parts = s.split('.'), frac = parts[1].replace(/0+$/, '');
                while (frac.length < min) frac += '0';
                s = parts[0] + (frac ? '.' + frac : '');
            }
            if (o.useGrouping !== false) s = s.replace(/^\d+/, function (i) { return i.replace(/\B(?=(\d{3})+$)/g, ','); });
            return (n < 0 ? '-' : '') + s + (o.style === 'percent' ? '%' : '');
        };
        var NF = function (locales, options) {
            if (!(this instanceof NF)) return new NF(locales, options);
            this._o = options || {};
        };
        NF.prototype.format = function (n) { return plainNumber(Number(n), this._o); };
        NF.prototype.resolvedOptions = function () { return { locale: 'en-US', numberingSystem: 'latn' }; };
        NF.supportedLocalesOf = function () { return ['en-US']; };
        Intl.NumberFormat = NF;
        Number.prototype.toLocaleString = function (l, o) { return plainNumber(Number(this), o); };
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
