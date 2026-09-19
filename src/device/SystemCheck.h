#pragma once

#include <QString>
#include <QStringList>

namespace quadcap::device {

/*!
 * What is standing between this machine and a working capture, in the order a person hits it.
 *
 * The card needs an out-of-tree driver that no distribution ships, and on a Secure Boot machine that
 * driver has to be signed by a key the firmware trusts. Neither is obvious from a black window, so
 * the app diagnoses it rather than leaving someone to guess.
 */
enum class SetupIssue {
    Ready,
    NoCard,
    DriverNotInstalled,
    DriverNotLoaded,
    AwaitingMokEnrollment,
    NodeUnreadable,
};

struct SetupStatus {
    SetupIssue issue = SetupIssue::Ready;

    bool cardPresent = false;
    bool moduleLoaded = false;
    bool dkmsRegistered = false;
    bool secureBootEnabled = false;
    bool videoNodePresent = false;
    bool videoNodeReadable = false;

    QString pciAddress;
    //! One line naming what is wrong.
    QString headline;
    //! What to do about it, in order.
    QStringList steps;
    //! The single command worth copying, empty when there is nothing to run.
    QString command;

    [[nodiscard]] bool ready() const { return issue == SetupIssue::Ready; }
};

/*!
 * Inspects the machine and reports the first thing blocking capture.
 *
 * Reads sysfs and the filesystem only — no subprocesses, so it is cheap enough to call whenever the
 * device state changes and safe to call from the UI thread.
 */
[[nodiscard]] SetupStatus inspectSystem();

//! Name of the issue, for logs and tests.
[[nodiscard]] QString setupIssueName(SetupIssue issue);

} // namespace quadcap::device
