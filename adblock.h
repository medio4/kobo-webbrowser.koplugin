// Ad and tracker blocking: requests to hosts on a hosts-style blocklist (and their
// subdomains) fail at once instead of going to the network. The list ships as
// adblock-hosts.txt next to the binary and is held as a sorted array of 64-bit host
// hashes (~8 bytes per entry), so a lookup is a few binary searches per request.
// Top-level page navigations are never blocked (tapping a link always works).
//   KB_NO_ADBLOCK=1          off for this run
//   KB_ADBLOCK_LIST=path     use another list (one host per line; "0.0.0.0 host" also accepted)
//   KB_ADBLOCK_LOG=1         log every blocked URL
// The on/off choice made with setEnabled() persists in adblock-off.txt beside the binary.
#ifndef ADBLOCK_H
#define ADBLOCK_H
#include <QtCore/QObject>
#include <QtCore/QUrl>
#include <QtCore/QVector>
#include <QtNetwork/QNetworkReply>

class AdBlock : public QObject {
    Q_OBJECT
public:
    static AdBlock *instance();
    bool isEnabled() const { return m_enabled; }
    // True when this request should be dropped.
    bool shouldBlock(const QUrl &url);
    // The main frame is about to load this URL: let it through even if listed.
    void allowNavigation(const QUrl &url) { m_navUrl = url; }
    int blockedSinceReset() const { return m_blocked; }
    void resetCount() { m_blocked = 0; }
public slots:
    void setEnabled(bool on);
    void toggle() { setEnabled(!m_enabled); }
signals:
    void enabledChanged(bool on);
private:
    explicit AdBlock(QObject *parent = 0);
    void load();
    QVector<quint64> m_hashes;   // sorted
    QUrl m_navUrl;
    bool m_enabled;
    bool m_loaded;
    bool m_log;
    int m_blocked;
};

// A reply that fails at once without touching the network.
class BlockedReply : public QNetworkReply {
    Q_OBJECT
public:
    BlockedReply(const QNetworkRequest &req, QNetworkAccessManager::Operation op, QObject *parent);
    void abort() {}
    qint64 bytesAvailable() const { return 0; }
    bool isSequential() const { return true; }
protected:
    qint64 readData(char *, qint64) { return -1; }
private slots:
    void fail();
};
#endif
