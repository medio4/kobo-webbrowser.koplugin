// Text entry through KOReader's own keyboard (KB_KO_LIVE launches). The browser writes a
// request file; the KOReader plugin (kobo-webbrowser.koplugin) picks it up, lifts its draw
// guard, shows an InputDialog over our frozen picture and writes a reply file. Meanwhile we
// hand it the touchscreen and stop refreshing. Files (set by the plugin):
//   KB_KBD_FILE   request: line 1 kind (url|field|text), line 2 label, rest = current text
//   KB_KBD_REPLY  reply:   line 1 action (go|set|cancel), rest = text
// The same exchange opens KOReader's top menu over the browser (swipe down on the
// toolbar): KB_MENU_FILE gets "menu", KB_MENU_REPLY answers "back" (menu closed, take the
// screen back) or "quit" (KOReader moved on, e.g. home / a book was opened: just exit).
#ifndef TEXTENTRY_H
#define TEXTENTRY_H
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QElapsedTimer>

class NativeTextEntry : public QObject {
    Q_OBJECT
public:
    // reqEnv / replyEnv: the environment variables that name the two files.
    NativeTextEntry(const char *reqEnv, const char *replyEnv, QObject *parent = 0);
    // False when no plugin is listening (not launched by it): use the built-in keyboard.
    bool available() const { return !m_reqPath.isEmpty() && !m_replyPath.isEmpty(); }
    bool active() const { return m_active; }
    // kind: "url" (address bar), "field" (single-line page field), "text" (multi-line).
    bool request(const QString &kind, const QString &label, const QString &current);
    void cancel();
signals:
    void started();                                    // hand off screen + touch
    void replied(const QString &action, const QString &text);   // take them back, apply
    void unavailable();                                // nobody picked the request up
    void keyLine(const QString &line);                 // live typing: one key from KOReader
private slots:
    void poll();
private:
    QString m_reqPath, m_replyPath, m_keysPath;
    qint64 m_keysPos;
    void readKeys();
    QTimer m_timer;
    QElapsedTimer m_since;
    bool m_active;
    bool m_pickedUp;
};
#endif
