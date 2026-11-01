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

void FbWriter::write(const QImage &srcIn, const QPoint &dst)
{
    if (!isOpen() || srcIn.isNull()) return;
    // 8-bit images without a palette are treated as raw grayscale.
    if (srcIn.depth() == 8 && srcIn.colorCount() == 0 && m_bpp == 8) {
        int x0 = dst.x(), y0 = dst.y();
        int w = qMin(srcIn.width(), m_w - x0), h = qMin(srcIn.height(), m_h - y0);
        if (w <= 0 || h <= 0 || x0 < 0 || y0 < 0) return;
        for (int y = 0; y < h; ++y)
            memcpy(m_mem + (unsigned long)(y0 + y) * m_stride + x0, srcIn.constScanLine(y), w);
        return;
    }
    QImage src = srcIn.format() == QImage::Format_RGB32 || srcIn.format() == QImage::Format_ARGB32
                 ? srcIn : srcIn.convertToFormat(QImage::Format_RGB32);
    int x0 = dst.x(), y0 = dst.y();
    int w = qMin(src.width(), m_w - x0), h = qMin(src.height(), m_h - y0);
    if (w <= 0 || h <= 0 || x0 < 0 || y0 < 0) return;
    for (int y = 0; y < h; ++y) {
        const QRgb *line = (const QRgb *)src.constScanLine(y);
        unsigned char *out = m_mem + (unsigned long)(y0 + y) * m_stride;
        if (m_bpp == 8) {
            out += x0;
            for (int x = 0; x < w; ++x) out[x] = (unsigned char)qGray(line[x]);
        } else if (m_bpp == 32) {
            memcpy(out + x0 * 4, line, w * 4);
        } else if (m_bpp == 16) {
            unsigned short *o16 = (unsigned short *)out + x0;
            for (int x = 0; x < w; ++x) {
                QRgb p = line[x];
                o16[x] = (unsigned short)(((qRed(p) >> 3) << 11) | ((qGreen(p) >> 2) << 5) | (qBlue(p) >> 3));
            }
        }
    }
}
