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
  // Name pattern of this page with its numbers masked ("part-#"), or null on an index page.
  var stem = p.charAt(p.length - 1) === '/' ? null
           : p.substring(base.length).replace(/\.[a-z0-9]+$/i, '').replace(/\d+/g, '#');
  if (stem !== null && (stem.indexOf('#') < 0 || /\d{4,}/.test(p.substring(base.length))))
    return JSON.stringify({ chapters: [], title: '' });
  var seen = {}, out = [];
  if (stem !== null) {   // this part itself, in case the page doesn't link to itself
    seen[here.origin + p] = true;
    out.push({ url: here.origin + p, title: document.title || p.substring(base.length), key: natKey(p) });
  }
  var links = document.getElementsByTagName('a');
  for (var i = 0; i < links.length; i++) {
    var a = links[i], href = a.getAttribute('href');
    if (!href || href.charAt(0) === '#' || /^(javascript|mailto):/i.test(href)) continue;
    var u = parse(a.href);
    if (!u || u.host !== here.host) continue;
    var path = u.path;
    // The page itself belongs in the book when it is one of the numbered parts.
    if (path.indexOf(base) !== 0 || path === base || (path === here.path && stem === null)) continue;
    if (/\.(css|js|png|jpe?g|gif|svg|ico|xml|rss|pdf|zip|mp3|mp4)$/i.test(path)) continue;
    // Only "leaf" links (files or single folders), not deeper sub-trees.
    var rest = path.substring(base.length).replace(/\/$/, '');
    if (!rest || rest.indexOf('/') >= 0) continue;
    // On a page that is itself one item in a big folder (an encyclopedia article, a
    // blog post), every same-folder link is not a series. There, only links named like
    // this page with a different number count (chapter-2, chapter-3, ...). A folder's
    // own index page (the address ends in "/") lists its parts, so all of them count.
    // Chapter numbers are small; long numbers are article IDs or dates, not parts.
    if (stem !== null && (rest.replace(/\.[a-z0-9]+$/i, '').replace(/\d+/g, '#') !== stem || !/\d/.test(rest)
                          || /\d{4,}/.test(rest))) continue;
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
