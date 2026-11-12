// Drives e-ink refreshes through Kobo's own platform plugin (libkobo.so) by calling
// KoboWindow::repaint(region, [(rect, flags)]) the way Nickel does.
#ifndef EINK_H
#define EINK_H
#include <QtCore/QObject>
#include <QtGui/QRegion>
#include <QtCore/QTimer>
#include <QtCore/QList>
#include <QtCore/QPair>
#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QElapsedTimer>
class QWindow;
class QWidget;
class FbWriter;
class HwtconRefresh;
class KoboEink : public QObject {
    Q_OBJECT
public:
    explicit KoboEink(QObject *parent = 0);
    bool available() const { return m_repaint != 0 || m_useFbink; }
    // Mark a rect (in top-level window coords) dirty; flushed shortly after.
    void markDirty(const QRect &r, quint64 flags);
    void refreshAll(quint64 flags);
    // Save what is currently painted (the widget backing store) as PNG. Returns the path or empty.
    QString saveScreenshot(const QString &dir);
    // Same, to an exact path, with light PNG compression (quick: used right before sleep).
    bool saveScreenshotTo(const QString &path);
    quint64 flagsPartial() const { return m_flagsPartial; }
    quint64 flagsFull() const { return m_flagsFull; }
    static KoboEink *instance() { return s_instance; }

    // --- Refresh policy (fbink backend; see refreshpolicy in eink.cpp) ---
    // The next flush of ordinary repaints uses the page-turn waveform (REAGL by
    // default, with a full GC16 flash every KB_FULL_EVERY turns).
    // (hintPageTurn/hintFast are slots, below.)
    // Load hold: while held, repaints of `w` (and its children) are not pushed to the
    // screen; they accumulate and come out as ONE clean refresh on releaseHold().
    // A slow page still gets a partial update every KB_LOAD_HOLD_MS so it never
    // looks hung. Repaints outside `w` (toolbar, status line) still go through.
    // Paint the whole screen white and flash it, synchronously: called on Exit so the
    // user sees at once that the browser is gone, before KOReader repaints itself.
    void clearScreenForExit();
    // After releaseHold(), late images/scripts keep repainting the page with partial
    // updates. Once those stop for KB_SETTLE_MS (within KB_SETTLE_WINDOW_MS of the load),
    // one more full flash cleans the ghosting. A page turn or user hint cancels it.
    void beginHold(QWidget *w);
    void releaseHold(bool fullFlash = true);
    bool holding() const { return m_holdWidget != 0; }
    quint64 flagsFast() const { return m_flagsFast; }
    // Clean grayscale (GL16) partial refresh of a rect, regardless of whether its
    // pixels changed: clears the ghosting left by fast DU updates.
    void cleanRect(const QRect &r);
    // While suspended (KOReader is drawing its keyboard over our picture) nothing is
    // pushed to the framebuffer or refreshed; resuming repaints and flashes everything.
    void setSuspended(bool on);
    bool suspended() const { return m_suspended; }
protected:
    bool eventFilter(QObject *obj, QEvent *e);
public slots:
    void refreshAllFull() { m_ready = true; refreshAll(m_flagsFull); }
    void setReady() { m_ready = true; }
    void hintPageTurn();
    void hintFast();   // next flush uses the fast waveform (DU), for small UI changes
    void hintFull();
    void audit();
    void flashOverlays();   // pop-ups (kb_overlay) that just closed: one flash of their area      // KB_AUDIT=1: log screen areas Qt painted that never reached the panel   // next flush is a flashing full refresh (e.g. after the keyboard hides)
private slots:
    void flush();
    void holdTick();
    void settleFast();   // after a burst of fast updates: clean refresh of their area
    void settleLoad();   // page stopped repainting after a load: one more full flash
private:
    // One refresh policy for both backends: picks the waveform name (fbink -W spelling:
    // AUTO, DU, GL16, GC16, REAGL, A2) and whether to flash, and keeps the counters.
    void chooseWaveform(quint64 flags, const QRect &r, QString *wf, bool *flash);
    static QList<QPair<QRect, quint64> > mergeRects(const QList<QPair<QRect, quint64> > &in);
    QStringList waveformArgs(quint64 flags, const QRect &r);
    int m_partialsEvery;   // large partial updates (live scrolling) before a full flash
    static KoboEink *s_instance;
    quint64 m_flagsPage, m_flagsFast;
    quint64 m_nextFlags;        // 0 = no hint pending
    QString m_wfPartial, m_wfFast, m_wfPage, m_wfFull;
    int m_fullEvery, m_turnsSinceFull;
    QWidget *m_holdWidget;
    QRect m_held;               // bounding rect of repaints swallowed during a hold
    QTimer m_holdTimer;
    QTimer m_settleTimer;
    QRect m_overlayRect;
    QTimer m_loadSettleTimer;      // quiet-period timer after a load
    QWidget *m_loadSettleWidget;   // the web view to flash when it settles (0 = inactive)
    QElapsedTimer m_loadSettleSince;
    int m_loadSettleWindowMs;
    bool m_loadSettleDirty;        // something repainted since the release flash
    bool m_suspended;
    QRegion m_fastDirty;        // areas refreshed with the fast waveform since the last cleanup
    QString m_wfClean;
    bool m_logRefresh;
    typedef void (*RepaintFn)(void *self, const QRegion &, const QList<QPair<QRect, quint64> > &);
    RepaintFn m_repaint;
    void *m_dl;
    QTimer m_timer;
    int m_fastDelayMs;
    QList<QPair<QRect, quint64> > m_pending;
    quint64 m_flagsPartial, m_flagsFull;
    bool m_useFbink;
    bool m_grab;
    bool m_ready;
    QString m_fbink;
    FbWriter *m_fb;
    HwtconRefresh *m_hw;
};
#endif
