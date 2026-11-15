// Site menu: many sites open their own menu (the "hamburger" button) with JavaScript that
// the firmware's WebKit can't run, so tapping the button does nothing. The links of that
// menu are in the page all the same. When a tap on what looks like a menu button changes
// nothing, the browser lists the page's navigation links itself (Browser::showSiteMenu).
//
// Both scripts are plain ES5, run in the page's main frame.
#ifndef KB_SITEMENU_H
#define KB_SITEMENU_H

// Installed after each page load. On a click on a menu button (a small button or
// script-only link whose id, class or label says menu / hamburger / nav toggle), it
// watches the document for 400 ms. A menu that works changes something (a class, a style,
// new elements) or leaves the page; if nothing happened, window.__kbMenuDead is set.
// Left alone: real links, and <label>/<summary> menus, which work without scripts.
static const char *const kSiteMenuWatchJs =
    "(function(){"
    "if(window.__kbMenuWatch)return;window.__kbMenuWatch=1;"
    "var RE=/(^|[^a-z])(menu|hamburger|burger|nav-?toggle|navbar-toggler|nav-?button|nav-?icon|drawer|sidebar-toggle)/i;"
    "function toggle(el){"
    "for(var i=0;i<5&&el&&el.nodeType==1;i++,el=el.parentNode){"
    "var t=el.tagName.toLowerCase();"
    "if(t=='label'||t=='input'||t=='summary'||t=='select'||t=='textarea')return null;"
    "var href=t=='a'?(el.getAttribute('href')||''):'';"
    "if(href&&!/^(#|javascript:)/i.test(href))return null;"
    "var s=(el.id||'')+' '+(typeof el.className=='string'?el.className:'')+' '+(el.getAttribute('aria-label')||'')"
    "+' '+(el.getAttribute('title')||'')+' '+(el.getAttribute('data-testid')||'');"
    "var ctl=t=='button'||t=='a'||el.getAttribute('role')=='button';"
    "if((ctl&&RE.test(s))||(el.hasAttribute('aria-expanded')&&el.hasAttribute('aria-controls')"
    "&&/menu|nav|drawer|sidebar/i.test(s+' '+el.getAttribute('aria-controls')))){"
    "var r=el.getBoundingClientRect();return r.width<=200&&r.height<=200?el:null;}"
    "}return null;}"
    "document.addEventListener('click',function(ev){"
    "if(!toggle(ev.target))return;"
    "function here(){return location.href.replace(/#.*$/,'');}var n=0,href=here(),MO=window.MutationObserver||window.WebKitMutationObserver;"
    "if(!MO)return;"
    "function count(recs){for(var i=0;i<recs.length;i++){var r=recs[i];"
    "if(r.type=='childList')n+=r.addedNodes.length+r.removedNodes.length;"
    "else if(/^(class|style|hidden|open|aria-expanded|aria-hidden)$/.test(r.attributeName||''))n++;}}"
    "var ob=new MO(count);"
    "ob.observe(document.documentElement,{attributes:true,childList:true,subtree:true});"
    "setTimeout(function(){count(ob.takeRecords());ob.disconnect();"
    "if(n==0&&here()==href)window.__kbMenuDead=1;},400);"
    "},true);"
    "})()";

// Did the last tap hit a menu button that did nothing? Clears the mark.
static const char *const kSiteMenuDeadJs =
    "(function(){var d=window.__kbMenuDead;window.__kbMenuDead=0;return d?1:0;})()";

// The page's navigation links, one "title<TAB>url" per line: links inside <nav> and
// role=navigation first; if that gives too few, links in the header and in anything whose
// id or class says menu or nav. No duplicates, no long texts, at most 96.
static const char *const kSiteMenuLinksJs =
    "(function(){"
    "var out=[],seen={};"
    "function add(list){for(var i=0;i<list.length&&out.length<96;i++){var a=list[i],u=a.href||'';"
    "if(!/^https?:/i.test(u))continue;"
    "var k=u.replace(/#.*$/,'');if(seen[k])continue;"
    "var t=(a.textContent||'').replace(/\\s+/g,' ').replace(/^ | $/g,'');"
    "if(!t)t=a.getAttribute('aria-label')||a.getAttribute('title')||'';"
    "if(!t||t.length>60)continue;seen[k]=1;out.push(t+'\\t'+u);}}"
    "add(document.querySelectorAll('header nav a[href],nav a[href],[role=navigation] a[href]'));"
    "if(out.length<3)add(document.querySelectorAll('header a[href],[class*=menu] a[href],[class*=nav] a[href],"
    "[id*=menu] a[href],[id*=nav] a[href]'));"
    "return out.join('\\n');})()";

#endif
