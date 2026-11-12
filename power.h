// Power button, sleep cover, auto-sleep and frontlight for the Kobo browser.
// KOReader closes its input devices while the browser runs, so nothing else answers
// the power button: this does what KOReader's Kobo suspend code does
// (frontend/device/kobo/device.lua): sleep screen, frontlight off, Wi-Fi off,
// "1" > /sys/power/state-extended, wait 2 s, "mem" > /sys/power/state.
#ifndef POWER_H
#define POWER_H
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QElapsedTimer>
class QSocketNotifier;
class QWidget;
class QProcess;
class KoboEink;

// Frontlight through sysfs (Clara BW: mxc_msp430.0 brightness 0..100, and the
// lm3630a_led "color" mixer for warmth, 0..10 where 10 = coldest).
class KoboFrontlight : public QObject {
    Q_OBJECT
public:
    explicit KoboFrontlight(QObject *parent = 0);
    static KoboFrontlight *instance() { return s_instance; }
    bool available() const { return !m_brightPath.isEmpty(); }
    bool hasWarmth() const { return !m_mixerPath.isEmpty(); }
    int brightness() const { return m_bright; }   // 0..100
    int warmth() const { return m_warmth; }       // 0..100 (0 = cold, 100 = warm)
    void setBrightness(int v);
    void setWarmth(int v);
    void announce(bool warmth);   // emit message() with the current value
    // Light off for sleep, and back to what it was on wake.
    void suspendOff();
    void resumeOn();
signals:
    void message(const QString &text);
private:
    static KoboFrontlight *s_instance;
    QString m_brightPath, m_mixerPath;
    int m_bright, m_warmth, m_mixerMax;
    bool m_mixerInverted;
    int m_savedBright;   // -1 = not suspended
};

class KoboPower : public QObject {
    Q_OBJECT
public:
    KoboPower(KoboEink *eink, QObject *parent = 0);
    ~KoboPower();
    bool sleeping() const { return m_state != Awake; }
signals:
    void message(const QString &text);   // for the status line
    // KB_KO_SLEEP=1 (set by the KOReader plugin): instead of suspending itself, the
    // browser hands sleep to KOReader (its screensaver/wallpaper and suspend code) and
    // exits; KOReader relaunches it on wake.
    void sleepRequested();
protected:
    bool eventFilter(QObject *obj, QEvent *e);   // any tap/key resets the idle timer
public slots:
    void sleep();        // show the sleep screen and suspend
    void wake();         // power button / cover opened while asleep
private slots:
    void readable();
    void beginSuspend(); // Wi-Fi off, sync, state-extended=1, then doSuspend 2 s later
    void doSuspend();    // "mem" > /sys/power/state; returns when the device wakes
    void retrySuspend(); // woke up without a power press (or suspend failed): again
    void wifiDone();
private:
    enum State { Awake, Entering, Asleep };
    void openPowerDevice();
    bool onBattery() const;
    bool writeSysfs(const char *path, const char *value);
    void showSleepScreen();
    void hideSleepScreen();

    KoboEink *m_eink;
    int m_fd;
    QSocketNotifier *m_notifier;
    State m_state;
    QWidget *m_screen;          // the sleep screen (child of the main window)
    QTimer m_idle;              // auto-sleep
    QTimer m_step;              // the 2 s between state-extended and state
    QTimer m_guard;             // re-suspend if we woke up and nobody pressed power
    int m_unexpected;           // wakeups in a row without a power press
    bool m_suspendEnabled;      // KB_SLEEP_SUSPEND=0: sleep screen only (for testing)
    bool m_wifiEnabled;         // KB_SLEEP_WIFI=0: leave Wi-Fi alone
    bool m_coverEnabled;        // KB_SLEEP_COVER=0: ignore the sleep cover
    bool m_wifiOff;             // Wi-Fi was turned off for this sleep
    bool m_extended;            // we wrote 1 to state-extended
    QProcess *m_wifi;
    QElapsedTimer m_lastToggle; // debounce: one press = one transition
    QElapsedTimer m_pressTimer; // length of the current power-key press
    bool m_pressActive;
    bool m_koSleep;             // KB_KO_SLEEP=1: emit sleepRequested() instead
    bool m_koRequested;
    QString m_dir;              // where wifi-sleep.sh lives (the binary's folder)
};
#endif
