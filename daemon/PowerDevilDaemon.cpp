/***************************************************************************
 *   Copyright (C) 2008 by Dario Freddi <drf@kde.org>                      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA .        *
 ***************************************************************************
 *                                                                         *
 *   Part of the code in this file was taken from KDE4Powersave and/or     *
 *   Lithium, where noticed.                                               *
 *                                                                         *
 **************************************************************************/

#include "PowerDevilDaemon.h"

#include <kdemacros.h>
#include <KAboutData>
#include <KAction>
#include <KActionCollection>
#include <KPluginFactory>
#include <KNotification>
#include <KIcon>
#include <KMessageBox>
#include <kpluginfactory.h>
#include <klocalizedstring.h>
#include <kjob.h>
#include <kworkspace/kworkspace.h>
#include <KApplication>
#include <kidletime.h>
#include <KStandardDirs>

#include <QWeakPointer>
#include <QWidget>
#include <QTimer>

#include "PowerDevilSettings.h"
#include "powerdeviladaptor.h"
#include "PowerManagementConnector.h"
#include "SuspensionLockHandler.h"

#include <solid/device.h>
#include <solid/deviceinterface.h>
#include <solid/processor.h>

#include "screensaver_interface.h"
//#include "kscreensaver_interface.h"
#include "ksmserver_interface.h"

#include <config-powerdevil.h>
#include <config-workspace.h>

#ifdef HAVE_DPMS
#include <X11/Xmd.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xos.h>
extern "C" {
#include <X11/extensions/dpms.h>
    int __kde_do_not_unload = 1;

#ifndef HAVE_DPMSCAPABLE_PROTO
    Bool DPMSCapable(Display *);
#endif

#ifndef HAVE_DPMSINFO_PROTO
    Status DPMSInfo(Display *, CARD16 *, BOOL *);
#endif
}

static XErrorHandler defaultHandler;

#endif

K_PLUGIN_FACTORY(PowerDevilFactory,
                 registerPlugin<PowerDevilDaemon>();)
K_EXPORT_PLUGIN(PowerDevilFactory("powerdevildaemon"))

class PowerDevilDaemon::Private
{
public:
    explicit Private()
            : notifier(0)
            , screenSaverIface(0)
            , ksmServerIface(0)
            , currentConfig(0)
            , lockHandler(0)
            , notificationTimer(0)
            , status(PowerDevilDaemon::NoAction)
            , ckSessionInterface(0) {}

    Solid::Control::PowerManager::Notifier *notifier;
    QWeakPointer<Solid::Battery> battery;

    OrgFreedesktopScreenSaverInterface *screenSaverIface;
    OrgKdeKSMServerInterfaceInterface *ksmServerIface;
//  Now we send a signal to trigger the configuration of Kscreensaver (Bug #177123)
//    and we don't need the interface anymore
//     OrgKdeScreensaverInterface * kscreenSaverIface;

    KComponentData applicationData;
    KSharedConfig::Ptr profilesConfig;
    KConfigGroup *currentConfig;
    SuspensionLockHandler *lockHandler;

    QString currentProfile;
    QStringList availableProfiles;

    QWeakPointer<KNotification> notification;
    QTimer *notificationTimer;

    PowerDevilDaemon::IdleStatus status;

    int batteryPercent;
    int brightness;
    bool isPlugged;

    // ConsoleKit stuff
    QDBusInterface *ckSessionInterface;
    bool ckAvailable;
};

PowerDevilDaemon::PowerDevilDaemon(QObject *parent, const QList<QVariant>&)
        : KDEDModule(parent),
        d(new Private())
{
    KGlobal::locale()->insertCatalog("powerdevil");

    KAboutData aboutData("powerdevil", "powerdevil", ki18n("PowerDevil"),
                         POWERDEVIL_VERSION, ki18n("A Power Management tool for KDE4"),
                         KAboutData::License_GPL, ki18n("(c) 2008 Dario Freddi"),
                         KLocalizedString(), "http://www.kde.org");

    aboutData.addAuthor(ki18n("Dario Freddi"), ki18n("Maintainer"), "drf@kde.org",
                        "http://drfav.wordpress.com");

    d->applicationData = KComponentData(aboutData);

    QTimer::singleShot(0, this, SLOT(init()));
}

PowerDevilDaemon::~PowerDevilDaemon()
{
    delete d->ckSessionInterface;
    delete d;
}

void PowerDevilDaemon::init()
{
    d->notifier = Solid::Control::PowerManager::notifier();
    d->lockHandler = new SuspensionLockHandler(this);
    d->notificationTimer = new QTimer(this);

    /* First things first: PowerDevil might be used when another powermanager is already
     * on. So we need to check the system bus and see if another known powermanager has
     * already been registered. Let's see.
     */

    QDBusConnection conn = QDBusConnection::systemBus();

    if (conn.interface()->isServiceRegistered("org.freedesktop.PowerManagement") ||
            conn.interface()->isServiceRegistered("com.novell.powersave") ||
            conn.interface()->isServiceRegistered("org.freedesktop.Policy.Power")) {
        kError() << "PowerDevil not initialized, another power manager has been detected";
        return;
    }

    // First things first: let's set up PowerDevil to be aware of active session
    setUpConsoleKit();

    d->profilesConfig = KSharedConfig::openConfig("powerdevil2profilesrc", KConfig::SimpleConfig);
    setAvailableProfiles(d->profilesConfig->groupList());

    recacheBatteryPointer(true);

    // Set up all needed DBus interfaces
    d->screenSaverIface = new OrgFreedesktopScreenSaverInterface("org.freedesktop.ScreenSaver",
                                                                 "/ScreenSaver",
                                                                 QDBusConnection::sessionBus(), this);
    d->ksmServerIface = new OrgKdeKSMServerInterfaceInterface("org.kde.ksmserver", "/KSMServer",
                                                              QDBusConnection::sessionBus(), this);

    /*  Not needed anymore; I am not sure if we will need that in a future, so I leave it here
     *  just in case.
     *
     *   d->kscreenSaverIface = new OrgKdeScreensaverInterface("org.freedesktop.ScreenSaver", "/ScreenSaver",
     *         QDBusConnection::sessionBus(), this);
    */
    connect(d->notifier, SIGNAL(brightnessChanged(float)), SLOT(brightnessChangedSlot(float)));
    connect(d->notifier, SIGNAL(buttonPressed(int)), this, SLOT(buttonPressed(int)));
    connect(d->notifier, SIGNAL(batteryRemainingTimeChanged(int)),
            this, SLOT(batteryRemainingTimeChanged(int)));
    connect(d->lockHandler, SIGNAL(streamCriticalNotification(const QString&, const QString&,
                                   const char*, const QString&)),
            SLOT(emitNotification(const QString&, const QString&,
                                          const char*, const QString&)));
    connect(KIdleTime::instance(), SIGNAL(timeoutReached(int,int)), this, SLOT(poll(int,int)));
    connect(KIdleTime::instance(), SIGNAL(resumingFromIdle()), this, SLOT(resumeFromIdle()));

    //DBus
    new PowerDevilAdaptor(this);
    new PowerManagementConnector(this);

    conn.interface()->registerService("org.freedesktop.Policy.Power");
    QDBusConnection::sessionBus().registerService("org.kde.powerdevil");
    // All systems up Houston, let's go!
    refreshStatus();

    KActionCollection* actionCollection = new KActionCollection( this );

    KAction* globalAction = actionCollection->addAction("Increase Screen Brightness");
    globalAction->setText(i18nc("Global shortcut", "Increase Screen Brightness"));
    globalAction->setGlobalShortcut(KShortcut(Qt::Key_MonBrightnessUp), KAction::ShortcutTypes(KAction::ActiveShortcut | KAction::DefaultShortcut), KAction::NoAutoloading);
    connect(globalAction, SIGNAL(triggered(bool)), SLOT(increaseBrightness()));

    globalAction = actionCollection->addAction("Decrease Screen Brightness");
    globalAction->setText(i18nc("Global shortcut", "Decrease Screen Brightness"));
    globalAction->setGlobalShortcut(KShortcut(Qt::Key_MonBrightnessDown), KAction::ShortcutTypes(KAction::ActiveShortcut | KAction::DefaultShortcut), KAction::NoAutoloading);
    connect(globalAction, SIGNAL(triggered(bool)), SLOT(decreaseBrightness()));
}

void PowerDevilDaemon::batteryRemainingTimeChanged(int time)
{
    kDebug() << KGlobal::locale()->formatDuration(time);
}

SuspensionLockHandler *PowerDevilDaemon::lockHandler()
{
    return d->lockHandler;
}

QString PowerDevilDaemon::profile() const
{
    return d->currentProfile;
}

bool PowerDevilDaemon::recacheBatteryPointer(bool force)
{
    /* You'll see some switches on d->battery. This is fundamental since PowerDevil might run
     * also on system without batteries. Most of modern desktop systems support CPU scaling,
     * so somebody might find PowerDevil handy, and we don't want it to crash on them. To put it
     * short, we simply bypass all adaptor and battery events if no batteries are found.
     */

    if (!d->battery.isNull()) {
        if (d->battery.data()->isValid() && !force) {
            return true;
        }
    }

    d->battery.clear();

    // Here we get our battery interface, it will be useful later.
    foreach(const Solid::Device &device, Solid::Device::listFromType(Solid::DeviceInterface::Battery,
                                                                     QString())) {
        Solid::Device dev = device;
        Solid::Battery *b = qobject_cast<Solid::Battery*> (dev.asDeviceInterface(
                Solid::DeviceInterface::Battery));

        if (b->type() != Solid::Battery::PrimaryBattery) {
            continue;
        }

        if (b->isValid()) {
            d->battery = b;
        }
    }

    /* Those slots are relevant only if we're on a system that has a battery. If not, we simply don't care
     * about them.
     */
    if (!d->battery.isNull()) {
        connect(d->notifier, SIGNAL(acAdapterStateChanged(int)), this, SLOT(acAdapterStateChanged(int)));

        if (!connect(d->battery.data(), SIGNAL(chargePercentChanged(int, const QString &)), this,
                     SLOT(batteryChargePercentChanged(int, const QString &)))) {

            emitNotification("powerdevilerror", i18n("Could not connect to battery interface.\n"
                                     "Please check your system configuration"), 0, "dialog-error");
            return false;
        }
    } else {
        return false;
    }

    return true;
}

void PowerDevilDaemon::resumeFromIdle()
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    Solid::Control::PowerManager::setBrightness(d->brightness);

    d->status = NoAction;
}

void PowerDevilDaemon::refreshStatus()
{
    /* The configuration could have changed if this function was called, so
     * let's resync it.
     */
    PowerDevilSettings::self()->readConfig();
    d->profilesConfig->reparseConfiguration();

    if (!reloadProfile()) {
        return;
    }

    getCurrentProfile(true);

    /* Let's force status update, if we have a battery. Otherwise, let's just
     * re-apply the current profile.
     */
    if (!d->battery.isNull()) {
        acAdapterStateChanged(Solid::Control::PowerManager::acAdapterState(), true);
    } else {
        applyProfile();
    }
}

void PowerDevilDaemon::acAdapterStateChanged(int state, bool forced)
{
    if (state == Solid::Control::PowerManager::Plugged && !forced) {
        setACPlugged(true);

        // If the AC Adaptor has been plugged in, let's clear some pending suspend actions
        if (!d->lockHandler->canStartNotification()) {
            cleanUpTimer();
            d->lockHandler->releaseNotificationLock();
            emitNotification("pluggedin", i18n("The power adaptor has been plugged in. Any pending "
                                               "suspend actions have been canceled."));
        } else {
            emitNotification("pluggedin", i18n("The power adaptor has been plugged in."));
        }
    }

    if (state == Solid::Control::PowerManager::Unplugged && !forced) {
        setACPlugged(false);
        emitNotification("unplugged", i18n("The power adaptor has been unplugged."));
    }

    if (!forced) {
        reloadProfile(state);
    }

    applyProfile();
}

#ifdef HAVE_DPMS
extern "C" {
    int dropError(Display *, XErrorEvent *);
    typedef int (*XErrFunc)(Display *, XErrorEvent *);
}

int dropError(Display *, XErrorEvent *)
{
    return 0;
}
#endif

void PowerDevilDaemon::applyProfile()
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    KConfigGroup * settings = getCurrentProfile();

    if (!settings) {
        return;
    }

    Solid::Control::PowerManager::setBrightness(settings->readEntry("brightness").toInt());
    d->brightness = settings->readEntry("brightness").toInt();

    Solid::Control::PowerManager::setPowerSave(settings->readEntry("setPowerSave", false));

    // Compositing!!

    if (settings->readEntry("disableCompositing", false)) {
        if (toggleCompositing(false)) {
            PowerDevilSettings::setCompositingChanged(true);
            PowerDevilSettings::self()->writeConfig();
        }
    } else if (PowerDevilSettings::compositingChanged()) {
        toggleCompositing(true);
        PowerDevilSettings::setCompositingChanged(false);
        PowerDevilSettings::self()->writeConfig();
    }

    if (PowerDevilSettings::manageDPMS()) {
        setUpDPMS();
    }
}

void PowerDevilDaemon::setUpDPMS()
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    KConfigGroup * settings = getCurrentProfile();

    if (!settings) {
        return;
    }

#ifdef HAVE_DPMS

    defaultHandler = XSetErrorHandler(dropError);

    Display *dpy = QX11Info::display();

    int dummy;
    bool has_DPMS = true;

    if (!DPMSQueryExtension(dpy, &dummy, &dummy) || !DPMSCapable(dpy)) {
        has_DPMS = false;
        XSetErrorHandler(defaultHandler);
    }

    if (has_DPMS) {

        if (settings->readEntry("DPMSEnabled", false)) {
            DPMSEnable(dpy);
        } else {
            DPMSDisable(dpy);
        }

        XFlush(dpy);
        XSetErrorHandler(defaultHandler);

        //

        int standby = 60 * settings->readEntry("DPMSStandby").toInt();
        int suspend = 60 * settings->readEntry("DPMSSuspend").toInt();
        int poff = 60 * settings->readEntry("DPMSPowerOff").toInt();

        if (!settings->readEntry("DPMSStandbyEnabled", false)) {
            standby = 0;
        }
        if (!settings->readEntry("DPMSSuspendEnabled", false)) {
            suspend = 0;
        }
        if (!settings->readEntry("DPMSPowerOffEnabled", false)) {
            poff = 0;
        }

        DPMSSetTimeouts(dpy, standby, suspend, poff);

        XFlush(dpy);
        XSetErrorHandler(defaultHandler);

    }

    // The screen saver depends on the DPMS settings
    // Emit a signal so that Kscreensaver knows it has to re-configure itself
    emit DPMSconfigUpdated();
#endif
}

void PowerDevilDaemon::batteryChargePercentChanged(int percent, const QString &udi)
{
    Q_UNUSED(udi)
    Q_UNUSED(percent)

    int charge = 0;

    foreach(const Solid::Device &device, Solid::Device::listFromType(Solid::DeviceInterface::Battery,
                                                                     QString())) {
        Solid::Device d = device;
        Solid::Battery *battery = qobject_cast<Solid::Battery*> (d.asDeviceInterface(
                Solid::DeviceInterface::Battery));
        if (battery->chargePercent() > 0 && battery->type() == Solid::Battery::PrimaryBattery) {
            charge += battery->chargePercent();
        }
    }

    setBatteryPercent(charge);

    if (Solid::Control::PowerManager::acAdapterState() == Solid::Control::PowerManager::Plugged) {
        return;
    }

    if (!checkIfCurrentSessionActive()) {
        return;
    }

    if (charge <= PowerDevilSettings::batteryCriticalLevel()) {
        switch (PowerDevilSettings::batLowAction()) {
        case Shutdown:
            if (PowerDevilSettings::waitBeforeSuspending()) {
                emitNotification("criticalbattery",
                                 i18np("Your battery level is critical, the computer will "
                                       "be halted in 1 second.",
                                       "Your battery level is critical, the computer will "
                                       "be halted in %1 seconds.",
                                       PowerDevilSettings::waitBeforeSuspendingTime()),
                                 SLOT(shutdown()), "dialog-warning");
            } else {
                shutdown();
            }
            break;
        case S2Disk:
            if (PowerDevilSettings::waitBeforeSuspending()) {
                emitNotification("criticalbattery",
                                 i18np("Your battery level is critical, the computer will "
                                       "be suspended to disk in 1 second.",
                                       "Your battery level is critical, the computer will "
                                       "be suspended to disk in %1 seconds.",
                                       PowerDevilSettings::waitBeforeSuspendingTime()),
                                 SLOT(suspendToDisk()), "dialog-warning");
            } else {
                suspendToDisk();
            }
            break;
        case S2Ram:
            if (PowerDevilSettings::waitBeforeSuspending()) {
                emitNotification("criticalbattery",
                                 i18np("Your battery level is critical, the computer "
                                       "will be suspended to RAM in 1 second.",
                                       "Your battery level is critical, the computer "
                                       "will be suspended to RAM in %1 seconds.",
                                       PowerDevilSettings::waitBeforeSuspendingTime()),
                                 SLOT(suspendToRam()), "dialog-warning");
            } else {
                suspendToRam();
            }
            break;
        case Standby:
            if (PowerDevilSettings::waitBeforeSuspending()) {
                emitNotification("criticalbattery",
                                 i18np("Your battery level is critical, the computer "
                                       "will be put into standby in 1 second.",
                                       "Your battery level is critical, the computer "
                                       "will be put into standby in %1 seconds.",
                                       PowerDevilSettings::waitBeforeSuspendingTime()),
                                 SLOT(standby()), "dialog-warning");
            } else {
                standby();
            }
            break;
        default:
            emitNotification("criticalbattery", i18n("Your battery level is critical: "
                                                     "save your work as soon as possible."),
                             0, "dialog-warning");
            break;
        }
    } else if (charge == PowerDevilSettings::batteryWarningLevel()) {
        emitNotification("warningbattery", i18n("Your battery has reached the warning level."),
                         0, "dialog-warning");
        refreshStatus();
    } else if (charge == PowerDevilSettings::batteryLowLevel()) {
        emitNotification("lowbattery", i18n("Your battery has reached a low level."),
                         0, "dialog-warning");
        refreshStatus();
    }
}

void PowerDevilDaemon::buttonPressed(int but)
{
    if (!checkIfCurrentSessionActive() || d->screenSaverIface->GetActive()) {
        return;
    }

    KConfigGroup * settings = getCurrentProfile();

    if (!settings)
        return;

    kDebug() << "A button was pressed, code" << but;

    if (but == Solid::Control::PowerManager::LidClose) {

        switch (settings->readEntry("lidAction").toInt()) {
        case Shutdown:
            shutdown();
            break;
        case S2Disk:
            suspendToDisk();
            break;
        case S2Ram:
            suspendToRam();
            break;
        case Standby:
            standby();
            break;
        case Lock:
            lockScreen();
            break;
        case TurnOffScreen:
            turnOffScreen();
            break;
        default:
            break;
        }
    } else if (but == Solid::Control::PowerManager::PowerButton) {

        switch (settings->readEntry("powerButtonAction").toInt()) {
        case Shutdown:
            shutdown();
            break;
        case S2Disk:
            suspendToDisk();
            break;
        case S2Ram:
            suspendToRam();
            break;
        case Standby:
            standby();
            break;
        case Lock:
            lockScreen();
            break;
        case ShutdownDialog:
            shutdownDialog();
            break;
        case TurnOffScreen:
            turnOffScreen();
            break;
        default:
            break;
        }
    } else if (but == Solid::Control::PowerManager::SleepButton) {

        switch (settings->readEntry("sleepButtonAction").toInt()) {
        case Shutdown:
            shutdown();
            break;
        case S2Disk:
            suspendToDisk();
            break;
        case S2Ram:
            suspendToRam();
            break;
        case Standby:
            standby();
            break;
        case Lock:
            lockScreen();
            break;
        case ShutdownDialog:
            shutdownDialog();
            break;
        case TurnOffScreen:
            turnOffScreen();
            break;
        default:
            break;
        }
    }
}

void PowerDevilDaemon::decreaseBrightness()
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    Solid::Control::PowerManager::brightnessKeyPressed(Solid::Control::PowerManager::Decrease);
    emit brightnessChanged(qRound(Solid::Control::PowerManager::brightness()), true);
}

void PowerDevilDaemon::increaseBrightness()
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    Solid::Control::PowerManager::brightnessKeyPressed(Solid::Control::PowerManager::Increase);
    emit brightnessChanged(qRound(Solid::Control::PowerManager::brightness()), true);
}

void PowerDevilDaemon::shutdownNotification(bool automated)
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    if (!d->lockHandler->setNotificationLock(automated)) {
        return;
    }

    if (PowerDevilSettings::waitBeforeSuspending()) {
        emitNotification("doingjob", i18np("The computer will be halted in 1 second.",
                                           "The computer will be halted in %1 seconds.",
                                           PowerDevilSettings::waitBeforeSuspendingTime()),
                         SLOT(shutdown()));
    } else {
        shutdown();
    }
}

void PowerDevilDaemon::suspendToDiskNotification(bool automated)
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    if (!d->lockHandler->setNotificationLock(automated)) {
        return;
    }

    if (PowerDevilSettings::waitBeforeSuspending()) {
        emitNotification("doingjob", i18np("The computer will be suspended to disk in 1 second.",
                                           "The computer will be suspended to disk in %1 seconds.",
                                           PowerDevilSettings::waitBeforeSuspendingTime()),
                         SLOT(suspendToDisk()));
    } else {
        suspendToDisk();
    }
}

void PowerDevilDaemon::suspendToRamNotification(bool automated)
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    if (!d->lockHandler->setNotificationLock(automated)) {
        return;
    }

    if (PowerDevilSettings::waitBeforeSuspending()) {
        emitNotification("doingjob", i18np("The computer will be suspended to RAM in 1 second.",
                                           "The computer will be suspended to RAM in %1 seconds.",
                                           PowerDevilSettings::waitBeforeSuspendingTime()),
                         SLOT(suspendToRam()));
    } else {
        suspendToRam();
    }
}

void PowerDevilDaemon::standbyNotification(bool automated)
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    if (!d->lockHandler->setNotificationLock(automated)) {
        return;
    }

    if (PowerDevilSettings::waitBeforeSuspending()) {
        emitNotification("doingjob", i18np("The computer will be put into standby in 1 second.",
                                           "The computer will be put into standby in %1 seconds.",
                                           PowerDevilSettings::waitBeforeSuspendingTime()),
                         SLOT(standby()));
    } else {
        standby();
    }
}

void PowerDevilDaemon::shutdown(bool automated)
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    if (!d->lockHandler->setJobLock(automated)) {
        return;
    }

    d->ksmServerIface->logout((int)KWorkSpace::ShutdownConfirmNo, (int)KWorkSpace::ShutdownTypeHalt,
                              (int)KWorkSpace::ShutdownModeTryNow);

    d->lockHandler->releaseAllLocks();
}

void PowerDevilDaemon::shutdownDialog()
{
    d->ksmServerIface->logout((int)KWorkSpace::ShutdownConfirmYes, (int)KWorkSpace::ShutdownTypeDefault,
                              (int)KWorkSpace::ShutdownModeDefault);
}

void PowerDevilDaemon::suspendToDisk(bool automated)
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    if (!d->lockHandler->setJobLock(automated)) {
        return;
    }

    KIdleTime::instance()->simulateUserActivity(); //prevent infinite suspension loops

    if (PowerDevilSettings::configLockScreen()) {
        lockScreen();
    }

    KJob * job = Solid::Control::PowerManager::suspend(Solid::Control::PowerManager::ToDisk);
    connect(job, SIGNAL(result(KJob *)), this, SLOT(suspendJobResult(KJob *)));
    job->start();

    // Temporary hack...
    QTimer::singleShot(10000, d->lockHandler, SLOT(releaseAllLocks()));
}

void PowerDevilDaemon::suspendToRam(bool automated)
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    if (!d->lockHandler->setJobLock(automated)) {
        return;
    }

    KIdleTime::instance()->simulateUserActivity(); //prevent infinite suspension loops

    if (PowerDevilSettings::configLockScreen()) {
        lockScreen();
    }

    KJob * job = Solid::Control::PowerManager::suspend(Solid::Control::PowerManager::ToRam);
    connect(job, SIGNAL(result(KJob *)), this, SLOT(suspendJobResult(KJob *)));
    job->start();
    // Temporary hack...
    QTimer::singleShot(10000, d->lockHandler, SLOT(releaseAllLocks()));
}

void PowerDevilDaemon::standby(bool automated)
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    if (!d->lockHandler->setJobLock(automated)) {
        return;
    }

    KIdleTime::instance()->simulateUserActivity(); //prevent infinite suspension loops

    if (PowerDevilSettings::configLockScreen()) {
        lockScreen();
    }

    KJob * job = Solid::Control::PowerManager::suspend(Solid::Control::PowerManager::Standby);
    connect(job, SIGNAL(result(KJob *)), this, SLOT(suspendJobResult(KJob *)));
    job->start();

    // Temporary hack...
    QTimer::singleShot(10000, d->lockHandler, SLOT(releaseAllLocks()));
}

void PowerDevilDaemon::suspendJobResult(KJob * job)
{
    if (job->error()) {
        emitNotification("joberror", QString(i18n("There was an error while suspending:")
                                 + QChar('\n') + job->errorString()),  0, "dialog-error");
    }

    KIdleTime::instance()->simulateUserActivity(); //prevent infinite suspension loops

    kDebug() << "Resuming from suspension";

    d->lockHandler->releaseAllLocks();

    job->deleteLater();
}

void PowerDevilDaemon::poll(int id, int idle)
{
    Q_UNUSED(id)

    /* The polling system is abstract. This function gets called when the current
     * system requests it. Usually, it is called only when needed (if you're using
     * an efficient backend such as XSync).
     * We make an intensive use of qMin/qMax here to determine the minimum time.
     */

    // kDebug() << "Polling started, idle time is" << idle << "seconds";

    if (!checkIfCurrentSessionActive()) {
        return;
    }

    KConfigGroup * settings = getCurrentProfile();

    if (!settings) {
        return;
    }

    int dimOnIdleTime = settings->readEntry("dimOnIdleTime").toInt() * 60 * 1000;

    /* You'll see we release input lock here sometimes. Why? Well,
     * after some tests, I found out that the offscreen widget doesn't work
     * if the monitor gets turned off or the PC is suspended. But, we don't care
     * about this for a simple reason: the only parameter we need to look into
     * is the brightness, so we just release the lock, set back the brightness
     * to normal, and that's it.
     */

    if (idle == settings->readEntry("idleTime").toInt() * 60 * 1000) {

        if (d->status == Action) {
            return;
        }

        d->status = Action;

        switch (settings->readEntry("idleAction").toInt()) {
        case Shutdown:
            KIdleTime::instance()->catchNextResumeEvent();
            shutdownNotification(true);
            break;
        case S2Disk:
            KIdleTime::instance()->catchNextResumeEvent();
            suspendToDiskNotification(true);
            break;
        case S2Ram:
            KIdleTime::instance()->catchNextResumeEvent();
            suspendToRamNotification(true);
            break;
        case Standby:
            KIdleTime::instance()->catchNextResumeEvent();
            standbyNotification(true);
            break;
        case Lock:
            KIdleTime::instance()->catchNextResumeEvent();
            lockScreen();
            break;
        case TurnOffScreen:
            KIdleTime::instance()->catchNextResumeEvent();
            turnOffScreen();
            break;
        default:
            break;
        }

        return;

    } else if (settings->readEntry("dimOnIdle", false)
               && (idle == dimOnIdleTime)) {
        if (d->status != DimTotal) {
            d->status = DimTotal;
            KIdleTime::instance()->catchNextResumeEvent();
            Solid::Control::PowerManager::setBrightness(0);
        }
    } else if (settings->readEntry("dimOnIdle", false)
               && (idle == (dimOnIdleTime * 3 / 4))) {
        if (d->status != DimThreeQuarters) {
            d->status = DimThreeQuarters;
            KIdleTime::instance()->catchNextResumeEvent();
            float newBrightness = Solid::Control::PowerManager::brightness() / 4;
            Solid::Control::PowerManager::setBrightness(newBrightness);
        }
    } else if (settings->readEntry("dimOnIdle", false) &&
               (idle == (dimOnIdleTime * 1 / 2))) {
        if (d->status != DimHalf) {
            d->brightness = Solid::Control::PowerManager::brightness();
            d->status = DimHalf;
            KIdleTime::instance()->catchNextResumeEvent();
            float newBrightness = Solid::Control::PowerManager::brightness() / 2;
            Solid::Control::PowerManager::setBrightness(newBrightness);
        }
    } else {
        d->status = NoAction;
        KIdleTime::instance()->stopCatchingResumeEvent();
    }
}

void PowerDevilDaemon::lockScreen()
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    emitNotification("doingjob", i18n("The screen is being locked"));
    d->screenSaverIface->Lock();
}

void PowerDevilDaemon::emitNotification(const QString &evid, const QString &message,
                                        const char *slot, const QString &iconname)
{
    if (!slot) {
        KNotification::event(evid, message, KIcon(iconname).pixmap(20, 20),
                             0, KNotification::CloseOnTimeout, d->applicationData);
    } else {
        d->notification = KNotification::event(evid, message, KIcon(iconname).pixmap(20, 20),
                                               0, KNotification::Persistent, d->applicationData);
        d->notification.data()->setActions(QStringList() << i18nc("Interrupts the suspension/shutdown process",
                                                           "Cancel"));

        connect(d->notificationTimer, SIGNAL(timeout()), slot);
        connect(d->notificationTimer, SIGNAL(timeout()), SLOT(cleanUpTimer()));

        d->lockHandler->connect(d->notification.data(), SIGNAL(activated(unsigned int)),
                                d->lockHandler, SLOT(releaseNotificationLock()));
        connect(d->notification.data(), SIGNAL(activated(unsigned int)), SLOT(cleanUpTimer()));

        d->notificationTimer->start(PowerDevilSettings::waitBeforeSuspendingTime() * 1000);
    }
}

void PowerDevilDaemon::cleanUpTimer()
{
    kDebug() << "Disconnecting signals";

    d->notificationTimer->disconnect();
    d->notificationTimer->stop();

    if (d->notification) {
        d->notification.data()->disconnect();
        d->notification.data()->deleteLater();
    }
}

KConfigGroup * PowerDevilDaemon::getCurrentProfile(bool forcereload)
{
    /* We need to access this a lot of times, so we use a cached
     * implementation here. We create the object just if we're sure
     * it is not already valid.
     *
     * IMPORTANT!!! This class already handles deletion of the config
     * object, so you don't have to delete it!!
     */

    if (d->currentConfig) {   // This HAS to be kept, since d->currentConfig could be not valid!!
        if (forcereload || d->currentConfig->name() != d->currentProfile) {
            delete d->currentConfig;
            d->currentConfig = 0;
        }
    }

    if (!d->currentConfig) {
        d->currentConfig = new KConfigGroup(d->profilesConfig, d->currentProfile);
    }

    if (!d->currentConfig->isValid() || !d->currentConfig->entryMap().size()) {
        emitNotification("powerdevilerror", i18n("The profile \"%1\" has been selected, "
                                 "but it does not exist.\nPlease check your PowerDevil configuration.",
                                 d->currentProfile),  0, "dialog-error");
        reloadProfile();
        delete d->currentConfig;
        d->currentConfig = 0;
    }

    return d->currentConfig;
}

bool PowerDevilDaemon::reloadProfile(int state)
{
    if (!checkIfCurrentSessionActive()) {
        return false;
    }

    if (!recacheBatteryPointer()) {
        setCurrentProfile(PowerDevilSettings::aCProfile());
    } else {
        if (state == -1) {
            state = Solid::Control::PowerManager::acAdapterState();
        }

        if (state == Solid::Control::PowerManager::Plugged) {
            setCurrentProfile(PowerDevilSettings::aCProfile());
        } else if (d->battery.data()->chargePercent() <= PowerDevilSettings::batteryWarningLevel()) {
            setCurrentProfile(PowerDevilSettings::warningProfile());
        } else if (d->battery.data()->chargePercent() <= PowerDevilSettings::batteryLowLevel()) {
            setCurrentProfile(PowerDevilSettings::lowProfile());
        } else {
            setCurrentProfile(PowerDevilSettings::batteryProfile());
        }
    }

    if (d->currentProfile.isEmpty()) {
        /* Ok, misconfiguration! Well, first things first: if we have some profiles,
         * let's just load the first available one.
         */

        if (!d->availableProfiles.isEmpty()) {
            setCurrentProfile(d->availableProfiles.at(0));
        } else {
            /* In this case, let's fill our profiles file with our
             * wonderful defaults!
             */

            restoreDefaultProfiles();

            PowerDevilSettings::setACProfile("Performance");
            PowerDevilSettings::setBatteryProfile("Powersave");
            PowerDevilSettings::setLowProfile("Aggressive Powersave");
            PowerDevilSettings::setWarningProfile("Xtreme Powersave");

            // TODO 4.5: uncomment (string freeze)
//             emitNotification("profileset", i18n("Loaded default powersaving profiles"),  0, "dialog-error");

            PowerDevilSettings::self()->writeConfig();

            reloadAndStream();

            return false;
        }
    }

    KConfigGroup * settings = getCurrentProfile();

    if (!settings) {
        return false;
    }

    // Set up timeouts
    KIdleTime::instance()->removeAllIdleTimeouts();

    if (settings->readEntry("idleAction", false)) {
        KIdleTime::instance()->addIdleTimeout(settings->readEntry("idleTime").toInt() * 60 * 1000);
    }
    if (settings->readEntry("dimOnIdle", false)) {
        int dimOnIdleTime = settings->readEntry("dimOnIdleTime").toInt() * 60 * 1000;
        KIdleTime::instance()->addIdleTimeout(dimOnIdleTime);
        KIdleTime::instance()->addIdleTimeout(dimOnIdleTime * 1 / 2);
        KIdleTime::instance()->addIdleTimeout(dimOnIdleTime * 3 / 4);
    }

    return true;
}

void PowerDevilDaemon::setProfile(const QString & profile)
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    setCurrentProfile(profile);

    /* Don't call refreshStatus() here, since we don't want the predefined profile
     * to be loaded!!
     */

    applyProfile();

    KConfigGroup * settings = getCurrentProfile();

    emitNotification("profileset", i18n("Profile changed to \"%1\"", profile),
                     0, settings->readEntry("iconname"));
}

void PowerDevilDaemon::reloadAndStream()
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    setAvailableProfiles(d->profilesConfig->groupList());

    streamData();

    refreshStatus();
}

void PowerDevilDaemon::streamData()
{
    emit profileChanged(d->currentProfile, d->availableProfiles);
    emit stateChanged(d->batteryPercent, d->isPlugged);
}

QVariantMap PowerDevilDaemon::getSupportedSuspendMethods()
{
    QVariantMap retlist;

    Solid::Control::PowerManager::SuspendMethods methods =
            Solid::Control::PowerManager::supportedSuspendMethods();

    if (methods & Solid::Control::PowerManager::ToDisk) {
        retlist[i18n("Suspend to Disk")] = (int) S2Disk;
    }

    if (methods & Solid::Control::PowerManager::ToRam) {
        retlist[i18n("Suspend to RAM")] = (int) S2Ram;
    }

    if (methods & Solid::Control::PowerManager::Standby) {
        retlist[i18n("Standby")] = (int) Standby;
    }

    return retlist;
}

void PowerDevilDaemon::setPowerSave(bool powersave)
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    Solid::Control::PowerManager::setPowerSave(powersave);
}

void PowerDevilDaemon::suspend(int method)
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    switch ((IdleAction) method) {
    case S2Disk:
        QTimer::singleShot(100, this, SLOT(suspendToDisk()));
        break;
    case S2Ram:
        QTimer::singleShot(100, this, SLOT(suspendToRam()));
        break;
    case Standby:
        QTimer::singleShot(100, this, SLOT(standby()));
        break;
    default:
        break;
    }
}

void PowerDevilDaemon::setBrightness(int value)
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    if (value == -2) {
        // Then set brightness to half the current brightness.

        float b = Solid::Control::PowerManager::brightness() / 2;
        Solid::Control::PowerManager::setBrightness(b);
        return;
    }
    Solid::Control::PowerManager::setBrightness(value);
}

void PowerDevilDaemon::turnOffScreen()
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    if (PowerDevilSettings::configLockScreen()) {
        lockScreen();
    }

#ifdef HAVE_DPMS

    CARD16 dummy;
    BOOL enabled;
    Display *dpy = QX11Info::display();

    DPMSInfo(dpy, &dummy, &enabled);

    if (enabled) {
        DPMSForceLevel(dpy, DPMSModeOff);
    } else {
        DPMSEnable(dpy);
        DPMSForceLevel(dpy, DPMSModeOff);
    }
#endif
}

void PowerDevilDaemon::profileFirstLoad()
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    KConfigGroup * settings = getCurrentProfile();

    if (!settings)
        return;

    kDebug() << "Profile initialization";

    if (!settings->readEntry("scriptpath", QString()).isEmpty()) {
        QProcess::startDetached(settings->readEntry("scriptpath"));
    }
}

bool PowerDevilDaemon::toggleCompositing(bool enabled)
{
    QDBusInterface kwiniface("org.kde.kwin", "/KWin", "org.kde.KWin", QDBusConnection::sessionBus());

    QDBusReply<bool> state = kwiniface.call("compositingActive");

    if (state.value() != enabled) {
        kwiniface.call("toggleCompositing");
        return true;
    } else {
        return false;
    }
}

void PowerDevilDaemon::restoreDefaultProfiles()
{
    QString path = KStandardDirs::locate("data", "powerdevil/default.powerdevilprofiles");

    KConfig toImport(path, KConfig::SimpleConfig);

    foreach(const QString &ent, toImport.groupList()) {
        KConfigGroup copyFrom(&toImport, ent);
        KConfigGroup copyTo(d->profilesConfig, ent);

        copyFrom.copyTo(&copyTo);
        copyTo.config()->sync();
    }
}

void PowerDevilDaemon::setBatteryPercent(int newpercent)
{
    d->batteryPercent = newpercent;
    emit stateChanged(d->batteryPercent, d->isPlugged);
}

void PowerDevilDaemon::setACPlugged(bool newplugged)
{
    d->isPlugged = newplugged;
    // Reset idle counter - otherwise we might run into some funny things, like immediate suspension
    KIdleTime::instance()->simulateUserActivity();
    emit stateChanged(d->batteryPercent, d->isPlugged);
}

void PowerDevilDaemon::setCurrentProfile(const QString &profile)
{
    if (!checkIfCurrentSessionActive()) {
        return;
    }

    if (!d->availableProfiles.contains(profile)) {
        // Wait a minute, there's something wrong.
        d->currentProfile.clear();
        if (d->profilesConfig.data()->groupList().count() > 0) {
            // Notify back only if some profiles were saved. Otherwise we're just initializing the configuration and
            // it's not really worth it streaming an error
            emitNotification("powerdevilerror", i18n("The profile \"%1\" has been selected, "
                                                     "but it does not exist.\nPlease check your PowerDevil configuration.",
                                                     d->currentProfile),  0, "dialog-error");
        }
        return;
    }

    if (profile != d->currentProfile) {
        d->currentProfile = profile;
        profileFirstLoad();
        emit profileChanged(d->currentProfile, d->availableProfiles);
    }
}

void PowerDevilDaemon::setAvailableProfiles(const QStringList &aProfiles)
{
    d->availableProfiles = aProfiles;
    if (!d->currentProfile.isEmpty()) {
        emit profileChanged(d->currentProfile, d->availableProfiles);
    }
}

bool PowerDevilDaemon::checkIfCurrentSessionActive()
{
    if (!d->ckAvailable) {
        // No way to determine if we are on the current session, simply suppose we are
        kDebug() << "Can't contact ck";
        return true;
    }

    QDBusReply<bool> rp = d->ckSessionInterface->call("IsActive");

    return rp.value();
}

void PowerDevilDaemon::setUpConsoleKit()
{
    // Let's cache the needed information to check if our session is actually active

    if (!QDBusConnection::systemBus().interface()->isServiceRegistered("org.freedesktop.ConsoleKit")) {
        // No way to determine if we are on the current session, simply suppose we are
        kDebug() << "Can't contact ck";
        d->ckAvailable = false;
        return;
    } else {
        d->ckAvailable = true;
    }

    // Otherwise, let's ask ConsoleKit
    QDBusInterface ckiface("org.freedesktop.ConsoleKit", "/org/freedesktop/ConsoleKit/Manager",
                           "org.freedesktop.ConsoleKit.Manager", QDBusConnection::systemBus());

    QDBusReply<QDBusObjectPath> sessionPath = ckiface.call("GetCurrentSession");

    if (!sessionPath.isValid() || sessionPath.value().path().isEmpty()) {
        kDebug() << "The session is not registered with ck";
        d->ckAvailable = false;
        return;
    }

    d->ckSessionInterface = new QDBusInterface("org.freedesktop.ConsoleKit", sessionPath.value().path(),
            "org.freedesktop.ConsoleKit.Session", QDBusConnection::systemBus());

    if (!d->ckSessionInterface->isValid()) {
        // As above
        kDebug() << "Can't contact iface";
        d->ckAvailable = false;
        return;
    }

    /* If everything's fine, let's attach ourself to the ActiveChanged signal.
     * You'll see we're attaching to refreshStatus without any further checks. You know why?
     * refreshStatus already checks if the console is active, so the check is already happening
     * in the underhood
     */

    QDBusConnection::systemBus().connect("org.freedesktop.ConsoleKit", sessionPath.value().path(),
                                         "org.freedesktop.ConsoleKit.Session", "ActiveChanged", this,
                                         SLOT(refreshStatus()));
}

void PowerDevilDaemon::brightnessChangedSlot(float brightness) {
    emit brightnessChanged(qRound(brightness), false);
}

#include "PowerDevilDaemon.moc"
