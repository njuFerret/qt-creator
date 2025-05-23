// Copyright (C) 2016 Dmitry Savchenko
// Copyright (C) 2016 Vasiliy Sorokin
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "todoitemsprovider.h"

#include "constants.h"
#include "cpptodoitemsscanner.h"
#include "qmljstodoitemsscanner.h"
#include "todoitemsmodel.h"
#include "todoitemsscanner.h"

#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/idocument.h>

#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/projectmanager.h>
#include <projectexplorer/projectnodes.h>
#include <projectexplorer/projecttree.h>

#include <utils/algorithm.h>

#include <QRegularExpression>
#include <QTimer>

using namespace ProjectExplorer;
using namespace Utils;

namespace Todo::Internal {

TodoItemsProvider::TodoItemsProvider(QObject *parent) :
    QObject(parent),
    m_settings(todoSettings())
{
    setupItemsModel();
    setupStartupProjectBinding();
    setupCurrentEditorBinding();
    setupUpdateListTimer();
    createScanners();
}

TodoItemsModel *TodoItemsProvider::todoItemsModel()
{
    return m_itemsModel;
}

void TodoItemsProvider::settingsChanged()
{
    if (todoSettings().keywords != m_settings.keywords) {
        for (TodoItemsScanner *scanner : std::as_const(m_scanners))
            scanner->setParams(todoSettings().keywords);
    }

    m_settings = todoSettings();

    updateList();
}

void TodoItemsProvider::projectSettingsChanged(Project *project)
{
    Q_UNUSED(project)
    updateList();
}

void TodoItemsProvider::updateList()
{
    m_itemsList.clear();

    // Show only items of the current file if any
    if (m_settings.scanningScope == ScanningScopeCurrentFile) {
        if (auto currentEditor = Core::EditorManager::currentEditor())
            m_itemsList = m_itemsHash.value(currentEditor->document()->filePath());
    // Show only items of the current sub-project
    } else if (m_settings.scanningScope == ScanningScopeSubProject) {
        if (m_startupProject)
            setItemsListWithinSubproject();
    // Show only items of the startup project if any
    } else if (m_startupProject) {
        setItemsListWithinStartupProject();
    }

    m_itemsModel->todoItemsListUpdated();
}

void TodoItemsProvider::createScanners()
{
    qRegisterMetaType<QList<TodoItem> >("QList<TodoItem>");

    if (CppEditor::CppModelManager::instance())
        m_scanners << new CppTodoItemsScanner(m_settings.keywords, this);

    if (QmlJS::ModelManagerInterface::instance())
        m_scanners << new QmlJsTodoItemsScanner(m_settings.keywords, this);

    for (TodoItemsScanner *scanner : std::as_const(m_scanners)) {
        connect(scanner, &TodoItemsScanner::itemsFetched, this,
            &TodoItemsProvider::itemsFetched, Qt::QueuedConnection);
    }
}

void TodoItemsProvider::setItemsListWithinStartupProject()
{
    const auto filePaths = Utils::toSet(m_startupProject->files(Project::SourceFiles));

    QVariantMap settings = m_startupProject->namedSettings(Constants::SETTINGS_NAME_KEY).toMap();

    for (auto it = m_itemsHash.cbegin(), end = m_itemsHash.cend(); it != end; ++it) {
        const FilePath filePath = it.key();
        if (filePaths.contains(filePath)) {
            bool skip = false;
            for (const QVariant &pattern : settings[Constants::EXCLUDES_LIST_KEY].toList()) {
                QRegularExpression re(pattern.toString());
                if (filePath.toUrlishString().indexOf(re) != -1) {
                    skip = true;
                    break;
                }
            }
            if (!skip)
                m_itemsList << it.value();
        }
    }
}

void TodoItemsProvider::setItemsListWithinSubproject()
{
    // TODO prefer current editor as source of sub-project
    const Node *node = ProjectTree::currentNode();
    if (node) {
        ProjectNode *projectNode = node->parentProjectNode();
        if (projectNode) {
            // FIXME: The name doesn't match the implementation that lists all files.
            QSet<FilePath> subprojectFileNames;
            projectNode->forEachGenericNode([&](Node *node) {
                 subprojectFileNames.insert(node->filePath());
            });

            // files must be both in the current subproject and the startup-project.
            const auto fileNames = Utils::toSet(m_startupProject->files(Project::SourceFiles));
            for (auto it = m_itemsHash.cbegin(), end = m_itemsHash.cend(); it != end; ++it) {
                if (subprojectFileNames.contains(it.key()) && fileNames.contains(it.key()))
                    m_itemsList << it.value();
            }
        }
    }
}

void TodoItemsProvider::itemsFetched(const QString &fileName, const QList<TodoItem> &items)
{
    // Replace old items with new ones
    m_itemsHash.insert(FilePath::fromString(fileName), items);

    m_shouldUpdateList = true;
}

void TodoItemsProvider::startupProjectChanged(Project *project)
{
    m_startupProject = project;
    updateList();
}

void TodoItemsProvider::projectsFilesChanged()
{
    updateList();
}

void TodoItemsProvider::currentEditorChanged()
{
    if (m_settings.scanningScope == ScanningScopeCurrentFile
            || m_settings.scanningScope == ScanningScopeSubProject) {
        updateList();
    }
}

void TodoItemsProvider::updateListTimeoutElapsed()
{
    if (m_shouldUpdateList) {
        m_shouldUpdateList = false;
        updateList();
    }
}

void TodoItemsProvider::setupStartupProjectBinding()
{
    m_startupProject = ProjectManager::startupProject();
    connect(ProjectManager::instance(), &ProjectManager::startupProjectChanged,
        this, &TodoItemsProvider::startupProjectChanged);
    connect(ProjectExplorerPlugin::instance(), &ProjectExplorerPlugin::fileListChanged,
            this, &TodoItemsProvider::projectsFilesChanged);
}

void TodoItemsProvider::setupCurrentEditorBinding()
{
    connect(Core::EditorManager::instance(), &Core::EditorManager::currentEditorChanged,
        this, &TodoItemsProvider::currentEditorChanged);
}

void TodoItemsProvider::setupUpdateListTimer()
{
    m_shouldUpdateList = false;
    QTimer *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &TodoItemsProvider::updateListTimeoutElapsed);
    timer->start(Constants::OUTPUT_PANE_UPDATE_INTERVAL);
}

void TodoItemsProvider::setupItemsModel()
{
    m_itemsModel = new TodoItemsModel(this);
    m_itemsModel->setTodoItemsList(&m_itemsList);
}

static TodoItemsProvider *s_instance = nullptr;

TodoItemsProvider &todoItemsProvider()
{
    return *s_instance;
}

void setupTodoItemsProvider(QObject *guard)
{
    s_instance = new TodoItemsProvider(guard);
}

} // Internal
