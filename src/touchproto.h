// Which finger a multitouch event belongs to. Kobo panels come in two kinds:
//
// "Snow" (Clara BW and others, see KOReader's handleTouchEvSnow): there is never an
// ABS_MT_SLOT event, ABS_MT_TRACKING_ID IS the finger number (0 = first, 1 = second), it is
// never set to -1, and a lift comes as BTN_TOUCH 0.
//
// Standard Linux multitouch, "type B" (Libra Colour's Elan panel and others): ABS_MT_SLOT
// names the finger, ABS_MT_TRACKING_ID is a counter that goes up with every new contact
// (2817, 2818, ...) and is set to -1 when that finger lifts.
//
// We start out reading the panel as Snow and switch to type B, for good, at the first
// thing a Snow panel never sends: an ABS_MT_SLOT event, a tracking id of -1, or an id
// above 1. Plain C++ without Qt, so tests/touch can run it on the desktop.
#ifndef KB_TOUCHPROTO_H
#define KB_TOUCHPROTO_H

class KbTouchProto {
public:
    // kernelSlot: the panel's current slot when we opened it (ABS_MT_SLOT's value from
    // EVIOCGABS; 0 if it has no such axis). The kernel only reports a slot when it changes.
    explicit KbTouchProto(int kernelSlot = 0)
        : m_typeB(false), m_kernelSlot(kernelSlot), m_slot(0), m_switched(false) {}

    bool typeB() const { return m_typeB; }
    // The finger the following position events belong to (0 = first, anything else = second).
    int slot() const { return m_slot; }
    // True once, right after the event that made us switch to type B: the caller should
    // forget a contact it filed under the wrong finger.
    bool takeSwitched() { bool s = m_switched; m_switched = false; return s; }

    void onSlot(int value)
    {
        m_kernelSlot = value;
        if (!m_typeB) becomeTypeB();
        m_slot = value;
    }

    // Returns true when the finger in slot() went down, false when it lifted.
    bool onTrackingId(int id)
    {
        if (!m_typeB && (id < 0 || id > 1)) becomeTypeB();
        if (m_typeB) return id >= 0;
        m_slot = id;
        return true;
    }

    // BTN_TOUCH 0: everything is up. A Snow panel's next contact starts at finger 0; a
    // type B panel keeps its slot until it says otherwise.
    void onAllUp() { if (!m_typeB) m_slot = 0; }

    // Events went unread for a while (KOReader's keyboard had the touchscreen): take the
    // panel's current slot again.
    void resync(int kernelSlot) { m_kernelSlot = kernelSlot; if (m_typeB) m_slot = kernelSlot; }

private:
    void becomeTypeB() { m_typeB = true; m_slot = m_kernelSlot; m_switched = true; }
    bool m_typeB;
    int m_kernelSlot;
    int m_slot;
    bool m_switched;
};

#endif
