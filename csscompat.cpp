#include "csscompat.h"
#include <QtCore/QList>
#include <QtCore/QVector>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace CssCompat {

// Screen the min()/max()/clamp() picks are made for: the Clara's 1072x1448 at zoom 2.
static const double kVw = 536, kVh = 724, kRem = 16;
static const int kMaxVarDepth = 8;

static inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static inline bool isIdent(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'
           || (unsigned char)c >= 0x80;
}
static inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

// Case-insensitive: does lit (lowercase) start at s[i]?
static bool at(const char *s, int n, int i, const char *lit)
{
    for (int k = 0; lit[k]; ++k)
        if (i + k >= n || lower(s[i + k]) != lit[k]) return false;
    return true;
}

static QByteArray trimmed(const QByteArray &a) { return a.trimmed(); }

// s[i] is a quote: index after the closing quote (or n).
static int skipString(const char *s, int n, int i)
{
    char q = s[i++];
    while (i < n) {
        if (s[i] == '\\') { i += 2; continue; }
        if (s[i] == q) return i + 1;
        if (s[i] == '\n') return i;   // unterminated: CSS ends the string at the line
        ++i;
    }
    return n;
}

// s[i..] is "/*": index after "*/" (or n).
static int skipComment(const char *s, int n, int i)
{
    for (i += 2; i + 1 < n; ++i)
        if (s[i] == '*' && s[i + 1] == '/') return i + 2;
    return n;
}

// s[i] == '(': index of the matching ')', or -1.
static int matchParen(const char *s, int n, int i)
{
    int depth = 0;
    while (i < n) {
        char c = s[i];
        if (c == '"' || c == '\'') { i = skipString(s, n, i); continue; }
        if (c == '(') ++depth;
        else if (c == ')' && --depth == 0) return i;
        ++i;
    }
    return -1;
}

// Splits at top-level commas.
static QList<QByteArray> splitArgs(const QByteArray &a)
{
    QList<QByteArray> out;
    const char *s = a.constData();
    int n = a.size(), depth = 0, start = 0;
    for (int i = 0; i < n; ) {
        char c = s[i];
        if (c == '"' || c == '\'') { i = skipString(s, n, i); continue; }
        if (c == '(' || c == '[') ++depth;
        else if (c == ')' || c == ']') --depth;
        else if (c == ',' && depth == 0) { out << a.mid(start, i - start); start = i + 1; }
        ++i;
    }
    out << a.mid(start);
    return out;
}

// The next function call "name(" at or after i whose name (lowercase) is in names
// (0-terminated list). Skips strings and url(). Returns its start, sets *open to the '('.
static int findFunc(const QByteArray &v, int i, const char *const *names, int *open, int *which)
{
    const char *s = v.constData();
    int n = v.size();
    while (i < n) {
        char c = s[i];
        if (c == '"' || c == '\'') { i = skipString(s, n, i); continue; }
        if (isIdent(c) && (i == 0 || !isIdent(s[i - 1]))) {
            int e = i;
            while (e < n && isIdent(s[e])) ++e;
            if (e < n && s[e] == '(') {
                if (e - i == 3 && at(s, n, i, "url")) {
                    int m = matchParen(s, n, e);
                    i = m < 0 ? n : m + 1;
                    continue;
                }
                for (int k = 0; names[k]; ++k)
                    if ((int)strlen(names[k]) == e - i && at(s, n, i, names[k])) {
                        *open = e;
                        if (which) *which = k;
                        return i;
                    }
            }
            i = e;
            continue;
        }
        ++i;
    }
    return -1;
}

static bool containsCI(const QByteArray &v, const char *lit)
{
    const char *s = v.constData();
    int n = v.size(), l = strlen(lit);
    for (int i = 0; i + l <= n; ++i)
        if (lower(s[i]) == lit[0] && at(s, n, i, lit)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Lengths, for picking a min()/max()/clamp() argument.

enum { EvalOk, EvalPercent, EvalFail };

struct Eval {
    const char *s;
    int n, i, status;
    void ws() { while (i < n && isSpace(s[i])) ++i; }
    void fail(int st) { if (status == EvalOk || st == EvalFail) status = st; }
    // Returns the value; *len is true for a length (px), false for a plain number.
    double sum(bool *len)
    {
        double v = product(len);
        for (;;) {
            ws();
            if (i >= n || (s[i] != '+' && s[i] != '-')) return v;
            char op = s[i++];
            bool l2;
            double w = product(&l2);
            if (*len != l2) fail(EvalFail);
            v = op == '+' ? v + w : v - w;
        }
    }
    double product(bool *len)
    {
        double v = factor(len);
        for (;;) {
            ws();
            if (i >= n || (s[i] != '*' && s[i] != '/')) return v;
            char op = s[i++];
            bool l2;
            double w = factor(&l2);
            if (op == '*') { if (*len && l2) fail(EvalFail); *len = *len || l2; v *= w; }
            else { if (l2 || w == 0) fail(EvalFail); v /= w; }
        }
    }
    double factor(bool *len)
    {
        ws();
        *len = false;
        if (i >= n) { fail(EvalFail); return 0; }
        if (s[i] == '(') {
            ++i;
            double v = sum(len);
            ws();
            if (i < n && s[i] == ')') ++i; else fail(EvalFail);
            return v;
        }
        if (at(s, n, i, "calc(") || at(s, n, i, "-webkit-calc(")) {
            while (s[i] != '(') ++i;
            return factor(len);
        }
        QByteArray num;
        int st = i;
        if (s[i] == '+' || s[i] == '-') ++i;
        while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) ++i;
        if (i < n && (s[i] == 'e' || s[i] == 'E') && i + 1 < n && (s[i + 1] == '-' || (s[i + 1] >= '0' && s[i + 1] <= '9'))) {
            i += 2;
            while (i < n && s[i] >= '0' && s[i] <= '9') ++i;
        }
        if (i == st || (i == st + 1 && (s[st] == '+' || s[st] == '-'))) { fail(EvalFail); i = n; return 0; }
        num = QByteArray(s + st, i - st);
        double v = strtod(num.constData(), 0);
        int u = i;
        while (i < n && (isIdent(s[i]) || s[i] == '%')) ++i;
        QByteArray unit = QByteArray(s + u, i - u).toLower();
        if (unit.isEmpty()) return v;
        *len = true;
        if (unit == "px") return v;
        if (unit == "rem" || unit == "em") return v * kRem;
        if (unit == "vw") return v * kVw / 100;
        if (unit == "vh" || unit == "dvh" || unit == "svh" || unit == "lvh") return v * kVh / 100;
        if (unit == "vmin") return v * kVw / 100;
        if (unit == "vmax") return v * kVh / 100;
        if (unit == "pt") return v * 4 / 3;
        if (unit == "pc") return v * 16;
        if (unit == "in") return v * 96;
        if (unit == "cm") return v * 96 / 2.54;
        if (unit == "mm") return v * 96 / 25.4;
        if (unit == "ch" || unit == "ex") return v * kRem / 2;
        if (unit == "%") { fail(EvalPercent); return 0; }
        fail(EvalFail);
        return 0;
    }
};

static int evalLength(const QByteArray &a, double *px)
{
    Eval e;
    e.s = a.constData(); e.n = a.size(); e.i = 0; e.status = EvalOk;
    bool len;
    *px = e.sum(&len);
    e.ws();
    if (e.i != e.n) e.fail(EvalFail);
    return e.status;
}

static bool hasOperator(const QByteArray &a)
{
    const char *s = a.constData();
    int n = a.size(), depth = 0;
    for (int i = 0; i < n; ++i) {
        char c = s[i];
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (depth == 0 && (c == '*' || c == '/')) return true;
        else if (depth == 0 && (c == '+' || c == '-') && i > 0 && isSpace(s[i - 1]) && i + 1 < n && isSpace(s[i + 1]))
            return true;
    }
    return false;
}

// min(a, b), max(a, b), clamp(lo, v, hi) -> one argument, picked for this screen.
static QByteArray pickArg(int fn, const QList<QByteArray> &args)
{
    int n = args.size();
    if (n == 0) return QByteArray();
    QVector<double> px(n);
    QVector<int> st(n);
    for (int k = 0; k < n; ++k) st[k] = evalLength(args[k], &px[k]);
    if (fn == 2 && n == 3) {   // clamp
        if (st[1] != EvalOk) return args[1];
        if (st[0] == EvalOk && px[1] < px[0]) return args[0];
        if (st[2] == EvalOk && px[1] > px[2]) return args[2];
        return args[1];
    }
    int best = -1;
    for (int k = 0; k < n; ++k)
        if (st[k] == EvalOk && (best < 0 || (fn == 0 ? px[k] < px[best] : px[k] > px[best]))) best = k;
    bool all = true;
    for (int k = 0; k < n; ++k) if (st[k] != EvalOk) all = false;
    if (all) return args[best];
    if (fn == 0) {
        // min(100%, 40rem): on a small screen the percentage is usually the smaller one.
        for (int k = 0; k < n; ++k) if (st[k] == EvalPercent) return args[k];
        return args[0];
    }
    return best >= 0 ? args[best] : args[0];
}

static const char *const kMinMax[] = { "min", "max", "clamp", 0 };

static QByteArray resolveMinMax(const QByteArray &v)
{
    int open, which;
    int f = findFunc(v, 0, kMinMax, &open, &which);
    if (f < 0) return v;
    QByteArray out;
    int pos = 0;
    while (f >= 0) {
        int close = matchParen(v.constData(), v.size(), open);
        if (close < 0) break;
        QByteArray inner = resolveMinMax(v.mid(open + 1, close - open - 1));
        QList<QByteArray> args = splitArgs(inner);
        for (int k = 0; k < args.size(); ++k) args[k] = trimmed(args[k]);
        QByteArray pick = pickArg(which, args);
        out += v.mid(pos, f - pos);
        out += hasOperator(pick) ? "calc(" + pick + ")" : pick;
        pos = close + 1;
        f = findFunc(v, pos, kMinMax, &open, &which);
    }
    out += v.mid(pos);
    return out;
}

// ---------------------------------------------------------------------------
// var()

struct Lookup {
    const Vars *local, *file, *page;
    Stats *stats;
    bool find(const QByteArray &name, QByteArray *val) const
    {
        const Vars *maps[3] = { local, file, page };
        for (int k = 0; k < 3; ++k) {
            if (!maps[k]) continue;
            Vars::const_iterator it = maps[k]->constFind(name);
            if (it != maps[k]->constEnd()) { *val = it.value(); return true; }
        }
        return false;
    }
};

static const char *const kVar[] = { "var", 0 };

static bool resolveVars(const QByteArray &v, const Lookup &lk, int depth, QByteArray *out)
{
    if (depth > kMaxVarDepth) return false;
    int open;
    int f = findFunc(v, 0, kVar, &open, 0);
    if (f < 0) { *out = v; return true; }
    QByteArray res;
    int pos = 0;
    while (f >= 0) {
        int close = matchParen(v.constData(), v.size(), open);
        if (close < 0) return false;
        QByteArray inner = v.mid(open + 1, close - open - 1);
        int comma = -1;
        { int d = 0; const char *ip = inner.constData(); for (int k = 0; k < inner.size(); ++k) {
            char c = ip[k];
            if (c == '(') ++d; else if (c == ')') --d; else if (c == ',' && d == 0) { comma = k; break; } } }
        QByteArray name = trimmed(comma < 0 ? inner : inner.left(comma));
        QByteArray val, sub;
        bool ok = false;
        if (lk.find(name, &val)) ok = resolveVars(val, lk, depth + 1, &sub);
        if (!ok && comma >= 0) ok = resolveVars(trimmed(inner.mid(comma + 1)), lk, depth + 1, &sub);
        if (!ok) return false;
        if (lk.stats) ++lk.stats->vars;
        res += v.mid(pos, f - pos);
        res += sub;
        pos = close + 1;
        f = findFunc(v, pos, kVar, &open, 0);
    }
    res += v.mid(pos);
    *out = res;
    return true;
}

// ---------------------------------------------------------------------------
// calc()

static const char *const kTranslate[] = { "translatex", "translatey", "translate", 0 };

// "calc(a - b + c)" -> ["a", "-b", "c"] when every term is a single number/dimension.
static bool calcTerms(const QByteArray &arg, QList<QByteArray> *terms)
{
    QByteArray a = trimmed(arg);
    int open;
    static const char *const kCalc[] = { "calc", "-webkit-calc", 0 };
    if (findFunc(a, 0, kCalc, &open, 0) != 0) return false;
    if (matchParen(a.constData(), a.size(), open) != a.size() - 1) return false;
    QList<QByteArray> tok = a.mid(open + 1, a.size() - open - 2).simplified().split(' ');
    bool neg = false;
    for (int k = 0; k < tok.size(); ++k) {
        const QByteArray &t = tok[k];
        if (k % 2 == 1) {
            if (t == "+") neg = false;
            else if (t == "-") neg = true;
            else return false;
            continue;
        }
        if (t.isEmpty() || t.contains('(') || t.contains('*') || t.contains('/')) return false;
        if (!neg) *terms << t;
        else *terms << (t.startsWith('-') ? t.mid(1) : t.startsWith('+') ? "-" + t.mid(1) : "-" + t);
    }
    return tok.size() % 2 == 1 && terms->size() > 1;
}

// Old WebKit has -webkit-calc() for lengths but not inside transform functions. A translation
// by a sum is the same as one translation per term, so split those.
static QByteArray splitTranslate(const QByteArray &v)
{
    int open, which;
    int f = findFunc(v, 0, kTranslate, &open, &which);
    if (f < 0) return v;
    QByteArray out;
    int pos = 0;
    while (f >= 0) {
        int close = matchParen(v.constData(), v.size(), open);
        if (close < 0) break;
        QList<QByteArray> args = splitArgs(v.mid(open + 1, close - open - 1));
        QByteArray rep;
        bool split = false;
        static const char *const axisFn[] = { "translateX(", "translateY(" };
        for (int k = 0; k < args.size() && k < 2; ++k) {
            int axis = which == 2 ? k : which;
            QList<QByteArray> terms;
            if (calcTerms(args[k], &terms)) split = true;
            else terms << trimmed(args[k]);
            for (int t = 0; t < terms.size(); ++t) {
                if (!rep.isEmpty()) rep += ' ';
                rep += axisFn[axis] + terms[t] + ')';
            }
        }
        out += v.mid(pos, f - pos);
        out += (split && args.size() <= 2) ? rep : v.mid(f, close + 1 - f);
        pos = close + 1;
        f = findFunc(v, pos, kTranslate, &open, &which);
    }
    out += v.mid(pos);
    return out;
}

// calc( -> -webkit-calc( ; a calc() nested in another one becomes plain parentheses.
static QByteArray prefixCalc(const QByteArray &v)
{
    const char *s = v.constData();
    int n = v.size();
    QByteArray out;
    out.reserve(n + 16);
    QVector<bool> stack;   // one entry per open '(': is it a calc?
    int calcDepth = 0;
    for (int i = 0; i < n; ) {
        char c = s[i];
        if (c == '"' || c == '\'') { int e = skipString(s, n, i); out.append(s + i, e - i); i = e; continue; }
        if (isIdent(c) && (i == 0 || !isIdent(s[i - 1]))) {
            int e = i;
            while (e < n && isIdent(s[e])) ++e;
            if (e < n && s[e] == '(') {
                if (e - i == 3 && at(s, n, i, "url")) {
                    int m = matchParen(s, n, e);
                    m = m < 0 ? n : m + 1;
                    out.append(s + i, m - i);
                    i = m;
                    continue;
                }
                bool isCalc = (e - i == 4 && at(s, n, i, "calc")) || (e - i == 12 && at(s, n, i, "-webkit-calc"))
                              || (e - i == 9 && at(s, n, i, "-moz-calc"));
                if (isCalc) out += calcDepth ? "(" : "-webkit-calc(";
                else out.append(s + i, e - i + 1);
                stack.append(isCalc);
                if (isCalc) ++calcDepth;
                i = e + 1;
                continue;
            }
            out.append(s + i, e - i);
            i = e;
            continue;
        }
        if (c == '(') stack.append(false);
        else if (c == ')' && !stack.isEmpty()) { if (stack.last()) --calcDepth; stack.removeLast(); }
        out += c;
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Declarations

static bool needsPrefix(const QByteArray &p)
{
    static const char *const names[] = {
        "transform", "transform-origin", "transform-style", "transition", "transition-property",
        "transition-duration", "transition-timing-function", "transition-delay", "perspective",
        "perspective-origin", "backface-visibility", "flex", "flex-grow", "flex-shrink", "flex-basis",
        "flex-direction", "flex-wrap", "flex-flow", "order", "justify-content", "align-items",
        "align-self", "align-content", 0 };
    for (int k = 0; names[k]; ++k) if (p == names[k]) return true;
    return false;
}

// transition: transform .3s -> -webkit-transition: -webkit-transform .3s
static QByteArray webkitTransitionValue(const QByteArray &v)
{
    QByteArray out;
    const char *s = v.constData();
    int n = v.size();
    for (int i = 0; i < n; ) {
        if (at(s, n, i, "transform") && (i == 0 || !isIdent(s[i - 1]))) {
            out += "-webkit-transform";
            i += 9;
            continue;
        }
        out += s[i++];
    }
    return out;
}

struct Decl {
    QByteArray raw;      // original text, no ';'
    QByteArray name;     // as written (custom properties are case-sensitive)
    QByteArray lname;    // lowercase
    QByteArray value;    // comments removed, trimmed, without !important
    bool important;
    bool ok;             // parsed as name: value
};

static QByteArray stripComments(const QByteArray &a)
{
    if (!a.contains("/*")) return a;
    QByteArray out;
    const char *s = a.constData();
    int n = a.size();
    for (int i = 0; i < n; ) {
        if (s[i] == '"' || s[i] == '\'') { int e = skipString(s, n, i); out.append(s + i, e - i); i = e; }
        else if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') { i = skipComment(s, n, i); out += ' '; }
        else out += s[i++];
    }
    return out;
}

static QByteArray range(const char *s, int a, int b)   // s[a..b) without surrounding spaces
{
    while (a < b && isSpace(s[a])) ++a;
    while (b > a && isSpace(s[b - 1])) --b;
    return QByteArray(s + a, b - a);
}

static bool hasUpper(const QByteArray &a)
{
    for (const char *p = a.constData(), *e = p + a.size(); p < e; ++p) if (*p >= 'A' && *p <= 'Z') return true;
    return false;
}

static QList<Decl> splitDecls(const QByteArray &body)
{
    QList<Decl> out;
    const char *s = body.constData();
    int n = body.size(), depth = 0, start = 0, colon = -1;
    bool comment = false, nameEnded = false;
    for (int i = 0; i <= n; ) {
        if (i < n) {
            char c = s[i];
            if (c == '"' || c == '\'') { i = skipString(s, n, i); nameEnded = true; continue; }
            if (c == '/' && i + 1 < n && s[i + 1] == '*') { i = skipComment(s, n, i); comment = true; continue; }
            if (colon < 0 && !nameEnded) {
                if (c == ':') colon = i;
                else if (!isIdent(c) && !isSpace(c)) nameEnded = true;
            }
            if (c == '(' || c == '[') { ++depth; ++i; continue; }
            if (c == ')' || c == ']') { if (depth) --depth; ++i; continue; }
            if (c != ';' || depth) { ++i; continue; }
        }
        Decl d;
        d.raw = QByteArray::fromRawData(s + start, i - start);   // body outlives the list
        d.important = false;
        d.ok = false;
        if (comment) {   // rare: parse a copy without the comments
            QByteArray t = stripComments(d.raw);
            colon = -1;
            for (int k = 0; k < t.size(); ++k) {
                char c = t.constData()[k];
                if (c == ':') { colon = k; break; }
                if (!isIdent(c) && !isSpace(c)) break;
            }
            if (colon > 0) { d.name = range(t.constData(), 0, colon); d.value = range(t.constData(), colon + 1, t.size()); }
        } else if (colon > start) {
            d.name = range(s, start, colon);
            d.value = range(s, colon + 1, i);
        }
        if (!d.name.isEmpty()) {
            d.lname = hasUpper(d.name) ? d.name.toLower() : d.name;
            int bang = d.value.lastIndexOf('!');
            if (bang >= 0 && range(d.value.constData(), bang + 1, d.value.size()).toLower() == "important") {
                d.important = true;
                d.value = range(d.value.constData(), 0, bang);
            }
            d.ok = !d.name.contains(' ');
        }
        if (i < n || !range(s, start, i).isEmpty()) out << d;
        start = i + 1;
        colon = -1;
        comment = nameEnded = false;
        ++i;
    }
    return out;
}

// Logical properties (CSS 2017+) -> what this WebKit has, assuming left-to-right text.
// Shorthands with two values give start, end.
struct Logical { const char *name, *start, *end; };
static const Logical kLogical[] = {
    { "inset-inline-start", "left", 0 }, { "inset-inline-end", "right", 0 },
    { "inset-block-start", "top", 0 }, { "inset-block-end", "bottom", 0 },
    { "inset-inline", "left", "right" }, { "inset-block", "top", "bottom" },
    { "margin-inline-start", "-webkit-margin-start", 0 }, { "margin-inline-end", "-webkit-margin-end", 0 },
    { "margin-block-start", "-webkit-margin-before", 0 }, { "margin-block-end", "-webkit-margin-after", 0 },
    { "margin-inline", "-webkit-margin-start", "-webkit-margin-end" },
    { "margin-block", "-webkit-margin-before", "-webkit-margin-after" },
    { "padding-inline-start", "-webkit-padding-start", 0 }, { "padding-inline-end", "-webkit-padding-end", 0 },
    { "padding-block-start", "-webkit-padding-before", 0 }, { "padding-block-end", "-webkit-padding-after", 0 },
    { "padding-inline", "-webkit-padding-start", "-webkit-padding-end" },
    { "padding-block", "-webkit-padding-before", "-webkit-padding-after" },
    { 0, 0, 0 } };

static const Logical *logical(const QByteArray &p)
{
    if (!p.contains("-inline") && !p.contains("-block")) return 0;
    for (int k = 0; kLogical[k].name; ++k) if (p == kLogical[k].name) return &kLogical[k];
    return 0;
}

static bool interesting(const Decl &d)
{
    if (d.value.contains('(')) return true;
    return needsPrefix(d.lname) || d.lname == "display" || d.lname == "position" || d.lname == "inset"
           || logical(d.lname);
}

// Cheap test before splitting a block into declarations (most blocks need nothing).
static bool has(const char *s, int n, const char *lit)
{
    return memmem(s, n, lit, strlen(lit)) != 0;
}

static bool maybeInteresting(const char *s, int n)
{
    static const char *const keys[] = { "var(", "calc(", "min(", "max(", "clamp(", "transform", "transition",
        "flex", "order", "justify-content", "align-", "position", "inset", "perspective", "backface",
        "-inline", "-block", 0 };
    for (int k = 0; keys[k]; ++k) if (has(s, n, keys[k])) return true;
    return false;
}

// Rewrites one declaration block. collect: only record custom properties into *collect.
static QByteArray rewriteBlock(const QByteArray &body, const Vars *file, const Vars *page, Vars *collect,
                               bool collectOnly, Stats *stats)
{
    QList<Decl> decls = splitDecls(body);
    Vars local;
    bool any = false;
    for (int k = 0; k < decls.size(); ++k) {
        const Decl &d = decls[k];
        if (!d.ok) continue;
        if (d.name.startsWith("--")) {
            local.insert(d.name, d.value);
            if (collect) collect->insert(d.name, d.value);
        } else if (!collectOnly && !any && interesting(d)) any = true;
    }
    if (collectOnly || !any) return body;
    QHash<QByteArray, bool> present;
    for (int k = 0; k < decls.size(); ++k) {
        const Decl &d = decls[k];
        if (!d.ok || d.name.startsWith("--")) continue;
        present.insert(d.lname, true);
        if (d.lname == "display" && d.value.toLower().startsWith("-webkit-")) present.insert("display:-webkit-", true);
        if (d.lname == "position" && d.value.toLower() == "-webkit-sticky") present.insert("position:-webkit-sticky", true);
    }

    Lookup lk;
    lk.local = &local; lk.file = file; lk.page = page; lk.stats = stats;
    QByteArray out;
    out.reserve(body.size() + 64);
    bool changed = false;
    for (int k = 0; k < decls.size(); ++k) {
        const Decl &d = decls[k];
        if (k) out += ';';
        if (!d.ok || d.name.startsWith("--") || !interesting(d)) { out += d.raw; continue; }
        QByteArray v = d.value;
        if (containsCI(v, "var(") && !resolveVars(v, lk, 0, &v)) { out += d.raw; continue; }
        if (v.contains('(')) {
            if (containsCI(v, "min(") || containsCI(v, "max(") || containsCI(v, "clamp(")) v = resolveMinMax(v);
            if (containsCI(v, "calc(")) {
                if (d.lname == "transform" || d.lname == "-webkit-transform") v = splitTranslate(v);
                v = prefixCalc(v);
            }
        }
        const char *imp = d.important ? " !important" : "";
        QByteArray lv = v.toLower();
        int before = out.size();
        if (needsPrefix(d.lname) && !present.contains("-webkit-" + d.lname)) {
            QByteArray pv = d.lname.startsWith("transition") ? webkitTransitionValue(v) : v;
            out += "-webkit-" + d.lname + ':' + pv + imp + ';';
        } else if (d.lname == "display" && (lv == "flex" || lv == "inline-flex") && !present.contains("display:-webkit-")) {
            out += "display:-webkit-" + lv + imp + ';';
        } else if (d.lname == "position" && lv == "sticky" && !present.contains("position:-webkit-sticky")) {
            out += QByteArray("position:-webkit-sticky") + imp + ';';
        } else if (d.lname == "inset") {
            QList<QByteArray> p = v.simplified().split(' ');
            if (p.size() >= 1 && p.size() <= 4 && !v.contains('(')) {
                QByteArray t = p[0], r = p.size() > 1 ? p[1] : t, b = p.size() > 2 ? p[2] : t, l = p.size() > 3 ? p[3] : r;
                out += "top:" + t + imp + ";right:" + r + imp + ";bottom:" + b + imp + ";left:" + l + imp + ';';
            }
        } else if (const Logical *lg = logical(d.lname)) {
            QList<QByteArray> p = v.simplified().split(' ');
            if (p.size() <= (lg->end ? 2 : 1) && !v.contains('(') && !present.contains(lg->start)) {
                out += lg->start + (':' + p[0]) + imp + ';';
                if (lg->end) out += lg->end + (':' + p.last()) + imp + ';';
            }
        }
        bool added = out.size() != before;
        if (v != d.value) out += d.name + ':' + v + imp;
        else out += d.raw;
        if (added || v != d.value) { changed = true; if (stats) ++stats->decls; }
    }
    if (!changed) return body;
    return out;
}

// ---------------------------------------------------------------------------
// Style sheets

static bool isRootSelector(const QByteArray &prelude)
{
    const char *p = prelude.constData();
    int n = prelude.size();
    if (!has(p, n, "root") && !has(p, n, "html") && !has(p, n, "host") && !has(p, n, "body")) return false;
    QList<QByteArray> sels = splitArgs(prelude);
    for (int k = 0; k < sels.size(); ++k) {
        QByteArray s = trimmed(stripComments(sels[k])).toLower();
        if (s == ":root" || s == "html" || s == ":host" || s == "html:root" || s == "body" || s == ":root:root")
            return true;
    }
    return false;
}

struct Sheet {
    const char *s;
    int n;
    QByteArray out;
    bool collectOnly;
    Vars *top, *media;       // collect pass: custom properties of :root rules
    const Vars *file, *page;
    Stats *stats;

    void emit(int from, int to) { if (!collectOnly && to > from) out.append(s + from, to - from); }
    void emit(const QByteArray &a) { if (!collectOnly) out += a; }

    // Index of the '{', ';' or '}' that ends a prelude starting at i (or n).
    int preludeEnd(int i)
    {
        int depth = 0;
        while (i < n) {
            char c = s[i];
            if (c == '"' || c == '\'') { i = skipString(s, n, i); continue; }
            if (c == '/' && i + 1 < n && s[i + 1] == '*') { i = skipComment(s, n, i); continue; }
            if (c == '(' || c == '[') ++depth;
            else if (c == ')' || c == ']') { if (depth) --depth; }
            else if (depth == 0 && (c == '{' || c == ';' || c == '}')) return i;
            ++i;
        }
        return n;
    }

    // s[i] == '{': index of the matching '}' (or n); *nested set if it holds another '{'.
    int blockEnd(int i, bool *nested)
    {
        int depth = 0;
        *nested = false;
        while (i < n) {
            char c = s[i];
            if (c == '"' || c == '\'') { i = skipString(s, n, i); continue; }
            if (c == '/' && i + 1 < n && s[i + 1] == '*') { i = skipComment(s, n, i); continue; }
            if (c == '{') { if (depth) *nested = true; ++depth; }
            else if (c == '}' && --depth == 0) return i;
            ++i;
        }
        return n;
    }

    // scope: 0 top level, 1 inside @media/@supports, 2 where custom properties aren't collected
    void rules(int &i, bool nested, int scope)
    {
        while (i < n) {
            char c = s[i];
            if (isSpace(c)) { emit(i, i + 1); ++i; continue; }
            if (c == '/' && i + 1 < n && s[i + 1] == '*') { int e = skipComment(s, n, i); emit(i, e); i = e; continue; }
            if (c == '}') {
                if (nested) return;
                emit(i, i + 1); ++i; continue;
            }
            int start = i, j = preludeEnd(i);
            if (j >= n) { emit(start, n); i = n; return; }
            if (s[j] != '{') { emit(start, j + (s[j] == ';')); i = j + (s[j] == ';'); continue; }
            QByteArray prelude = QByteArray::fromRawData(s + start, j - start);   // the sheet outlives it
            if (c == '@') {
                int e = start + 1;
                while (e < j && isIdent(s[e])) ++e;
                QByteArray name = QByteArray(s + start + 1, e - start - 1).toLower();
                if (name == "layer") {
                    // Cascade layers are unknown to this WebKit, which drops the whole block.
                    i = j + 1;
                    rules(i, true, scope);
                    if (i < n) ++i;
                    if (stats) ++stats->decls;
                    continue;
                }
                if (name == "media" || name == "supports" || name == "document" || name == "-moz-document"
                    || name == "container" || name == "scope" || name.endsWith("keyframes")) {
                    int sc = scope > 1 ? 2 : 1;
                    if (name.endsWith("keyframes")) sc = 2;
                    else if (name == "media") {
                        QByteArray q = prelude.toLower();
                        q.replace(" ", "");
                        if (q.contains("dark") || q.contains("print") || q.contains("forced-colors")
                            || q.contains("prefers-contrast")) sc = 2;
                    }
                    emit(start, j + 1);
                    i = j + 1;
                    rules(i, true, sc);
                    if (i < n) { emit(i, i + 1); ++i; }
                    continue;
                }
                bool nest;
                int k = blockEnd(j, &nest);
                if ((name == "font-face" || name == "page") && !nest && k < n)
                    declBlock(start, j, k, 0);
                else
                    emit(start, k < n ? k + 1 : n);
                i = k < n ? k + 1 : n;
                continue;
            }
            bool nest;
            int k = blockEnd(j, &nest);
            if (nest || k >= n) { emit(start, k < n ? k + 1 : n); i = k < n ? k + 1 : n; continue; }
            Vars *target = 0;
            if (collectOnly && scope < 2 && isRootSelector(prelude)) target = scope == 0 ? top : media;
            declBlock(start, j, k, target);
            i = k + 1;
        }
    }

    void declBlock(int start, int open, int close, Vars *target)
    {
        const char *b = s + open + 1;
        int bn = close - open - 1;
        if (collectOnly ? !target || !has(b, bn, "--") : !maybeInteresting(b, bn)) {
            emit(start, close + 1);
            return;
        }
        QByteArray body = QByteArray::fromRawData(b, bn);
        QByteArray r = rewriteBlock(body, file, page, target, collectOnly, stats);
        if (collectOnly) return;
        emit(start, open + 1);
        out += r;
        out += '}';
    }
};

static bool looksUtf16(const QByteArray &a)
{
    int n = qMin(a.size(), 64);
    if (a.size() >= 2 && (((uchar)a[0] == 0xFE && (uchar)a[1] == 0xFF) || ((uchar)a[0] == 0xFF && (uchar)a[1] == 0xFE)))
        return true;
    return memchr(a.constData(), 0, n) != 0;
}

QByteArray rewriteSheet(const QByteArray &css, Vars *pageVars, Stats *stats)
{
    if (css.isEmpty() || looksUtf16(css)) return css;
    Vars top, media;
    Sheet sh;
    sh.s = css.constData(); sh.n = css.size(); sh.stats = stats;
    sh.page = pageVars;
    if (css.contains("--")) {
        sh.collectOnly = true; sh.top = &top; sh.media = &media; sh.file = 0;
        int i = 0;
        sh.rules(i, false, 0);
        for (Vars::const_iterator it = media.constBegin(); it != media.constEnd(); ++it)
            if (!top.contains(it.key())) top.insert(it.key(), it.value());
    }
    sh.collectOnly = false; sh.top = sh.media = 0; sh.file = &top;
    sh.out.reserve(css.size() + css.size() / 8);
    int i = 0;
    sh.rules(i, false, 0);
    if (pageVars)
        for (Vars::const_iterator it = top.constBegin(); it != top.constEnd(); ++it)
            if (!pageVars->contains(it.key()))   // first wins: a dark-scheme sheet loaded with
                pageVars->insert(it.key(), it.value());   // media="(prefers-color-scheme: dark)" comes later
    return sh.out;
}

QByteArray rewriteDeclarations(const QByteArray &decls, const Vars *pageVars, Stats *stats)
{
    if (!maybeInteresting(decls.constData(), decls.size())) return decls;
    return rewriteBlock(decls, 0, pageVars, 0, false, stats);
}

// ---------------------------------------------------------------------------
// HTML

// s[i] == '<': index after the '>' closing the tag (quotes respected), or -1.
static int tagEnd(const char *s, int n, int i)
{
    char q = 0;
    for (++i; i < n; ++i) {
        char c = s[i];
        if (q) { if (c == q) q = 0; }
        else if (c == '"' || c == '\'') {
            if (isSpace(s[i - 1]) || s[i - 1] == '=') q = c;
        } else if (c == '>') return i + 1;
    }
    return -1;
}

// Finds "</name" (case-insensitive) from i; -1 if absent.
static int findClose(const char *s, int n, int i, const char *name)
{
    for (; i + 1 < n; ++i)
        if (s[i] == '<' && s[i + 1] == '/' && at(s, n, i + 2, name)) return i;
    return -1;
}

// A start tag with a style="" attribute: rewrite that attribute's declarations.
static QByteArray rewriteTag(const QByteArray &tag, const Vars *vars, Stats *stats)
{
    if (!containsCI(tag, "style")) return tag;
    const char *s = tag.constData();
    int n = tag.size(), i = 1;
    while (i < n && !isSpace(s[i]) && s[i] != '>' && s[i] != '/') ++i;   // tag name
    for (;;) {
        while (i < n && (isSpace(s[i]) || s[i] == '/')) ++i;
        if (i >= n || s[i] == '>') return tag;
        int ns = i;
        while (i < n && !isSpace(s[i]) && s[i] != '=' && s[i] != '>' && s[i] != '/') ++i;
        bool isStyle = i - ns == 5 && at(s, n, ns, "style");
        while (i < n && isSpace(s[i])) ++i;
        if (i >= n || s[i] != '=') continue;
        ++i;
        while (i < n && isSpace(s[i])) ++i;
        int vs, ve;
        if (i < n && (s[i] == '"' || s[i] == '\'')) {
            char q = s[i];
            vs = ++i;
            while (i < n && s[i] != q) ++i;
            ve = i;
            if (i < n) ++i;
        } else {
            vs = i;
            while (i < n && !isSpace(s[i]) && s[i] != '>') ++i;
            ve = i;
        }
        if (isStyle) {
            QByteArray val(s + vs, ve - vs);
            QByteArray r = rewriteDeclarations(val, vars, stats);
            if (r == val) return tag;
            if (vs > 0 && (s[vs - 1] == '"' || s[vs - 1] == '\''))
                return tag.left(vs) + r + tag.mid(ve);
            return tag.left(vs) + '"' + r + '"' + tag.mid(ve);   // was unquoted
        }
    }
}

static const int kMaxHold = 2 * 1024 * 1024;

QByteArray HtmlRewriter::feed(const QByteArray &data)
{
    if (m_off) return data;
    if (m_pending.isEmpty() && looksUtf16(data)) { m_off = true; return data; }
    m_pending += data;
    return run(false);
}

QByteArray HtmlRewriter::finish()
{
    if (m_off) return QByteArray();
    QByteArray out = run(true);
    out += m_pending;
    m_pending.clear();
    return out;
}

QByteArray HtmlRewriter::run(bool final)
{
    const char *s = m_pending.constData();
    int n = m_pending.size(), i = 0;
    QByteArray out;
    out.reserve(n + 64);
    while (i < n) {
        if (m_inScript) {
            int c = findClose(s, n, i, "script");
            if (c < 0) {
                int keep = qMax(i, n - 8);   // "</script" may be split across chunks
                out.append(s + i, keep - i);
                i = keep;
                break;
            }
            out.append(s + i, c - i);
            i = c;
            m_inScript = false;
            continue;
        }
        const char *lt = (const char *)memchr(s + i, '<', n - i);
        if (!lt) { out.append(s + i, n - i); i = n; break; }
        int p = lt - s;
        out.append(s + i, p - i);
        i = p;
        if (n - i < 10 && !final) break;   // too short to tell what the tag is
        if (at(s, n, i, "<!--")) {
            int e = -1;
            for (int k = i + 4; k + 2 < n; ++k) if (s[k] == '-' && s[k + 1] == '-' && s[k + 2] == '>') { e = k + 3; break; }
            if (e < 0) break;
            out.append(s + i, e - i);
            i = e;
            continue;
        }
        char c1 = i + 1 < n ? s[i + 1] : 0;
        if (!((c1 >= 'a' && c1 <= 'z') || (c1 >= 'A' && c1 <= 'Z') || c1 == '/' || c1 == '!' || c1 == '?')) {
            out += '<';
            ++i;
            continue;
        }
        int e = tagEnd(s, n, i);
        if (e < 0) break;
        bool isStyle = at(s, n, i, "<style") && (isSpace(s[i + 6]) || s[i + 6] == '>' || s[i + 6] == '/');
        bool isScript = at(s, n, i, "<script") && (isSpace(s[i + 7]) || s[i + 7] == '>');
        if (isStyle) {
            int c = findClose(s, n, e, "style");
            if (c < 0) break;
            out.append(s + i, e - i);
            out += rewriteSheet(QByteArray(s + e, c - e), m_vars, &stats);
            i = c;
            continue;
        }
        QByteArray tag(s + i, e - i);
        out += c1 == '/' || c1 == '!' || c1 == '?' ? tag : rewriteTag(tag, m_vars, &stats);
        i = e;
        if (isScript) m_inScript = true;
    }
    m_pending = m_pending.mid(i);
    if (final || m_pending.size() > kMaxHold) {   // unfinished tag at the end, or far too long: pass on
        out += m_pending;
        m_pending.clear();
    }
    return out;
}

}
