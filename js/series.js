// Runs inside the page. Finds the chapters of a multi-part work that this page belongs to
// (or lists): same-site links under the current page's folder, in document order, then
// sorted naturally by the numbers in their paths. Returns JSON {chapters:[{url,title}]}.
(function () {
  function natKey(s) {
    return s.toLowerCase().replace(/\d+/g, function (n) { return ('00000000' + n).slice(-8); });
  }
  var here, base;
  try {
    here = new URL(document.URL);
    var p = here.pathname;
    base = p.endsWith('/') ? p : p.slice(0, p.lastIndexOf('/') + 1);
  } catch (e) { return JSON.stringify({ chapters: [], error: String(e) }); }
  var seen = {}, out = [];
  var links = document.getElementsByTagName('a');
  for (var i = 0; i < links.length; i++) {
    var a = links[i], href = a.getAttribute('href');
    if (!href || href.charAt(0) === '#' || /^(javascript|mailto):/i.test(href)) continue;
    var u;
    try { u = new URL(href, document.baseURI); } catch (e2) { continue; }
    if (u.host !== here.host) continue;
    var path = u.pathname;
    if (path.indexOf(base) !== 0 || path === here.pathname || path === base) continue;
    if (/\.(css|js|png|jpe?g|gif|svg|ico|xml|rss|pdf|zip|mp3|mp4)$/i.test(path)) continue;
    var key = u.origin + path;
    if (seen[key]) continue;
    seen[key] = true;
    var title = (a.textContent || '').replace(/\s+/g, ' ').trim();
    if (!title) title = decodeURIComponent(path.replace(base, '').replace(/\/$/, ''));
    out.push({ url: u.origin + path, title: title, key: natKey(path) });
  }
  // Only keep "leaf" links (files or single folders), not deep sub-trees.
  out = out.filter(function (c) {
    var rest = c.url.substring(c.url.indexOf(base) + base.length);
    return rest.replace(/\/$/, '').indexOf('/') < 0;
  });
  out.sort(function (a, b) { return a.key < b.key ? -1 : a.key > b.key ? 1 : 0; });
  for (var j = 0; j < out.length; j++) delete out[j].key;
  return JSON.stringify({ chapters: out, title: document.title || '' });
})();
