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
  var pathOk = !(stem !== null && (stem.indexOf('#') < 0 || /\d{4,}/.test(p.substring(base.length))));
  var seen = {}, out = [];
  if (pathOk && stem !== null) {   // this part itself, in case the page doesn't link to itself
    seen[here.origin + p] = true;
    out.push({ url: here.origin + p, title: document.title || p.substring(base.length), key: natKey(p) });
  }
  var links = document.getElementsByTagName('a');
  for (var i = 0; pathOk && i < links.length; i++) {
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
  if (out.length >= 2) return JSON.stringify({ chapters: out, title: document.title || '' });

  // No numbered addresses: numbered link titles instead ("... Daze 1", "... Daze 2" on an
  // artist's or a series' page; posts on such sites live in one flat folder). Only on a
  // list page (address ends in "/") or when this page's own title is one of them, so an
  // article that links "Apollo 11", "Apollo 12" is not taken for a series.
  // Link text without post dates ("November 6, 2026", "2026-11-06", "11/06/2026").
  function textOf(el) {
    return (el.textContent || '').replace(/\s+/g, ' ')
      .replace(/\b(jan|feb|mar|apr|may|jun|jul|aug|sep|oct|nov|dec)[a-z]*\.? \d{1,2},? \d{4}\b/gi, '')
      .replace(/\b\d{1,2} (jan|feb|mar|apr|may|jun|jul|aug|sep|oct|nov|dec)[a-z]* \d{4}\b/gi, '')
      .replace(/\b\d{4}-\d{1,2}-\d{1,2}\b|\b\d{1,2}\/\d{1,2}\/\d{2,4}\b/g, '')
      .replace(/^\s+|\s+$/g, '');
  }
  function mask(t) { return t.toLowerCase().replace(/\d+/g, '#'); }
  // Last resort: a numbered list of parts whose titles all differ ("1: The Start",
  // "2: Next Day", or "Chapter 1 - ...") and whose addresses differ only in their numbers,
  // wherever on the site they live. The numbers have to count up from 1 without a gap.
  function numberedList() {
    var sets = {}, names = [];
    for (var i = 0; i < links.length; i++) {
      var a = links[i], u = parse(a.href);
      if (!u || u.host !== here.host || u.path === here.path) continue;
      var t = (a.textContent || '').replace(/\s+/g, ' ').replace(/^\s+|\s+$/g, '');
      var m = /^(?:(?:chapter|part|episode|ch\.?|pt\.?|ep\.?)\s*)?(\d{1,3})\s*[:.)–—-]\s*\S/i.exec(t);
      if (!m) continue;
      var k = u.path.replace(/\d+/g, '#');
      if (k.indexOf('#') < 0) continue;
      if (!sets[k]) { sets[k] = { byNum: {}, count: 0 }; names.push(k); }
      var n = parseInt(m[1], 10), url = u.origin + u.path;
      if (sets[k].byNum[n]) continue;      // the first link with that number wins
      sets[k].byNum[n] = { url: url, title: t };
      sets[k].count++;
    }
    var best = null;
    for (var s = 0; s < names.length; s++) {
      var set = sets[names[s]], run = 0;
      while (set.byNum[run + 1]) run++;
      if (run < 2 || run !== set.count) continue;
      if (!best || run > best.count) best = set;
    }
    if (!best) return JSON.stringify({ chapters: [], title: '' });
    var list = [];
    for (var c = 1; c <= best.count; c++) list.push(best.byNum[c]);
    return JSON.stringify({ chapters: list, title: document.title || '' });
  }
  var listPage = p.charAt(p.length - 1) === '/';
  var hereKey = mask((document.title || '').replace(/\s+/g, ' '));
  var groups = {}, order = [];
  for (var g = 0; g < links.length; g++) {
    var la = links[g], lu = parse(la.href);
    if (!lu || lu.host !== here.host || lu.path === here.path) continue;
    var t = textOf(la);
    if (t.length < 15 || !/\d/.test(t) || /\d{4,}/.test(t)) continue;
    var k = mask(t);
    if (!groups[k]) { groups[k] = { items: [], nums: {}, urls: {} }; order.push(k); }
    var gr = groups[k], lurl = lu.origin + lu.path;
    if (gr.urls[lurl]) continue;
    gr.urls[lurl] = true;
    gr.nums[(t.match(/\d+/g) || []).join('.')] = true;
    gr.items.push({ url: lurl, title: t, key: natKey(t) });
  }
  var bestK = null;
  for (var b = 0; b < order.length; b++) {
    var cand = groups[order[b]], distinct = 0;
    for (var nk in cand.nums) if (cand.nums.hasOwnProperty(nk)) distinct++;
    if (distinct < 2 || (!listPage && hereKey.indexOf(order[b].replace(/^\[[^\]]*\]\s*/, '')) < 0)) continue;
    if (!bestK || cand.items.length > groups[bestK].items.length) bestK = order[b];
  }
  if (!bestK) return numberedList();
  var items = groups[bestK].items;
  if (!listPage) items.push({ url: here.origin + here.path, title: document.title || '', key: natKey(document.title || '') });
  items.sort(function (x, y) { return x.key < y.key ? -1 : x.key > y.key ? 1 : 0; });
  for (var r = 0; r < items.length; r++) delete items[r].key;
  var name = items[0].title.replace(/\s*\d+\s*/g, ' ').replace(/\s+/g, ' ').replace(/^\s+|\s+$/g, '');
  return JSON.stringify({ chapters: items, title: name });
})();
