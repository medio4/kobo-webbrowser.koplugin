#include "eink.h"
#include <QtCore/QCoreApplication>
#include "fbwriter.h"
#include "hwtcon.h"
#include <QtGui/QScreen>
#include <QtGui/QPixmap>
#include <QtGui/QBackingStore>
#include "rotation.h"
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
    m_nextFlags = 0; m_fullEvery = 0; m_turnsSinceFull = 0; m_holdWidget = 0; m_suspended = false;
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
    m_loadSettleWidget = 0; m_loadSettleDirty = false;
    m_loadSettleTimer.setSingleShot(true);
    m_loadSettleTimer.setInterval((int)envFlags("KB_SETTLE_MS", 1500));
    m_loadSettleWindowMs = (int)envFlags("KB_SETTLE_WINDOW_MS", 12000);
    connect(&m_loadSettleTimer, SIGNAL(timeout()), this, SLOT(settleLoad()));
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
    m_fastDelayMs = (int)envFlags("KB_FAST_DELAY_MS", 25);
    m_batching = false;
    m_batchOverlay = false;
    m_shrinkFull = false;
    m_batchTimer.setSingleShot(true);
    m_batchTimer.setInterval((int)envFlags("KB_POPUP_SETTLE_MS", 90));
    connect(&m_batchTimer, SIGNAL(timeout()), this, SLOT(flashOverlays()));
    if (getenv("KB_AUDIT")) {
        QTimer *at = new QTimer(this);
        connect(at, SIGNAL(timeout()), this, SLOT(audit()));
        at->start(1500);
    }
    connect(&m_timer, SIGNAL(timeout()), this, SLOT(flush()));
}

bool KoboEink::canRotate() const { return m_grab && m_fb && m_fb->isOpen(); }

void KoboEink::markDirty(const QRect &rIn, quint64 flags)
{
    if (!available() || rIn.isEmpty() || m_suspended) return;
    QRect r = rIn;
    if (!m_clip.isEmpty()) {          // KOReader's keyboard owns the rest of the screen
        r &= m_clip;
        if (r.isEmpty()) return;
    }
    if (m_batching) {
        m_overlayRect |= r;
        // Keep waiting while repaints keep coming, but never longer than 400 ms in all.
        if (m_batchSince.elapsed() < 400) m_batchTimer.start();
        return;
    }
    m_pending.append(qMakePair(r, flags));
    // Key presses and button taps (fast waveform) go out almost at once; everything else
    // waits a little longer so a burst of repaints becomes one update.
    const int wait = (flags == m_flagsFast || m_nextFlags == m_flagsFast) ? m_fastDelayMs : 80;
    if (!m_timer.isActive()) m_timer.start(wait);
    else if (wait < m_timer.remainingTime()) m_timer.start(wait);
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
    // A pop-up (menu, dialog, list) closing: the toolbar and status line under it repaint
    // with the fast 1-bit waveform, which left pieces of the pop-up's frame and buttons on
    // the panel. Once everything has repainted, flash just the area it covered.
    // Same when one opens: it is often drawn over another (Menu > Bookmarks) and its 1-bit
    // update let the old buttons show through, so its area also gets a flash once drawn.
    if ((e->type() == QEvent::Hide || e->type() == QEvent::Show) && obj->isWidgetType()
            && obj->property("kb_overlay").toBool()) {
        QWidget *w = static_cast<QWidget *>(obj);
        if (e->type() == QEvent::Show) {
            m_shownOverlays << QPointer<QWidget>(w);   // its box is measured once laid out
        } else {
            QWidget *top = w->window();
            foreach (QObject *o, w->children())
                if (o->isWidgetType()) {
                    QWidget *ch = static_cast<QWidget *>(o);
                    m_overlayRect |= QRect(ch->mapTo(top, QPoint(0, 0)), ch->size()).adjusted(-4, -4, 4, 4);
                }
        }
        // Start (or extend) a batch: the pop-up, whatever it covers or uncovers, and a
        // pop-up opened right after it (Menu > Bookmarks) all appear in one refresh.
        if (!m_batching) { m_batching = true; m_batchSince.start(); }
        m_batchOverlay = true;
        m_batchTimer.start();
    }
    // A tap inside a pop-up often closes it or opens another: hold the button's own repaint
    // in a batch too, so it doesn't show as a separate step. A tap that changes nothing else
    // just ends as a normal update.
    if (e->type() == QEvent::MouseButtonPress && obj->isWidgetType()) {
        for (QWidget *p = static_cast<QWidget *>(obj); p; p = p->parentWidget())
            if (p->property("kb_overlay").toBool() && p->isVisible()) {
                if (!m_batching) { m_batching = true; m_batchSince.start(); }
                m_batchTimer.start();
                break;
            }
    }
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
                if (m_loadSettleWidget && (w == m_loadSettleWidget || m_loadSettleWidget->isAncestorOf(w))) {
                    if (m_loadSettleSince.elapsed() > m_loadSettleWindowMs) { m_loadSettleWidget = 0; m_loadSettleTimer.stop(); }
                    else { m_loadSettleDirty = true; m_loadSettleTimer.start(); }   // restart the quiet period
                }
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
            QRect r = merged[j].first;
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
                    // Never past the window: a rect queued just before a rotation can be
                    // in the old (turned) size, and the panel driver rejects any update
                    // that runs off the screen.
                    r &= QRect(QPoint(0, 0), src->size());
                    if (r.isEmpty()) continue;
                    QImage img = src->copy(r);
                    // Rotated screen: Qt painted the window at its turned (logical) size;
                    // turn the piece and its rect to the panel's portrait orientation.
                    // From here on r is in panel coordinates (shrink, refresh, cleanup).
                    if (KbRotation::angle()) {
                        r &= QRect(QPoint(0, 0), src->size());   // never past the turned window
                        if (r.isEmpty()) continue;
                        img = src->copy(r);
                        img = KbRotation::toPhysical(img); r = KbRotation::toPhysical(r);
                    }
                    if (dbg < 3) {
                        ++dbg;
                        qDebug() << "grab:" << r << "backingstore image fmt" << src->format() << src->size()
                                 << "depth" << src->depth();
                        if (getenv("KB_DEBUG_GRAB")) img.save(QCoreApplication::applicationDirPath() + QString("/grab%1.png").arg(dbg));
                    }
                    int fy = -1, ly = -1, fx = -1, lx = -1;
                    changed = m_fb->write(img, r.topLeft(), &fy, &ly, false, &fx, &lx);
                    // Only refresh what really changed (a panel over the page, a repaint of
                    // identical text): a smaller update is quicker and flashes less. A pop-up
                    // flash shrinks too, to the box of changed pixels.
                    bool mayShrink = !full || m_shrinkFull;
                    if (changed && mayShrink && fy >= r.top() && ly >= fy) {
                        QRect c(r.left(), fy, r.width(), ly - fy + 1);
                        if (fx >= r.left() && lx >= fx) { c.setLeft(qMax(r.left(), fx - 8)); c.setRight(qMin(r.right(), lx + 8)); }
                        r = c;
                    }
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
        m_shrinkFull = false;
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

void KoboEink::setSuspended(bool on, bool refresh)
{
    if (on == m_suspended) return;
    m_suspended = on;
    if (on) {
        m_timer.stop();
        m_settleTimer.stop();
        m_pending.clear();
        m_fastDirty = QRegion();
    } else if (refresh) {
        refreshAllFull();   // our backing store still has the page: put it all back, flashed
    }
}

void KoboEink::cleanRect(const QRect &rIn)
{
    QRect r = rIn;
    if (!m_clip.isEmpty()) r &= m_clip;
    if (r.isEmpty() || !m_useFbink || m_suspended) return;
    QWindowList wl = QGuiApplication::topLevelWindows();
    if (!wl.isEmpty()) r &= QRect(QPoint(0, 0), wl.first()->geometry().size());
    if (r.isEmpty()) return;
    cleanPanelRect(m_grab ? KbRotation::toPhysical(r) : r);
}

// Same, for a rect already in panel coordinates (m_fastDirty is collected after rotation).
void KoboEink::cleanPanelRect(const QRect &rIn)
{
    QRect r = rIn;
    if (m_fb && m_fb->isOpen()) r &= QRect(0, 0, m_fb->width(), m_fb->height());
    if (r.isEmpty() || !m_useFbink || m_suspended) return;
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
    for (int i = 0; i < merged.size(); ++i) cleanPanelRect(merged[i].first);
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

void KoboEink::hintPageTurn() { m_nextFlags = m_flagsPage; m_loadSettleWidget = 0; m_loadSettleTimer.stop(); }
void KoboEink::hintFast()     { m_nextFlags = m_flagsFast; }
void KoboEink::hintFull()     { m_nextFlags = m_flagsFull; m_loadSettleWidget = 0; m_loadSettleTimer.stop(); }

void KoboEink::settleLoad()
{
    QWidget *w = m_loadSettleWidget;
    m_loadSettleWidget = 0;
    if (!w || !m_loadSettleDirty || !w->window() || m_holdWidget || m_suspended) return;
    QRect r(w->mapTo(w->window(), QPoint(0, 0)), w->size());
    if (m_logRefresh) qDebug() << "eink: settle flash" << r;
    markDirty(r, m_flagsFull);
}

void KoboEink::beginHold(QWidget *w)
{
    if (!w) return;
    if (m_holdWidget == w) return;
    if (m_holdWidget) releaseHold(false);
    m_loadSettleWidget = 0; m_loadSettleTimer.stop();
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
    if (fullFlash) {
        // Watch for late repaints (lazy images, scripts) and flash once more when they stop.
        m_loadSettleWidget = w;
        m_loadSettleDirty = false;
        m_loadSettleSince.start();
        m_loadSettleTimer.stop();
    }
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

bool KoboEink::saveScreenshotTo(const QString &path)
{
    QImage *src = 0;
    foreach (QWidget *tw, QApplication::topLevelWidgets()) {
        if (!tw->isVisible()) continue;
        QBackingStore *bs = tw->backingStore();
        if (!bs) continue;
        QPaintDevice *pd = bs->paintDevice();
        if (pd && pd->devType() == QInternal::Image) { src = static_cast<QImage *>(pd); break; }
    }
    if (!src || src->isNull()) return false;
    QImage copy = src->convertToFormat(QImage::Format_RGB32);
    // quality 80: zlib level ~2 instead of the default; a full screen saves in well under a second.
    bool ok = copy.save(path, "PNG", 80);
    qDebug() << "screenshot:" << path << (ok ? "saved" : "save failed");
    return ok;
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


// Refresh audit (KB_AUDIT=1): every 1.5 s, when nothing is waiting to be flushed, compare
// what Qt painted (the backing store) with the framebuffer. Rows that differ were painted
// but never copied out, so the panel still shows something old there. Logged only when the
// same difference is seen twice in a row (not a repaint that is still on its way).
void KoboEink::audit()
{
    static QRect last;
    // Skip while a page load holds its repaints on purpose (they are pushed later).
    if (!m_grab || !m_fb || !m_fb->isOpen() || m_suspended || !m_pending.isEmpty() || m_timer.isActive()
            || m_holdWidget || m_holdTimer.isActive()) { last = QRect(); return; }
    QImage *src = 0;
    foreach (QWidget *tw, QApplication::topLevelWidgets()) {
        if (!tw->isVisible()) continue;
        QBackingStore *bs = tw->backingStore();
        if (!bs) continue;
        QPaintDevice *pd = bs->paintDevice();
        if (pd && pd->devType() == QInternal::Image) { src = static_cast<QImage *>(pd); break; }
    }
    if (!src || src->isNull()) return;
    int fy = -1, ly = -1;
    if (KbRotation::angle()) {   // compare in panel orientation (logged rows are panel rows)
        QImage turned = KbRotation::toPhysical(*src);
        bool d = m_fb->write(turned, QPoint(0, 0), &fy, &ly, true);
        QRect r = d ? QRect(0, fy, turned.width(), ly - fy + 1) : QRect();
        if (d && r == last) qWarning() << "audit: STALE panel rows" << fy << "-" << ly << "(rotated)";
        last = r;
        return;
    }
    bool differs = m_fb->write(*src, QPoint(0, 0), &fy, &ly, true);
    QRect r = differs ? QRect(0, fy, src->width(), ly - fy + 1) : QRect();
    if (differs && r == last) {
        qWarning() << "audit: STALE rows" << fy << "-" << ly << "(painted, never sent to the panel)";
        static int dumps = 0;
        if (getenv("KB_AUDIT_DUMP") && dumps < 6)
            src->copy(r).save(QString("/tmp/kb-test/stale%1.png").arg(dumps++));
    }
    last = r;
}

void KoboEink::flashOverlays()
{
    foreach (const QPointer<QWidget> &p, m_shownOverlays) {
        if (!p || !p->isVisible()) continue;
        QWidget *top = p->window();
        foreach (QObject *o, p->children())
            if (o->isWidgetType()) {
                QWidget *ch = static_cast<QWidget *>(o);
                m_overlayRect |= QRect(ch->mapTo(top, QPoint(0, 0)), ch->size()).adjusted(-4, -4, 4, 4);
            }
    }
    m_shownOverlays.clear();
    m_batching = false;
    bool flash = m_batchOverlay;
    m_batchOverlay = false;
    if (m_overlayRect.isEmpty()) return;
    QRect r = m_overlayRect;
    m_overlayRect = QRect();
    if (flash) { m_shrinkFull = true; markDirty(r, m_flagsFull); }
    else markDirty(r, m_flagsPartial);
}
