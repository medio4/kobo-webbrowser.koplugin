// Runs inside the page (old WebKit: ES5 only, no URL constructor). Finds the chapters of a
// multi-part work that this page belongs to (or lists): same-site links under the current
// page's folder, sorted naturally by the numbers in their paths.
// Returns JSON {chapters:[{url,title}], title}.
(function () {
  function natKey(s) {
    return s.toLowerCase().replace(/\d+/g, function (n) { return ('00000000' + n).slice(-8); });
  }
  // Resolve a URL the way the page would: an anchor element does it in every WebKit.
  function parse(href) {
    var a = document.createElement('a');
    a.href = href;
    if (!a.protocol || !/^https?:$/.test(a.protocol)) return null;
    var path = a.pathname || '/';
    if (path.charAt(0) !== '/') path = '/' + path;
    return { host: a.host, origin: a.protocol + '//' + a.host, path: path };
  }
  var here = parse(document.URL);
  if (!here) return JSON.stringify({ chapters: [], title: '' });
  var p = here.path;
  var base = p.charAt(p.length - 1) === '/' ? p : p.slice(0, p.lastIndexOf('/') + 1);
  var seen = {}, out = [];
  var links = document.getElementsByTagName('a');
  for (var i = 0; i < links.length; i++) {
    var a = links[i], href = a.getAttribute('href');
    if (!href || href.charAt(0) === '#' || /^(javascript|mailto):/i.test(href)) continue;
    var u = parse(a.href);
    if (!u || u.host !== here.host) continue;
    var path = u.path;
    if (path.indexOf(base) !== 0 || path === here.path || path === base) continue;
    if (/\.(css|js|png|jpe?g|gif|svg|ico|xml|rss|pdf|zip|mp3|mp4)$/i.test(path)) continue;
    // Only "leaf" links (files or single folders), not deeper sub-trees.
    var rest = path.substring(base.length).replace(/\/$/, '');
    if (!rest || rest.indexOf('/') >= 0) continue;
    var key = u.origin + path;
    if (seen[key]) continue;
    seen[key] = true;
    var title = (a.textContent || '').replace(/\s+/g, ' ').replace(/^\s+|\s+$/g, '');
    if (!title) { try { title = decodeURIComponent(rest); } catch (e) { title = rest; } }
    out.push({ url: key, title: title, key: natKey(path) });
  }
  // Numbered parts (part-1, part-2, ...) are put in number order. Named pages (a
  // tutorial's "appetite", "interpreter", ...) keep the order the page lists them in,
  // which is the author's order; sorting their names alphabetically would scramble it.
  var allNumbered = out.length > 0;
  for (var n = 0; n < out.length; n++)
    if (!/\d/.test(out[n].url.substring(out[n].url.lastIndexOf('/', out[n].url.length - 2)))) { allNumbered = false; break; }
  if (allNumbered) out.sort(function (x, y) { return x.key < y.key ? -1 : x.key > y.key ? 1 : 0; });
  for (var j = 0; j < out.length; j++) delete out[j].key;
  return JSON.stringify({ chapters: out, title: document.title || '' });
})();
