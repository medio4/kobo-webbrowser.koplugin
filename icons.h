// Icons from KOReader's own icon set (resources/icons/mdlight/*.svg in the KOReader
// folder), drawn with QtSvg from Kobo's firmware. Nothing is bundled: when the file is
// missing (another KOReader version), callers fall back to plain text.
#ifndef KB_ICONS_H
#define KB_ICONS_H
#include <QtGui/QPixmap>
#include <QtCore/QString>
class QAbstractButton;
namespace KbIcons {
    // Pixmap of the named icon ("home", "chevron.left", ...) at size x size; null if missing.
    QPixmap get(const QString &name, int size);
    // Puts the named icon on a dialog button, left of its label. Does nothing if missing.
    void setOn(QAbstractButton *b, const QString &name, int size = 48);
}
#endif
