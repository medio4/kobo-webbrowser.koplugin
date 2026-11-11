// Copies QImage pixels into /dev/fb0 (8bpp Y8, 16bpp RGB565 or 32bpp).
#ifndef FBWRITER_H
#define FBWRITER_H
#include <QtGui/QImage>
#include <QtCore/QPoint>
class FbWriter {
public:
    FbWriter();
    ~FbWriter();
    bool isOpen() const { return m_fd >= 0 && m_mem; }
    int fd() const { return m_fd; }
    int width() const { return m_w; }
    int height() const { return m_h; }
    // Writes src at dst. Returns true if any pixel actually changed (so callers can
    // skip the e-ink refresh for repaints that drew the same picture again, e.g. a
    // button's pressed/released repaint or a non-blinking cursor).
    // Copies src to the framebuffer at dst; returns whether any pixel changed. With
    // firstY/lastY, also reports the first and last changed row (screen coordinates).
    bool write(const QImage &src, const QPoint &dst, int *firstY = 0, int *lastY = 0, bool compareOnly = false);
private:
    int m_fd;
    unsigned char *m_mem;
    size_t m_len;
    int m_w, m_h, m_bpp, m_stride;
    QByteArray m_line;
};
#endif
