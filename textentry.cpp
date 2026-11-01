#include <string.h>
#include "textentry.h"
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QDebug>
#include <QtCore/QStringList>
#include <stdlib.h>

NativeTextEntry::NativeTextEntry(const char *reqEnv, const char *replyEnv, QObject *parent)
    : QObject(parent), m_active(false), m_pickedUp(false)
{
    // The keyboard channel (KB_KBD_FILE) is used only when asked for explicitly
    // (KB_NATIVE_KBD=1, the plugin's experimental "Use KOReader keyboard" option); a plugin
    // version KOReader loaded before an update may still pass KB_KBD_FILE on its own.
    // Other channels (the KOReader menu hand-off, KB_MENU_FILE) are always on.
    bool enabled = true;
    if (!strcmp(reqEnv, "KB_KBD_FILE")) {
        const char *on = getenv("KB_NATIVE_KBD");
        enabled = on && atoi(on) != 0 && !getenv("KB_NO_NATIVE_KBD");
    }
    if (enabled) {
        m_reqPath = QString::fromLocal8Bit(getenv(reqEnv));
        m_replyPath = QString::fromLocal8Bit(getenv(replyEnv));
    }
    m_timer.setInterval(100);
    connect(&m_timer, SIGNAL(timeout()), this, SLOT(poll()));
    if (available()) {
        QFile::remove(m_reqPath); QFile::remove(m_replyPath);
        qDebug() << "textentry: KOReader hand-off via" << m_reqPath;
    }
}

static bool writeAtomically(const QString &path, const QByteArray &data)
{
    QFile f(path + ".tmp");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(data);
    f.close();
    QFile::remove(path);
    return QFile::rename(path + ".tmp", path);
}

bool NativeTextEntry::request(const QString &kind, const QString &label, const QString &current)
{
    if (!available() || m_active) return false;
    QFile::remove(m_replyPath);
    QString lbl = label; lbl.replace('\n', ' ');
    QByteArray data = (kind + "\n" + lbl + "\n" + current).toUtf8();
    if (!writeAtomically(m_reqPath, data)) {
        qWarning() << "textentry: cannot write" << m_reqPath;
        return false;
    }
    m_active = true;
    m_pickedUp = false;
    m_since.start();
    m_timer.start();
    emit started();
    return true;
}

void NativeTextEntry::cancel()
{
    if (!m_active) return;
    m_timer.stop();
    m_active = false;
    QFile::remove(m_reqPath);
}

void NativeTextEntry::poll()
{
    if (!m_active) { m_timer.stop(); return; }
    if (!m_pickedUp) {
        if (!QFile::exists(m_reqPath)) {
            m_pickedUp = true;
        } else if (m_since.elapsed() > 2500) {
            // The plugin isn't listening (old plugin, or launched another way).
            qWarning() << "textentry: KOReader didn't pick up" << m_reqPath;
            cancel();
            emit unavailable();
            return;
        }
    }
    QFile f(m_replyPath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;
    QByteArray data = f.readAll();
    f.close();
    QFile::remove(m_replyPath);
    m_timer.stop();
    m_active = false;
    QString s = QString::fromUtf8(data);
    int nl = s.indexOf('\n');
    QString action = nl < 0 ? s.trimmed() : s.left(nl).trimmed();
    QString text = nl < 0 ? QString() : s.mid(nl + 1);
    if (text.endsWith('\n')) text.chop(1);
    qDebug() << "textentry: reply" << action << text.left(60);
    emit replied(action, text);
}
