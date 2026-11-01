// Writes grayscale pixels straight into the Kobo framebuffer (/dev/fb0, 8bpp or 32bpp).
#ifndef FBWRITER_H
#define FBWRITER_H
#include <QtGui/QImage>
#include <QtCore/QRect>
class FbWriter {
public:
    FbWriter();
    ~FbWriter();
    bool isOpen() const { return m_fd >= 0 && m_mem; }
    int width() const { return m_w; }
    int height() const { return m_h; }
    // Copy img (any format) into the framebuffer at dst.topLeft(); sizes clipped to the screen.
    void write(const QImage &img, const QPoint &dst);
private:
    int m_fd; unsigned char *m_mem; unsigned long m_len;
    int m_w, m_h, m_bpp, m_stride;
};
#endif
