#include "fbwriter.h"
#include <QtCore/QDebug>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

FbWriter::FbWriter() : m_fd(-1), m_mem(0), m_len(0), m_w(0), m_h(0), m_bpp(0), m_stride(0)
{
    m_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (m_fd < 0) { qWarning() << "fb: open failed" << strerror(errno); return; }
    struct fb_var_screeninfo var; struct fb_fix_screeninfo fix;
    if (ioctl(m_fd, FBIOGET_VSCREENINFO, &var) || ioctl(m_fd, FBIOGET_FSCREENINFO, &fix)) {
        qWarning() << "fb: ioctl failed" << strerror(errno); close(m_fd); m_fd = -1; return;
    }
    m_w = var.xres; m_h = var.yres; m_bpp = var.bits_per_pixel; m_stride = fix.line_length;
    m_len = fix.smem_len;
    m_mem = (unsigned char *)mmap(0, m_len, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
    if (m_mem == MAP_FAILED) { qWarning() << "fb: mmap failed" << strerror(errno); m_mem = 0; }
    qDebug() << "fb:" << m_w << "x" << m_h << "bpp" << m_bpp << "stride" << m_stride << "rota" << var.rotate;
}

FbWriter::~FbWriter()
{
    if (m_mem) munmap(m_mem, m_len);
    if (m_fd >= 0) close(m_fd);
}

// Compare-then-copy one converted scanline; returns true if it differed.
static bool s_compareOnly = false;   // audit: report differences without copying
static int s_firstByte, s_lastByte;    // changed byte span across the lines of one write()
static inline bool putLine(unsigned char *dst, const unsigned char *src, int bytes)
{
    if (memcmp(dst, src, bytes) == 0) return false;
    int a = 0; while (a < bytes && dst[a] == src[a]) ++a;
    int b = bytes - 1; while (b > a && dst[b] == src[b]) --b;
    if (s_firstByte < 0 || a < s_firstByte) s_firstByte = a;
    if (b > s_lastByte) s_lastByte = b;
    if (!s_compareOnly) memcpy(dst, src, bytes);
    return true;
}

bool FbWriter::write(const QImage &srcIn, const QPoint &dst, int *firstY, int *lastY, bool compareOnly,
                     int *firstX, int *lastX)
{
    s_firstByte = -1; s_lastByte = -1;
    struct CmpGuard { CmpGuard(bool c) { s_compareOnly = c; } ~CmpGuard() { s_compareOnly = false; } } guard(compareOnly);
    int fy = -1, ly = -1;
    if (!isOpen() || srcIn.isNull()) return false;
    bool changed = false;
    int x0 = dst.x(), y0 = dst.y();
    int w = qMin(srcIn.width(), m_w - x0), h = qMin(srcIn.height(), m_h - y0);
    if (w <= 0 || h <= 0 || x0 < 0 || y0 < 0) return false;

    // 8-bit images without a palette are treated as raw grayscale.
    if (srcIn.depth() == 8 && srcIn.colorCount() == 0 && m_bpp == 8) {
        for (int y = 0; y < h; ++y)
            if (putLine(m_mem + (unsigned long)(y0 + y) * m_stride + x0, srcIn.constScanLine(y), w)) { changed = true; if (fy < 0) fy = y0 + y; ly = y0 + y; }
        if (firstY) *firstY = fy;
        if (lastY) *lastY = ly;
        if (firstX) *firstX = s_firstByte < 0 ? -1 : x0 + s_firstByte;
        if (lastX) *lastX = s_lastByte < 0 ? -1 : x0 + s_lastByte;
        return changed;
    }
    QImage src = srcIn.format() == QImage::Format_RGB32 || srcIn.format() == QImage::Format_ARGB32
                 ? srcIn : srcIn.convertToFormat(QImage::Format_RGB32);
    const int outBytes = (m_bpp / 8) * w;
    if (m_line.size() < outBytes) m_line.resize(outBytes);
    unsigned char *tmp = (unsigned char *)m_line.data();
    for (int y = 0; y < h; ++y) {
        const QRgb *line = (const QRgb *)src.constScanLine(y);
        unsigned char *out = m_mem + (unsigned long)(y0 + y) * m_stride;
        if (m_bpp == 8) {
            for (int x = 0; x < w; ++x) tmp[x] = (unsigned char)qGray(line[x]);
            if (putLine(out + x0, tmp, w)) { changed = true; if (fy < 0) fy = y0 + y; ly = y0 + y; }
        } else if (m_bpp == 32) {
            if (putLine(out + x0 * 4, (const unsigned char *)line, w * 4)) { changed = true; if (fy < 0) fy = y0 + y; ly = y0 + y; }
        } else if (m_bpp == 16) {
            unsigned short *o16 = (unsigned short *)tmp;
            for (int x = 0; x < w; ++x) {
                QRgb p = line[x];
                o16[x] = (unsigned short)(((qRed(p) >> 3) << 11) | ((qGreen(p) >> 2) << 5) | (qBlue(p) >> 3));
            }
            if (putLine(out + x0 * 2, tmp, w * 2)) { changed = true; if (fy < 0) fy = y0 + y; ly = y0 + y; }
        }
    }
    if (firstY) *firstY = fy;
    if (lastY) *lastY = ly;
    const int bpp = (m_bpp / 8 > 0 ? m_bpp / 8 : 1);
    if (firstX) *firstX = s_firstByte < 0 ? -1 : x0 + s_firstByte / bpp;
    if (lastX) *lastX = s_lastByte < 0 ? -1 : x0 + s_lastByte / bpp;
    return changed;
}
