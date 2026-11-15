// Links that only work with the site's own scripts. Some sites write their links as
//   <a href="javascript:void(0)" data-nav-url="/story/123">
// and let a script do the navigation. When that script is written in JavaScript the
// firmware's WebKit can't run, every such link is dead: tapping it does nothing.
//
// Installed after each page load (plain ES5, main frame). On a click on a link whose href
// goes nowhere but which carries an address in a data- attribute, it watches the document
// for 400 ms. A site whose script works reacts (it shows a spinner, changes a class, leaves
// the page); if nothing at all happened, the browser opens the address itself.
#ifndef KB_DEADLINKS_H
#define KB_DEADLINKS_H

static const char *const kDeadLinksJs =
    "(function(){"
    "if(window.__kbDeadLinks)return;window.__kbDeadLinks=1;"
    "var ATTRS=['data-nav-url','data-href','data-url','data-link','data-target-url'];"
    "function target(el){"
    "for(var i=0;i<6&&el&&el.nodeType==1;i++,el=el.parentNode){"
    "if(el.tagName.toLowerCase()!='a')continue;"
    "var h=el.getAttribute('href');"
    "if(h&&!/^\\s*(#|javascript:)/i.test(h))return null;"
    "for(var k=0;k<ATTRS.length;k++){var u=el.getAttribute(ATTRS[k]);"
    "if(u&&/^(https?:\\/\\/|\\/|\\.\\.?\\/|\\?)/i.test(u))return u;}"
    "return null;}"
    "return null;}"
    "function here(){return location.href.replace(/#.*$/,'');}"
    "document.addEventListener('click',function(ev){"
    "var u=target(ev.target);if(!u)return;"
    "var MO=window.MutationObserver||window.WebKitMutationObserver;if(!MO)return;"
    "var n=0,href=here();"
    "function count(recs){for(var i=0;i<recs.length;i++){var r=recs[i];"
    "if(r.type=='childList')n+=r.addedNodes.length+r.removedNodes.length;"
    "else if(/^(class|style|hidden|open|aria-expanded|aria-hidden)$/.test(r.attributeName||''))n++;}}"
    "var ob=new MO(count);"
    "ob.observe(document.documentElement,{attributes:true,childList:true,subtree:true});"
    "setTimeout(function(){count(ob.takeRecords());ob.disconnect();"
    "if(n==0&&here()==href&&!ev.defaultPrevented)location.href=u;},400);"
    "},true);"
    "})()";

#endif
