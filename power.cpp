#include "power.h"
#include "eink.h"
#include <QtCore/QSocketNotifier>
#include <QtCore/QProcess>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QDebug>
#include <QtCore/QCoreApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>
#include <QtGui/QPainter>
#include <QtGui/QMouseEvent>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

static int envInt(const char *name, int def)
{
    const char *v = getenv(name);
    return (v && *v) ? atoi(v) : def;
}

static QString readLine(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromLatin1(f.readLine()).trimmed();
}

static bool writeFile(const QString &path, const QByteArray &value)
{
    int fd = open(path.toLocal8Bit().constData(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = write(fd, value.constData(), value.size());
    close(fd);
    return n == value.size();
}

// ================================================================ frontlight

KoboFrontlight *KoboFrontlight::s_instance = 0;

KoboFrontlight::KoboFrontlight(QObject *parent)
    : QObject(parent), m_bright(0), m_warmth(0), m_mixerMax(10), m_mixerInverted(true), m_savedBright(-1)
{
    s_instance = this;
    const char *b = getenv("KB_FL_BRIGHTNESS");
    QString bp = (b && *b) ? QString::fromLocal8Bit(b) : QString("/sys/class/backlight/mxc_msp430.0/brightness");
    if (QFile::exists(bp)) {
        m_brightPath = bp;
        m_bright = qBound(0, readLine(bp).toInt(), 100);
    }
    const char *m = getenv("KB_FL_MIXER");
    QString mp = (m && *m) ? QString::fromLocal8Bit(m) : QString("/sys/class/backlight/lm3630a_led/color");
    if (QFile::exists(mp)) {
        m_mixerPath = mp;
        m_mixerMax = envInt("KB_FL_MIXER_MAX", 10);
        m_mixerInverted = envInt("KB_FL_MIXER_INVERTED", 1) != 0;
        int raw = qBound(0, readLine(mp).toInt(), m_mixerMax);
        int w = m_mixerInverted ? m_mixerMax - raw : raw;
        m_warmth = m_mixerMax > 0 ? w * 100 / m_mixerMax : 0;
    }
    qDebug() << "frontlight:" << (m_brightPath.isEmpty() ? QString("none") : m_brightPath) << m_bright
             << "warmth:" << (m_mixerPath.isEmpty() ? QString("none") : m_mixerPath) << m_warmth;
}

void KoboFrontlight::setBrightness(int v)
{
    v = qBound(0, v, 100);
    if (m_brightPath.isEmpty() || v == m_bright) return;
    if (writeFile(m_brightPath, QByteArray::number(v))) m_bright = v;
}

void KoboFrontlight::setWarmth(int v)
{
    v = qBound(0, v, 100);
    if (m_mixerPath.isEmpty()) return;
    int step = (v * m_mixerMax + 50) / 100;
    int oldStep = (m_warmth * m_mixerMax + 50) / 100;
    m_warmth = v;
    if (step == oldStep) return;   // the mixer only has m_mixerMax + 1 levels
    int raw = m_mixerInverted ? m_mixerMax - step : step;
    writeFile(m_mixerPath, QByteArray::number(raw));
}

void KoboFrontlight::announce(bool warmth)
{
    if (warmth) emit message(QString("Warmth %1/%2").arg((m_warmth * m_mixerMax + 50) / 100).arg(m_mixerMax));
    else emit message(m_bright == 0 ? QString("Frontlight off") : QString("Frontlight %1%").arg(m_bright));
}

void KoboFrontlight::suspendOff()
{
    if (m_brightPath.isEmpty() || m_savedBright >= 0) return;
    m_savedBright = m_bright;
    if (m_bright > 0) writeFile(m_brightPath, "0");
}

void KoboFrontlight::resumeOn()
{
    if (m_brightPath.isEmpty() || m_savedBright < 0) return;
    if (m_savedBright > 0) writeFile(m_brightPath, QByteArray::number(m_savedBright));
    m_savedBright = -1;
}

// ============================================================== sleep screen

class SleepScreen : public QWidget {
public:
    explicit SleepScreen(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setGeometry(parent->rect());
    }
protected:
    void paintEvent(QPaintEvent *)
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::white);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        // A framed box in the middle, KOReader style.
        QRect box(width() / 8, height() * 2 / 5, width() * 3 / 4, height() / 5);
        p.setPen(QPen(Qt::black, 4));
        p.setBrush(Qt::white);
        p.drawRoundedRect(box, 16, 16);
        QFont big; big.setPixelSize(96); big.setBold(true);
        QFont small; small.setPixelSize(40);
        p.setPen(Qt::black);
        p.setFont(big);
        p.drawText(box.adjusted(0, 0, 0, -box.height() / 3), Qt::AlignCenter, "Sleeping");
        p.setFont(small);
        p.drawText(box.adjusted(0, box.height() * 2 / 4, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
                   "Press the power button to wake");
    }
    // Swallow every tap while the sleep screen is up.
    void mousePressEvent(QMouseEvent *e) { e->accept(); }
    void mouseReleaseEvent(QMouseEvent *e) { e->accept(); }
    void mouseMoveEvent(QMouseEvent *e) { e->accept(); }
};

// ===================================================================== power

#ifndef KEY_POWER
#define KEY_POWER 116
#endif
static const int kSleepCover = 59;   // KEY_F1 on Kobo = hall sensor (KOReader event_map)

KoboPower::KoboPower(KoboEink *eink, QObject *parent)
    : QObject(parent), m_eink(eink), m_fd(-1), m_notifier(0), m_state(Awake), m_screen(0),
      m_unexpected(0), m_wifiOff(false), m_extended(false), m_wifi(0)
{
    m_dir = QCoreApplication::applicationDirPath();
    m_suspendEnabled = envInt("KB_SLEEP_SUSPEND", 1) != 0;
    m_wifiEnabled = envInt("KB_SLEEP_WIFI", 1) != 0;
    m_coverEnabled = envInt("KB_SLEEP_COVER", 1) != 0;

    openPowerDevice();

    // Auto-sleep after KB_AUTOSLEEP_MIN minutes without a tap (0 = never).
    int idleMin = envInt("KB_AUTOSLEEP_MIN", 30);
    m_idle.setSingleShot(true);
    if (idleMin > 0) {
        m_idle.setInterval(idleMin * 60 * 1000);
        connect(&m_idle, SIGNAL(timeout()), this, SLOT(sleep()));
        m_idle.start();
        qApp->installEventFilter(this);
    }
    m_step.setSingleShot(true);
    connect(&m_step, SIGNAL(timeout()), this, SLOT(doSuspend()));
    m_guard.setSingleShot(true);
    connect(&m_guard, SIGNAL(timeout()), this, SLOT(retrySuspend()));
    m_lastToggle.start();
    qDebug() << "power: device fd" << m_fd << "autosleep" << idleMin << "min suspend" << m_suspendEnabled
             << "wifi" << m_wifiEnabled << "cover" << m_coverEnabled;
}

KoboPower::~KoboPower()
{
    if (m_extended) writeSysfs("/sys/power/state-extended", "0");
    if (KoboFrontlight::instance()) KoboFrontlight::instance()->resumeOn();
    if (m_fd >= 0) close(m_fd);
}

// The power button lives on its own input device (event0 on the Clara BW). Find the
// first event device that can send KEY_POWER, unless KB_POWER_DEV names one
// ("none" turns power handling off).
void KoboPower::openPowerDevice()
{
    const char *env = getenv("KB_POWER_DEV");
    QStringList candidates;
    if (env && *env) {
        if (!strcmp(env, "none")) return;
        candidates << QString::fromLocal8Bit(env);
    } else {
        QString touch = QString::fromLocal8Bit(getenv("KB_TOUCH_DEV"));
        for (int i = 0; i < 10; ++i) {
            QString p = QString("/dev/input/event%1").arg(i);
            if (p != touch && QFile::exists(p)) candidates << p;
        }
    }
    foreach (const QString &p, candidates) {
        int fd = open(p.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned char keys[KEY_MAX / 8 + 1];
        memset(keys, 0, sizeof keys);
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) >= 0 && (keys[KEY_POWER / 8] & (1 << (KEY_POWER % 8)))) {
            char name[64] = "";
            ioctl(fd, EVIOCGNAME(sizeof name), name);
            qDebug() << "power: button on" << p << name;
            m_fd = fd;
            m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
            connect(m_notifier, SIGNAL(activated(int)), this, SLOT(readable()));
            return;
        }
        close(fd);
    }
    qWarning() << "power: no input device with a power button found";
}

void KoboPower::readable()
{
    struct input_event ev[16];
    for (;;) {
        ssize_t n = read(m_fd, ev, sizeof ev);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EINTR) {
                qWarning() << "power: read error" << strerror(errno);
                m_notifier->setEnabled(false);
            }
            return;
        }
        for (int i = 0; i < int(n / sizeof(struct input_event)); ++i) {
            const struct input_event &e = ev[i];
            if (e.type != EV_KEY) continue;
            qDebug() << "power: key" << e.code << e.value << "state" << m_state;
            if (e.code == KEY_POWER && e.value == 1) {
                // Act on the press, never the release: the release of the press that
                // woke the device must not put it straight back to sleep.
                if (m_lastToggle.elapsed() < 800) continue;
                if (m_state == Awake) sleep(); else wake();
            } else if (e.code == kSleepCover && m_coverEnabled) {
                if (e.value == 1 && m_state == Awake) sleep();
                else if (e.value == 0 && m_state != Awake) wake();
            }
        }
    }
}

bool KoboPower::eventFilter(QObject *obj, QEvent *e)
{
    if (m_state == Awake && (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::KeyPress))
        m_idle.start();
    return QObject::eventFilter(obj, e);
}

bool KoboPower::onBattery() const
{
    // MTK kernels hang if they suspend while plugged in (KOReader skips it too).
    QDir ps("/sys/class/power_supply");
    foreach (const QString &name, ps.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (readLine(ps.filePath(name + "/capacity")).isEmpty()) continue;
        return readLine(ps.filePath(name + "/status")) == "Discharging";
    }
    return false;   // unknown: don't risk it
}

bool KoboPower::writeSysfs(const char *path, const char *value)
{
    bool ok = writeFile(QString::fromLatin1(path), QByteArray(value));
    qDebug() << "power:" << value << ">" << path << (ok ? "ok" : strerror(errno));
    return ok;
}

void KoboPower::showSleepScreen()
{
    if (!m_screen) {
        QWidget *top = 0;
        foreach (QWidget *w, QApplication::topLevelWidgets())
            if (w->isVisible()) { top = w; break; }
        if (!top) return;
        m_screen = new SleepScreen(top);
    }
    m_screen->setGeometry(m_screen->parentWidget()->rect());
    m_screen->show();
    m_screen->raise();
    m_screen->repaint();
    if (m_eink) {
        if (m_eink->holding()) m_eink->releaseHold(false);
        m_eink->refreshAllFull();
    }
}

void KoboPower::hideSleepScreen()
{
    if (!m_screen) return;
    m_screen->hide();
    if (m_eink) m_eink->hintFull();
    m_screen->parentWidget()->update();
    if (m_eink) QTimer::singleShot(100, m_eink, SLOT(refreshAllFull()));
}

void KoboPower::sleep()
{
    if (m_state != Awake) return;
    qDebug() << "power: going to sleep";
    m_state = Entering;
    m_lastToggle.restart();
    m_idle.stop();
    m_unexpected = 0;
    showSleepScreen();
    if (KoboFrontlight::instance()) KoboFrontlight::instance()->suspendOff();
    // Give the full refresh time to land before Wi-Fi and the kernel get busy.
    QTimer::singleShot(800, this, SLOT(beginSuspend()));
}

void KoboPower::beginSuspend()
{
    if (m_state != Entering) return;     // woken up again in the meantime
    if (!m_suspendEnabled) { m_state = Asleep; return; }
    if (!onBattery()) {
        // Plugged in: keep the sleep screen and try again in a minute.
        qDebug() << "power: plugged in, not suspending (MTK kernels hang); retry in 60 s";
        m_state = Asleep;
        m_guard.start(60000);
        return;
    }
    if (m_wifiEnabled && !m_wifiOff) {
        // Synchronous: the radio must be down before the kernel suspends.
        QProcess p;
        p.setWorkingDirectory(m_dir);
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start("/bin/sh", QStringList() << m_dir + "/wifi-sleep.sh" << "suspend");
        if (p.waitForFinished(20000)) {
            QByteArray out = p.readAll().trimmed();
            qDebug() << "power: wifi-sleep.sh suspend:" << out;
            m_wifiOff = out.endsWith("wifi-was-on");
        } else {
            qWarning() << "power: wifi-sleep.sh suspend timed out";
            p.kill(); p.waitForFinished(1000);
            m_wifiOff = true;
        }
        if (m_state != Entering) return;
    }
    ::sync();
    if (!writeSysfs("/sys/power/state-extended", "1")) {
        m_state = Asleep;
        m_guard.start(15000);
        return;
    }
    m_extended = true;
    // KOReader: "I have traumatic memories of things going awry if we don't sleep
    // between the two writes". Keep reading input meanwhile, so a press cancels.
    m_step.start(2000);
}

void KoboPower::doSuspend()
{
    if (m_state != Entering) return;
    ::sync();
    qDebug() << "power: suspend to RAM";
    QElapsedTimer t; t.start();
    bool ok = writeSysfs("/sys/power/state", "mem");   // blocks until wakeup
    qDebug() << "power: back after" << t.elapsed() << "ms" << (ok ? "" : "(suspend refused)");
    if (!ok) {
        writeSysfs("/sys/power/state-extended", "0");   // like Nickel's 1 -> mem -> 0 on failure
        m_extended = false;
    }
    m_state = Asleep;
    m_lastToggle.restart();
    // A real wakeup comes with a power press (or the cover opening) that readable()
    // will see now. If nothing arrives, it was something else (RTC, charger): go back
    // to sleep, like KOReader's unexpected-wakeup guard.
    m_guard.start(ok ? 15000 : 5000);
}

void KoboPower::retrySuspend()
{
    if (m_state != Asleep) return;
    if (!onBattery()) { m_guard.start(60000); return; }
    if (!m_suspendEnabled) return;
    if (++m_unexpected > 20) {
        qWarning() << "power: too many wakeups without a power press, staying on the sleep screen";
        return;
    }
    qDebug() << "power: unexpected wakeup" << m_unexpected << ", suspending again";
    m_state = Entering;
    beginSuspend();
}

void KoboPower::wake()
{
    if (m_state == Awake) return;
    qDebug() << "power: waking up";
    m_step.stop();
    m_guard.stop();
    m_lastToggle.restart();
    if (m_extended) {
        writeSysfs("/sys/power/state-extended", "0");
        m_extended = false;
        usleep(100000);   // KOReader: let the kernel catch up
    }
    m_state = Awake;
    m_unexpected = 0;
    if (KoboFrontlight::instance()) KoboFrontlight::instance()->resumeOn();
    hideSleepScreen();
    if (m_idle.interval() > 0 && m_idle.interval() < 0x7fffffff) m_idle.start();
    if (m_wifiOff) {
        m_wifiOff = false;
        emit message("Reconnecting Wi-Fi...");
        if (!m_wifi) {
            m_wifi = new QProcess(this);
            m_wifi->setWorkingDirectory(m_dir);
            m_wifi->setProcessChannelMode(QProcess::MergedChannels);
            connect(m_wifi, SIGNAL(finished(int,QProcess::ExitStatus)), this, SLOT(wifiDone()));
        }
        if (m_wifi->state() == QProcess::NotRunning)
            m_wifi->start("/bin/sh", QStringList() << m_dir + "/wifi-sleep.sh" << "resume");
    }
}

void KoboPower::wifiDone()
{
    QByteArray out = m_wifi->readAll().trimmed();
    qDebug() << "power: wifi-sleep.sh resume:" << out;
    QList<QByteArray> lines = out.split('\n');
    QString last = lines.isEmpty() ? QString() : QString::fromLocal8Bit(lines.last()).trimmed();
    if (last.startsWith("connected")) emit message("Wi-Fi reconnected");
    else emit message("Wi-Fi did not reconnect" + (last.isEmpty() ? QString() : ": " + last));
}
