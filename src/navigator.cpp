#include "icons.h"
#include "navigator.h"

#include <stdlib.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtGui/QFont>
#include <QtGui/QIcon>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPalette>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWebKitWidgets/QWebFrame>
#include <QtWebKitWidgets/QWebPage>
#include <QtWebKitWidgets/QWebView>

namespace {
const int kGap = 14;
const int kMaxScreens = 400;   // a pathological page: overview of the first 400 screens only

// Hides pictures while the thumbnails are drawn: they are the slow part to scale and
// mostly noise at this size. visibility/background-image change no layout, so the
// pages keep their shape. The image boxes are drawn back as gray blocks (kImagesJs).
const char *kHideJs =
    "(function(){var s=document.createElement('style'); s.id='kb-nav-hide';"
    " s.textContent='img,picture,video,canvas,svg,image,iframe,frame,embed,object,applet,input[type=image]"
    "{visibility:hidden!important} *,*:before,*:after{background-image:none!important;"
    " list-style-image:none!important;border-image:none!important}';"
    " (document.head||document.documentElement).appendChild(s);"
    // Read a layout value so the new style is applied now, before the first render.
    " return document.documentElement.offsetHeight>=0;})()";
const char *kShowJs =
    "(function(){var s=document.getElementById('kb-nav-hide'); if(s) s.parentNode.removeChild(s); return true;})()";

// Boxes of the visible pictures, page (CSS) pixels: x,y,w,h,x,y,w,h,... Includes lazy-image
// placeholders (pictures that will load when scrolled to).
const char *kImagesJs =
    "(function(){var out=[]; var e=document.querySelectorAll('img,picture,video,canvas,svg,iframe,embed,object,"
    "input[type=image],.lazy-image-placeholder,[data-src]:not(img)');"
    " var sx=window.pageXOffset, sy=window.pageYOffset;"
    " for(var i=0;i<e.length && out.length<8000;i++){ var r=e[i].getBoundingClientRect();"
    "  if(!r||r.width<24||r.height<24) continue;"
    "  out.push(Math.round(r.left+sx),Math.round(r.top+sy),Math.round(r.width),Math.round(r.height)); }"
    " return out.join(',');})()";

QString settingFile()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath("navigator-on.txt");
}

double envDouble(const char *name, double def)
{
    const char *v = getenv(name);
    return (v && *v) ? atof(v) : def;
}
}

// The thumbnails of one overview page; a tap picks one.
class NavGrid : public QWidget {
public:
    explicit NavGrid(PageNavigator *nav) : QWidget(nav), m_nav(nav) {}
protected:
    void paintEvent(QPaintEvent *);
    void mouseReleaseEvent(QMouseEvent *e);
    void mousePressEvent(QMouseEvent *e) { e->accept(); }
private:
    QRect cell(int slot) const;
    PageNavigator *m_nav;
};

QRect NavGrid::cell(int slot) const
{
    const PageNavigator *n = m_nav;
    int c = slot % n->m_cols, r = slot / n->m_cols;
    int totalW = n->m_cols * n->m_thumbW + (n->m_cols - 1) * kGap;
    int x0 = (width() - totalW) / 2;
    return QRect(x0 + c * (n->m_thumbW + kGap), r * (n->m_thumbH + kGap), n->m_thumbW, n->m_thumbH);
}

void NavGrid::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::white);
    const PageNavigator *n = m_nav;
    int per = n->m_cols * n->m_rows;
    QFont f; f.setPixelSize(26); f.setBold(true);
    p.setFont(f);
    for (int s = 0; s < per; ++s) {
        int i = n->m_page * per + s;
        if (i >= n->m_count) break;
        QRect c = cell(s);
        if (n->m_thumbs.contains(i)) p.drawImage(c.topLeft(), n->m_thumbs.value(i));
        // Frame: thick for the screen shown now.
        bool cur = (i == n->m_current);
        int pw = cur ? 6 : 2;
        p.setPen(QPen(cur ? Qt::black : QColor(110, 110, 110), pw));
        p.setBrush(Qt::NoBrush);
        p.drawRect(c.adjusted(pw / 2, pw / 2, -(pw + 1) / 2, -(pw + 1) / 2));
        // Screen number, bottom left.
        QString num = QString::number(i + 1);
        QRect tag(c.left() + pw, c.bottom() - 36 - pw, qMax(44, p.fontMetrics().width(num) + 16), 36);
        p.fillRect(tag, cur ? Qt::black : Qt::white);
        p.setPen(cur ? Qt::white : Qt::black);
        p.drawText(tag, Qt::AlignCenter, num);
    }
}

void NavGrid::mouseReleaseEvent(QMouseEvent *e)
{
    const PageNavigator *n = m_nav;
    int per = n->m_cols * n->m_rows;
    for (int s = 0; s < per; ++s) {
        int i = n->m_page * per + s;
        if (i >= n->m_count) break;
        if (cell(s).adjusted(-kGap / 2, -kGap / 2, kGap / 2, kGap / 2).contains(e->pos())) {
            m_nav->jumpTo(i);
            return;
        }
    }
}

bool PageNavigator::enabled() { return QFile::exists(settingFile()); }

void PageNavigator::setEnabled(bool on)
{
    if (!on) QFile::remove(settingFile());
    else { QFile f(settingFile()); if (f.open(QIODevice::WriteOnly)) f.write("on\n"); }
}

QPushButton *PageNavigator::navButton(const QString &text, const QString &icon, const QFont &font)
{
    QPushButton *b = new QPushButton;
    b->setFont(font);
    b->setFocusPolicy(Qt::NoFocus);
    QPixmap pm = KbIcons::get(icon, 52);
    if (pm.isNull()) {
        b->setText(text);
        b->setMinimumSize(110, 84);
    } else {
        b->setIcon(QIcon(pm));
        b->setIconSize(pm.size());
        b->setMinimumSize(100, 84);
    }
    return b;
}

PageNavigator::PageNavigator(QWebView *view, QWidget *parent)
    : QWidget(parent), m_view(view), m_viewW(1), m_viewH(1), m_step(1), m_count(0), m_current(0),
      m_scrollX(0), m_cols(3), m_rows(3), m_thumbW(1), m_thumbH(1), m_page(0)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setProperty("kb_overlay", true);   // e-ink: flash its area clean when it closes
    // Not kb_fast: the thumbnails are gray text and need the grayscale waveform.
    setAutoFillBackground(true);       // opaque, so the page underneath never repaints through
    QPalette wp = palette();
    wp.setColor(QPalette::Window, Qt::white);
    setPalette(wp);
    measure();

    m_box = new QFrame(this);
    static_cast<QFrame *>(m_box)->setFrameStyle(QFrame::Box | QFrame::Plain);
    static_cast<QFrame *>(m_box)->setLineWidth(3);
    m_box->setAutoFillBackground(true);
    QPalette pal = m_box->palette();
    pal.setColor(QPalette::Window, Qt::white);
    pal.setColor(QPalette::WindowText, Qt::black);
    m_box->setPalette(pal);

    QVBoxLayout *v = new QVBoxLayout(m_box);
    v->setContentsMargins(16, 12, 16, 16);
    v->setSpacing(10);
    QFont fh; fh.setPixelSize(32); fh.setBold(true);
    m_head = new QLabel;
    m_head->setFont(fh);
    m_head->setAlignment(Qt::AlignCenter);
    v->addWidget(m_head);

    m_grid = new NavGrid(this);
    m_grid->setParent(m_box);
    v->addWidget(m_grid, 1);

    QHBoxLayout *row = new QHBoxLayout;
    row->setSpacing(12);
    QFont fb; fb.setPixelSize(34);
    // Paging buttons are icon-only when KOReader's icons are there (six buttons don't fit a
    // portrait row with labels), else short words. No "<": the Kobo UI font draws a clock glyph.
    m_first = navButton("First", "chevron.first", fb);
    m_prev = navButton("Prev", "chevron.left", fb);
    m_next = navButton("Next", "chevron.right", fb);
    m_last = navButton("Last", "chevron.last", fb);
    QPushButton *close = new QPushButton("Close");
    close->setFont(fb);
    close->setMinimumSize(190, 84);
    close->setFocusPolicy(Qt::NoFocus);
    KbIcons::setOn(close, "close", 44);
    m_pageLabel = new QLabel;
    m_pageLabel->setFont(fb);
    m_pageLabel->setAlignment(Qt::AlignCenter);
    row->addWidget(m_first); row->addWidget(m_prev); row->addWidget(m_pageLabel, 1);
    row->addWidget(m_next); row->addWidget(m_last); row->addWidget(close);
    v->addLayout(row);
    connect(m_first, SIGNAL(clicked()), this, SLOT(firstPage()));
    connect(m_prev, SIGNAL(clicked()), this, SLOT(prevPage()));
    connect(m_next, SIGNAL(clicked()), this, SLOT(nextPage()));
    connect(m_last, SIGNAL(clicked()), this, SLOT(lastPage()));
    connect(close, SIGNAL(clicked()), this, SLOT(closeClicked()));

    if (parent) resize(parent->size());
    layoutGrid();
    showPage(m_current / (m_cols * m_rows));
    raise();
    show();
}

// Screens of the page: the first at the top, then one page turn (PageScroller's step,
// a screen minus a little overlap) further each, so screen N is where N-1 turns land.
void PageNavigator::measure()
{
    QWebFrame *f = m_view->page() ? m_view->page()->mainFrame() : 0;
    if (!f) return;
    m_viewW = qMax(1, m_view->width());
    m_viewH = qMax(1, m_view->height());
    int overlap = qMax((int)envDouble("KB_PAGE_OVERLAP_MIN", 30), (int)(m_viewH * envDouble("KB_PAGE_OVERLAP", 0.06)));
    m_step = qMax(1, m_viewH - overlap);
    int contentH = f->contentsSize().height();
    int maxY = qMax(0, contentH - m_viewH);
    m_count = qBound(1, 1 + (maxY + m_step - 1) / m_step, kMaxScreens);
    m_scrollX = f->scrollPosition().x();
    int y = f->scrollPosition().y();
    // The screen showing most of the view now; the last screen is pinned to the bottom.
    m_current = qBound(0, (y + m_step / 2) / m_step, m_count - 1);
}

void PageNavigator::layoutGrid()
{
    int w = width() - 24, h = height() - 24;
    m_box->setGeometry(12, 12, w, h);
    m_cols = width() > height() ? 4 : 3;
    int gridW = w - 6 - 32;                                // frame lines, margins
    int gridH = h - 6 - 12 - 16 - 10 - 44 - 10 - 84 - 10;   // frame, margins, header, spacing, buttons
    m_thumbW = qMax(40, (gridW - (m_cols - 1) * kGap) / m_cols);
    m_thumbH = qMax(40, m_thumbW * m_viewH / m_viewW);
    m_rows = qMax(1, (gridH + kGap) / (m_thumbH + kGap));
    // Rows don't fill the height: grow the thumbnails a bit if a full row is still out of reach.
    int fitH = (gridH - (m_rows - 1) * kGap) / m_rows;
    if (fitH > m_thumbH) {
        int fitW = qMin((gridW - (m_cols - 1) * kGap) / m_cols, fitH * m_viewW / m_viewH);
        m_thumbW = fitW;
        m_thumbH = fitW * m_viewH / m_viewW;
    }
}

void PageNavigator::showPage(int page)
{
    int per = m_cols * m_rows;
    int pages = qMax(1, (m_count + per - 1) / per);
    m_page = qBound(0, page, pages - 1);
    m_head->setText(m_count == 1 ? QString("Page overview (1 screen)")
                                 : QString("Page overview (%1 screens)").arg(m_count));
    m_pageLabel->setText(QString("%1 / %2").arg(m_page + 1).arg(pages));
    m_first->setEnabled(m_page > 0);
    m_prev->setEnabled(m_page > 0);
    m_next->setEnabled(m_page < pages - 1);
    m_last->setEnabled(m_page < pages - 1);
    renderVisible();
    m_grid->update();
}

void PageNavigator::renderVisible()
{
    QWebFrame *f = m_view->page() ? m_view->page()->mainFrame() : 0;
    if (!f) return;
    int per = m_cols * m_rows;
    int first = m_page * per, last = qMin(m_count, first + per) - 1;
    bool need = false;
    for (int i = first; i <= last; ++i) if (!m_thumbs.contains(i)) { need = true; break; }
    if (!need) return;

    QElapsedTimer t; t.start();
    // Image boxes first (while they are still laid out as shown), in frame pixels.
    QList<QRect> images;
    qreal z = m_view->zoomFactor();
    QStringList nums = f->evaluateJavaScript(QString::fromLatin1(kImagesJs)).toString().split(',', QString::SkipEmptyParts);
    for (int k = 0; k + 3 < nums.size(); k += 4)
        images << QRect(int(nums[k].toInt() * z), int(nums[k + 1].toInt() * z),
                        int(nums[k + 2].toInt() * z), int(nums[k + 3].toInt() * z));
    QPoint back = f->scrollPosition();
    f->evaluateJavaScript(QString::fromLatin1(kHideJs));
    for (int i = first; i <= last; ++i)
        if (!m_thumbs.contains(i)) m_thumbs.insert(i, renderThumb(i, images));
    f->evaluateJavaScript(QString::fromLatin1(kShowJs));
    f->setScrollPosition(back);
    qDebug() << "navigator: screens" << first + 1 << "-" << last + 1 << "of" << m_count
             << "drawn in" << t.elapsed() << "ms," << images.size() << "images";
}

QImage PageNavigator::renderThumb(int index, const QList<QRect> &images)
{
    QImage img(m_thumbW, m_thumbH, QImage::Format_RGB32);
    img.fill(Qt::white);
    QWebFrame *f = m_view->page() ? m_view->page()->mainFrame() : 0;
    if (!f) return img;
    int maxY = qMax(0, f->contentsSize().height() - m_viewH);
    int y = qMin(index * m_step, maxY);
    QRect area(m_scrollX, y, m_viewW, m_viewH);   // frame (contents) pixels

    QPainter p(&img);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    qreal s = qreal(m_thumbW) / m_viewW;
    p.scale(s, s);
    p.setClipRect(QRect(0, 0, m_viewW, m_viewH));
    // QWebFrame::render draws only what is in the viewport, so the frame is scrolled to
    // each screen in turn (renderVisible puts it back). Rendering the document element
    // instead misses most pages that make html/body exactly one screen high.
    f->setScrollPosition(area.topLeft());
    QPoint at = f->scrollPosition();
    p.save();
    p.translate(0, at.y() - area.y());   // the last screen can't scroll past the end
    f->render(&p, QWebFrame::ContentsLayer, QRegion(0, 0, m_viewW, m_viewH));
    p.restore();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(200, 200, 200));
    foreach (const QRect &r, images)
        if (r.intersects(area)) p.drawRect(r.translated(-area.topLeft()));
    return img;
}

void PageNavigator::jumpTo(int index)
{
    QWebFrame *f = m_view->page() ? m_view->page()->mainFrame() : 0;
    if (f) {
        int maxY = qMax(0, f->contentsSize().height() - m_viewH);
        int y = qMin(index * m_step, maxY);
        emit beforeJump();
        f->setScrollPosition(QPoint(f->scrollPosition().x(), y));
    }
    closeClicked();
}

void PageNavigator::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    if (!m_box) return;
    int per = m_cols * m_rows;
    int firstShown = m_page * per;
    layoutGrid();
    if (m_cols * m_rows != per || e->oldSize().isValid()) {
        m_thumbs.clear();     // thumbnail size changed (rotation)
        showPage(firstShown / (m_cols * m_rows));
    }
}

void PageNavigator::mousePressEvent(QMouseEvent *e) { e->accept(); }   // swallow taps outside the box

void PageNavigator::firstPage() { showPage(0); }
void PageNavigator::prevPage() { showPage(m_page - 1); }
void PageNavigator::lastPage() { showPage(m_count); }   // showPage clamps to the last one
void PageNavigator::nextPage() { showPage(m_page + 1); }
void PageNavigator::closeClicked() { emit closed(); close(); }
