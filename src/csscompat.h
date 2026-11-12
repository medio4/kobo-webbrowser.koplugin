// Modern CSS compatibility layer. The firmware's QtWebKit (Qt 5.2.1, WebKit of 2013) only
// knows the -webkit- prefixed forms of transforms, transitions, flexbox, calc() and sticky
// positioning, and has no CSS variables, min()/max()/clamp() or cascade layers. Pages that
// rely on them fall apart (menus that should slide off screen cover the page, flex rows stack
// up). This rewrites style sheets before WebKit parses them:
//   - adds -webkit- copies of transform*, transition*, perspective*, backface-visibility,
//     flex*, order, justify-content, align-*; display:flex/inline-flex; position:sticky
//   - calc() -> -webkit-calc(); translate*(calc(a - b)) -> translate*(a) translate*(-b)
//   - var(--x, fallback) resolved from the same rule, then :root/html custom properties
//     (collected per page across style sheets), then the fallback
//   - min()/max()/clamp() -> the argument that wins on a ~540 CSS px wide screen
//   - inset: -> top/right/bottom/left; @layer blocks unwrapped
// The prefixed copy goes before the original declaration, so an engine that understands the
// standard form still uses it. Declarations whose var() can't be resolved are left alone.
// Works on bytes: ASCII-compatible encodings (UTF-8, Latin-1); UTF-16 input is passed through.
#ifndef CSSCOMPAT_H
#define CSSCOMPAT_H
#include <QtCore/QByteArray>
#include <QtCore/QHash>

namespace CssCompat {

typedef QHash<QByteArray, QByteArray> Vars;   // "--name" -> raw value

struct Stats {
    int decls;     // declarations changed
    int vars;      // var() references resolved
    Stats() : decls(0), vars(0) {}
};

// A whole style sheet (a .css file or the inside of a <style> element). Custom properties
// set on :root / html are added to *pageVars (may be 0) and used for var() lookups.
QByteArray rewriteSheet(const QByteArray &css, Vars *pageVars, Stats *stats = 0);
// The declarations of a style="" attribute.
QByteArray rewriteDeclarations(const QByteArray &decls, const Vars *pageVars, Stats *stats = 0);

// Rewrites <style> elements and style="" attributes of an HTML document as it streams in.
// feed() returns what can be passed on now; the rest of an unfinished tag or <style> element
// is held back until more arrives. finish() returns everything still held.
class HtmlRewriter {
public:
    explicit HtmlRewriter(Vars *pageVars) : m_vars(pageVars), m_inScript(false), m_off(false) {}
    QByteArray feed(const QByteArray &data);
    QByteArray finish();
    Stats stats;
private:
    QByteArray run(bool final);
    Vars *m_vars;
    QByteArray m_pending;
    bool m_inScript;
    bool m_off;   // input looked like UTF-16: pass everything through
};

}
#endif
