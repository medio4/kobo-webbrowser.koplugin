#include "eink.h"
#include "fbwriter.h"
#include "hwtcon.h"
#include <QtGui/QScreen>
#include <QtGui/QPixmap>
#include <QtGui/QBackingStore>
#include <QtGui/QPaintDevice>
#include <QtWidgets/QApplication>
#include <QtCore/QDebug>
#include <QtCore/QEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtGui/QPaintEvent>
#include <QtWidgets/QWidget>
#include <qpa/qplatformwindow.h>
#include <dlfcn.h>
#include <QtCore/QProcess>
#include <QtCore/QStringList>
#include <QtCore/QFile>
#include <stdlib.h>

static quint64 envFlags(const char *name, quint64 def)
{
    const char *v = getenv(name);
    if (!v || !*v) return def;
    return strtoull(v, 0, 0);
}

KoboEink::KoboEink(QObject *parent)
    : QObject(parent), m_repaint(0), m_dl(0), m_grab(false), m_ready(false), m_fb(0), m_hw(0)
{
    m_flagsPartial = envFlags("KB_FLAGS_PARTIAL", 0);
    m_flagsFast = envFlags("KB_FLAGS_FAST", 0x100);   // UI widgets with property kb_fast (keyboard, toolbar)
    m_flagsFull = envFlags("KB_FLAGS_FULL", 16);
    // Refresh backend: "fbink" (default, works with the linuxfb platform plugin) or "kobo"
    // (call KoboWindow::repaint inside Kobo's own platform plugin).
    const char *mode = getenv("KB_EINK");
    m_useFbink = !(mode && !strcmp(mode, "kobo"));
    m_grab = (mode && !strcmp(mode, "grab"));
    if (m_grab) {
        m_fb = new FbWriter;
        qDebug() << "eink: grab mode, framebuffer" << (m_fb->isOpen() ? "open" : "FAILED");
        if (m_fb->isOpen()) {
            m_hw = new HwtconRefresh(m_fb->fd());
            qDebug() << "eink: direct hwtcon refresh" << (m_hw->available() ? "enabled" : "disabled");
        }
    }
    if (m_useFbink) {
        const char *fb = getenv("KB_FBINK");
        m_fbink = (fb && *fb) ? QString::fromLocal8Bit(fb) : QString("/mnt/onboard/.adds/koreader/fbink");
        if (!QFile::exists(m_fbink)) qWarning() << "eink: fbink not found at" << m_fbink;
        qDebug() << "eink: using fbink" << m_fbink;
    } else {
        const char *path = getenv("KB_KOBO_PLUGIN");
        if (!path || !*path) path = "/usr/local/Kobo/platforms/libkobo.so";
        m_dl = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
        if (!m_dl) m_dl = dlopen(path, RTLD_NOW);
        if (m_dl)
            m_repaint = (RepaintFn)dlsym(m_dl, "_ZN10KoboWindow7repaintERK7QRegionRK5QListI5QPairI5QRectyEE");
        qDebug() << "eink: libkobo" << (m_dl ? "ok" : dlerror()) << "repaint" << (m_repaint ? "found" : "MISSING")
                 << "flags partial" << m_flagsPartial << "full" << m_flagsFull;
    }
    m_timer.setSingleShot(true);
    m_timer.setInterval(80);
    connect(&m_timer, SIGNAL(timeout()), this, SLOT(flush()));
}

void KoboEink::markDirty(const QRect &r, quint64 flags)
{
    if (!available() || r.isEmpty()) return;
    m_pending.append(qMakePair(r, flags));
    if (!m_timer.isActive()) m_timer.start();
}

void KoboEink::refreshAll(quint64 flags)
{
    QWindowList wl = QGuiApplication::topLevelWindows();
    if (wl.isEmpty()) return;
    markDirty(QRect(QPoint(0, 0), wl.first()->geometry().size()), flags);
}

bool KoboEink::eventFilter(QObject *obj, QEvent *e)
{
    if (e->type() == QEvent::Paint && obj->isWidgetType()) {
        QWidget *w = static_cast<QWidget *>(obj);
        QWidget *top = w->window();
        if (top && w->isVisible()) {
            QRect r = static_cast<QPaintEvent *>(e)->rect();
            QRect tr(w->mapTo(top, r.topLeft()), r.size());
            // Keyboard / toolbar widgets (property kb_fast on them or an ancestor) get the
            // fast black-and-white DU waveform instead of a grayscale refresh.
            quint64 flags = m_flagsPartial;
            for (QWidget *p = w; p && p != top; p = p->parentWidget())
                if (p->property("kb_fast").toBool()) { flags = m_flagsFast; break; }
            markDirty(tr, flags);
        }
    }
    return false;
}

void KoboEink::flush()
{
    if (m_pending.isEmpty()) return;
    if (!m_ready) { m_timer.start(200); return; }   // wait until the platform screen is claimed
    if (m_useFbink) {
        // One refresh per distinct flag value, bounding rect each.
        QList<QPair<QRect, quint64> > merged;
        for (int i = 0; i < m_pending.size(); ++i) {
            bool found = false;
            for (int j = 0; j < merged.size(); ++j)
                if (merged[j].second == m_pending[i].second) { merged[j].first |= m_pending[i].first; found = true; break; }
            if (!found) merged.append(m_pending[i]);
        }
        m_pending.clear();
        for (int j = 0; j < merged.size(); ++j) {
            const QRect &r = merged[j].first;
            const bool full = merged[j].second == m_flagsFull;
            const bool fast = merged[j].second == m_flagsFast;
            bool changed = true;
            if (m_grab && m_fb && m_fb->isOpen()) {
                // Read the platform backing store's image directly (what Qt painted into).
                QImage *src = 0;
                foreach (QWidget *tw, QApplication::topLevelWidgets()) {
                    if (!tw->isVisible()) continue;
                    QBackingStore *bs = tw->backingStore();
                    if (!bs) continue;
                    QPaintDevice *pd = bs->paintDevice();
                    if (pd && pd->devType() == QInternal::Image) { src = static_cast<QImage *>(pd); break; }
                }
                static int dbg = 0;
                if (src && !src->isNull()) {
                    QImage img = src->copy(r);
                    if (dbg < 3) {
                        ++dbg;
                        qDebug() << "grab:" << r << "backingstore image fmt" << src->format() << src->size()
                                 << "depth" << src->depth();
                        if (getenv("KB_DEBUG_GRAB")) img.save(QString("/mnt/onboard/.adds/kobo-browser/grab%1.png").arg(dbg));
                    }
                    changed = m_fb->write(img, r.topLeft());
                } else if (dbg < 3) {
                    ++dbg;
                    qDebug() << "grab: no backing store image";
                }
            }
            // Same pixels as before (e.g. a button repainted on press/release): nothing to show.
            if (!changed && !full) continue;
            if (m_hw && m_hw->refresh(r, full ? HwtconRefresh::GC16 : fast ? HwtconRefresh::DU : HwtconRefresh::Auto, full))
                continue;
            QStringList args;
            args << "-q" << "-s" << QString("top=%1,left=%2,width=%3,height=%4").arg(r.y()).arg(r.x()).arg(r.width()).arg(r.height());
            if (full) args << "-f" << "-W" << "GC16";
            else if (fast) args << "-W" << "DU";
            QProcess::startDetached(m_fbink, args);
        }
        return;
    }
    if (!m_repaint) { m_pending.clear(); return; }
    QWindowList wl = QGuiApplication::topLevelWindows();
    if (wl.isEmpty()) { m_pending.clear(); return; }
    QWindow *win = wl.first();
    QPlatformWindow *pw = win->handle();
    if (!pw) { m_pending.clear(); return; }
    // Merge: one rect per distinct flag value (bounding rects).
    QList<QPair<QRect, quint64> > merged;
    QRegion region;
    for (int i = 0; i < m_pending.size(); ++i) {
        const QRect &r = m_pending[i].first;
        quint64 f = m_pending[i].second;
        region += r;
        bool found = false;
        for (int j = 0; j < merged.size(); ++j)
            if (merged[j].second == f) { merged[j].first |= r; found = true; break; }
        if (!found) merged.append(qMakePair(r, f));
    }
    m_pending.clear();
    m_repaint(pw, region, merged);
}
