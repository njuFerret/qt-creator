// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "iosrunner.h"

#include "devicectlutils.h"
#include "iosconfigurations.h"
#include "iosconstants.h"
#include "iosdevice.h"
#include "iosrunconfiguration.h"
#include "iossimulator.h"
#include "iostoolhandler.h"
#include "iostr.h"

#include <debugger/debuggerconstants.h>
#include <debugger/debuggerkitaspect.h>
#include <debugger/debuggerruncontrol.h>

#include <projectexplorer/devicesupport/devicekitaspects.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/qmldebugcommandlinearguments.h>
#include <projectexplorer/runconfigurationaspects.h>
#include <projectexplorer/target.h>
#include <projectexplorer/taskhub.h>
#include <projectexplorer/toolchain.h>

#include <utils/fileutils.h>
#include <utils/qtcprocess.h>
#include <utils/stringutils.h>
#include <utils/temporaryfile.h>
#include <utils/url.h>
#include <utils/utilsicons.h>

#include <solutions/tasking/tasktree.h>

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>
#include <QTcpServer>
#include <QTime>
#include <QTimer>

#include <memory>

#include <fcntl.h>
#ifdef Q_OS_UNIX
#include <unistd.h>
#else
#include <io.h>
#endif

using namespace Debugger;
using namespace ProjectExplorer;
using namespace Utils;
using namespace Tasking;

namespace Ios::Internal {

char QML_DEBUGGER_WAITING[] = "QML Debugger: Waiting for connection on port ([0-9]+)...";

static QString identifierForRunControl(RunControl *runControl)
{
    const IosDeviceTypeAspect::Data *data = runControl->aspectData<IosDeviceTypeAspect>();
    return data ? data->deviceType.identifier : QString();
}

static void stopRunningRunControl(RunControl *runControl)
{
    static QMap<Id, QPointer<RunControl>> activeRunControls;

    // clean up deleted
    Utils::erase(activeRunControls, [](const QPointer<RunControl> &rc) { return !rc; });

    const Id devId = RunDeviceKitAspect::deviceId(runControl->kit());
    const QString identifier = identifierForRunControl(runControl);

    // The device can only run an application at a time, if an app is running stop it.
    if (QPointer<RunControl> activeRunControl = activeRunControls[devId]) {
        if (identifierForRunControl(activeRunControl) == identifier) {
            activeRunControl->initiateStop();
            activeRunControls.remove(devId);
        }
    }

    if (devId.isValid())
        activeRunControls[devId] = runControl;
}

static QString getBundleIdentifier(const FilePath &bundlePath)
{
    QSettings settings(bundlePath.pathAppended("Info.plist").toUrlishString(), QSettings::NativeFormat);
    return settings.value(QString::fromLatin1("CFBundleIdentifier")).toString();
}

struct AppInfo
{
    QUrl pathOnDevice;
    qint64 processIdentifier = -1;
    IosDevice::ConstPtr device;
    FilePath bundlePath;
    QString bundleIdentifier;
    QStringList arguments;
};

class DeviceCtlRunnerBase : public RunWorker
{
public:
    DeviceCtlRunnerBase(RunControl *runControl);

    void start();

protected:
    void reportStoppedImpl();

    IosDevice::ConstPtr m_device;

private:
    virtual GroupItem launchTask(const Storage<AppInfo> &appInfo) = 0;

    FilePath m_bundlePath;
    std::unique_ptr<TaskTree> m_startTask;
};

class DeviceCtlPollingRunner final : public DeviceCtlRunnerBase
{
public:
    DeviceCtlPollingRunner(RunControl *runControl);

    void stop();

private:
    GroupItem launchTask(const Storage<AppInfo> &appInfo);
    void checkProcess();

    qint64 m_processIdentifier = -1;
    std::unique_ptr<TaskTree> m_stopTask;
    std::unique_ptr<TaskTree> m_pollTask;
    QTimer m_pollTimer;
};

class DeviceCtlRunner final : public DeviceCtlRunnerBase
{
public:
    DeviceCtlRunner(RunControl *runControl);

    void stop();

    void setStartStopped(bool startStopped) { m_startStopped = startStopped; }

private:
    GroupItem launchTask(const Storage<AppInfo> &appInfo);

    Process m_process;
    std::unique_ptr<TemporaryFile> m_deviceCtlOutput;
    std::unique_ptr<TaskTree> m_processIdTask;
    bool m_startStopped = false;
};

DeviceCtlRunnerBase::DeviceCtlRunnerBase(RunControl *runControl)
    : RunWorker(runControl)
{
    setId("IosDeviceCtlRunner");
    const IosDeviceTypeAspect::Data *data = runControl->aspectData<IosDeviceTypeAspect>();
    QTC_ASSERT(data, return);
    m_bundlePath = data->bundleDirectory;
    m_device = std::dynamic_pointer_cast<const IosDevice>(RunDeviceKitAspect::device(runControl->kit()));
}

static GroupItem initSetup(RunControl *runControl, const Storage<AppInfo> &appInfo)
{
    const auto onSetup = [runControl, appInfo] {
        const IosDeviceTypeAspect::Data *data = runControl->aspectData<IosDeviceTypeAspect>();
        if (!data)
            return false;

        appInfo->bundlePath = data->bundleDirectory;
        appInfo->bundleIdentifier = getBundleIdentifier(appInfo->bundlePath);
        if (appInfo->bundleIdentifier.isEmpty()) {
            runControl->postMessage(Tr::tr("Failed to determine bundle identifier."), ErrorMessageFormat);
            return false;
        }
        runControl->postMessage(Tr::tr("Running \"%1\" on %2...")
            .arg(appInfo->bundlePath.toUserOutput(), runControl->device()->displayName()),
            NormalMessageFormat);
        appInfo->device = std::dynamic_pointer_cast<const IosDevice>(runControl->device());
        appInfo->arguments = ProcessArgs::splitArgs(runControl->commandLine().arguments(), OsTypeMac);
        return true;
    };
    return Sync(onSetup);
}

static GroupItem findApp(RunControl *runControl, const Storage<AppInfo> &appInfo)
{
    const auto onSetup = [appInfo](Process &process) {
        if (!appInfo->device)
            return SetupResult::StopWithSuccess; // don't block the following tasks
        process.setCommand({FilePath::fromString("/usr/bin/xcrun"),
                            {"devicectl",
                             "device",
                             "info",
                             "apps",
                             "--device",
                             appInfo->device->uniqueInternalDeviceId(),
                             "--quiet",
                             "--json-output",
                             "-"}});
        return SetupResult::Continue;
    };
    const auto onDone = [runControl, appInfo](const Process &process) {
        if (process.error() != QProcess::UnknownError) {
            runControl->postMessage(Tr::tr("Failed to run devicectl: %1.").arg(process.errorString()),
                                    ErrorMessageFormat);
            return DoneResult::Error;
        }
        const Result<QUrl> pathOnDevice = parseAppInfo(process.rawStdOut(), appInfo->bundleIdentifier);
        if (pathOnDevice) {
            appInfo->pathOnDevice = *pathOnDevice;
            return DoneResult::Success;
        }
        runControl->postMessage(pathOnDevice.error(), ErrorMessageFormat);
        return DoneResult::Error;
    };
    return ProcessTask(onSetup, onDone);
}

static GroupItem findProcess(RunControl *runControl, const Storage<AppInfo> &appInfo)
{
    const auto onSetup = [appInfo](Process &process) {
        if (!appInfo->device || appInfo->pathOnDevice.isEmpty())
            return SetupResult::StopWithSuccess; // don't block the following tasks
        process.setCommand(
            {FilePath::fromString("/usr/bin/xcrun"),
             {"devicectl",
              "device",
              "info",
              "processes",
              "--device",
              appInfo->device->uniqueInternalDeviceId(),
              "--quiet",
              "--json-output",
              "-",
              "--filter",
              QLatin1String("executable.path BEGINSWITH '%1'").arg(appInfo->pathOnDevice.path())}});
        return SetupResult::Continue;
    };
    const auto onDone = [runControl, appInfo](const Process &process) {
        const Utils::Result<qint64> pid = parseProcessIdentifier(process.rawStdOut());
        if (pid) {
            appInfo->processIdentifier = *pid;
            return DoneResult::Success;
        }
        runControl->postMessage(pid.error(), ErrorMessageFormat);
        return DoneResult::Error;
    };
    return ProcessTask(onSetup, onDone);
}

static GroupItem killProcess(const Storage<AppInfo> &appInfo)
{
    const auto onSetup = [appInfo](Process &process) {
        if (!appInfo->device || appInfo->processIdentifier < 0)
            return SetupResult::StopWithSuccess; // don't block the following tasks
        process.setCommand({FilePath::fromString("/usr/bin/xcrun"),
                            {"devicectl",
                             "device",
                             "process",
                             "signal",
                             "--device",
                             appInfo->device->uniqueInternalDeviceId(),
                             "--quiet",
                             "--json-output",
                             "-",
                             "--signal",
                             "SIGKILL",
                             "--pid",
                             QString::number(appInfo->processIdentifier)}});
        return SetupResult::Continue;
    };
    return ProcessTask(onSetup, DoneResult::Success); // we tried our best and don't care at this point
}

GroupItem DeviceCtlPollingRunner::launchTask(const Storage<AppInfo> &appInfo)
{
    const auto onSetup = [this, appInfo](Process &process) {
        if (!m_device) {
            reportFailure(Tr::tr("Running failed. No iOS device found."));
            return SetupResult::StopWithError;
        }
        process.setCommand({FilePath::fromString("/usr/bin/xcrun"),
                            {"devicectl",
                             "device",
                             "process",
                             "launch",
                             "--device",
                             m_device->uniqueInternalDeviceId(),
                             "--quiet",
                             "--json-output",
                             "-",
                             appInfo->bundleIdentifier,
                             appInfo->arguments}});
        return SetupResult::Continue;
    };
    const auto onDone = [this](const Process &process, DoneWith result) {
        if (result == DoneWith::Cancel) {
            reportFailure(Tr::tr("Running canceled."));
            return DoneResult::Error;
        }
        if (process.error() != QProcess::UnknownError) {
            reportFailure(Tr::tr("Failed to run devicectl: %1.").arg(process.errorString()));
            return DoneResult::Error;
        }
        const Utils::Result<qint64> pid = parseLaunchResult(process.rawStdOut());
        if (pid) {
            m_processIdentifier = *pid;
            runControl()->setAttachPid(ProcessHandle(m_processIdentifier));
            m_pollTimer.start();
            reportStarted();
            return DoneResult::Success;
        }
        // failure
        reportFailure(pid.error());
        return DoneResult::Error;
    };
    return ProcessTask(onSetup, onDone);
}

void DeviceCtlRunnerBase::reportStoppedImpl()
{
    appendMessage(Tr::tr("\"%1\" exited.").arg(m_bundlePath.toUserOutput()),
                  Utils::NormalMessageFormat);
    reportStopped();
}

void DeviceCtlRunnerBase::start()
{
    // If the app is already running, we should first kill it, then launch again.
    // Usually deployment already kills the running app, but we support running without
    // deployment. Restarting is then e.g. needed if the app arguments changed.
    // Unfortunately the information about running processes only includes the path
    // on device and processIdentifier.
    // So we find out if the app is installed, and its path on device.
    // Check if a process is running for that path, and get its processIdentifier.
    // Try to kill that.
    // Then launch the app (again).
    const Storage<AppInfo> appInfo;
    m_startTask.reset(new TaskTree(Group{
        sequential,
        appInfo,
        initSetup(runControl(), appInfo),
        findApp(runControl(), appInfo),
        findProcess(runControl(), appInfo),
        killProcess(appInfo),
        launchTask(appInfo)}));
    m_startTask->start();
}

DeviceCtlPollingRunner::DeviceCtlPollingRunner(RunControl *runControl)
    : DeviceCtlRunnerBase(runControl)
{
    using namespace std::chrono_literals;
    m_pollTimer.setInterval(500ms); // not too often since running devicectl takes time
    connect(&m_pollTimer, &QTimer::timeout, this, &DeviceCtlPollingRunner::checkProcess);
}

void DeviceCtlPollingRunner::stop()
{
    // stop polling, we handle the reportStopped in the done handler
    m_pollTimer.stop();
    if (m_pollTask)
        m_pollTask.release()->deleteLater();
    const auto onSetup = [this](Process &process) {
        if (!m_device) {
            reportStoppedImpl();
            return SetupResult::StopWithError;
        }
        process.setCommand({FilePath::fromString("/usr/bin/xcrun"),
                            {"devicectl",
                             "device",
                             "process",
                             "signal",
                             "--device",
                             m_device->uniqueInternalDeviceId(),
                             "--quiet",
                             "--json-output",
                             "-",
                             "--signal",
                             "SIGKILL",
                             "--pid",
                             QString::number(m_processIdentifier)}});
        return SetupResult::Continue;
    };
    const auto onDone = [this](const Process &process) {
        if (process.error() != QProcess::UnknownError) {
            reportFailure(Tr::tr("Failed to run devicectl: %1.").arg(process.errorString()));
            return DoneResult::Error;
        }
        const Utils::Result<QJsonValue> resultValue = parseDevicectlResult(
            process.rawStdOut());
        if (!resultValue) {
            reportFailure(resultValue.error());
            return DoneResult::Error;
        }
        reportStoppedImpl();
        return DoneResult::Success;
    };
    m_stopTask.reset(new TaskTree(Group{ProcessTask(onSetup, onDone)}));
    m_stopTask->start();
}

void DeviceCtlPollingRunner::checkProcess()
{
    if (m_pollTask)
        return;
    const auto onSetup = [this](Process &process) {
        if (!m_device)
            return SetupResult::StopWithError;
        process.setCommand(
            {FilePath::fromString("/usr/bin/xcrun"),
             {"devicectl",
              "device",
              "info",
              "processes",
              "--device",
              m_device->uniqueInternalDeviceId(),
              "--quiet",
              "--json-output",
              "-",
              "--filter",
              QLatin1String("processIdentifier == %1").arg(QString::number(m_processIdentifier))}});
        return SetupResult::Continue;
    };
    const auto onDone = [this](const Process &process) {
        const Utils::Result<QJsonValue> resultValue = parseDevicectlResult(
            process.rawStdOut());
        if (!resultValue || (*resultValue)["runningProcesses"].toArray().size() < 1) {
            // no process with processIdentifier found, or some error occurred, device disconnected
            // or such, assume "stopped"
            m_pollTimer.stop();
            reportStoppedImpl();
        }
        if (m_pollTask)
            m_pollTask.release()->deleteLater();
        return DoneResult::Success;
    };
    m_pollTask.reset(new TaskTree(Group{ProcessTask(onSetup, onDone)}));
    m_pollTask->start();
}

DeviceCtlRunner::DeviceCtlRunner(RunControl *runControl)
    : DeviceCtlRunnerBase(runControl)
{}

void DeviceCtlRunner::stop()
{
    if (m_process.isRunning())
        m_process.stop();
}

GroupItem DeviceCtlRunner::launchTask(const Storage<AppInfo> &appInfo)
{
    const auto onSetup = [this, appInfo] {
        if (!m_device) {
            reportFailure(Tr::tr("Running failed. No iOS device found."));
            return false;
        }
        m_deviceCtlOutput.reset(new TemporaryFile("devicectl"));
        if (!m_deviceCtlOutput->open() || m_deviceCtlOutput->fileName().isEmpty()) {
            reportFailure(Tr::tr("Running failed. Failed to create the temporary output file."));
            return false;
        }
        const QStringList startStoppedArg = m_startStopped ? QStringList("--start-stopped")
                                                           : QStringList();
        const QStringList args = QStringList(
                                     {"devicectl",
                                      "device",
                                      "process",
                                      "launch",
                                      "--device",
                                      appInfo->device->uniqueInternalDeviceId(),
                                      "--quiet",
                                      "--json-output",
                                      m_deviceCtlOutput->fileName()})
                                 + startStoppedArg
                                 + QStringList({"--console", appInfo->bundleIdentifier})
                                 + appInfo->arguments;
        m_process.setCommand({FilePath::fromString("/usr/bin/xcrun"), args});
        connect(&m_process, &Process::started, this, [this, appInfoCopy = *appInfo] {
            // devicectl does report the process ID in its json output, but that is broken
            // for --console. When that is used, the json output is only written after the process
            // finished, which is not helpful.
            // Manually find the process ID for the bundle identifier.
            const Storage<AppInfo> appInfo{appInfoCopy};
            m_processIdTask.reset(new TaskTree(Group{
                sequential,
                appInfo,
                findApp(runControl(), appInfo),
                findProcess(runControl(), appInfo),
                onGroupDone([this, appInfo](DoneWith doneWith) {
                    if (doneWith == DoneWith::Success) {
                        runControl()->setAttachPid(ProcessHandle(appInfo->processIdentifier));
                        reportStarted();
                    } else {
                        reportFailure(Tr::tr("Failed to retrieve process ID."));
                    }
                })}));
            m_processIdTask->start();
        });
        connect(&m_process, &Process::done, this, [this] {
            if (m_process.error() != QProcess::UnknownError)
                reportFailure(Tr::tr("Failed to run devicectl: %1.").arg(m_process.errorString()));
            m_deviceCtlOutput->reset();
            m_processIdTask.reset();
            reportStoppedImpl();
        });
        connect(&m_process, &Process::readyReadStandardError, this, [this] {
            appendMessage(m_process.readAllStandardError(), StdErrFormat, false);
        });
        connect(&m_process, &Process::readyReadStandardOutput, this, [this] {
            appendMessage(m_process.readAllStandardOutput(), StdOutFormat, false);
        });

        m_process.start();
        return true;
    };
    return Sync(onSetup);
}

class IosRunner : public RunWorker
{
public:
    IosRunner(RunControl *runControl);
    ~IosRunner() override;

    void setCppDebugging(bool cppDebug);
    void setQmlDebugging(QmlDebugServicesPreset qmlDebugServices);

    void start() override;
    void stop() final;

    Port gdbServerPort() const;
    bool isAppRunning() const;

private:
    void handleGotServerPorts(Ios::IosToolHandler *handler, const FilePath &bundlePath,
                              const QString &deviceId, Port gdbPort, Port qmlPort);
    void handleGotInferiorPid(Ios::IosToolHandler *handler, const FilePath &bundlePath,
                              const QString &deviceId, qint64 pid);
    void handleAppOutput(Ios::IosToolHandler *handler, const QString &output);
    void handleMessage(const QString &msg);
    void handleErrorMsg(Ios::IosToolHandler *handler, const QString &msg);
    void handleToolExited(Ios::IosToolHandler *handler, int code);
    void handleFinished(Ios::IosToolHandler *handler);

    IosToolHandler *m_toolHandler = nullptr;
    FilePath m_bundleDir;
    IDeviceConstPtr m_device;
    IosDeviceType m_deviceType;
    bool m_cppDebug = false;
    QmlDebugServicesPreset m_qmlDebugServices = NoQmlDebugServices;

    bool m_cleanExit = false;
    Port m_gdbServerPort;
};

IosRunner::IosRunner(RunControl *runControl)
    : RunWorker(runControl)
{
    setId("IosRunner");
    stopRunningRunControl(runControl);
    const IosDeviceTypeAspect::Data *data = runControl->aspectData<IosDeviceTypeAspect>();
    QTC_ASSERT(data, return);
    m_bundleDir = data->bundleDirectory;
    m_device = RunDeviceKitAspect::device(runControl->kit());
    m_deviceType = data->deviceType;
}

IosRunner::~IosRunner()
{
    stop();
}

void IosRunner::setCppDebugging(bool cppDebug)
{
    m_cppDebug = cppDebug;
}

void IosRunner::setQmlDebugging(QmlDebugServicesPreset qmlDebugServices)
{
    m_qmlDebugServices = qmlDebugServices;
}

void IosRunner::start()
{
    if (m_toolHandler && isAppRunning())
        m_toolHandler->stop();

    m_cleanExit = false;
    if (!m_bundleDir.exists()) {
        TaskHub::addTask(DeploymentTask(Task::Warning,
            Tr::tr("Could not find %1.").arg(m_bundleDir.toUserOutput())));
        reportFailure();
        return;
    }

    m_toolHandler = new IosToolHandler(m_deviceType, this);
    connect(m_toolHandler, &IosToolHandler::appOutput,
            this, &IosRunner::handleAppOutput);
    connect(m_toolHandler, &IosToolHandler::message, this, &IosRunner::handleMessage);
    connect(m_toolHandler, &IosToolHandler::errorMsg, this, &IosRunner::handleErrorMsg);
    connect(m_toolHandler, &IosToolHandler::gotServerPorts,
            this, &IosRunner::handleGotServerPorts);
    connect(m_toolHandler, &IosToolHandler::gotInferiorPid,
            this, &IosRunner::handleGotInferiorPid);
    connect(m_toolHandler, &IosToolHandler::toolExited,
            this, &IosRunner::handleToolExited);
    connect(m_toolHandler, &IosToolHandler::finished,
            this, &IosRunner::handleFinished);

    const CommandLine command = runControl()->commandLine();
    QStringList args = ProcessArgs::splitArgs(command.arguments(), OsTypeMac);
    const Port portOnDevice = Port(runControl()->qmlChannel().port());
    if (portOnDevice.isValid()) {
        QUrl qmlServer;
        qmlServer.setPort(portOnDevice.number());
        args.append(qmlDebugTcpArguments(m_qmlDebugServices, qmlServer));
    }

    appendMessage(Tr::tr("Starting remote process."), NormalMessageFormat);
    QString deviceId;
    if (IosDevice::ConstPtr dev = std::dynamic_pointer_cast<const IosDevice>(m_device))
        deviceId = dev->uniqueDeviceID();
    const IosToolHandler::RunKind runKind = m_cppDebug ? IosToolHandler::DebugRun : IosToolHandler::NormalRun;
    m_toolHandler->requestRunApp(m_bundleDir, args, runKind, deviceId);
}

void IosRunner::stop()
{
    if (isAppRunning())
        m_toolHandler->stop();
}

void IosRunner::handleGotServerPorts(IosToolHandler *handler, const FilePath &bundlePath,
                                     const QString &deviceId, Port gdbPort,
                                     Port qmlPort)
{
    // Called when debugging on Device.
    Q_UNUSED(bundlePath)
    Q_UNUSED(deviceId)

    if (m_toolHandler != handler)
        return;

    m_gdbServerPort = gdbPort;
    // The run control so far knows about the port on the device side,
    // but the QML Profiler has to actually connect to a corresponding
    // local port. That port is reported here, so we need to adapt the runControl's
    // "qmlChannel", so the QmlProfilerRunner uses the right port.
    QUrl qmlChannel = runControl()->qmlChannel();
    const int qmlPortOnDevice = qmlChannel.port();
    qmlChannel.setPort(qmlPort.number());
    runControl()->setQmlChannel(qmlChannel);

    if (m_cppDebug) {
        if (!m_gdbServerPort.isValid()) {
            reportFailure(Tr::tr("Failed to get a local debugger port."));
            return;
        }
        appendMessage(
            Tr::tr("Listening for debugger on local port %1.").arg(m_gdbServerPort.number()),
            LogMessageFormat);
    }
    if (m_qmlDebugServices != NoQmlDebugServices) {
        if (!qmlPort.isValid()) {
            reportFailure(Tr::tr("Failed to get a local debugger port."));
            return;
        }
        appendMessage(
            Tr::tr("Listening for QML debugger on local port %1 (port %2 on the device).")
                .arg(qmlPort.number()).arg(qmlPortOnDevice),
            LogMessageFormat);
    }

    reportStarted();
}

void IosRunner::handleGotInferiorPid(IosToolHandler *handler, const FilePath &bundlePath,
                                     const QString &deviceId, qint64 pid)
{
    // Called when debugging on Simulator.
    Q_UNUSED(bundlePath)
    Q_UNUSED(deviceId)

    if (m_toolHandler != handler)
        return;

    if (pid <= 0) {
        reportFailure(Tr::tr("Could not get inferior PID."));
        return;
    }
    runControl()->setAttachPid(ProcessHandle(pid));

    if (m_qmlDebugServices != NoQmlDebugServices && runControl()->qmlChannel().port() == -1)
        reportFailure(Tr::tr("Could not get necessary ports for the debugger connection."));
    else
        reportStarted();
}

void IosRunner::handleAppOutput(IosToolHandler *handler, const QString &output)
{
    Q_UNUSED(handler)
    appendMessage(output, StdOutFormat);
}

void IosRunner::handleMessage(const QString &msg)
{
    appendMessage(msg, StdOutFormat);
}

void IosRunner::handleErrorMsg(IosToolHandler *handler, const QString &msg)
{
    Q_UNUSED(handler)
    QString res(msg);
    QString lockedErr ="Unexpected reply: ELocked (454c6f636b6564) vs OK (4f4b)";
    if (msg.contains("AMDeviceStartService returned -402653150")) {
        TaskHub::addTask(DeploymentTask(Task::Warning, Tr::tr("Run failed. "
           "The settings in the Organizer window of Xcode might be incorrect.")));
    } else if (res.contains(lockedErr)) {
        QString message = Tr::tr("The device is locked, please unlock.");
        TaskHub::addTask(DeploymentTask(Task::Error, message));
        res.replace(lockedErr, message);
    }
    appendMessage(res, StdErrFormat);
}

void IosRunner::handleToolExited(IosToolHandler *handler, int code)
{
    Q_UNUSED(handler)
    m_cleanExit = (code == 0);
}

void IosRunner::handleFinished(IosToolHandler *handler)
{
    if (m_toolHandler == handler) {
        if (m_cleanExit)
            appendMessage(Tr::tr("Run ended."), NormalMessageFormat);
        else
            appendMessage(Tr::tr("Run ended with error."), ErrorMessageFormat);
        m_toolHandler = nullptr;
    }
    handler->deleteLater();
    reportStopped();
}

bool IosRunner::isAppRunning() const
{
    return m_toolHandler && m_toolHandler->isRunning();
}

Port IosRunner::gdbServerPort() const
{
    return m_gdbServerPort;
}

static Result<FilePath> findDeviceSdk(IosDevice::ConstPtr dev)
{
    const QString osVersion = dev->osVersion();
    const QString productType = dev->productType();
    const QString cpuArchitecture = dev->cpuArchitecture();
    const FilePath home = FilePath::fromString(QDir::homePath());
    const FilePaths symbolsPathCandidates
        = {home / "Library/Developer/Xcode/iOS DeviceSupport" / (productType + " " + osVersion)
               / "Symbols",
           home / "Library/Developer/Xcode/iOS DeviceSupport" / (osVersion + " " + cpuArchitecture)
               / "Symbols",
           home / "Library/Developer/Xcode/iOS DeviceSupport" / osVersion / "Symbols",
           IosConfigurations::developerPath() / "Platforms/iPhoneOS.platform/DeviceSupport"
               / osVersion / "Symbols"};
    const FilePath deviceSdk = Utils::findOrDefault(symbolsPathCandidates, &FilePath::isDir);
    if (deviceSdk.isEmpty()) {
        return Utils::make_unexpected(
            Tr::tr("Could not find device specific debug symbols at %1. "
                   "Debugging initialization will be slow until you open the Organizer window of "
                   "Xcode with the device connected to have the symbols generated.")
                .arg(symbolsPathCandidates.constFirst().toUserOutput()));
    }
    return deviceSdk;
}

// Factories

IosRunWorkerFactory::IosRunWorkerFactory()
{
    setProducer([](RunControl *control) -> RunWorker * {
        IosDevice::ConstPtr iosdevice = std::dynamic_pointer_cast<const IosDevice>(control->device());
        if (iosdevice && iosdevice->handler() == IosDevice::Handler::DeviceCtl) {
            if (IosDeviceManager::isDeviceCtlOutputSupported())
                return new DeviceCtlRunner(control);
            // TODO Remove the polling runner when we decide not to support iOS 17+ devices
            // with Xcode < 16 at all
            return new DeviceCtlPollingRunner(control);
        }
        control->setIcon(Icons::RUN_SMALL_TOOLBAR);
        control->setDisplayName(QString("Run on %1")
                                       .arg(iosdevice ? iosdevice->displayName() : QString()));
        return new IosRunner(control);
    });
    addSupportedRunMode(ProjectExplorer::Constants::NORMAL_RUN_MODE);
    addSupportedRunConfig(Constants::IOS_RUNCONFIG_ID);
}

static void startDebugger(RunControl *runControl, DebuggerRunTool *debugger, IosRunner *iosRunner)
{
    if (!iosRunner->isAppRunning()) {
        debugger->reportFailure(Tr::tr("Application not running."));
        return;
    }

    DebuggerRunParameters &rp = debugger->runParameters();
    const bool cppDebug = rp.isCppDebugging();
    const bool qmlDebug = rp.isQmlDebugging();
    if (cppDebug) {
        const IosDeviceTypeAspect::Data *data = runControl->aspectData<IosDeviceTypeAspect>();
        rp.setInferiorExecutable(data->localExecutable);
        QUrl channel;
        channel.setScheme("connect");
        channel.setHost("localhost");
        channel.setPort(iosRunner->gdbServerPort().number());
        rp.setRemoteChannel(channel);

        QString bundlePath = data->bundleDirectory.toUrlishString();
        bundlePath.chop(4);
        const FilePath dsymPath = FilePath::fromString(bundlePath.append(".dSYM"));
        if (dsymPath.exists()
            && dsymPath.lastModified() < data->localExecutable.lastModified()) {
            TaskHub::addTask(DeploymentTask(Task::Warning,
                                            Tr::tr("The dSYM %1 seems to be outdated, it might confuse the debugger.")
                                                .arg(dsymPath.toUserOutput())));
        }
    }

    if (qmlDebug) {
        QTcpServer server;
        const bool isListening = server.listen(QHostAddress::LocalHost)
                                 || server.listen(QHostAddress::LocalHostIPv6);
        QTC_ASSERT(isListening, return);
        QUrl qmlServer;
        qmlServer.setHost(server.serverAddress().toString());
        if (!cppDebug)
            rp.setStartMode(AttachToRemoteServer);
        qmlServer.setPort(runControl->qmlChannel().port());
        rp.setQmlServer(qmlServer);
    }
}

static RunWorker *createWorker(RunControl *runControl)
{
    DebuggerRunTool *debugger = new DebuggerRunTool(runControl);
    debugger->setId("IosDebugSupport");

    IosDevice::ConstPtr dev = std::dynamic_pointer_cast<const IosDevice>(runControl->device());
    const bool isIosDeviceType = runControl->device()->type() == Ios::Constants::IOS_DEVICE_TYPE;
    const bool isIosDeviceInstance = bool(dev);
    // type info and device class must match
    QTC_ASSERT(isIosDeviceInstance == isIosDeviceType,
               runControl->postMessage(Tr::tr("Internal error."), ErrorMessageFormat); return nullptr);
    DebuggerRunParameters &rp = debugger->runParameters();
    // TODO cannot use setupPortsGatherer() from DebuggerRunTool, because that also requests
    // the "debugChannel", which then results in runControl trying to retrieve ports&URL for that
    // via IDevice, which doesn't really work with the iOS setup, and also completely changes
    // how the DebuggerRunTool works, breaking debugging on iOS <= 16 devices.
    if (rp.isQmlDebugging())
        runControl->requestQmlChannel();

    IosRunner *iosRunner = nullptr;
    DeviceCtlRunner *deviceCtlRunner = nullptr;
    RunWorker *runner = nullptr;
    if (!isIosDeviceInstance /*== simulator */ || dev->handler() == IosDevice::Handler::IosTool) {
        runner = iosRunner = new IosRunner(runControl);
        iosRunner->setCppDebugging(rp.isCppDebugging());
        iosRunner->setQmlDebugging(rp.isQmlDebugging() ? QmlDebuggerServices : NoQmlDebugServices);
    } else {
        QTC_CHECK(rp.isCppDebugging());
        runner = deviceCtlRunner = new DeviceCtlRunner(runControl);
        deviceCtlRunner->setStartStopped(true);
    }
    debugger->addStartDependency(runner);

    if (isIosDeviceInstance) {
        if (dev->handler() == IosDevice::Handler::DeviceCtl) {
            QTC_CHECK(IosDeviceManager::isDeviceCtlDebugSupported());
            rp.setStartMode(AttachToIosDevice);
            rp.setDeviceUuid(dev->uniqueInternalDeviceId());
        } else {
            rp.setStartMode(AttachToRemoteProcess);
        }
        rp.setLldbPlatform("remote-ios");
        const Result<FilePath> deviceSdk = findDeviceSdk(dev);

        if (!deviceSdk)
            TaskHub::addTask(DeploymentTask(Task::Warning, deviceSdk.error()));
        else
            rp.setDeviceSymbolsRoot(deviceSdk->path());
    } else {
        rp.setStartMode(AttachToLocalProcess);
        rp.setLldbPlatform("ios-simulator");
    }

    const IosDeviceTypeAspect::Data *data = runControl->aspectData<IosDeviceTypeAspect>();
    QTC_ASSERT(data, runControl->postMessage("Broken IosDeviceTypeAspect setup.", ErrorMessageFormat);
               return nullptr);
    rp.setDisplayName(data->applicationName);
    rp.setContinueAfterAttach(true);

    if (isIosDeviceInstance && dev->handler() == IosDevice::Handler::DeviceCtl) {
        const auto msgOnlyCppDebuggingSupported = [] {
            return Tr::tr("Only C++ debugging is supported for devices with iOS 17 and later.");
        };
        if (!rp.isCppDebugging()) {
            // TODO: The message is not shown currently, fix me before 17.0.
            runControl->postMessage(msgOnlyCppDebuggingSupported(), ErrorMessageFormat);
            return nullptr;
        }
        if (rp.isQmlDebugging()) {
            rp.setQmlDebugging(false);
            // TODO: The message is not shown currently, fix me before 17.0.
            runControl->postMessage(msgOnlyCppDebuggingSupported(), LogMessageFormat);
        }
        rp.setInferiorExecutable(data->localExecutable);
    } else {
        QObject::connect(runner, &RunWorker::started, debugger, [runControl, debugger, iosRunner] {
            startDebugger(runControl, debugger, iosRunner);
        });
    }
    return debugger;
}

IosDebugWorkerFactory::IosDebugWorkerFactory()
{
    setProducer([](RunControl *runControl) { return createWorker(runControl); });
    addSupportedRunMode(ProjectExplorer::Constants::DEBUG_RUN_MODE);
    addSupportedRunConfig(Constants::IOS_RUNCONFIG_ID);
}

IosQmlProfilerWorkerFactory::IosQmlProfilerWorkerFactory()
{
    setProducer([](RunControl *runControl) {
        auto runner = new IosRunner(runControl);
        runner->setQmlDebugging(QmlProfilerServices);

        auto profiler = runControl->createWorker(ProjectExplorer::Constants::QML_PROFILER_RUNNER);
        profiler->addStartDependency(runner);
        return profiler;
    });
    addSupportedRunMode(ProjectExplorer::Constants::QML_PROFILER_RUN_MODE);
    addSupportedRunConfig(Constants::IOS_RUNCONFIG_ID);
}

} // Ios::Internal
