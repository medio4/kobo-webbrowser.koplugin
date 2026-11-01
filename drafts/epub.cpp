#include "epub.h"
#include <QtCore/QFile>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QVariant>
#include <QtCore/QRegExp>
#include <QtCore/QtEndian>
#include <QtWebKitWidgets/QWebFrame>

namespace {

quint32 crcTable[256];
bool crcInit = false;
quint32 crc32(const QByteArray &d)
{
    if (!crcInit) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            crcTable[i] = c;
        }
        crcInit = true;
    }
    quint32 c = 0xFFFFFFFFu;
    for (int i = 0; i < d.size(); ++i) c = crcTable[(c ^ (unsigned char)d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

struct Entry { QByteArray name; QByteArray data; quint32 crc; quint32 offset; };

void put16(QByteArray &b, quint16 v) { quint16 le = qToLittleEndian(v); b.append((const char *)&le, 2); }
void put32(QByteArray &b, quint32 v) { quint32 le = qToLittleEndian(v); b.append((const char *)&le, 4); }

QByteArray buildZip(QList<Entry> &entries)
{
    QByteArray out;
    const quint16 dosTime = 0, dosDate = (1 << 5) | 1; // 1980-01-01
    for (int i = 0; i < entries.size(); ++i) {
        Entry &e = entries[i];
        e.crc = crc32(e.data);
        e.offset = out.size();
        put32(out, 0x04034b50); put16(out, 20); put16(out, 0); put16(out, 0); // stored
        put16(out, dosTime); put16(out, dosDate);
        put32(out, e.crc); put32(out, e.data.size()); put32(out, e.data.size());
        put16(out, e.name.size()); put16(out, 0);
        out.append(e.name); out.append(e.data);
    }
    quint32 cdStart = out.size();
    for (int i = 0; i < entries.size(); ++i) {
        const Entry &e = entries[i];
        put32(out, 0x02014b50); put16(out, 20); put16(out, 20); put16(out, 0); put16(out, 0);
        put16(out, dosTime); put16(out, dosDate);
        put32(out, e.crc); put32(out, e.data.size()); put32(out, e.data.size());
        put16(out, e.name.size()); put16(out, 0); put16(out, 0); put16(out, 0); put16(out, 0);
        put32(out, 0); put32(out, e.offset);
        out.append(e.name);
    }
    quint32 cdSize = out.size() - cdStart;
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0);
    put16(out, entries.size()); put16(out, entries.size());
    put32(out, cdSize); put32(out, cdStart); put16(out, 0);
    return out;
}

QString xmlEscape(QString s)
{
    return s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;");
}

} // namespace

QString Epub::extractBodyXhtml(QWebFrame *frame)
{
    static const char *js =
        "(function(){"
        " var d=document.documentElement.cloneNode(true);"
        " var kill='script,style,iframe,noscript,svg,canvas,form,input,button,select,textarea,video,audio,link,meta,nav,object,embed';"
        " var rm=d.querySelectorAll(kill); for(var i=0;i<rm.length;i++){ if(rm[i].parentNode) rm[i].parentNode.removeChild(rm[i]); }"
        " var all=d.getElementsByTagName('*');"
        " for(var i=0;i<all.length;i++){ var e=all[i]; var a=e.attributes;"
        "   for(var j=a.length-1;j>=0;j--){ var n=a[j].name; if(n!=='href'&&n!=='alt'&&n!=='title') e.removeAttribute(n); } }"
        " var imgs=d.getElementsByTagName('img'); for(var i=imgs.length-1;i>=0;i--){ var p=imgs[i].parentNode; if(p) p.removeChild(imgs[i]); }"
        " var body=d.getElementsByTagName('body')[0]; if(!body) return '';"
        " var as=body.getElementsByTagName('a'); for(var i=0;i<as.length;i++){ try{ as[i].setAttribute('href', as[i].href); }catch(e){} }"
        " return new XMLSerializer().serializeToString(body);"
        "})()";
    QVariant v = frame->evaluateJavaScript(QString::fromLatin1(js));
    QString s = v.toString();
    // XMLSerializer on HTML documents may leave HTML named entities; make them XML-safe.
    s.replace("&nbsp;", "&#160;");
    s.replace(QRegExp("&(?!(amp|lt|gt|quot|apos|#\\d+|#x[0-9a-fA-F]+);)"), "&amp;");
    return s;
}

QString Epub::safeFileName(const QString &title)
{
    QString t = title.simplified();
    t.replace(QRegExp("[\\\\/:*?\"<>|]"), "-");
    if (t.size() > 80) t = t.left(80);
    if (t.isEmpty()) t = "page";
    return t;
}

bool Epub::write(const QString &path, const QString &title, const QString &sourceUrl, const QString &bodyXhtml)
{
    QString t = title.isEmpty() ? QString("Saved page") : title;
    QString xhtml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<!DOCTYPE html>\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>" + xmlEscape(t) + "</title>"
        "<style>body{font-family:serif;line-height:1.4} img{max-width:100%} pre{white-space:pre-wrap}</style>"
        "</head>\n" + bodyXhtml + "\n</html>\n";
    QString opf = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"uid\" version=\"2.0\">\n"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        "<dc:title>" + xmlEscape(t) + "</dc:title>\n"
        "<dc:language>en</dc:language>\n"
        "<dc:identifier id=\"uid\">" + xmlEscape(sourceUrl) + "</dc:identifier>\n"
        "<dc:source>" + xmlEscape(sourceUrl) + "</dc:source>\n"
        "<dc:date>" + QDateTime::currentDateTime().toString(Qt::ISODate) + "</dc:date>\n"
        "</metadata>\n"
        "<manifest>\n"
        "<item id=\"page\" href=\"page.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
        "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
        "</manifest>\n"
        "<spine toc=\"ncx\"><itemref idref=\"page\"/></spine>\n"
        "</package>\n";
    QString ncx = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n"
        "<head><meta name=\"dtb:uid\" content=\"" + xmlEscape(sourceUrl) + "\"/></head>\n"
        "<docTitle><text>" + xmlEscape(t) + "</text></docTitle>\n"
        "<navMap><navPoint id=\"p1\" playOrder=\"1\"><navLabel><text>" + xmlEscape(t) +
        "</text></navLabel><content src=\"page.xhtml\"/></navPoint></navMap>\n</ncx>\n";
    QString container = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
        "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n"
        "</container>\n";

    QList<Entry> entries;
    Entry e;
    e.name = "mimetype"; e.data = "application/epub+zip"; entries << e;
    e.name = "META-INF/container.xml"; e.data = container.toUtf8(); entries << e;
    e.name = "OEBPS/content.opf"; e.data = opf.toUtf8(); entries << e;
    e.name = "OEBPS/toc.ncx"; e.data = ncx.toUtf8(); entries << e;
    e.name = "OEBPS/page.xhtml"; e.data = xhtml.toUtf8(); entries << e;
    QByteArray zip = buildZip(entries);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "epub: cannot write" << path << f.errorString();
        return false;
    }
    f.write(zip);
    f.close();
    return true;
}
