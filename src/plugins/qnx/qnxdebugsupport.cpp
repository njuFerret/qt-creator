// Copyright (C) 2016 BlackBerry Limited. All rights reserved.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "qnxdebugsupport.h"

#include "qnxconstants.h"
#include "qnxqtversion.h"
#include "qnxtr.h"
#include "slog2inforunner.h"

#include <coreplugin/icore.h>

#include <debugger/debuggerkitaspect.h>
#include <debugger/debuggerruncontrol.h>
#include <debugger/debuggertr.h>

#include <projectexplorer/devicesupport/deviceprocessesdialog.h>
#include <projectexplorer/devicesupport/deviceusedportsgatherer.h>
#include <projectexplorer/devicesupport/idevice.h>
#include <projectexplorer/kit.h>
#include <projectexplorer/kitaspects.h>
#include <projectexplorer/kitchooser.h>
#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/projectmanager.h>
#include <projectexplorer/qmldebugcommandlinearguments.h>
#include <projectexplorer/runconfigurationaspects.h>
#include <projectexplorer/target.h>
#include <projectexplorer/toolchain.h>

#include <qtsupport/qtkitaspect.h>

#include <utils/fileutils.h>
#include <utils/pathchooser.h>
#include <utils/portlist.h>
#include <utils/qtcprocess.h>
#include <utils/processinfo.h>
#include <utils/qtcassert.h>

#include <QDir>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

using namespace Debugger;
using namespace ProjectExplorer;
using namespace Utils;

namespace Qnx::Internal {

const char QNX_DEBUG_EXECUTABLE[] = "pdebug";

static QStringList searchPaths(Kit *kit)
{
    auto qtVersion = dynamic_cast<QnxQtVersion *>(QtSupport::QtKitAspect::qtVersion(kit));
    if (!qtVersion)
        return {};

    const QDir pluginDir(qtVersion->pluginPath().toString());
    const QStringList pluginSubDirs = pluginDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    QStringList searchPaths;

    for (const QString &dir : pluginSubDirs)
        searchPaths << qtVersion->pluginPath().toString() + '/' + dir;

    searchPaths << qtVersion->libraryPath().toString();
    searchPaths << qtVersion->qnxTarget().pathAppended(qtVersion->cpuDir() + "/lib").toString();
    searchPaths << qtVersion->qnxTarget().pathAppended(qtVersion->cpuDir() + "/usr/lib").toString();

    return searchPaths;
}

// QnxDebuggeeRunner

class QnxDebuggeeRunner : public ProjectExplorer::SimpleTargetRunner
{
public:
    explicit QnxDebuggeeRunner(RunControl *runControl)
        : SimpleTargetRunner(runControl)
    {
        setId("QnxDebuggeeRunner");

        setStartModifier([this] {
            CommandLine cmd = commandLine();
            QStringList arguments;
            if (usesDebugChannel()) {
                int pdebugPort = debugChannel().port();
                cmd.setExecutable(device()->filePath(QNX_DEBUG_EXECUTABLE));
                arguments.append(QString::number(pdebugPort));
            }
            if (usesQmlChannel()) {
                arguments.append(qmlDebugTcpArguments(QmlDebuggerServices, qmlChannel()));
            }
            cmd.setArguments(ProcessArgs::joinArgs(arguments));
            setCommandLine(cmd);
        });
    }
};


// QnxDebugSupport

class QnxDebugSupport : public Debugger::DebuggerRunTool
{
public:
    explicit QnxDebugSupport(ProjectExplorer::RunControl *runControl)
        : DebuggerRunTool(runControl)
    {
        setId("QnxDebugSupport");
        appendMessage(Tr::tr("Preparing remote side..."), LogMessageFormat);

        setUsePortsGatherer(isCppDebugging(), isQmlDebugging());

        auto debuggeeRunner = new QnxDebuggeeRunner(runControl);

        auto slog2InfoRunner = new Slog2InfoRunner(runControl);
        debuggeeRunner->addStartDependency(slog2InfoRunner);

        addStartDependency(debuggeeRunner);

        Kit *k = runControl->kit();

        setStartMode(AttachToRemoteServer);
        setCloseMode(KillAtClose);
        setUseCtrlCStub(true);
        setSolibSearchPath(FileUtils::toFilePathList(searchPaths(k)));
        if (auto qtVersion = dynamic_cast<QnxQtVersion *>(QtSupport::QtKitAspect::qtVersion(k))) {
            setSysRoot(qtVersion->qnxTarget());
            modifyDebuggerEnvironment(qtVersion->environment());
        }
    }
};


// QnxAttachDebugDialog

class QnxAttachDebugDialog : public DeviceProcessesDialog
{
public:
    QnxAttachDebugDialog(KitChooser *kitChooser)
        : DeviceProcessesDialog(kitChooser, Core::ICore::dialogParent())
    {
        auto sourceLabel = new QLabel(Tr::tr("Project source directory:"), this);
        m_projectSource = new PathChooser(this);
        m_projectSource->setExpectedKind(PathChooser::ExistingDirectory);

        auto binaryLabel = new QLabel(Tr::tr("Local executable:"), this);
        m_localExecutable = new PathChooser(this);
        m_localExecutable->setExpectedKind(PathChooser::File);

        auto formLayout = new QFormLayout;
        formLayout->addRow(sourceLabel, m_projectSource);
        formLayout->addRow(binaryLabel, m_localExecutable);

        auto mainLayout = dynamic_cast<QVBoxLayout*>(layout());
        QTC_ASSERT(mainLayout, return);
        mainLayout->insertLayout(mainLayout->count() - 2, formLayout);
    }

    QString projectSource() const { return m_projectSource->filePath().toString(); }
    FilePath localExecutable() const { return m_localExecutable->filePath(); }

private:
    PathChooser *m_projectSource;
    PathChooser *m_localExecutable;
};


// QnxAttachDebugSupport

class PDebugRunner : public ProjectExplorer::SimpleTargetRunner
{
public:
    PDebugRunner(RunControl *runControl, DebuggerRunTool *debugger)
        : SimpleTargetRunner(runControl)
    {
        setId("PDebugRunner");

        setStartModifier([this, debugger] {
            const int pdebugPort = debugger->debugChannel().port();
            setCommandLine({QNX_DEBUG_EXECUTABLE, {QString::number(pdebugPort)}});
        });
    }
};

class QnxAttachDebugSupport : public Debugger::DebuggerRunTool
{
public:
    explicit QnxAttachDebugSupport(ProjectExplorer::RunControl *runControl)
        : DebuggerRunTool(runControl)
    {
        setId("QnxAttachDebugSupport");
        setUsePortsGatherer(isCppDebugging(), isQmlDebugging());
        setUseCtrlCStub(true);

        if (isCppDebugging()) {
            auto pdebugRunner = new PDebugRunner(runControl, this);
            addStartDependency(pdebugRunner);
        }
    }
};

void showAttachToProcessDialog()
{
    auto kitChooser = new KitChooser;
    kitChooser->setKitPredicate([](const Kit *k) {
        return k->isValid() && DeviceTypeKitAspect::deviceTypeId(k) == Constants::QNX_QNX_OS_TYPE;
    });

    QnxAttachDebugDialog dlg(kitChooser);
    dlg.addAcceptButton(::Debugger::Tr::tr("&Attach to Process"));
    dlg.showAllDevices();
    if (dlg.exec() == QDialog::Rejected)
        return;

    Kit *kit = kitChooser->currentKit();
    if (!kit)
        return;

    // FIXME: That should be somehow related to the selected kit.
    auto runConfig = ProjectManager::startupRunConfiguration();

    const int pid = dlg.currentProcess().processId;
//    QString projectSourceDirectory = dlg.projectSource();
    FilePath localExecutable = dlg.localExecutable();
    if (localExecutable.isEmpty()) {
        if (auto aspect = runConfig->aspect<SymbolFileAspect>())
            localExecutable = aspect->expandedValue();
    }

    auto runControl = new RunControl(ProjectExplorer::Constants::DEBUG_RUN_MODE);
    runControl->copyDataFromRunConfiguration(runConfig);
    auto debugger = new QnxAttachDebugSupport(runControl);
    debugger->setStartMode(AttachToRemoteServer);
    debugger->setCloseMode(DetachAtClose);
    debugger->setSymbolFile(localExecutable);
    debugger->setAttachPid(pid);
//    setRunControlName(Tr::tr("Remote: \"%1\" - Process %2").arg(remoteChannel).arg(m_process.pid));
    debugger->setRunControlName(Tr::tr("Remote QNX process %1").arg(pid));
    debugger->setSolibSearchPath(FileUtils::toFilePathList(searchPaths(kit)));
    if (auto qtVersion = dynamic_cast<QnxQtVersion *>(QtSupport::QtKitAspect::qtVersion(kit)))
        debugger->setSysRoot(qtVersion->qnxTarget());
    debugger->setUseContinueInsteadOfRun(true);

    ProjectExplorerPlugin::startRunControl(runControl);
}

// QnxDebugWorkerFactory

class QnxDebugWorkerFactory final : public RunWorkerFactory
{
public:
    QnxDebugWorkerFactory()
    {
        setProduct<QnxDebugSupport>();
        addSupportedRunMode(ProjectExplorer::Constants::DEBUG_RUN_MODE);
        addSupportedRunConfig(Constants::QNX_RUNCONFIG_ID);
    }
};

void setupQnxDebugging()
{
    static QnxDebugWorkerFactory theQnxDebugWorkerFactory;
}

} // Qnx::Internal
