#ifndef FLATBUTTON_H
#define FLATBUTTON_H
#include <QtWidgets/QAbstractButton>
#include <QtCore/QString>

// A flat, e-ink friendly button in the style of KOReader's keys: white face,
// thin black border, rounded corners, plain text. It draws no pressed state
// unless KB_KEY_FLASH is set, so a tap costs no extra screen refresh.
class FlatButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit FlatButton(const QString &text, QWidget *parent = 0);
    void setBold(bool b);
    void setPixelSize(int px);
    void setDark(bool d) { m_dark = d; update(); }   // gray face for utility keys
    void setFlash(bool f) { m_flash = f; }
    // Largest pixel size, up to maxPx, at which the label fits the button beside its icon.
    int fitPixelSize(int maxPx) const;
    // Icon from KOReader's set (icons.h), drawn left of the text, or alone (iconOnly).
    // Falls back to the text when the icon file isn't there.
    void setIconName(const QString &name, bool iconOnly = false) { m_iconName = name; m_iconOnly = iconOnly; update(); }
protected:
    void paintEvent(QPaintEvent *e);
private:
    bool m_dark;
    bool m_flash;
    QString m_iconName;
    bool m_iconOnly;
};
#endif
