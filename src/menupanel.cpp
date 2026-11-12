#include "menupanel.h"
#include "browser.h"
#include <QtCore/QTimer>
#include <QtCore/QMetaObject>
#include <QtGui/QResizeEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QLabel>

MenuPanel::MenuPanel(QWidget *parent, int top)
    : QWidget(parent), m_top(top), m_row(0), m_col(0), m_cols(2)
{
    // A landscape screen is too short for two columns of buttons: use four.
    if (parent && parent->width() > parent->height()) m_cols = 4;
    setAttribute(Qt::WA_DeleteOnClose);
    setProperty("kb_overlay", true);   // e-ink: flash its area clean when it closes
    setAutoFillBackground(false);
    setProperty("kb_fast", true);
    QFrame *box = new QFrame(this);
    m_box = box;
    box->setFrameStyle(QFrame::Box | QFrame::Plain);
    box->setLineWidth(3);
    box->setAutoFillBackground(true);
    QPalette pal = box->palette();
    pal.setColor(QPalette::Window, Qt::white);
    pal.setColor(QPalette::WindowText, Qt::black);
    box->setPalette(pal);
    m_grid = new QGridLayout(box);
    m_grid->setContentsMargins(16, 16, 16, 16);
    m_grid->setHorizontalSpacing(12);
    m_grid->setVerticalSpacing(10);
}

FlatButton *MenuPanel::addAction(const QString &label, QObject *receiver, const char *slot, bool wide, const QString &icon)
{
    FlatButton *b = new FlatButton(label);
    if (!icon.isEmpty()) b->setIconName(icon);
    b->setPixelSize(38);
    b->setMinimumHeight(88);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setFixedHeight(88);
    if (wide && m_col != 0) { m_row++; m_col = 0; }
    m_grid->addWidget(b, m_row, m_col, 1, wide ? m_cols : 1);
    if (wide || m_col == m_cols - 1) { m_row++; m_col = 0; } else m_col++;
    Target t; t.receiver = receiver; t.slot = slot;
    // SLOT() strings carry a leading code digit; strip it and the "()" for invokeMethod.
    QByteArray name = t.slot;
    if (!name.isEmpty() && name[0] >= '0' && name[0] <= '9') name = name.mid(1);
    int paren = name.indexOf('(');
    if (paren >= 0) name = name.left(paren);
    t.slot = name;
    m_targets << qMakePair(static_cast<QAbstractButton *>(b), t);
    connect(b, SIGNAL(clicked()), this, SLOT(picked()));
    return b;
}

void MenuPanel::addSection(const QString &title)
{
    if (m_col != 0) { m_row++; m_col = 0; }
    QLabel *l = new QLabel(title);
    QFont f; f.setPixelSize(28); f.setBold(true);
    l->setFont(f);
    l->setContentsMargins(6, m_row ? 10 : 0, 0, 0);
    m_grid->addWidget(l, m_row, 0, 1, m_cols);
    m_row++;
}

void MenuPanel::addSpacer()
{
    if (m_col != 0) { m_row++; m_col = 0; }
    m_grid->setRowMinimumHeight(m_row, 10);
    m_row++;
}

void MenuPanel::popup()
{
    if (parentWidget()) resize(parentWidget()->size());
    place();
    raise();
    show();
}

void MenuPanel::picked()
{
    QAbstractButton *b = qobject_cast<QAbstractButton *>(sender());
    for (int i = 0; i < m_targets.size(); ++i) {
        if (m_targets[i].first != b) continue;
        Target t = m_targets[i].second;
        hide();
        emit closed();
        // Run after the panel is gone, so dialogs the action opens are drawn on a clean page.
        QMetaObject::invokeMethod(t.receiver, t.slot.constData(), Qt::QueuedConnection);
        close();
        return;
    }
}

void MenuPanel::place()
{
    int w = qMin(width() - 24, m_cols > 2 ? 1400 : 820);
    m_box->setFixedWidth(w);
    m_box->adjustSize();
    // One text size for every button, the largest at which all labels fit, so the panel
    // looks even instead of having one squeezed label.
    m_grid->activate();
    int px = 38;
    for (int i = 0; i < m_targets.size(); ++i) {
        FlatButton *b = static_cast<FlatButton *>(m_targets[i].first);
        px = qMin(px, b->fitPixelSize(38));
    }
    for (int i = 0; i < m_targets.size(); ++i)
        static_cast<FlatButton *>(m_targets[i].first)->setPixelSize(px);
    // Hang it from the right end of the toolbar, under the Menu button.
    m_box->move(width() - m_box->width() - 12, m_top);
}

void MenuPanel::resizeEvent(QResizeEvent *e) { QWidget::resizeEvent(e); place(); }

// A tap outside the panel closes it. Close on the release, not the press: otherwise the
// release lands on the page underneath and follows whatever link is there.
void MenuPanel::mousePressEvent(QMouseEvent *e) { e->accept(); }

void MenuPanel::mouseReleaseEvent(QMouseEvent *e)
{
    e->accept();
    if (!m_box->geometry().contains(e->pos())) { hide(); emit closed(); close(); }
}
