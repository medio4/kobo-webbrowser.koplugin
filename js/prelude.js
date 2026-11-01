// Polyfills for the 2013-era JavaScriptCore inside Kobo's QtWebKit 5.2.1, so that
// Mozilla's Readability.js (transpiled to ES5 with Babel) can run. Everything here is
// wrapped in one function with Readability.js and extract.js (see epub.cpp), so the
// polyfills that are plain variables shadow only inside that scope.
var module = undefined;
if (!Array.prototype.includes) Array.prototype.includes = function (x) { return this.indexOf(x) !== -1; };
if (!Array.prototype.find) Array.prototype.find = function (fn, ctx) {
  for (var i = 0; i < this.length; i++) { if (fn.call(ctx, this[i], i, this)) return this[i]; } return undefined; };
if (!Array.from) Array.from = function (src, fn, ctx) {
  var out = []; var n = src ? src.length : 0;
  for (var i = 0; i < n; i++) out.push(fn ? fn.call(ctx, src[i], i) : src[i]); return out; };
if (!String.prototype.startsWith) String.prototype.startsWith = function (s, p) { p = p || 0; return this.substr(p, s.length) === s; };
if (!String.prototype.endsWith) String.prototype.endsWith = function (s) { return this.substr(this.length - s.length) === s; };
if (!String.prototype.trim) String.prototype.trim = function () { return this.replace(/^\s+|\s+$/g, ''); };
var Set = (typeof window.Set === 'function' && (function () { try { var s = new window.Set(['a']); return s.has('a') && !s.has('b'); } catch (e) { return false; } })())
  ? window.Set
  : function (items) {
      this._m = {};
      if (items) for (var i = 0; i < items.length; i++) this._m[items[i]] = true;
    };
if (Set !== window.Set) {
  Set.prototype.has = function (k) { return this._m.hasOwnProperty(k); };
  Set.prototype.add = function (k) { this._m[k] = true; return this; };
}
// URL(relative, base): resolve through an anchor element when the URL constructor is missing.
var URL = (function () {
  try { if (new window.URL('a/b', 'http://x.y/z/').href === 'http://x.y/z/a/b') return window.URL; } catch (e) {}
  return function (u, base) {
    var a = document.createElement('a');
    a.href = u;
    this.href = a.href;
  };
})();
