// "Send to Kobo": a tiny HTTP server inside the browser. A phone on the same Wi-Fi opens
// http://<kobo-ip>:<port>/, pastes a link, and the browser loads it.
#ifndef PHONESHARE_H
#define PHONESHARE_H

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QByteArray>
#include <QtCore/QMap>
#include <QtNetwork/QTcpServer>

class QTcpSocket;

class PhoneShare : public QObject {
    Q_OBJECT
public:
    explicit PhoneShare(QObject *parent = 0);
    bool listen(quint16 port);
    QString address() const;            // "http://192.168.1.5:8080/" or empty when no LAN address
    quint16 port() const { return m_port; }
    void setCurrentPage(const QString &title, const QString &url) { m_pageTitle = title; m_pageUrl = url; }

signals:
    void urlReceived(const QString &url);

private slots:
    void newConnection();
    void readyRead();

private:
    void handle(QTcpSocket *s, const QByteArray &request);
    QByteArray page(const QString &notice) const;
    static QMap<QString, QString> params(const QByteArray &query);

    QTcpServer m_server;
    quint16 m_port;
    QMap<QTcpSocket *, QByteArray> m_buf;
    QString m_pageTitle, m_pageUrl;
    QStringList m_recent;
};


#include <QtWidgets/QWidget>
class QResizeEvent;
class QMouseEvent;

// Overlay with a QR code of the phone page's address (scan it with the phone's camera).
class PhoneDialog : public QWidget {
    Q_OBJECT
public:
    PhoneDialog(const QString &url, QWidget *parent);
signals:
    void dismissed();
protected:
    void resizeEvent(QResizeEvent *e);
    void mousePressEvent(QMouseEvent *e);
private:
    void place();
    QWidget *m_box;
};

#endif
