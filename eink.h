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
class QWindow;
class FbWriter;
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
    quint64 flagsPartial() const { return m_flagsPartial; }
    quint64 flagsFull() const { return m_flagsFull; }
protected:
    bool eventFilter(QObject *obj, QEvent *e);
public slots:
    void refreshAllFull() { m_ready = true; refreshAll(m_flagsFull); }
    void setReady() { m_ready = true; }
private slots:
    void flush();
private:
    typedef void (*RepaintFn)(void *self, const QRegion &, const QList<QPair<QRect, quint64> > &);
    RepaintFn m_repaint;
    void *m_dl;
    QTimer m_timer;
    QList<QPair<QRect, quint64> > m_pending;
    quint64 m_flagsPartial, m_flagsFull;
    bool m_useFbink;
    bool m_grab;
    bool m_ready;
    QString m_fbink;
    FbWriter *m_fb;
};
#endif
