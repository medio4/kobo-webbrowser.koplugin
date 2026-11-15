// Desktop test for src/touchproto.h: event streams as a Snow panel (Clara BW) and a standard
// multitouch panel (Libra Colour) send them, run through the same finger bookkeeping as
// KoboTouchInput::readable(). Run with tests/touch/run.sh.
#include "../../src/touchproto.h"
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

enum { SLOT, ID, X, BTN, SYN };
struct Ev { int kind, value; };

// The part of KoboTouchInput that decides which finger is down.
struct Panel {
    KbTouchProto proto;
    bool down0, down1, multi;   // first finger, second finger, "a second finger was seen"
    int x0, x1;
    explicit Panel(int kernelSlot = 0) : proto(kernelSlot), down0(false), down1(false), multi(false), x0(-1), x1(-1) {}
    void switched() { if (proto.takeSwitched()) { down1 = false; multi = false; } }
    void feed(const Ev *ev, int n)
    {
        for (int i = 0; i < n; ++i) {
            const Ev &e = ev[i];
            if (e.kind == SLOT) { proto.onSlot(e.value); switched(); }
            else if (e.kind == ID) {
                bool down = proto.onTrackingId(e.value);
                switched();
                if (proto.slot() == 0) down0 = down;
                else { down1 = down; if (down) multi = true; }
            } else if (e.kind == X) {
                if (proto.slot() == 0) { x0 = e.value; down0 = true; }
                else { x1 = e.value; down1 = true; multi = true; }
            } else if (e.kind == BTN) {
                down0 = e.value != 0;
                if (!e.value) { down1 = false; proto.onAllUp(); }
            }
        }
    }
};
#define FEED(p, ...) do { const Ev evs_[] = { __VA_ARGS__ }; (p).feed(evs_, sizeof evs_ / sizeof evs_[0]); } while (0)

int main()
{
    {   // Clara BW: one finger. The id is the finger number, the lift is BTN_TOUCH 0.
        Panel p;
        FEED(p, {ID, 0}, {X, 100}, {BTN, 1}, {SYN, 0});
        CHECK(p.down0 && !p.down1 && !p.multi && p.x0 == 100);
        FEED(p, {ID, 0}, {X, 120}, {SYN, 0});
        CHECK(p.down0 && !p.multi && p.x0 == 120);
        FEED(p, {BTN, 0}, {SYN, 0});
        CHECK(!p.down0 && !p.down1 && !p.proto.typeB());
    }
    {   // Clara BW: two fingers (pinch), then one finger again.
        Panel p;
        FEED(p, {ID, 0}, {X, 100}, {BTN, 1}, {SYN, 0});
        FEED(p, {ID, 0}, {X, 100}, {ID, 1}, {X, 500}, {SYN, 0});
        CHECK(p.down0 && p.down1 && p.multi && p.x0 == 100 && p.x1 == 500);
        FEED(p, {BTN, 0}, {SYN, 0});
        CHECK(!p.down0 && !p.down1 && !p.proto.typeB());
        p.multi = false;
        FEED(p, {ID, 0}, {X, 300}, {BTN, 1}, {SYN, 0});
        CHECK(p.down0 && !p.down1 && !p.multi && p.x0 == 300);
    }
    {   // Libra Colour: one finger. Ids count up; the tap must be the FIRST finger.
        Panel p;
        FEED(p, {ID, 2817}, {X, 640}, {BTN, 1}, {SYN, 0});
        CHECK(p.proto.typeB());
        CHECK(p.down0 && !p.down1 && !p.multi && p.x0 == 640);
        FEED(p, {X, 650}, {SYN, 0});
        CHECK(p.down0 && !p.multi && p.x0 == 650);
        FEED(p, {ID, -1}, {BTN, 0}, {SYN, 0});
        CHECK(!p.down0 && !p.down1 && !p.multi);
        FEED(p, {ID, 2818}, {X, 200}, {BTN, 1}, {SYN, 0});
        CHECK(p.down0 && !p.down1 && !p.multi && p.x0 == 200);
        FEED(p, {ID, -1}, {BTN, 0}, {SYN, 0});
        CHECK(!p.down0);
    }
    {   // Libra Colour: two fingers, the first lifts first, then a single tap in slot 0.
        Panel p;
        FEED(p, {ID, 40}, {X, 100}, {BTN, 1}, {SYN, 0});
        FEED(p, {SLOT, 1}, {ID, 41}, {X, 900}, {SYN, 0});
        CHECK(p.down0 && p.down1 && p.multi && p.x0 == 100 && p.x1 == 900);
        FEED(p, {SLOT, 0}, {ID, -1}, {SYN, 0});
        CHECK(!p.down0 && p.down1);
        FEED(p, {SLOT, 1}, {ID, -1}, {BTN, 0}, {SYN, 0});
        CHECK(!p.down0 && !p.down1);
        p.multi = false;
        FEED(p, {SLOT, 0}, {ID, 42}, {X, 333}, {BTN, 1}, {SYN, 0});
        CHECK(p.down0 && !p.down1 && !p.multi && p.x0 == 333);
    }
    {   // Libra Colour right after boot: ids 0 and 1 look like a Snow panel at first.
        Panel p;
        FEED(p, {ID, 0}, {X, 10}, {BTN, 1}, {SYN, 0});
        CHECK(p.down0 && !p.multi);
        FEED(p, {ID, -1}, {BTN, 0}, {SYN, 0});
        CHECK(!p.down0 && p.proto.typeB());
        FEED(p, {ID, 1}, {X, 20}, {BTN, 1}, {SYN, 0});   // second tap: still the first finger
        CHECK(p.down0 && !p.down1 && !p.multi && p.x0 == 20);
    }
    {   // Libra Colour, panel left on slot 1 before we opened it: the first tap names slot 0.
        Panel p(1);
        FEED(p, {SLOT, 0}, {ID, 77}, {X, 50}, {BTN, 1}, {SYN, 0});
        CHECK(p.down0 && !p.down1 && !p.multi && p.x0 == 50);
    }
    {   // After KOReader's keyboard had the touchscreen: take the panel's slot again.
        Panel p;
        FEED(p, {ID, 500}, {X, 1}, {BTN, 1}, {SYN, 0}, {ID, -1}, {BTN, 0}, {SYN, 0});
        p.proto.resync(1);
        CHECK(p.proto.slot() == 1);
        FEED(p, {SLOT, 0}, {ID, 501}, {X, 60}, {BTN, 1}, {SYN, 0});
        CHECK(p.down0 && !p.down1 && p.x0 == 60);
        KbTouchProto snow;
        snow.onTrackingId(0);
        snow.resync(1);          // a Snow panel has no slots: nothing changes
        CHECK(snow.slot() == 0 && !snow.typeB());
    }
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
