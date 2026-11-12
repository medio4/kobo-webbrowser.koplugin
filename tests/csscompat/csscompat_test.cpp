// Desktop tests for csscompat.cpp (no WebKit needed): ./run.sh [file.css ...]
// Each extra file is rewritten 20 times to time it; the output goes to <file>.out.css.
#include "../../csscompat.h"
#include <QtCore/QFile>
#include <QtCore/QElapsedTimer>
#include <cstdio>

static int failures = 0;

static void check(const char *what, const QByteArray &got, const QByteArray &want)
{
    if (got == want) return;
    ++failures;
    printf("FAIL %s\n  got:  %s\n  want: %s\n", what, got.constData(), want.constData());
}

static QByteArray sheet(const char *css, CssCompat::Vars *vars = 0)
{
    return CssCompat::rewriteSheet(QByteArray(css), vars);
}

static QByteArray html(const QList<QByteArray> &chunks)
{
    CssCompat::Vars vars;
    CssCompat::HtmlRewriter r(&vars);
    QByteArray out;
    foreach (const QByteArray &c, chunks) out += r.feed(c);
    return out + r.finish();
}

int main(int argc, char **argv)
{
    // Prefixes
    check("transform", sheet("a{transform:rotate(1deg)}"), "a{-webkit-transform:rotate(1deg);transform:rotate(1deg)}");
    check("already prefixed", sheet("a{-webkit-transform:none;transform:none}"), "a{-webkit-transform:none;transform:none}");
    check("transition value", sheet("a{transition:transform .3s,opacity 1s}"),
          "a{-webkit-transition:-webkit-transform .3s,opacity 1s;transition:transform .3s,opacity 1s}");
    check("display flex", sheet(".r{display:flex;flex-wrap:wrap}"),
          ".r{display:-webkit-flex;display:flex;-webkit-flex-wrap:wrap;flex-wrap:wrap}");
    check("display flex important", sheet(".r{display: inline-flex !important}"),
          ".r{display:-webkit-inline-flex !important;display: inline-flex !important}");
    check("autoprefixed flex kept", sheet(".r{display:-webkit-box;display:-webkit-flex;display:flex}"),
          ".r{display:-webkit-box;display:-webkit-flex;display:flex}");
    check("sticky", sheet("h{position:sticky;top:0}"), "h{position:-webkit-sticky;position:sticky;top:0}");
    check("inset", sheet("i{inset:0}"), "i{top:0;right:0;bottom:0;left:0;inset:0}");
    check("inset 2", sheet("i{inset:1px 2px}"), "i{top:1px;right:2px;bottom:1px;left:2px;inset:1px 2px}");
    check("logical inset", sheet("m{inset-inline-start:0}"), "m{left:0;inset-inline-start:0}");
    check("logical margin", sheet("m{margin-inline:auto 1em}"), "m{-webkit-margin-start:auto;-webkit-margin-end:1em;margin-inline:auto 1em}");
    check("logical with physical present", sheet("m{left:2px;inset-inline-start:0}"), "m{left:2px;inset-inline-start:0}");
    check("untouched", sheet("p{color:red; margin:0 auto}\n"), "p{color:red; margin:0 auto}\n");

    // calc
    check("calc", sheet("a{width:calc(100% - 2em)}"), "a{width:-webkit-calc(100% - 2em)}");
    check("nested calc", sheet("a{width:calc(100% - calc(2em + 1px))}"), "a{width:-webkit-calc(100% - (2em + 1px))}");
    check("translate calc", sheet("m{transform:translateX(calc(-100% - 10px))}"),
          "m{-webkit-transform:translateX(-100%) translateX(-10px);transform:translateX(-100%) translateX(-10px)}");
    check("translate 2 args", sheet("m{transform:translate(calc(50% + 4px), -50%)}"),
          "m{-webkit-transform:translateX(50%) translateX(4px) translateY(-50%);transform:translateX(50%) translateX(4px) translateY(-50%)}");
    check("calc in url untouched", sheet("a{background:url(calc(1).png)}"), "a{background:url(calc(1).png)}");

    // var()
    check("var root", sheet(":root{--c:#123}a{color:var(--c)}"), ":root{--c:#123}a{color:#123}");
    check("var defined later", sheet("a{color:var(--c)}html{--c:blue}"), "a{color:blue}html{--c:blue}");
    check("var fallback", sheet("a{color:var(--nope, red)}"), "a{color:red}");
    check("var unresolved left alone", sheet("a{color:var(--nope)}"), "a{color:var(--nope)}");
    check("var local", sheet(".b{--p:4px;padding:var(--p) 0}"), ".b{--p:4px;padding:4px 0}");
    check("var chain", sheet(":root{--a:var(--b);--b:2px}a{margin:var(--a)}"), ":root{--a:var(--b);--b:2px}a{margin:2px}");
    check("var cycle", sheet(":root{--a:var(--a)}a{margin:var(--a)}"), ":root{--a:var(--a)}a{margin:var(--a)}");
    check("var in calc", sheet(":root{--h:3rem}a{top:calc(var(--h) + 1px)}"), ":root{--h:3rem}a{top:-webkit-calc(3rem + 1px)}");
    check("dark media not collected", sheet(":root{--c:#fff}@media (prefers-color-scheme: dark){:root{--c:#000}}a{color:var(--c)}"),
          ":root{--c:#fff}@media (prefers-color-scheme: dark){:root{--c:#000}}a{color:#fff}");
    {
        CssCompat::Vars v;
        sheet(":root{--x:1px}", &v);
        check("page vars across sheets", sheet("a{margin:var(--x)}", &v), "a{margin:1px}");
        sheet(":root{--x:9px}", &v);
        check("page vars first wins", sheet("a{margin:var(--x)}", &v), "a{margin:1px}");
    }

    // min / max / clamp
    check("min", sheet("a{width:min(100%, 40rem)}"), "a{width:100%}");
    check("min abs", sheet("a{width:min(90vw, 600px)}"), "a{width:90vw}");
    check("max", sheet("a{padding:max(1rem, 2px)}"), "a{padding:1rem}");
    check("clamp mid", sheet("h1{font-size:clamp(1.5rem, 5vw, 3rem)}"), "h1{font-size:5vw}");
    check("clamp low", sheet("h1{font-size:clamp(2rem, 2vw, 3rem)}"), "h1{font-size:2rem}");
    check("clamp expr", sheet("h1{font-size:clamp(1rem, 1rem + 1vw, 2rem)}"), "h1{font-size:-webkit-calc(1rem + 1vw)}");
    check("minmax untouched", sheet("g{grid-template-columns:minmax(0,1fr)}"), "g{grid-template-columns:minmax(0,1fr)}");

    // Structure
    check("media kept", sheet("@media (max-width:600px){a{display:flex}}"), "@media (max-width:600px){a{display:-webkit-flex;display:flex}}");
    check("layer unwrapped", sheet("@layer base{a{color:red}}"), "a{color:red}");
    check("layer statement kept", sheet("@layer base, ui;a{color:red}"), "@layer base, ui;a{color:red}");
    check("keyframes", sheet("@keyframes k{from{transform:none}}"), "@keyframes k{from{-webkit-transform:none;transform:none}}");
    check("strings and comments", sheet("a::after{content:\"{transform:x}\"/* } */}b{order:1}"),
          "a::after{content:\"{transform:x}\"/* } */}b{-webkit-order:1;order:1}");
    check("nesting left alone", sheet(".a{display:flex;.b{color:red}}"), ".a{display:flex;.b{color:red}}");
    check("import", sheet("@import url(x.css);a{order:2}"), "@import url(x.css);a{-webkit-order:2;order:2}");

    // HTML
    QList<QByteArray> c;
    c << "<html><head><style>:root{--g:8px}.x{display:flex;gap:var(--g)}</style></head>"
      << "<body><div style=\"margin:var(--g)\">a<b</div>"
      << "<script>if (a<b) x='<style>a{order:1}</style>';</script><p style='order:2'></p></body></html>";
    check("html", html(c),
          "<html><head><style>:root{--g:8px}.x{display:-webkit-flex;display:flex;gap:8px}</style></head>"
          "<body><div style=\"margin:8px\">a<b</div>"
          "<script>if (a<b) x='<style>a{order:1}</style>';</script><p style='-webkit-order:2;order:2'></p></body></html>");
    QList<QByteArray> split;
    split << "<div st" << "yle=\"display:fl" << "ex\">x</div><sty" << "le>a{order:1}</st" << "yle>";
    check("html split chunks", html(split), "<div style=\"display:-webkit-flex;display:flex\">x</div><style>a{-webkit-order:1;order:1}</style>");
    QList<QByteArray> unq;
    unq << "<p style=order:1>x</p>";
    check("html unquoted", html(unq), "<p style=\"-webkit-order:1;order:1\">x</p>");

    for (int k = 1; k < argc; ++k) {
        QFile f(argv[k]);
        if (!f.open(QIODevice::ReadOnly)) { printf("can't read %s\n", argv[k]); continue; }
        QByteArray in = f.readAll(), out;
        if (QByteArray(argv[k]).endsWith(".html")) {   // HTML: streamed in network-sized chunks
            QElapsedTimer t; t.start();
            CssCompat::Vars v;
            CssCompat::HtmlRewriter r(&v);
            for (int p = 0; p < in.size(); p += 1400) out += r.feed(in.mid(p, 1400));
            out += r.finish();
            printf("%-40s %8d -> %8d bytes  %6.2f ms  %d decls, %d vars\n", argv[k], in.size(), out.size(),
                   t.nsecsElapsed() / 1e6, r.stats.decls, r.stats.vars);
            QFile o(QString::fromLocal8Bit(argv[k]) + ".out.html");
            if (o.open(QIODevice::WriteOnly)) o.write(out);
            continue;
        }
        QElapsedTimer t; t.start();
        CssCompat::Stats st;
        for (int r = 0; r < 20; ++r) { CssCompat::Vars v; st = CssCompat::Stats(); out = CssCompat::rewriteSheet(in, &v, &st); }
        double ms = t.nsecsElapsed() / 1e6 / 20;
        printf("%-40s %8d -> %8d bytes  %6.2f ms  %5.1f MB/s  %d decls, %d vars\n", argv[k], in.size(), out.size(),
               ms, in.size() / ms / 1000, st.decls, st.vars);
        QFile o(QString::fromLocal8Bit(argv[k]) + ".out.css");
        if (o.open(QIODevice::WriteOnly)) o.write(out);
    }
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
