#include "menupanel.h"
#include "browser.h"
#include <QtCore/QTimer>
#include <QtCore/QMetaObject>
#include <QtGui/QResizeEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>

MenuPanel::MenuPanel(QWidget *parent, int top)
    : QWidget(parent), m_top(top), m_row(0), m_col(0)
{
    setAttribute(Qt::WA_DeleteOnClose);
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
    m_grid->setSpacing(12);
}

void MenuPanel::addAction(const QString &label, QObject *receiver, const char *slot, bool wide)
{
    FlatButton *b = new FlatButton(label);
    b->setPixelSize(38);
    b->setMinimumHeight(100);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setFixedHeight(100);
    if (wide && m_col != 0) { m_row++; m_col = 0; }
    m_grid->addWidget(b, m_row, m_col, 1, wide ? 2 : 1);
    if (wide || m_col == 1) { m_row++; m_col = 0; } else m_col = 1;
    Target t; t.receiver = receiver; t.slot = slot;
    // SLOT() strings carry a leading code digit; strip it and the "()" for invokeMethod.
    QByteArray name = t.slot;
    if (!name.isEmpty() && name[0] >= '0' && name[0] <= '9') name = name.mid(1);
    int paren = name.indexOf('(');
    if (paren >= 0) name = name.left(paren);
    t.slot = name;
    m_targets << qMakePair(static_cast<QAbstractButton *>(b), t);
    connect(b, SIGNAL(clicked()), this, SLOT(picked()));
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
    int w = qMin(width() - 24, 820);
    m_box->setFixedWidth(w);
    m_box->adjustSize();
    // Hang it from the right end of the toolbar, under the Menu button.
    m_box->move(width() - m_box->width() - 12, m_top);
}

void MenuPanel::resizeEvent(QResizeEvent *e) { QWidget::resizeEvent(e); place(); }

void MenuPanel::mousePressEvent(QMouseEvent *e)
{
    e->accept();
    if (!m_box->geometry().contains(e->pos())) { hide(); emit closed(); close(); }
}
