// Which finger a multitouch event belongs to. Kobo panels come in three kinds:
//
// "Snow" (Clara BW, Clara HD, Libra H2O and others, see KOReader's handleTouchEvSnow): there
// is never an ABS_MT_SLOT event, ABS_MT_TRACKING_ID IS the finger number (0 = first,
// 1 = second), it is never set to -1, and a lift comes as BTN_TOUCH 0.
//
// "Phoenix" (Aura, Glo HD, Aura One, Nia and others, handleTouchEvPhoenix): the tracking id
// is the finger number too, but a finger lifts with ABS_MT_TOUCH_MAJOR 0 after its id, and
// the positions that follow in that report are just where it was. The Aura H2O numbers its
// fingers from 1.
//
// Standard Linux multitouch, "type B" (Libra Colour, Libra 2, Clara 2E, Elipsa 2E and
// others): ABS_MT_SLOT names the finger, ABS_MT_TRACKING_ID is a counter that goes up with
// every new contact (2817, 2818, ...) and is set to -1 when that finger lifts.
//
// The KOReader plugin says which one the model has (KB_TOUCH_PROTO). Without that we start
// out reading the panel as Snow and switch to type B, for good, at the first thing a Snow
// panel never sends: an ABS_MT_SLOT event, a tracking id of -1, or an id above 1.
// Plain C++ without Qt, so tests/touch can run it on the desktop.
#ifndef KB_TOUCHPROTO_H
#define KB_TOUCHPROTO_H
#include <string.h>

class KbTouchProto {
public:
    enum Kind { Auto, Snow, Phoenix, TypeB };

    // kernelSlot: the panel's current slot when we opened it (ABS_MT_SLOT's value from
    // EVIOCGABS; 0 if it has no such axis). The kernel only reports a slot when it changes.
    explicit KbTouchProto(int kernelSlot = 0, Kind kind = Auto)
        : m_kind(kind), m_typeB(kind == TypeB), m_kernelSlot(kernelSlot), m_slot(kind == TypeB ? kernelSlot : 0),
          m_switched(false), m_firstId(-1), m_down(0), m_lifted(false) {}

    // "snow", "phoenix" or "standard" (KB_TOUCH_PROTO); anything else: find out from the events.
    static Kind kindFromName(const char *name)
    {
        if (!name) return Auto;
        if (!strcmp(name, "snow")) return Snow;
        if (!strcmp(name, "phoenix")) return Phoenix;
        if (!strcmp(name, "standard")) return TypeB;
        return Auto;
    }
    const char *kindName() const
    {
        return m_kind == Snow ? "snow" : m_kind == Phoenix ? "phoenix" : m_kind == TypeB ? "standard" : "auto";
    }

    bool typeB() const { return m_typeB; }
    // The finger the following position events belong to (0 = first, anything else = second).
    int slot() const { return m_slot; }
    // True once, right after the event that made us switch to type B: the caller should
    // forget a contact it filed under the wrong finger.
    bool takeSwitched() { bool s = m_switched; m_switched = false; return s; }
    // Phoenix: the finger in slot() lifted in this report, so its positions are stale.
    bool lifted() const { return m_lifted; }

    void onSlot(int value)
    {
        m_kernelSlot = value;
        if (m_kind == Snow || m_kind == Phoenix) return;   // these panels have no slots
        if (!m_typeB) becomeTypeB();
        m_slot = value;
    }

    // Returns true when the finger in slot() went down, false when it lifted.
    bool onTrackingId(int id)
    {
        if (m_kind == Phoenix) {
            if (!m_down || m_firstId < 0) m_firstId = id;   // whatever number the first finger has
            m_slot = id == m_firstId ? 0 : 1;
            m_down |= 1 << m_slot;
            m_lifted = false;
            return true;
        }
        if (m_kind == Auto && !m_typeB && (id < 0 || id > 1)) becomeTypeB();
        if (m_typeB) return id >= 0;
        m_slot = id < 0 ? 0 : id;
        return true;
    }

    // ABS_MT_TOUCH_MAJOR. Returns true when this means the finger in slot() lifted (Phoenix).
    bool onTouchMajor(int value)
    {
        if (m_kind != Phoenix || value != 0) return false;
        m_down &= ~(1 << m_slot);
        m_lifted = true;
        return true;
    }

    // BTN_TOUCH 0: everything is up. A Snow or Phoenix panel's next contact starts at
    // finger 0; a type B panel keeps its slot until it says otherwise.
    void onAllUp()
    {
        if (!m_typeB) m_slot = 0;
        m_down = 0;
        m_lifted = false;
    }

    // Events went unread for a while (KOReader's keyboard had the touchscreen): take the
    // panel's current slot again.
    void resync(int kernelSlot)
    {
        m_kernelSlot = kernelSlot;
        if (m_typeB) m_slot = kernelSlot;
        m_down = 0;
        m_lifted = false;
    }

private:
    void becomeTypeB() { m_typeB = true; m_slot = m_kernelSlot; m_switched = true; }
    Kind m_kind;
    bool m_typeB;
    int m_kernelSlot;
    int m_slot;
    bool m_switched;
    int m_firstId;   // Phoenix: the id of the finger that went down first
    int m_down;      // Phoenix: bit 0 / bit 1 = first / second finger down
    bool m_lifted;
};

#endif
