// Toolbar "Menu": an in-window panel of large buttons for the less frequent actions,
// so the toolbar itself fits on one row. A tap outside the panel closes it.
#ifndef MENUPANEL_H
#define MENUPANEL_H

#include <QtWidgets/QWidget>
#include <QtCore/QList>
#include <QtCore/QPair>

class QGridLayout;
class QResizeEvent;
class QMouseEvent;
class QAbstractButton;

class MenuPanel : public QWidget {
    Q_OBJECT
public:
    MenuPanel(QWidget *parent, int top);
    // Adds a button; picking it closes the panel and then calls receiver->slot().
    void addAction(const QString &label, QObject *receiver, const char *slot, bool wide = false);
    void popup();
    bool atRowStart() const { return m_col == 0; }
    void addSection(const QString &title);   // small heading across both columns
    void addSpacer();
signals:
    void closed();
protected:
    void resizeEvent(QResizeEvent *e);
    void mousePressEvent(QMouseEvent *e);
    void mouseReleaseEvent(QMouseEvent *e);
private slots:
    void picked();
private:
    void place();
    QWidget *m_box;
    QGridLayout *m_grid;
    int m_top, m_row, m_col;
    struct Target { QObject *receiver; QByteArray slot; };
    QList<QPair<QAbstractButton *, Target> > m_targets;
};

#endif
