#include "remotecmd.h"
#include <QtCore/QSocketNotifier>
#include <QtCore/QDebug>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <cstdlib>

RemoteCommands::RemoteCommands(QObject *parent) : QObject(parent), m_fd(-1), m_notifier(0)
{
    const char *p = getenv("KB_CMD_FIFO");
    if (!p || !*p) return;
    m_path = QString::fromLocal8Bit(p);
    unlink(p);   // a leftover from a browser that was killed
    if (mkfifo(p, 0600) != 0) { qWarning() << "commands: cannot create" << m_path << strerror(errno); return; }
    // Read-write: the FIFO then always has a writer (us), so it never reports end-of-file
    // between two commands, and the plugin's open for writing never blocks.
    m_fd = open(p, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) { qWarning() << "commands: cannot open" << m_path << strerror(errno); unlink(p); return; }
    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, SIGNAL(activated(int)), this, SLOT(readable()));
    qDebug() << "commands: listening on" << m_path;
}

RemoteCommands::~RemoteCommands()
{
    if (m_fd >= 0) { close(m_fd); unlink(m_path.toLocal8Bit().constData()); }
}

void RemoteCommands::readable()
{
    char buf[256];
    for (;;) {
        ssize_t n = read(m_fd, buf, sizeof buf);
        if (n <= 0) break;
        m_buf.append(buf, n);
    }
    if (m_buf.size() > 4096) m_buf.clear();   // nothing we know is that long
    int nl;
    while ((nl = m_buf.indexOf('\n')) >= 0) {
        QString word = QString::fromLatin1(m_buf.left(nl)).trimmed();
        m_buf.remove(0, nl + 1);
        if (!word.isEmpty()) emit command(word);
    }
}
