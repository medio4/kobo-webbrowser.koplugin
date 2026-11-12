#ifndef ONSCREENKEYBOARD_H
#define ONSCREENKEYBOARD_H
#include <QtWidgets/QWidget>
#include <QtCore/QList>
#include "flatbutton.h"
class QSpacerItem;

// On-screen keyboard laid out like KOReader's English keyboard, with URL
// helpers. Built once; shift / symbol layers only relabel the keys.
class OnScreenKeyboard : public QWidget {
    Q_OBJECT
public:
    explicit OnScreenKeyboard(QWidget *parent = 0);
    int preferredHeight() const;
    // Stretches the keys to fill width w (landscape is wider), with flatter keys there.
    void fitWidth(int w);
signals:
    void goPressed();
    void hidePressed();
    void backspacePressed();
private slots:
    void keyClicked();
    void toggleShift();
    void toggleSymbols();
private:
    void build();
    void relabel();
    void sendKey(int code, const QString &text);
    QList<FlatButton *> m_charKeys;   // keys whose label changes with the layer
    FlatButton *m_shift;
    FlatButton *m_sym;
    bool m_shift_on;
    bool m_symbols;
    int m_rows;
    int m_keyH;
    int m_fitW;
    QList<QSpacerItem *> m_halfSpacers;   // the half-key indents of the a-l row
};
#endif
