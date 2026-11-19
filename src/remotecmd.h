// Commands from the KOReader plugin while the browser is on screen. KOReader keeps running
// behind us and still gets the device's buttons (we only take the touchscreen), so the plugin
// passes the page-turn buttons on: one word per line ("next", "prev") written to a FIFO
// whose path it gives us in KB_CMD_FIFO. The FIFO lives in /tmp: the user partition is FAT
// and can't hold one.
#ifndef KB_REMOTECMD_H
#define KB_REMOTECMD_H
#include <QtCore/QObject>
#include <QtCore/QByteArray>
#include <QtCore/QString>
class QSocketNotifier;

class RemoteCommands : public QObject {
    Q_OBJECT
public:
    explicit RemoteCommands(QObject *parent = 0);   // does nothing without KB_CMD_FIFO
    ~RemoteCommands();
    bool isOpen() const { return m_fd >= 0; }
signals:
    void command(const QString &word);
private slots:
    void readable();
private:
    int m_fd;
    QString m_path;
    QByteArray m_buf;
    QSocketNotifier *m_notifier;
};
#endif
