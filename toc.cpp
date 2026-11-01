#include "toc.h"

#include <QtCore/QDebug>
#include <QtCore/QVariant>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtGui/QFont>
#include <QtGui/QResizeEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPalette>
#include <QtWidgets/QFrame>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWebKitWidgets/QWebView>
#include <QtWebKitWidgets/QWebFrame>

namespace {
const int kRowH = 74;
const int kMaxText = 70;

// Headings in document order with their position in page (CSS) pixels. Hidden
// headings and the browser's own chrome (nav, header, footer) are skipped.
const char *kCollectJs =
    "(function(){"
    " var out=[]; var hs=document.querySelectorAll('h1,h2,h3,h4');"
    " for(var i=0;i<hs.length;i++){ var h=hs[i];"
    "  var r=h.getBoundingClientRect(); if(!r||r.height<=0||r.width<=0) continue;"
    "  var p=h, skip=false; while(p&&p!==document.body){ var tn=p.tagName; var id=(p.id||'')+' '+(p.className||'');"
    "   if(tn==='NAV'||tn==='FOOTER'||tn==='ASIDE'||/\\b(navbox|sidebar|footer|site-nav|toc|vector-toc|mw-footer)\\b/i.test(id)){skip=true;break;}"
    "   p=p.parentNode; }"
    "  if(skip) continue;"
    "  var t=(h.innerText||h.textContent||'').replace(/\\s+/g,' ').replace(/\\[\\s*edit\\s*\\]/gi,'').trim();"
    "  if(!t) continue;"
    "  out.push({l:parseInt(h.tagName.charAt(1)),t:t.substr(0,120),y:Math.round(r.top+window.pageYOffset),x:Math.round(r.left+window.pageXOffset)});"
    " }"
    " return JSON.stringify(out);"
    "})()";
}

TocOverlay::TocOverlay(QWebView *view, QWidget *parent)
    : QWidget(parent), m_view(view), m_page(0), m_perPage(10)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setProperty("kb_fast", true);           // list pages flip with the fast waveform
    collect();

    m_box = new QFrame(this);
    static_cast<QFrame *>(m_box)->setFrameStyle(QFrame::Box | QFrame::Plain);
    static_cast<QFrame *>(m_box)->setLineWidth(3);
    m_box->setAutoFillBackground(true);
    QPalette pal = m_box->palette();
    pal.setColor(QPalette::Window, Qt::white);
    pal.setColor(QPalette::WindowText, Qt::black);
    m_box->setPalette(pal);

    QVBoxLayout *v = new QVBoxLayout(m_box);
    v->setContentsMargins(16, 16, 16, 16);
    v->setSpacing(8);
    QFont fh; fh.setPixelSize(36); fh.setBold(true);
    QLabel *head = new QLabel("Contents");
    head->setFont(fh);
    head->setAlignment(Qt::AlignCenter);
    v->addWidget(head);

    m_list = new QVBoxLayout;
    m_list->setSpacing(6);
    v->addLayout(m_list);
    v->addStretch(1);

    QHBoxLayout *row = new QHBoxLayout;
    row->setSpacing(12);
    QFont fb; fb.setPixelSize(34);
    m_prev = new QPushButton("< Prev");
    m_next = new QPushButton("Next >");
    QPushButton *close = new QPushButton("Close");
    m_pageLabel = new QLabel;
    m_pageLabel->setFont(fb);
    m_pageLabel->setAlignment(Qt::AlignCenter);
    foreach (QPushButton *b, QList<QPushButton *>() << m_prev << m_next << close) {
        b->setFont(fb);
        b->setMinimumSize(190, 84);
        b->setFocusPolicy(Qt::NoFocus);
    }
    row->addWidget(m_prev); row->addWidget(m_pageLabel, 1); row->addWidget(m_next); row->addWidget(close);
    v->addLayout(row);
    connect(m_prev, SIGNAL(clicked()), this, SLOT(prevPage()));
    connect(m_next, SIGNAL(clicked()), this, SLOT(nextPage()));
    connect(close, SIGNAL(clicked()), this, SLOT(closeClicked()));

    if (parent) resize(parent->size());
    // Rows that fit: box height minus header, buttons and margins.
    int avail = height() - 40 - 60 - 110 - 40;
    m_perPage = qBound(4, avail / (kRowH + 6), 14);
    showPage(0);
    place();
    raise();
    show();
}

void TocOverlay::collect()
{
    QWebFrame *f = m_view->page() ? m_view->page()->mainFrame() : 0;
    if (!f) return;
    QVariant v = f->evaluateJavaScript(QString::fromLatin1(kCollectJs));
    QJsonDocument doc = QJsonDocument::fromJson(v.toString().toUtf8());
    if (!doc.isArray()) { qWarning() << "toc: no headings result"; return; }
    QJsonArray arr = doc.array();
    int minLevel = 9;
    for (int i = 0; i < arr.size(); ++i) minLevel = qMin(minLevel, arr[i].toObject().value("l").toInt(1));
    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject o = arr[i].toObject();
        Item it;
        it.level = o.value("l").toInt(1) - minLevel;    // 0 = top level on this page
        it.text = o.value("t").toString();
        it.y = o.value("y").toInt();
        it.x = o.value("x").toInt();
        m_items << it;
    }
    // Drop the single page-level h1 when everything else is deeper (it is just the title).
    if (m_items.size() > 1 && m_items[0].level == 0) {
        bool others = false;
        for (int i = 1; i < m_items.size(); ++i) if (m_items[i].level == 0) { others = true; break; }
        if (!others && m_items[0].y <= 0) m_items.removeFirst();
    }
}

void TocOverlay::showPage(int page)
{
    int pages = qMax(1, (m_items.size() + m_perPage - 1) / m_perPage);
    m_page = qBound(0, page, pages - 1);
    while (QLayoutItem *li = m_list->takeAt(0)) {
        if (li->widget()) li->widget()->deleteLater();
        delete li;
    }
    QFont f; f.setPixelSize(32);
    int start = m_page * m_perPage;
    for (int i = start; i < m_items.size() && i < start + m_perPage; ++i) {
        const Item &it = m_items[i];
        QString text = it.text;
        if (text.size() > kMaxText) text = text.left(kMaxText - 1) + "...";
        QPushButton *b = new QPushButton(text);
        b->setFont(f);
        b->setFixedHeight(kRowH);
        b->setFocusPolicy(Qt::NoFocus);
        b->setProperty("tocIndex", i);
        int indent = 16 + qMin(it.level, 3) * 40;
        b->setStyleSheet(QString("QPushButton { text-align: left; padding-left: %1px; border: 2px solid #777; border-radius: 6px;"
                                 " background: white; color: black; } QPushButton:pressed { background: black; color: white; }").arg(indent));
        connect(b, SIGNAL(clicked()), this, SLOT(entryClicked()));
        m_list->addWidget(b);
    }
    if (m_items.isEmpty()) {
        QLabel *none = new QLabel("No headings on this page.");
        none->setFont(f);
        none->setAlignment(Qt::AlignCenter);
        m_list->addWidget(none);
    }
    m_pageLabel->setText(QString("%1 / %2").arg(m_page + 1).arg(pages));
    m_prev->setEnabled(m_page > 0);
    m_next->setEnabled(m_page < pages - 1);
}

void TocOverlay::place()
{
    int w = qMin(width() - 24, 1040);
    int h = height() - 24;
    m_box->setGeometry((width() - w) / 2, (height() - h) / 2, w, h);
}

void TocOverlay::resizeEvent(QResizeEvent *e) { QWidget::resizeEvent(e); place(); }
void TocOverlay::mousePressEvent(QMouseEvent *e) { e->accept(); }   // swallow taps outside the box

void TocOverlay::entryClicked()
{
    int i = sender()->property("tocIndex").toInt();
    if (i < 0 || i >= m_items.size()) return;
    QWebFrame *f = m_view->page() ? m_view->page()->mainFrame() : 0;
    if (f) {
        // Positions were measured in page pixels at the current zoom; QWebFrame scrolls in the same units.
        QSize content = f->contentsSize();
        int y = qBound(0, m_items[i].y - 8, qMax(0, content.height() - m_view->height()));
        int x = f->scrollPosition().x();
        emit beforeJump();
        f->setScrollPosition(QPoint(x, y));
    }
    closeClicked();
}

void TocOverlay::prevPage() { showPage(m_page - 1); }
void TocOverlay::nextPage() { showPage(m_page + 1); }
void TocOverlay::closeClicked() { emit closed(); close(); }
