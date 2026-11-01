#include "eink.h"
#include "fbwriter.h"
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
#include <QtCore/QDir>
#include <QtCore/QDateTime>
#include <stdlib.h>
#include <string.h>

static quint64 envFlags(const char *name, quint64 def)
{
    const char *v = getenv(name);
    if (!v || !*v) return def;
    return strtoull(v, 0, 0);
}

static QString envStr(const char *name, const char *def)
{
    const char *v = getenv(name);
    return (v && *v) ? QString::fromLocal8Bit(v) : QString::fromLatin1(def);
}

KoboEink *KoboEink::s_instance = 0;

KoboEink::KoboEink(QObject *parent)
    : QObject(parent), m_repaint(0), m_dl(0), m_grab(false), m_ready(false), m_fb(0)
{
    s_instance = this;
    m_nextFlags = 0; m_fullEvery = 0; m_turnsSinceFull = 0; m_holdWidget = 0;
    m_flagsPartial = envFlags("KB_FLAGS_PARTIAL", 0);
    m_flagsFull = envFlags("KB_FLAGS_FULL", 16);
    // Pseudo-flags for the fbink backend: they only select a waveform (refreshpolicy).
    m_flagsPage = envFlags("KB_FLAGS_PAGE", 1ULL << 40);
    m_flagsFast = envFlags("KB_FLAGS_FAST", 1ULL << 41);
    // Waveforms (fbink -W names). AUTO = let the controller pick (no -W passed).
    m_wfPartial = envStr("KB_WF_PARTIAL", "AUTO");
    m_wfFast    = envStr("KB_WF_FAST", "DU");       // 1-bit, fastest: button presses, typing
    m_wfPage    = envStr("KB_WF_PAGE", "REAGL");    // non-flashing text update for page turns
    m_wfFull    = envStr("KB_WF_FULL", "GC16");     // flashing full refresh
    m_fullEvery = (int)envFlags("KB_FULL_EVERY", 6);  // full flash every N page turns (0 = never)
    m_logRefresh = getenv("KB_EINK_LOG") != 0;   // log every fbink refresh
    m_holdTimer.setSingleShot(true);
    m_holdTimer.setInterval((int)envFlags("KB_LOAD_HOLD_MS", 8000));
    connect(&m_holdTimer, SIGNAL(timeout()), this, SLOT(holdTick()));
    // Refresh backend: "fbink" (default, works with the linuxfb platform plugin) or "kobo"
    // (call KoboWindow::repaint inside Kobo's own platform plugin).
    const char *mode = getenv("KB_EINK");
    m_useFbink = !(mode && !strcmp(mode, "kobo"));
    m_grab = (mode && !strcmp(mode, "grab"));
    if (m_grab) {
        m_fb = new FbWriter;
        qDebug() << "eink: grab mode, framebuffer" << (m_fb->isOpen() ? "open" : "FAILED");
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
    QRect all(QPoint(0, 0), wl.first()->geometry().size());
    if (m_holdWidget && m_holdWidget->window()) {
        // A page is still loading: refresh everything except the held web view, which
        // gets its own clean refresh when the load finishes.
        QRect held(m_holdWidget->mapTo(m_holdWidget->window(), QPoint(0, 0)), m_holdWidget->size());
        QRegion rest = QRegion(all) - QRegion(held);
        foreach (const QRect &r, rest.rects()) markDirty(r, flags);
        m_held |= held;
        return;
    }
    markDirty(all, flags);
}

bool KoboEink::eventFilter(QObject *obj, QEvent *e)
{
    if (e->type() == QEvent::Paint && obj->isWidgetType()) {
        QWidget *w = static_cast<QWidget *>(obj);
        QWidget *top = w->window();
        if (top && w->isVisible()) {
            QRect r = static_cast<QPaintEvent *>(e)->rect();
            QRect tr(w->mapTo(top, r.topLeft()), r.size());
            if (m_holdWidget && (w == m_holdWidget || m_holdWidget->isAncestorOf(w))) {
                // Load hold: Qt still paints into the backing store, we just don't
                // push it to the screen yet. Remember the area for the release.
                m_held |= tr;
                if (!m_holdTimer.isActive()) m_holdTimer.start();
            } else {
                markDirty(tr, m_nextFlags ? m_nextFlags : m_flagsPartial);
            }
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
                    m_fb->write(img, r.topLeft());
                } else if (dbg < 3) {
                    ++dbg;
                    qDebug() << "grab: no backing store image";
                }
            }
            QStringList args;
            args << "-q" << "-s" << QString("top=%1,left=%2,width=%3,height=%4").arg(r.y()).arg(r.x()).arg(r.width()).arg(r.height());
            args << waveformArgs(merged[j].second);
            if (m_logRefresh) qDebug() << "eink: fbink" << args.join(" ");
            QProcess::startDetached(m_fbink, args);
        }
        m_nextFlags = 0;
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

// ------------------------------------------------------------- refresh policy

QStringList KoboEink::waveformArgs(quint64 flags)
{
    QStringList a;
    QString wf;
    bool flash = false;
    if (flags == m_flagsFull) {
        wf = m_wfFull; flash = true;
        m_turnsSinceFull = 0;
    } else if (flags == m_flagsPage) {
        // Page turn: non-flashing text waveform, with a periodic full flash to clear
        // the ghosting that builds up, like KOReader's "full refresh every N pages".
        if (m_fullEvery > 0 && ++m_turnsSinceFull >= m_fullEvery) {
            wf = m_wfFull; flash = true; m_turnsSinceFull = 0;
        } else {
            wf = m_wfPage;
        }
    } else if (flags == m_flagsFast) {
        wf = m_wfFast;
    } else {
        wf = m_wfPartial;
    }
    if (flash) a << "-f";
    if (!wf.isEmpty() && wf != "AUTO") a << "-W" << wf;
    return a;
}

void KoboEink::hintPageTurn() { m_nextFlags = m_flagsPage; }
void KoboEink::hintFast()     { m_nextFlags = m_flagsFast; }

void KoboEink::beginHold(QWidget *w)
{
    if (!w) return;
    if (m_holdWidget == w) return;
    if (m_holdWidget) releaseHold(false);
    m_holdWidget = w;
    m_held = QRect();
}

void KoboEink::releaseHold(bool fullFlash)
{
    if (!m_holdWidget) return;
    m_holdTimer.stop();
    QWidget *w = m_holdWidget;
    m_holdWidget = 0;
    // Refresh the whole widget (not just what repainted): the backing store holds the
    // complete, final page, and one clean flash beats a trail of partial updates.
    QWidget *top = w->window();
    QRect r = (fullFlash && top) ? QRect(w->mapTo(top, QPoint(0, 0)), w->size()) : m_held;
    m_held = QRect();
    if (!r.isEmpty()) markDirty(r, fullFlash ? m_flagsFull : m_flagsPartial);
}

void KoboEink::holdTick()
{
    // The page is taking a while: show what we have so far (one partial update),
    // then keep holding until the load finishes.
    if (!m_holdWidget || m_held.isEmpty()) return;
    markDirty(m_held, m_flagsPartial);
    m_held = QRect();
}

QString KoboEink::saveScreenshot(const QString &dir)
{
    QImage *src = 0;
    foreach (QWidget *tw, QApplication::topLevelWidgets()) {
        if (!tw->isVisible()) continue;
        QBackingStore *bs = tw->backingStore();
        if (!bs) continue;
        QPaintDevice *pd = bs->paintDevice();
        if (pd && pd->devType() == QInternal::Image) { src = static_cast<QImage *>(pd); break; }
    }
    if (!src || src->isNull()) return QString();
    QDir().mkpath(dir);
    QString path = dir + "/kobo-browser-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".png";
    QImage copy = src->copy().convertToFormat(QImage::Format_RGB32);
    if (!copy.save(path, "PNG")) { qWarning() << "screenshot: save failed" << path; return QString(); }
    qDebug() << "screenshot saved" << path;
    return path;
}
