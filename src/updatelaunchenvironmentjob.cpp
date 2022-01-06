/*
    SPDX-FileCopyrightText: 2020 Kai Uwe Broulik <kde@broulik.de>
    SPDX-FileCopyrightText: 2021 David Edmundson <davidedmundson@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "updatelaunchenvironmentjob.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusPendingReply>

#include <QTimer>

#include "kdbusaddons_debug.h"

class KUpdateLaunchEnvironmentJobPrivate
{
public:
    explicit KUpdateLaunchEnvironmentJobPrivate(UpdateLaunchEnvironmentJob *q);
    void monitorReply(const QDBusPendingReply<> &reply);

    static bool isPosixName(const QString &name);
    static bool isSystemdApprovedValue(const QString &value);

    UpdateLaunchEnvironmentJob *q;
    QProcessEnvironment environment;
    int pendingReplies = 0;
};

KUpdateLaunchEnvironmentJobPrivate::KUpdateLaunchEnvironmentJobPrivate(UpdateLaunchEnvironmentJob *q)
    : q(q)
{
}

void KUpdateLaunchEnvironmentJobPrivate::monitorReply(const QDBusPendingReply<> &reply)
{
    ++pendingReplies;

    auto *watcher = new QDBusPendingCallWatcher(reply, q);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, q, [this](QDBusPendingCallWatcher *watcher) {
        watcher->deleteLater();
        --pendingReplies;

        if (pendingReplies == 0) {
            Q_EMIT q->finished();
            q->deleteLater();
        }
    });
}

// KF6 TODO: add K-prefix to class name
UpdateLaunchEnvironmentJob::UpdateLaunchEnvironmentJob(const QProcessEnvironment &environment)
    : d(new KUpdateLaunchEnvironmentJobPrivate(this))
{
    d->environment = environment;
    QTimer::singleShot(0, this, &UpdateLaunchEnvironmentJob::start);
}

UpdateLaunchEnvironmentJob::~UpdateLaunchEnvironmentJob() = default;

void UpdateLaunchEnvironmentJob::start()
{
    qDBusRegisterMetaType<QMap<QString, QString>>();
    QMap<QString, QString> dbusActivationEnv;
    QStringList systemdUpdates;

    for (const auto &varName : d->environment.keys()) {
        if (!KUpdateLaunchEnvironmentJobPrivate::isPosixName(varName)) {
            qCWarning(KDBUSADDONS_LOG) << "Skipping syncing of environment variable " << varName << "as name contains unsupported characters";
            continue;
        }
        const QString value = d->environment.value(varName);

        // KLauncher; remove this in KF6 (by then KInit will be gone)
        QDBusMessage klauncherMsg = QDBusMessage::createMethodCall(QStringLiteral("org.kde.klauncher5"),
                                                                   QStringLiteral("/KLauncher"),
                                                                   QStringLiteral("org.kde.KLauncher"),
                                                                   QStringLiteral("setLaunchEnv"));
        klauncherMsg.setArguments({QVariant::fromValue(varName), QVariant::fromValue(value)});
        auto klauncherReply = QDBusConnection::sessionBus().asyncCall(klauncherMsg);
        d->monitorReply(klauncherReply);

        // plasma-session
        QDBusMessage plasmaSessionMsg = QDBusMessage::createMethodCall(QStringLiteral("org.kde.Startup"),
                                                                       QStringLiteral("/Startup"),
                                                                       QStringLiteral("org.kde.Startup"),
                                                                       QStringLiteral("updateLaunchEnv"));
        plasmaSessionMsg.setArguments({QVariant::fromValue(varName), QVariant::fromValue(value)});
        auto plasmaSessionReply = QDBusConnection::sessionBus().asyncCall(plasmaSessionMsg);
        d->monitorReply(plasmaSessionReply);

        // DBus-activation environment
        dbusActivationEnv.insert(varName, value);

        // _user_ systemd env
        // Systemd has stricter parsing of valid environment variables
        // https://github.com/systemd/systemd/issues/16704
        // validate here
        if (!KUpdateLaunchEnvironmentJobPrivate::isSystemdApprovedValue(value)) {
            qCWarning(KDBUSADDONS_LOG) << "Skipping syncing of environment variable " << varName << "as value contains unsupported characters";
            continue;
        }
        const QString updateString = varName + QStringLiteral("=") + value;
        systemdUpdates.append(updateString);
    }

    // DBus-activation environment
    QDBusMessage dbusActivationMsg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                                                    QStringLiteral("/org/freedesktop/DBus"),
                                                                    QStringLiteral("org.freedesktop.DBus"),
                                                                    QStringLiteral("UpdateActivationEnvironment"));
    dbusActivationMsg.setArguments({QVariant::fromValue(dbusActivationEnv)});

    auto dbusActivationReply = QDBusConnection::sessionBus().asyncCall(dbusActivationMsg);
    d->monitorReply(dbusActivationReply);

    // _user_ systemd env
    QDBusMessage systemdActivationMsg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
                                                                       QStringLiteral("/org/freedesktop/systemd1"),
                                                                       QStringLiteral("org.freedesktop.systemd1.Manager"),
                                                                       QStringLiteral("SetEnvironment"));
    systemdActivationMsg.setArguments({systemdUpdates});

    auto systemdActivationReply = QDBusConnection::sessionBus().asyncCall(systemdActivationMsg);
    d->monitorReply(systemdActivationReply);
}

bool KUpdateLaunchEnvironmentJobPrivate::isPosixName(const QString &name)
{
    // Posix says characters like % should be 'tolerated', but it gives issues in practice.
    // https://bugzilla.redhat.com/show_bug.cgi?id=1754395
    // https://bugzilla.redhat.com/show_bug.cgi?id=1879216
    // Ensure systemd compat by only allowing alphanumerics and _ in names.
    bool first = true;
    for (const QChar c : name) {
        if (first && !c.isLetter() && c != QLatin1Char('_')) {
            return false;
        } else if (first) {
            first = false;
        } else if (!c.isLetterOrNumber() && c != QLatin1Char('_')) {
            return false;
        }
    }
    return !first;
}

bool KUpdateLaunchEnvironmentJobPrivate::isSystemdApprovedValue(const QString &value)
{
    // systemd code checks that a value contains no control characters except \n \t
    // effectively copied from systemd's string_has_cc
    for (const char &it : value.toLatin1()) {
        if (it == QLatin1Char('\n') || it == QLatin1Char('\t')) {
            continue;
        }
        if (it > 0 && it < ' ') {
            return false;
        }
        if (it == 127) {
            return false;
        }
    }
    return true;
}
