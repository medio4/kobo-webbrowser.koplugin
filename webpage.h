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
private slots:
    void fixupSite();
    void runBingFixup();
    void adblockReset();
    void adblockReport();
};
#endif
