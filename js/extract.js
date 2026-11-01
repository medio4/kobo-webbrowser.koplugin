// Runs after prelude.js + Readability.js inside one function (see epub.cpp).
// Returns a JSON string describing the article, or {error: ...}.
function kbCleanFallback() {
  var d = document.body.cloneNode(true);
  var kill = 'script,style,iframe,noscript,svg,canvas,form,input,button,select,textarea,video,audio,link,meta,object,embed';
  var rm = d.querySelectorAll(kill);
  for (var i = 0; i < rm.length; i++) if (rm[i].parentNode) rm[i].parentNode.removeChild(rm[i]);
  var all = d.getElementsByTagName('*');
  for (var j = 0; j < all.length; j++) {
    var e = all[j], a = e.attributes;
    for (var k = a.length - 1; k >= 0; k--) { var n = a[k].name; if (n !== 'href' && n !== 'src' && n !== 'alt') e.removeAttribute(n); }
  }
  var imgs = d.getElementsByTagName('img');
  for (var m = 0; m < imgs.length; m++) { try { imgs[m].setAttribute('src', new URL(imgs[m].getAttribute('src') || '', document.baseURI).href); } catch (ex) {} }
  return d;
}
function kbSerialize(el) {
  return new XMLSerializer().serializeToString(el);
}
return (function () {
  var out = { url: document.URL, title: document.title || '', fallback: false };
  try {
    var r = new Readability(document.cloneNode(true), { serializer: kbSerialize });
    var art = r.parse();
    if (art && art.content && art.textContent && art.textContent.replace(/\s+/g, ' ').length > 200) {
      out.title = art.title || out.title;
      out.byline = art.byline || '';
      out.siteName = art.siteName || '';
      out.excerpt = art.excerpt || '';
      out.lang = art.lang || document.documentElement.getAttribute('lang') || '';
      out.content = art.content;
      return JSON.stringify(out);
    }
  } catch (e) {
    out.readabilityError = String(e) + (e && e.stack ? '\n' + e.stack : '');
  }
  try {
    out.fallback = true;
    out.lang = document.documentElement.getAttribute('lang') || '';
    out.content = kbSerialize(kbCleanFallback());
    return JSON.stringify(out);
  } catch (e2) {
    out.error = String(e2);
    return JSON.stringify(out);
  }
})();
