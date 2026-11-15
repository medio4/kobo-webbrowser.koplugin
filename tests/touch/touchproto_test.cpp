// Desktop test for src/touchproto.h: event streams as a Snow panel (Clara BW) and a standard
// multitouch panel (Libra Colour) send them, run through the same finger bookkeeping as
// KoboTouchInput::readable(). Run with tests/touch/run.sh.
#include "../../src/touchproto.h"
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

enum { SLOT, ID, X, BTN, SYN, MAJOR };
struct Ev { int kind, value; };

// The part of KoboTouchInput that decides which finger is down.
struct Panel {
    KbTouchProto proto;
    bool down0, down1, multi;   // first finger, second finger, "a second finger was seen"
    int x0, x1;
    explicit Panel(int kernelSlot = 0, KbTouchProto::Kind kind = KbTouchProto::Auto) : proto(kernelSlot, kind), down0(false), down1(false), multi(false), x0(-1), x1(-1) {}
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
            } else if (e.kind == MAJOR) {
                if (proto.onTouchMajor(e.value)) { if (proto.slot() == 0) down0 = false; else down1 = false; }
            } else if (e.kind == X) {
                if (proto.lifted()) continue;
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
    {   // Named by the plugin: a standard panel whose ids happen to be 0 and 1 is not Snow.
        Panel p(0, KbTouchProto::TypeB);
        FEED(p, {ID, 1}, {X, 70}, {BTN, 1}, {SYN, 0});
        CHECK(p.down0 && !p.down1 && !p.multi && p.x0 == 70);
        FEED(p, {ID, -1}, {BTN, 0}, {SYN, 0});
        CHECK(!p.down0);
    }
    {   // Named Snow: stays Snow whatever comes.
        Panel p(0, KbTouchProto::Snow);
        FEED(p, {SLOT, 1}, {ID, 0}, {X, 5}, {BTN, 1}, {SYN, 0});
        CHECK(p.down0 && !p.down1 && !p.proto.typeB() && p.x0 == 5);
    }
    {   // Phoenix (Aura, Glo HD, ...): a finger lifts with TOUCH_MAJOR 0; the positions after
        // it are where the finger was and must not count as a new touch.
        Panel p(0, KbTouchProto::Phoenix);
        FEED(p, {ID, 0}, {MAJOR, 1}, {X, 100}, {SYN, 0});
        CHECK(p.down0 && !p.down1 && !p.multi && p.x0 == 100);
        FEED(p, {ID, 0}, {MAJOR, 1}, {X, 110}, {ID, 1}, {MAJOR, 1}, {X, 600}, {SYN, 0});
        CHECK(p.down0 && p.down1 && p.multi && p.x0 == 110 && p.x1 == 600);
        FEED(p, {ID, 0}, {MAJOR, 1}, {X, 110}, {ID, 1}, {MAJOR, 0}, {X, 600}, {SYN, 0});
        CHECK(p.down0 && !p.down1);
        FEED(p, {ID, 0}, {MAJOR, 0}, {X, 110}, {SYN, 0});
        CHECK(!p.down0 && !p.down1 && p.x0 == 110);
        p.multi = false;
        FEED(p, {ID, 0}, {MAJOR, 1}, {X, 300}, {SYN, 0});
        CHECK(p.down0 && !p.multi && p.x0 == 300);
    }
    {   // Phoenix on the Aura H2O: the first finger is number 1, the second 2.
        Panel p(0, KbTouchProto::Phoenix);
        FEED(p, {ID, 1}, {MAJOR, 1}, {X, 40}, {SYN, 0});
        CHECK(p.down0 && !p.down1 && !p.multi && p.x0 == 40);
        FEED(p, {ID, 1}, {MAJOR, 1}, {X, 40}, {ID, 2}, {MAJOR, 1}, {X, 90}, {SYN, 0});
        CHECK(p.down0 && p.down1 && p.x1 == 90);
        FEED(p, {ID, 1}, {MAJOR, 0}, {X, 40}, {ID, 2}, {MAJOR, 0}, {X, 90}, {SYN, 0});
        CHECK(!p.down0 && !p.down1);
        FEED(p, {ID, 1}, {MAJOR, 1}, {X, 44}, {SYN, 0});
        CHECK(p.down0 && !p.down1 && p.x0 == 44);
    }
    {   // Standard panel that puts a lone finger in slot 1 (the kernel was left on slot 1 and
        // says nothing new): still the first finger, a plain tap, not a two-finger gesture.
        Panel p(1, KbTouchProto::TypeB);
        FEED(p, {ID, 90}, {X, 250}, {BTN, 1}, {SYN, 0});
        CHECK(p.down0 && !p.down1 && !p.multi && p.x0 == 250);
        FEED(p, {X, 260}, {SYN, 0});
        CHECK(p.down0 && !p.multi && p.x0 == 260);
        FEED(p, {ID, -1}, {BTN, 0}, {SYN, 0});
        CHECK(!p.down0 && !p.down1);
        // A second finger then lands in slot 0: it is the second finger while the first is down.
        FEED(p, {ID, 91}, {X, 300}, {BTN, 1}, {SYN, 0});
        FEED(p, {SLOT, 0}, {ID, 92}, {X, 700}, {SYN, 0});
        CHECK(p.down0 && p.down1 && p.multi && p.x0 == 300 && p.x1 == 700);
        FEED(p, {ID, -1}, {SYN, 0});
        CHECK(p.down0 && !p.down1);
        FEED(p, {SLOT, 1}, {ID, -1}, {BTN, 0}, {SYN, 0});
        CHECK(!p.down0 && !p.down1);
    }
    {   // The same without being told the kind: found out from the events.
        Panel p(1);
        FEED(p, {ID, 300}, {X, 80}, {BTN, 1}, {SYN, 0});
        CHECK(p.proto.typeB() && p.down0 && !p.down1 && !p.multi && p.x0 == 80);
        FEED(p, {ID, -1}, {BTN, 0}, {SYN, 0});
        CHECK(!p.down0);
    }
    CHECK(KbTouchProto::kindFromName("phoenix") == KbTouchProto::Phoenix);
    CHECK(KbTouchProto::kindFromName("standard") == KbTouchProto::TypeB);
    CHECK(KbTouchProto::kindFromName("snow") == KbTouchProto::Snow);
    CHECK(KbTouchProto::kindFromName(0) == KbTouchProto::Auto && KbTouchProto::kindFromName("") == KbTouchProto::Auto);
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
