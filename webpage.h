// QWebPage tuned for an e-ink reader: mobile user agent, mobile-site redirects,
// and an e-ink friendly user stylesheet.
#ifndef WEBPAGE_H
#define WEBPAGE_H
#include <QtWebKitWidgets/QWebPage>
class KoboWebPage : public QWebPage {
    Q_OBJECT
public:
    explicit KoboWebPage(QObject *parent = 0);
    // Rewrite URLs to simpler mobile variants where known (e.g. Wikipedia -> m.wikipedia).
    static QUrl mobileVariant(const QUrl &url);
protected:
    QString userAgentForUrl(const QUrl &url) const;
    bool acceptNavigationRequest(QWebFrame *frame, const QNetworkRequest &request, NavigationType type);
public:
    // A page that fails to load (no such site, no connection, timeout) shows a short
    // error page instead of silently keeping the previous one.
    bool supportsExtension(Extension extension) const;
    bool extension(Extension extension, const ExtensionOption *option = 0, ExtensionReturn *output = 0);
protected:
private slots:
    void fixupSite();
    void runBingFixup();
    void adblockReset();
    void adblockReport();
};
#endif
