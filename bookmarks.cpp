#include "icons.h"
#include "bookmarks.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtGui/QResizeEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QFrame>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

QString BookmarkStore::file()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath("bookmarks.txt");
}

QList<Bookmark> BookmarkStore::load()
{
    QList<Bookmark> out;
    QFile f(file());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    in.setCodec("UTF-8");
    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.trimmed().isEmpty()) continue;
        Bookmark b;
        b.url = line.section('\t', 0, 0).trimmed();
        b.title = line.section('\t', 1).trimmed();
        if (b.title.isEmpty()) b.title = b.url;
        if (!b.url.isEmpty()) out << b;
    }
    return out;
}

bool BookmarkStore::save(const QList<Bookmark> &list)
{
    QString tmp = file() + ".tmp";
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;
    QTextStream out(&f);
    out.setCodec("UTF-8");
    foreach (const Bookmark &b, list) {
        QString t = b.title; t.replace('\t', ' ').replace('\n', ' ');
        out << b.url << '\t' << t << '\n';
    }
    out.flush();
    f.close();
    QFile::remove(file());
    return QFile::rename(tmp, file());
}

bool BookmarkStore::add(const QString &url, const QString &title)
{
    QList<Bookmark> list = load();
    foreach (const Bookmark &b, list) if (b.url == url) return false;
    Bookmark b; b.url = url; b.title = title.simplified().isEmpty() ? url : title.simplified();
    list.prepend(b);
    return save(list);
}

// ------------------------------------------------------------- history

static const int kHistoryMax = 300;

QString HistoryStore::file()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath("history.txt");
}

static QString historyOffFile()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath("history-off.txt");
}

bool HistoryStore::enabled() { return !QFile::exists(historyOffFile()); }

void HistoryStore::setEnabled(bool on)
{
    if (on) QFile::remove(historyOffFile());
    else { QFile f(historyOffFile()); if (f.open(QIODevice::WriteOnly)) f.write("off\n"); }
}

static QList<Bookmark> loadList(const QString &path)
{
    QList<Bookmark> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    in.setCodec("UTF-8");
    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.trimmed().isEmpty()) continue;
        Bookmark b;
        b.url = line.section('\t', 0, 0).trimmed();
        b.title = line.section('\t', 1).trimmed();
        if (b.title.isEmpty()) b.title = b.url;
        if (!b.url.isEmpty()) out << b;
    }
    return out;
}

static bool saveList(const QString &path, const QList<Bookmark> &list)
{
    QString tmp = path + ".tmp";
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;
    QTextStream out(&f);
    out.setCodec("UTF-8");
    foreach (const Bookmark &b, list) {
        QString t = b.title; t.replace('\t', ' ').replace('\n', ' ');
        out << b.url << '\t' << t << '\n';
    }
    out.flush();
    f.close();
    QFile::remove(path);
    return QFile::rename(tmp, path);
}

QList<Bookmark> HistoryStore::load() { return loadList(file()); }
bool HistoryStore::save(const QList<Bookmark> &list) { return saveList(file(), list); }
void HistoryStore::clear() { QFile::remove(file()); }

void HistoryStore::record(const QString &url, const QString &title)
{
    if (!enabled() || url.isEmpty() || url == "about:blank" || url.startsWith("data:")) return;
    QList<Bookmark> list = load();
    // A page visited again moves to the top instead of appearing twice.
    for (int i = list.size() - 1; i >= 0; --i) if (list[i].url == url) list.removeAt(i);
    Bookmark b; b.url = url; b.title = title.simplified().isEmpty() ? url : title.simplified();
    list.prepend(b);
    while (list.size() > kHistoryMax) list.removeLast();
    save(list);
}

// ------------------------------------------------------------- overlay

BookmarksOverlay::BookmarksOverlay(QWidget *parent, bool history)
    : QWidget(parent), m_page(0), m_perPage(8), m_history(history)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setProperty("kb_overlay", true);   // e-ink: flash its area clean when it closes
    m_items = history ? HistoryStore::load() : BookmarkStore::load();
    QFrame *box = new QFrame(this);
    m_box = box;
    box->setFrameStyle(QFrame::Box | QFrame::Plain);
    box->setLineWidth(3);
    box->setAutoFillBackground(true);
    QPalette pal = box->palette();
    pal.setColor(QPalette::Window, Qt::white);
    pal.setColor(QPalette::WindowText, Qt::black);
    box->setPalette(pal);

    QVBoxLayout *v = new QVBoxLayout(box);
    v->setContentsMargins(20, 20, 20, 20);
    v->setSpacing(12);
    QFont f; f.setPixelSize(38);
    QLabel *head = new QLabel(history ? (HistoryStore::enabled() ? "History" : "History (paused)") : "Bookmarks");
    head->setFont(f); head->setAlignment(Qt::AlignCenter);
    v->addWidget(head);
    m_list = new QVBoxLayout;
    m_list->setSpacing(10);
    v->addLayout(m_list);
    v->addStretch(1);

    QHBoxLayout *nav = new QHBoxLayout;
    m_prev = new QPushButton("Prev");
    m_next = new QPushButton("Next");
    QPushButton *close = new QPushButton("Close");
    m_pageLabel = new QLabel;
    m_pageLabel->setAlignment(Qt::AlignCenter);
    QFont fs; fs.setPixelSize(32);
    m_pageLabel->setFont(fs);
    foreach (QPushButton *b, QList<QPushButton *>() << m_prev << m_next << close) {
        b->setFont(f); b->setMinimumHeight(96); b->setFocusPolicy(Qt::NoFocus);
    }
    KbIcons::setOn(m_prev, "chevron.left");
    KbIcons::setOn(m_next, "chevron.right");
    KbIcons::setOn(close, "close");
    nav->addWidget(m_prev); nav->addWidget(m_pageLabel, 1); nav->addWidget(m_next);
    if (history) {
        QPushButton *clear = new QPushButton("Clear all");
        clear->setFont(f); clear->setMinimumHeight(96); clear->setFocusPolicy(Qt::NoFocus);
        KbIcons::setOn(clear, "cancel");
        connect(clear, SIGNAL(clicked()), this, SLOT(clearClicked()));
        nav->addWidget(clear);
    }
    nav->addWidget(close);
    v->addLayout(nav);
    connect(m_prev, SIGNAL(clicked()), this, SLOT(prevPage()));
    connect(m_next, SIGNAL(clicked()), this, SLOT(nextPage()));
    connect(close, SIGNAL(clicked()), this, SLOT(closeClicked()));

    if (parent) resize(parent->size());
    showPage(0);
    raise();
    show();
}

void BookmarksOverlay::showPage(int page)
{
    QLayoutItem *it;
    while ((it = m_list->takeAt(0)) != 0) {
        if (it->widget()) it->widget()->deleteLater();
        if (it->layout()) {
            QLayoutItem *c;
            while ((c = it->layout()->takeAt(0)) != 0) { if (c->widget()) c->widget()->deleteLater(); delete c; }
        }
        delete it;
    }
    int pages = qMax(1, (m_items.size() + m_perPage - 1) / m_perPage);
    m_page = qBound(0, page, pages - 1);
    QFont f; f.setPixelSize(32);
    if (m_items.isEmpty()) {
        QLabel *e = new QLabel(m_history
            ? (HistoryStore::enabled() ? "No history yet." : "History is off. Turn it on in the Menu.")
            : "No bookmarks yet. Open a page, then Menu > Add bookmark.");
        e->setFont(f); e->setWordWrap(true); e->setAlignment(Qt::AlignCenter);
        m_list->addWidget(e);
    }
    for (int i = m_page * m_perPage; i < m_items.size() && i < (m_page + 1) * m_perPage; ++i) {
        QHBoxLayout *row = new QHBoxLayout;
        row->setSpacing(10);
        QString t = m_items[i].title;
        QFontMetrics fm(f);
        QPushButton *open = new QPushButton(fm.elidedText(t, Qt::ElideRight, qMax(200, width() - 380)));
        open->setFont(f); open->setMinimumHeight(96); open->setFocusPolicy(Qt::NoFocus);
        open->setStyleSheet("QPushButton { text-align: left; padding-left: 16px; border: 2px solid black; border-radius: 8px; background: white; color: black; }");
        open->setProperty("idx", i);
        QPushButton *del = new QPushButton("Remove");
        del->setFont(f); del->setFixedSize(210, 96); del->setFocusPolicy(Qt::NoFocus);
        KbIcons::setOn(del, "cancel", 40);
        del->setProperty("idx", i);
        connect(open, SIGNAL(clicked()), this, SLOT(entryClicked()));
        connect(del, SIGNAL(clicked()), this, SLOT(removeClicked()));
        row->addWidget(open, 1);
        row->addWidget(del);
        m_list->addLayout(row);
    }
    m_pageLabel->setText(QString("%1 / %2").arg(m_page + 1).arg(pages));
    m_prev->setEnabled(m_page > 0);
    m_next->setEnabled(m_page < pages - 1);
    place();
}

void BookmarksOverlay::entryClicked()
{
    int i = sender()->property("idx").toInt();
    if (i < 0 || i >= m_items.size()) return;
    QString url = m_items[i].url;
    hide();
    emit openUrl(url);
    emit closed();
    close();
}

void BookmarksOverlay::removeClicked()
{
    int i = sender()->property("idx").toInt();
    if (i < 0 || i >= m_items.size()) return;
    m_items.removeAt(i);
    store();
    showPage(m_page);
}

void BookmarksOverlay::store()
{
    if (m_history) HistoryStore::save(m_items); else BookmarkStore::save(m_items);
}

void BookmarksOverlay::clearClicked()
{
    m_items.clear();
    HistoryStore::clear();
    showPage(0);
}

void BookmarksOverlay::prevPage() { showPage(m_page - 1); }
void BookmarksOverlay::nextPage() { showPage(m_page + 1); }
void BookmarksOverlay::closeClicked() { hide(); emit closed(); close(); }

void BookmarksOverlay::place()
{
    m_box->setGeometry(20, 20, width() - 40, height() - 40);
}

void BookmarksOverlay::resizeEvent(QResizeEvent *e) { QWidget::resizeEvent(e); place(); }
void BookmarksOverlay::mousePressEvent(QMouseEvent *e) { e->accept(); }
