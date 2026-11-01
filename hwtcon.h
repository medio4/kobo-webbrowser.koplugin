// Direct e-ink refresh through the MediaTek "hwtcon" framebuffer driver used by
// Kobo Clara BW and friends (HWTCON_SEND_UPDATE ioctl, 36-byte update struct, same
// layout as KOReader's ffi/mxcfb_kobo_h.lua). Avoids spawning fbink for every
// screen update (~90 ms each on the device).
#ifndef HWTCON_H
#define HWTCON_H
#include <QtCore/QRect>

class HwtconRefresh {
public:
    enum Waveform { Auto, DU, GC16, GL16, REAGL, A2 };
    // fd: an open /dev/fb0 descriptor (not owned).
    explicit HwtconRefresh(int fd);
    bool available() const { return m_ok; }
    // Returns false (and disables itself) if the ioctl is not supported.
    bool refresh(const QRect &r, Waveform w, bool full);
private:
    int m_fd;
    bool m_ok;
    unsigned m_marker;
};
#endif
