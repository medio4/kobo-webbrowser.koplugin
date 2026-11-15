// Dark pages on e-ink. The browser's page style sheet makes text and links black (gray
// text is hard to read on this screen), which on a site with a dark theme gives black
// text on dark boxes. Large dark areas are also slow and smeary to refresh. So a page that
// is mostly dark boxes is turned light: white backgrounds, black text.
//
// kLightPageJs (plain ES5, run after a page has loaded and once more a little later for
// content that arrives late) measures how much of the page's coloured background area is
// dark and, past a third, puts the class "kb-light" on <html>; kLightPageCss (part of the
// page style sheet, webpage.cpp) does the rest, so content added later is covered too.
// Returns 1 if the page was (or already is) turned light.
#ifndef KB_LIGHTPAGE_H
#define KB_LIGHTPAGE_H

static const char *const kLightPageJs =
    "(function(){"
    "var d=document,r=d.documentElement;if(!d.body||!r)return 0;"
    "if(/(^| )kb-light( |$)/.test(r.className))return 1;"
    "function lum(c){var m=/rgba?\\((\\d+),\\s*(\\d+),\\s*(\\d+)(?:,\\s*([\\d.]+))?\\)/.exec(c||'');"
    "if(!m||(m[4]!==undefined&&parseFloat(m[4])<0.5))return -1;"
    "return(0.299*m[1]+0.587*m[2]+0.114*m[3])/255;}"
    "var els=d.body.querySelectorAll('*'),n=Math.min(els.length,2500),vw=window.innerWidth||600,dark=0,all=0;"
    "for(var i=0;i<n;i++){var e=els[i],t=e.tagName;"
    "if(t=='IMG'||t=='VIDEO'||t=='CANVAS'||t=='svg'||t=='SCRIPT'||t=='STYLE')continue;"
    "var l=lum(getComputedStyle(e).backgroundColor);if(l<0)continue;"
    "var b=e.getBoundingClientRect(),a=Math.min(b.width,vw)*Math.min(b.height,1500);"
    "if(a<15000)continue;all+=a;if(l<0.4)dark+=a;}"
    "if(all<60000||dark<all/3)return 0;"
    "r.className+=' kb-light';return 1;})()";

static const char *const kLightPageCss =
    "html.kb-light body,html.kb-light body *{background-color:#fff !important;color:#000 !important;"
    "border-color:#777 !important}"
    "html.kb-light body svg{fill:#000 !important}";

#endif
