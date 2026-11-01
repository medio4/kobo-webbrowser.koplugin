// Minimal EPUB writer (stored zip entries, no compression).
#ifndef EPUB_H
#define EPUB_H
#include <QtCore/QString>
#include <QtCore/QByteArray>
class QWebFrame;
namespace Epub {
    // Serialize the frame's current document to a cleaned XHTML body (via page JS).
    QString extractBodyXhtml(QWebFrame *frame);
    // Write a single-chapter EPUB. Returns true on success.
    bool write(const QString &path, const QString &title, const QString &sourceUrl, const QString &bodyXhtml);
    QString safeFileName(const QString &title);
}
#endif
