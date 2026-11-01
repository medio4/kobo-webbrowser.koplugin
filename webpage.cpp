#include "webpage.h"
#include <QtWebKit/QWebSettings>
#include <QtWebKitWidgets/QWebFrame>
#include <QtNetwork/QNetworkRequest>
#include <QtCore/QUrl>
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

KoboWebPage::KoboWebPage(QObject *parent) : QWebPage(parent)
{
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
    return QWebPage::acceptNavigationRequest(frame, request, type);
}
