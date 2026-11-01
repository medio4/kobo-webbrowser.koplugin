#include "hwtcon.h"
#include <QtCore/QDebug>
#include <sys/ioctl.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

// From KOReader ffi/mxcfb_kobo_h.lua (Kobo MTK/hwtcon).
struct hwtcon_rect { uint32_t top, left, width, height; };
struct hwtcon_update_data {
    struct hwtcon_rect update_region;
    uint32_t waveform_mode;
    uint32_t update_mode;
    uint32_t update_marker;
    uint32_t flags;
    int32_t  dither_mode;
};
static const unsigned long HWTCON_SEND_UPDATE = 1076119086UL;   // _IOW('F', 0x2E, 36 bytes)
enum {
    HWTCON_WAVEFORM_MODE_DU = 1, HWTCON_WAVEFORM_MODE_GC16 = 2, HWTCON_WAVEFORM_MODE_GL16 = 3,
    HWTCON_WAVEFORM_MODE_REAGL = 4, HWTCON_WAVEFORM_MODE_A2 = 6, HWTCON_WAVEFORM_MODE_AUTO = 257,
};
enum { HWTCON_UPDATE_MODE_PARTIAL = 0, HWTCON_UPDATE_MODE_FULL = 1 };

HwtconRefresh::HwtconRefresh(int fd) : m_fd(fd), m_ok(fd >= 0), m_marker(0)
{
    if (getenv("KB_NO_HWTCON")) m_ok = false;
}

bool HwtconRefresh::refresh(const QRect &r, Waveform w, bool full)
{
    if (!m_ok || r.isEmpty()) return false;
    struct hwtcon_update_data u;
    memset(&u, 0, sizeof u);
    u.update_region.top = r.y(); u.update_region.left = r.x();
    u.update_region.width = r.width(); u.update_region.height = r.height();
    switch (w) {
    case DU:    u.waveform_mode = HWTCON_WAVEFORM_MODE_DU; break;
    case GC16:  u.waveform_mode = HWTCON_WAVEFORM_MODE_GC16; break;
    case GL16:  u.waveform_mode = HWTCON_WAVEFORM_MODE_GL16; break;
    case REAGL: u.waveform_mode = HWTCON_WAVEFORM_MODE_REAGL; break;
    case A2:    u.waveform_mode = HWTCON_WAVEFORM_MODE_A2; break;
    default:    u.waveform_mode = HWTCON_WAVEFORM_MODE_AUTO; break;
    }
    u.update_mode = full ? HWTCON_UPDATE_MODE_FULL : HWTCON_UPDATE_MODE_PARTIAL;
    u.update_marker = ++m_marker;
    if (ioctl(m_fd, HWTCON_SEND_UPDATE, &u) < 0) {
        qWarning() << "hwtcon: HWTCON_SEND_UPDATE failed:" << strerror(errno) << "- falling back to fbink";
        m_ok = false;
        return false;
    }
    return true;
}
