#include "eink.h"
#include <QtCore/QCoreApplication>
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

static HwtconRefresh::Waveform hwWaveform(const QString &wf)
{
    if (wf == "DU") return HwtconRefresh::DU;
    if (wf == "GC16") return HwtconRefresh::GC16;
    if (wf == "GL16") return HwtconRefresh::GL16;
    if (wf == "REAGL") return HwtconRefresh::REAGL;
    if (wf == "A2") return HwtconRefresh::A2;
    return HwtconRefresh::Auto;
}

KoboEink *KoboEink::s_instance = 0;

KoboEink::KoboEink(QObject *parent)
    : QObject(parent), m_repaint(0), m_dl(0), m_grab(false), m_ready(false), m_fb(0), m_hw(0)
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
    m_wfClean   = envStr("KB_WF_CLEAN", "GL16");    // non-flashing grayscale cleanup after DU bursts
    m_settleTimer.setSingleShot(true);
    m_settleTimer.setInterval((int)envFlags("KB_FAST_SETTLE_MS", 400));
    connect(&m_settleTimer, SIGNAL(timeout()), this, SLOT(settleFast()));
    m_fullEvery = (int)envFlags("KB_FULL_EVERY", 6);  // full flash every N page turns (0 = never)
    m_partialsEvery = (int)envFlags("KB_FULL_EVERY_PARTIALS", 12);  // ...or N large partial updates
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
            // Keyboard / toolbar widgets (property kb_fast on them or an ancestor) get the
            // fast black-and-white DU waveform instead of a grayscale refresh.
            quint64 flags = m_flagsPartial;
            for (QWidget *p = w; p && p != top; p = p->parentWidget())
                if (p->property("kb_fast").toBool()) { flags = m_flagsFast; break; }
            if (m_holdWidget && (w == m_holdWidget || m_holdWidget->isAncestorOf(w))) {
                // Load hold: Qt still paints into the backing store, we just don't
                // push it to the screen yet. Remember the area for the release.
                m_held |= tr;
                if (!m_holdTimer.isActive()) m_holdTimer.start();
            } else {
                markDirty(tr, m_nextFlags ? m_nextFlags : flags);
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
        // Merge pending rects per flag value, but only when they overlap or nearly touch:
        // a bounding box of the address bar and the keyboard would drag the whole page
        // in between through the DU waveform (seen as a full-screen "DU partial").
        QList<QPair<QRect, quint64> > merged = mergeRects(m_pending);
        m_pending.clear();
        for (int j = 0; j < merged.size(); ++j) {
            const QRect &r = merged[j].first;
            const bool full = merged[j].second == m_flagsFull;
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
                        if (getenv("KB_DEBUG_GRAB")) img.save(QCoreApplication::applicationDirPath() + QString("/grab%1.png").arg(dbg));
                    }
                    changed = m_fb->write(img, r.topLeft());
                } else if (dbg < 3) {
                    ++dbg;
                    qDebug() << "grab: no backing store image";
                }
            }
            // Same pixels as before (e.g. a button repainted on press/release): nothing to show.
            if (!changed && !full) continue;
            // Waveform + flash come from the refresh policy (page turns, periodic full
            // flashes, fast UI), whichever backend sends the update.
            QString wf; bool flash = false;
            chooseWaveform(merged[j].second, r, &wf, &flash);
            const bool fastWf = (wf == m_wfFast && !flash);
            if (fastWf) {
                // DU leaves gray edges and ghosts; once the burst settles, clean the area.
                m_fastDirty += r;
                m_settleTimer.start();
            } else if (!m_fastDirty.isEmpty()) {
                m_fastDirty -= r;   // that part got a proper refresh anyway
            }
            if (m_hw && m_hw->refresh(r, hwWaveform(wf), flash)) {
                if (m_logRefresh) qDebug() << "eink: hwtcon" << r << wf << (flash ? "flash" : "partial");
                continue;
            }
            QStringList args;
            args << "-q" << "-s" << QString("top=%1,left=%2,width=%3,height=%4").arg(r.y()).arg(r.x()).arg(r.width()).arg(r.height());
            if (flash) args << "-f";
            if (!wf.isEmpty() && wf != "AUTO") args << "-W" << wf;
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

QStringList KoboEink::waveformArgs(quint64 flags, const QRect &r)
{
    QStringList a;
    QString wf;
    bool flash = false;
    chooseWaveform(flags, r, &wf, &flash);
    if (flash) a << "-f";
    if (!wf.isEmpty() && wf != "AUTO") a << "-W" << wf;
    return a;
}

void KoboEink::chooseWaveform(quint64 flags, const QRect &r, QString *wfOut, bool *flashOut)
{
    QString wf;
    bool flash = false;
    // A large ordinary update (live-mode scrolling, zoom relayout) ghosts like a page
    // turn does; after KB_FULL_EVERY_PARTIALS of them, flash.
    bool big = false;
    {
        QWindowList wl = QGuiApplication::topLevelWindows();
        if (!wl.isEmpty()) {
            QSize ws = wl.first()->geometry().size();
            big = (qint64)r.width() * r.height() * 3 >= (qint64)ws.width() * ws.height();
        }
    }
    if (flags == m_flagsPartial && big && m_partialsEvery > 0 && ++m_turnsSinceFull >= m_partialsEvery)
        flags = m_flagsFull;
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
    *wfOut = wf;
    *flashOut = flash;
}

void KoboEink::cleanRect(const QRect &rIn)
{
    QRect r = rIn;
    if (r.isEmpty() || !m_useFbink) return;
    QWindowList wl = QGuiApplication::topLevelWindows();
    if (!wl.isEmpty()) r &= QRect(QPoint(0, 0), wl.first()->geometry().size());
    if (r.isEmpty()) return;
    if (m_logRefresh) qDebug() << "eink: clean" << r << m_wfClean;
    if (m_hw && m_hw->refresh(r, hwWaveform(m_wfClean), false)) return;
    QStringList args;
    args << "-q" << "-s" << QString("top=%1,left=%2,width=%3,height=%4").arg(r.y()).arg(r.x()).arg(r.width()).arg(r.height());
    if (m_wfClean != "AUTO") args << "-W" << m_wfClean;
    QProcess::startDetached(m_fbink, args);
}

void KoboEink::settleFast()
{
    if (m_timer.isActive()) { m_settleTimer.start(); return; }   // more repaints on the way
    QList<QPair<QRect, quint64> > parts;
    foreach (const QRect &r, m_fastDirty.rects()) parts.append(qMakePair(r, (quint64)0));
    m_fastDirty = QRegion();
    QList<QPair<QRect, quint64> > merged = mergeRects(parts);
    for (int i = 0; i < merged.size(); ++i) cleanRect(merged[i].first);
}

// Groups rects by flag; rects of one flag are unioned only when they overlap or come
// within `gap` px of each other, so disjoint areas stay separate updates. If a flag
// ends up with too many pieces, they collapse to one bounding rect.
QList<QPair<QRect, quint64> > KoboEink::mergeRects(const QList<QPair<QRect, quint64> > &in)
{
    const int gap = 24;
    const int maxPieces = 6;
    QList<QPair<QRect, quint64> > out;
    for (int i = 0; i < in.size(); ++i) {
        QRect r = in[i].first;
        quint64 f = in[i].second;
        if (r.isEmpty()) continue;
        bool absorbed = true;
        while (absorbed) {           // keep absorbing: a union may now touch another piece
            absorbed = false;
            for (int j = 0; j < out.size(); ++j) {
                if (out[j].second != f) continue;
                if (out[j].first.adjusted(-gap, -gap, gap, gap).intersects(r)) {
                    r |= out[j].first;
                    out.removeAt(j);
                    absorbed = true;
                    break;
                }
            }
        }
        out.append(qMakePair(r, f));
        int pieces = 0;
        for (int j = 0; j < out.size(); ++j) if (out[j].second == f) ++pieces;
        if (pieces > maxPieces) {
            QRect all;
            for (int j = out.size() - 1; j >= 0; --j)
                if (out[j].second == f) { all |= out[j].first; out.removeAt(j); }
            out.append(qMakePair(all, f));
        }
    }
    return out;
}

void KoboEink::hintPageTurn() { m_nextFlags = m_flagsPage; }
void KoboEink::hintFast()     { m_nextFlags = m_flagsFast; }
void KoboEink::hintFull()     { m_nextFlags = m_flagsFull; }

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

void KoboEink::clearScreenForExit()
{
    m_timer.stop();
    m_pending.clear();
    m_holdWidget = 0;
    if (!m_useFbink) return;
    QRect all;
    if (m_fb && m_fb->isOpen()) {
        all = QRect(0, 0, m_fb->width(), m_fb->height());
        QImage white(all.size(), QImage::Format_RGB32);
        white.fill(0xffffffff);
        m_fb->write(white, QPoint(0, 0));
    }
    if (m_hw && !all.isEmpty() && m_hw->refresh(all, HwtconRefresh::GC16, true)) {
        if (m_logRefresh) qDebug() << "eink: exit clear via hwtcon";
        return;
    }
    // fbink: clear + flash, and wait for the update so it lands before we exit.
    QStringList args; args << "-q" << "-c" << "-f" << "-W" << "GC16" << "-w";
    if (m_logRefresh) qDebug() << "eink: fbink" << args.join(" ");
    QProcess::execute(m_fbink, args);
}
