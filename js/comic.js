// Comic / manga pages: the page's images in document order, for a CBZ (epub.cpp).
// ES5 only (the firmware's old JavaScriptCore). Returns a JSON string:
// {title, images: [url...], big: n, bigHeight: px, docHeight: px, text: chars}
(function () {
  var a = document.createElement('a');
  function abs(u) { a.href = u; return a.href; }
  function pick(img) {
    var attrs = ['data-src', 'data-original', 'data-lazy-src', 'data-lazy', 'data-url', 'data-echo', 'data-cfsrc', 'src'];
    for (var i = 0; i < attrs.length; i++) {
      var v = img.getAttribute(attrs[i]);
      if (!v) continue;
      v = v.replace(/^\s+|\s+$/g, '');
      if (!v || /^data:image\/(gif|svg)/i.test(v) || /(blank|placeholder|loading|lazy)\.(gif|png|svg)/i.test(v)) continue;
      return v;
    }
    var ss = img.getAttribute('srcset') || img.getAttribute('data-srcset');
    if (ss) {
      var parts = ss.split(',');
      var last = parts[parts.length - 1].replace(/^\s+/, '').split(/\s+/)[0];
      if (last) return last;
    }
    return '';
  }
  var skipTags = { NAV: 1, HEADER: 1, FOOTER: 1, ASIDE: 1 };
  var bad = /(logo|icon|avatar|banner|sprite|emoji|button|badge|thumb|spinner|loading|gravatar|advert|\bads?\b)/i;
  var out = [], seen = {}, big = 0, bigHeight = 0;
  var vw = window.innerWidth || document.documentElement.clientWidth || 1000;
  var imgs = document.getElementsByTagName('img');
  for (var i = 0; i < imgs.length; i++) {
    var img = imgs[i], p = img.parentNode, skip = false;
    while (p && p !== document.body) {
      if (skipTags[p.tagName]) { skip = true; break; }
      p = p.parentNode;
    }
    if (skip) continue;
    var src = pick(img);
    if (!src || /^javascript:/i.test(src)) continue;
    var u = /^data:/i.test(src) ? src : abs(src);
    if (bad.test((img.className || '') + ' ' + (img.id || '') + ' ' + (img.getAttribute('alt') || '') + ' ' + u)) continue;
    var wa = parseInt(img.getAttribute('width'), 10), ha = parseInt(img.getAttribute('height'), 10);
    if ((wa && wa < 200) || (ha && ha < 200)) continue;
    out.push({ u: u, el: img });
  }
  // Keep the block that holds the pages: the deepest element with at least 70% of the
  // pictures. Covers of other posts, share images and sidebars sit outside it.
  var counts = [], owners = [];
  for (var j = 0; j < out.length; j++) {
    for (var q = out[j].el.parentNode; q && q !== document; q = q.parentNode) {
      var k = owners.indexOf(q);
      if (k < 0) { owners.push(q); counts.push(1); } else counts[k]++;
    }
  }
  var best = null, bestDepth = -1;
  for (var m = 0; m < owners.length; m++) {
    if (counts[m] < Math.max(3, Math.ceil(out.length * 0.7))) continue;
    var depth = 0;
    for (var z = owners[m]; z; z = z.parentNode) depth++;
    if (depth > bestDepth) { bestDepth = depth; best = owners[m]; }
  }
  // A list of other posts (an archive with a cover per post): each picture links to a
  // different page. Not a comic, even when the covers are big.
  function linkOf(el) { for (var q = el.parentNode; q && q !== document.body; q = q.parentNode) if (q.tagName === 'A') return q; return null; }
  var targets = {}, ntargets = 0, linked = 0;
  for (var lt = 0; lt < out.length; lt++) {
    var la = linkOf(out[lt].el);
    if (!la || !la.href || /\.(jpe?g|png|gif|webp)(\?|$)/i.test(la.href) || la.href === document.URL) continue;
    linked++;
    if (!targets[la.href]) { targets[la.href] = 1; ntargets++; }
  }
  var listing = ntargets >= 3 && linked * 2 >= out.length;
  var urls = [];
  for (var t = 0; t < out.length; t++) {
    if (best && !best.contains(out[t].el)) continue;
    if (seen[out[t].u]) continue;          // after the block test: a cover above the comic may repeat page 1
    seen[out[t].u] = 1;
    urls.push(out[t].u);
    var r = out[t].el.getBoundingClientRect();
    if (r && r.width >= vw * 0.5 && r.height >= 300) { big++; bigHeight += r.height; }
  }
  var de = document.documentElement;
  return JSON.stringify({
    title: document.title || '',
    images: urls,
    listing: listing,
    big: big,
    bigHeight: Math.round(bigHeight),
    docHeight: Math.max(de.scrollHeight || 0, document.body ? document.body.scrollHeight : 0),
    text: document.body ? (document.body.innerText || document.body.textContent || '').length : 0
  });
})()
