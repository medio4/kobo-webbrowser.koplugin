#include "webpage.h"
#include "memtune.h"
#include "adblock.h"
#include <QtWebKit/QWebSettings>
#include <QtWebKitWidgets/QWebFrame>
#include <QtNetwork/QNetworkRequest>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QTimer>
#include <QtCore/QStringList>
#include <QtCore/QDebug>
#include <cstdlib>

static const char *kMobileUA =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 9_3 like Mac OS X) AppleWebKit/601.1.46 (KHTML, like Gecko) "
    "Version/9.0 Mobile/13E233 Safari/601.1 KoboBrowser/0.1";

static const char *kEinkCss =
    "html,body{background:#fff !important;color:#000 !important}"
    "a,a:visited,a:link{color:#000 !important;text-decoration:underline !important}"
    "*{text-shadow:none !important;box-shadow:none !important;transition:none !important;animation:none !important}"
    "img,video{max-width:100% !important;height:auto}"
    "body{line-height:1.45}"
    // Wikipedia (Vector 2022 / Minerva): hide chrome that this WebKit can't lay out.
    ".vector-header-container,.vector-page-toolbar,.vector-column-start,.vector-column-end,"
    ".vector-sticky-header,#p-lang-btn,.vector-page-titlebar-toc,#vector-toc-pinned-container,"
    ".mw-footer-container,#mw-navigation,.header-container,.minerva-footer,.mw-editsection,"
    ".mw-indicators,.pre-content .page-actions-menu,.last-modified-bar{display:none !important}"
    ".mw-body,.mw-page-container,.mw-content-container,.vector-body{margin:0 !important;padding:0 8px !important;max-width:none !important}";

// Bing Images stands in for Google Images (see mobileVariant). Its results page draws
// its thumbnails and viewer with JavaScript this WebKit can't run, so the page is
// replaced with a plain grid built from the result data Bing puts in each tile's link:
// thumbnail (JPEG) -> full image, the source site under it, a search box, and a
// "More images" link for the next page.
static const char *kBingImagesJs =
    "(function(){"
    "if(document.getElementById('kb-grid'))return;"
    // Results: each tile's link carries JSON (title, thumbnail, full image, source page).
    "var a=document.querySelectorAll('a.iusc[m],a[m]'),r=[];"
    "for(var j=0;j<a.length;j++){try{var m=JSON.parse(a[j].getAttribute('m'));"
    " if(m.turl&&m.murl)r.push(m);}catch(e){}}"
    "if(!r.length)return;"
    "var q=(location.search.match(/[?&]q=([^&]*)/)||[])[1]||'';"
    "var f=parseInt((location.search.match(/[?&]first=(\\d+)/)||[])[1]||'1',10);"
    "function esc(s){return String(s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/\"/g,'&quot;');}"
    "function host(u){var x=document.createElement('a');x.href=u;return x.hostname.replace(/^www\\./,'');}"
    "var dq='';try{dq=decodeURIComponent(q.replace(/\\+/g,' '));}catch(e){dq=q;}"
    "var h='<div id=\"kb-grid\" style=\"font-family:sans-serif;padding:6px\">'"
    " +'<form action=\"/images/search\" method=\"get\" style=\"margin:4px 0 10px\">'"
    " +'<input name=\"q\" value=\"'+esc(dq)+'\" style=\"width:70%;font-size:1.1em;padding:6px;border:2px solid #000;border-radius:6px\">'"
    " +' <input type=\"submit\" value=\"Search\" style=\"font-size:1.1em;padding:6px 12px;border:2px solid #000;border-radius:6px;background:#fff\"></form>';"
    "for(var k=0;k<r.length;k++){var m=r[k];"
    " h+='<div style=\"display:inline-block;vertical-align:top;width:48%;margin:0 1% 14px;text-align:center\">'"
    "  +'<a href=\"'+esc(m.murl)+'\"><img src=\"'+esc(m.turl)+'\" style=\"max-width:100%;max-height:260px;border:1px solid #888\"></a>'"
    "  +'<div style=\"font-size:.8em;overflow:hidden;white-space:nowrap;text-overflow:ellipsis\">'+esc(m.t||'')+'</div>'"
    "  +(m.purl?'<a href=\"'+esc(m.purl)+'\" style=\"font-size:.75em;color:#444\">'+esc(host(m.purl))+'</a>':'')+'</div>';}"
    "h+='<a id=\"kb-more\" href=\"/images/search?q='+q+'&first='+(f+r.length)+'\" style=\"display:block;font-size:1.3em;padding:16px;text-align:center\">More images \\u203a</a></div>';"
    // Replace Bing's script-driven page with this plain grid; its styles go with it.
    "var ss=document.querySelectorAll('style,link[rel=stylesheet]');for(var s=0;s<ss.length;s++)ss[s].parentNode.removeChild(ss[s]);"
    "document.body.setAttribute('style','margin:0;background:#fff');"
    "document.body.innerHTML=h;window.scrollTo(0,0);"
    "})()";

static bool isBing(const QUrl &url)
{
    QString host = url.host().toLower();
    return host == "bing.com" || host.endsWith(".bing.com");
}

static bool isGoogle(const QString &host)
{
    // google.com, www.google.co.uk, images.google.de ...
    QStringList parts = host.split('.');
    if (parts.size() < 2) return false;
    if (parts.first() == "www" || parts.first() == "images") parts.removeFirst();
    if (parts.size() < 2 || parts.first() != "google") return false;
    return parts.size() <= 3;
}

KoboWebPage::KoboWebPage(QObject *parent) : QWebPage(parent)
{
    connect(mainFrame(), SIGNAL(loadFinished(bool)), this, SLOT(fixupSite()));
    connect(mainFrame(), SIGNAL(loadStarted()), this, SLOT(adblockReset()));
    connect(mainFrame(), SIGNAL(loadFinished(bool)), this, SLOT(adblockReport()));
    setNetworkAccessManager(new KoboNetworkAccessManager(this));
    if (!getenv("KB_NO_EINK_CSS")) {
        QByteArray css(kEinkCss);
        settings()->setUserStyleSheetUrl(QUrl("data:text/css;charset=utf-8;base64," + css.toBase64()));
    }
}

QString KoboWebPage::userAgentForUrl(const QUrl &url) const
{
    const char *ua = getenv("KB_USER_AGENT");
    if (ua && *ua) return QString::fromLocal8Bit(ua);
    if (getenv("KB_DESKTOP_UA")) return QWebPage::userAgentForUrl(url);
    return QString::fromLatin1(kMobileUA);
}

QUrl KoboWebPage::mobileVariant(const QUrl &url)
{
    if (getenv("KB_NO_MOBILE_SITES")) return url;
    QString host = url.host().toLower();
    // (Wikipedia now serves its mobile layout on the main host based on the user agent,
    //  and redirects xx.m.wikipedia.org back, so no rewrite for it.)
    // Google Images now builds its results only with JavaScript this WebKit can't run,
    // and sends no-JS browsers an "update your browser" page, so send image searches
    // to Bing Images (fixed up in fixupSite).
    if (isGoogle(host)) {
        QUrlQuery query(url);
        QString path = url.path();
        bool images = host.startsWith("images.") || path.startsWith("/imghp")
            || query.queryItemValue("tbm") == "isch" || query.queryItemValue("udm") == "2";
        if (images) {
            QString q = query.queryItemValue("q", QUrl::FullyDecoded);
            QUrl u("https://www.bing.com/images/search");
            if (!q.isEmpty()) { QUrlQuery bq; bq.addQueryItem("q", q); u.setQuery(bq); }
            else u.setPath("/images/");
            return u;
        }
    }
    // A Bing Images tile that still points at Bing's JavaScript viewer: open the image.
    if (isBing(url) && url.path().startsWith("/images/search")) {
        QUrlQuery query(url);
        if (query.queryItemValue("view") == "detailV2") {
            QString media = query.queryItemValue("mediaurl", QUrl::FullyDecoded);
            if (media.isEmpty()) media = query.queryItemValue("cdnurl", QUrl::FullyDecoded);
            QUrl m(media);
            if (m.isValid() && (m.scheme() == "http" || m.scheme() == "https")) return m;
        }
    }
    if (host == "reddit.com" || host == "www.reddit.com") { QUrl u = url; u.setHost("old.reddit.com"); return u; }
    if (host == "duckduckgo.com" || host == "www.duckduckgo.com") {
        if (!url.path().startsWith("/lite")) { QUrl u = url; u.setHost("lite.duckduckgo.com"); u.setPath("/lite/"); return u; }
    }
    return url;
}

bool KoboWebPage::acceptNavigationRequest(QWebFrame *frame, const QNetworkRequest &request, NavigationType type)
{
    QUrl alt = mobileVariant(request.url());
    if (alt != request.url() && frame == mainFrame()) {
        qDebug() << "redirecting to mobile site" << alt.toString();
        mainFrame()->load(alt);
        return false;
    }
    bool ok = QWebPage::acceptNavigationRequest(frame, request, type);
    // A page the user asked for loads even if its host is on the blocklist;
    // only subresources and subframes are filtered.
    if (ok && (!frame || frame == mainFrame())) AdBlock::instance()->allowNavigation(request.url());
    return ok;
}

void KoboWebPage::adblockReset()
{
    AdBlock::instance()->refresh();   // settings changed in KOReader's menu apply from here on
    AdBlock::instance()->resetCount();
}

void KoboWebPage::adblockReport()
{
    AdBlock *a = AdBlock::instance();
    if (a->isEnabled())
        qDebug() << "adblock:" << a->blockedSinceReset() << "requests blocked on" << mainFrame()->url().host();
}

void KoboWebPage::fixupSite()
{
    if (const char *probe = getenv("KB_JS_PROBE"))   // debugging: log a JS expression after each load
        qDebug() << "probe:" << mainFrame()->evaluateJavaScript(QString::fromLocal8Bit(probe)).toString();
    if (!isBing(mainFrame()->url())) return;
    runBingFixup();
    // Bing's own scripts may rebuild tiles shortly after load; go over them again.
    QTimer::singleShot(1500, this, SLOT(runBingFixup()));
}

void KoboWebPage::runBingFixup()
{
    if (isBing(mainFrame()->url())) mainFrame()->evaluateJavaScript(QString::fromLatin1(kBingImagesJs));
}


// ------------------------------------------------------------- error page

bool KoboWebPage::supportsExtension(Extension extension) const
{
    return extension == ErrorPageExtension || QWebPage::supportsExtension(extension);
}

static QString htmlEscape(const QString &s)
{
    QString t = s;
    t.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;");
    return t;
}

bool KoboWebPage::extension(Extension extension, const ExtensionOption *option, ExtensionReturn *output)
{
    if (extension != ErrorPageExtension || !option || !output)
        return QWebPage::extension(extension, option, output);
    const ErrorPageExtensionOption *opt = static_cast<const ErrorPageExtensionOption *>(option);
    ErrorPageExtensionReturn *ret = static_cast<ErrorPageExtensionReturn *>(output);
    // Only for the page itself; a missing image or a blocked ad stays silent.
    if (opt->frame != mainFrame()) return false;
    // Cancelled loads (Stop, a new link tapped meanwhile) are not errors.
    if (opt->domain == QtNetwork && opt->error == 5 /* OperationCanceledError */) return false;
    QString host = opt->url.host().isEmpty() ? opt->url.toString() : opt->url.host();
    QString why = opt->errorString;
    if (opt->domain == QtNetwork) {
        switch (opt->error) {
        case 3: why = "That site could not be found. Check the address, or the Wi-Fi connection."; break;   // HostNotFound
        case 1: why = "The site refused the connection."; break;
        case 4: why = "The site took too long to answer."; break;
        case 99: case 2: why = "The connection was lost. Check the Wi-Fi and try again."; break;
        default: break;
        }
    } else if (opt->domain == Http) {
        why = QString("The site answered with an error (HTTP %1).").arg(opt->error);
    }
    QString u = htmlEscape(opt->url.toString());
    QString html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Could not open " + htmlEscape(host) +
        "</title><style>body{font-family:sans-serif;font-size:22px;margin:40px 30px;line-height:1.5}"
        "h1{font-size:30px}a.b{display:inline-block;border:2px solid #000;border-radius:8px;padding:12px 28px;"
        "color:#000;text-decoration:none;margin-top:20px}</style></head><body>"
        "<h1>Could not open " + htmlEscape(host) + "</h1><p>" + htmlEscape(why) + "</p>"
        "<p style=\"word-break:break-all;color:#444\">" + u + "</p>"
        "<a class=\"b\" href=\"" + u + "\">Try again</a></body></html>";
    ret->content = html.toUtf8();
    ret->contentType = "text/html";
    ret->encoding = "utf-8";
    ret->baseUrl = opt->url;
    return true;
}
