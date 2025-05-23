// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "propertyeditorcontextobject.h"

#include "abstractview.h"
#include "compatibleproperties.h"
#include "easingcurvedialog.h"
#include "nodemetainfo.h"
#include "propertyeditorutils.h"
#include "qml3dnode.h"
#include "qmldesignerconstants.h"
#include "qmldesignerplugin.h"
#include "qmlmodelnodeproxy.h"
#include "qmlobjectnode.h"
#include "qmltimeline.h"

#include <qmldesignerbase/settings/designersettings.h>

#include <coreplugin/messagebox.h>
#include <utils/algorithm.h>
#include <utils/qtcassert.h>

#include <QApplication>
#include <QCursor>
#include <QFontDatabase>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QQmlContext>
#include <QQuickWidget>
#include <QWindow>

#include <coreplugin/icore.h>

namespace QmlDesigner {

static Q_LOGGING_CATEGORY(urlSpecifics, "qtc.propertyeditor.specifics", QtWarningMsg)

PropertyEditorContextObject::PropertyEditorContextObject(QQuickWidget *widget, QObject *parent)
    : QObject(parent)
    , m_isBaseState(false)
    , m_selectionChanged(false)
    , m_backendValues(nullptr)
    , m_qmlComponent(nullptr)
    , m_qmlContext(nullptr)
    , m_quickWidget(widget)
{}

QString PropertyEditorContextObject::convertColorToString(const QVariant &color)
{
    QString colorString;
    QColor theColor;
    if (color.canConvert(QMetaType(QMetaType::QColor))) {
        theColor = color.value<QColor>();
    } else if (color.canConvert(QMetaType(QMetaType::QVector3D))) {
        auto vec = color.value<QVector3D>();
        theColor = QColor::fromRgbF(vec.x(), vec.y(), vec.z());
    }

    colorString = theColor.name();

    if (theColor.alpha() != 255) {
        QString hexAlpha = QString("%1").arg(theColor.alpha(), 2, 16, QLatin1Char('0'));
        colorString.remove(0, 1);
        colorString.prepend(hexAlpha);
        colorString.prepend(QStringLiteral("#"));
    }
    return colorString;
}

QColor PropertyEditorContextObject::colorFromString(const QString &colorString)
{
    return QColor::fromString(colorString);
}

QString PropertyEditorContextObject::translateFunction()
{
    if (QmlDesignerPlugin::instance()->settings().value(
            DesignerSettingsKey::TYPE_OF_QSTR_FUNCTION).toInt())

        switch (QmlDesignerPlugin::instance()->settings().value(
                    DesignerSettingsKey::TYPE_OF_QSTR_FUNCTION).toInt()) {
        case 0: return QLatin1String("qsTr");
        case 1: return QLatin1String("qsTrId");
        case 2: return QLatin1String("qsTranslate");
        default:
            break;
        }
    return QLatin1String("qsTr");
}

QStringList PropertyEditorContextObject::autoComplete(const QString &text, int pos, bool explicitComplete, bool filter)
{
    if (m_model && m_model->rewriterView())
        return  Utils::filtered(m_model->rewriterView()->autoComplete(text, pos, explicitComplete), [filter](const QString &string) {
            return !filter || (!string.isEmpty() && string.at(0).isUpper()); });

    return {};
}

void PropertyEditorContextObject::toggleExportAlias()
{
    QTC_ASSERT(m_model && m_model->rewriterView(), return);

    /* Ideally we should not missuse the rewriterView
     * If we add more code here we have to forward the property editor view */
    RewriterView *rewriterView = m_model->rewriterView();

    QTC_ASSERT(!rewriterView->selectedModelNodes().isEmpty(), return);

    const ModelNode selectedNode = rewriterView->selectedModelNodes().constFirst();

    if (QmlObjectNode::isValidQmlObjectNode(selectedNode)) {
        QmlObjectNode objectNode(selectedNode);

        PropertyName modelNodeId = selectedNode.id().toUtf8();
        ModelNode rootModelNode = rewriterView->rootModelNode();

        rewriterView->executeInTransaction("PropertyEditorContextObject:toggleExportAlias",
                                           [&objectNode, &rootModelNode, modelNodeId]() {
                                               if (!objectNode.isAliasExported())
                                                   objectNode.ensureAliasExport();
                                               else if (rootModelNode.hasProperty(modelNodeId))
                                                   rootModelNode.removeProperty(modelNodeId);
                                           });
    }
}

void PropertyEditorContextObject::goIntoComponent()
{
    QTC_ASSERT(m_model && m_model->rewriterView(), return);

    /* Ideally we should not missuse the rewriterView
     * If we add more code here we have to forward the property editor view */
    RewriterView *rewriterView = m_model->rewriterView();

    QTC_ASSERT(!rewriterView->selectedModelNodes().isEmpty(), return);

    const ModelNode selectedNode = rewriterView->selectedModelNodes().constFirst();

    DocumentManager::goIntoComponent(selectedNode);
}

void PropertyEditorContextObject::changeTypeName(const QString &typeName)
{
    QTC_ASSERT(m_model && m_model->rewriterView(), return);

    /* Ideally we should not missuse the rewriterView
     * If we add more code here we have to forward the property editor view */
    RewriterView *rewriterView = m_model->rewriterView();

    QTC_ASSERT(!m_editorNodes.isEmpty(), return);

    auto changeNodeTypeName = [&](ModelNode &selectedNode) {
        // Check if the requested type is the same as already set
        if (selectedNode.simplifiedTypeName() == typeName)
            return;

        NodeMetaInfo metaInfo = m_model->metaInfo(typeName.toLatin1());
        if (!metaInfo.isValid()) {
            Core::AsynchronousMessageBox::warning(tr("Invalid Type"),
                                                  tr("%1 is an invalid type.").arg(typeName));
            return;
        }

        // Create a list of properties available for the new type
        auto propertiesAndSignals = Utils::transform<PropertyNameList>(
            PropertyEditorUtils::filteredProperties(metaInfo), &PropertyMetaInfo::name);
        // Add signals to the list

        const PropertyNameList &signalNames = metaInfo.signalNames();
        for (const PropertyName &signal : signalNames) {
            if (signal.isEmpty())
                continue;

            PropertyName name = signal;
            QChar firstChar = QChar(signal.at(0)).toUpper().toLatin1();
            name[0] = firstChar.toLatin1();
            name.prepend("on");
            propertiesAndSignals.append(name);
        }

        // Add dynamic properties and respective change signals
        const QList<AbstractProperty> &nodeProperties = selectedNode.properties();
        for (const AbstractProperty &property : nodeProperties) {
            if (!property.isDynamic())
                continue;

            // Add dynamic property
            propertiesAndSignals.append(property.name().toByteArray());
            // Add its change signal
            PropertyName name = property.name().toByteArray();
            QChar firstChar = QChar(property.name().at(0)).toUpper().toLatin1();
            name[0] = firstChar.toLatin1();
            name.prepend("on");
            name.append("Changed");
            propertiesAndSignals.append(name);
        }

        // Compare current properties and signals with the once available for change type
        QList<PropertyName> incompatibleProperties;
        for (const AbstractProperty &property : nodeProperties) {
            if (!propertiesAndSignals.contains(property.name()))
                incompatibleProperties.append(property.name().toByteArray());
        }

        CompatibleProperties compatibleProps(selectedNode.metaInfo(), metaInfo);
        compatibleProps.createCompatibilityMap(incompatibleProperties);

        Utils::sort(incompatibleProperties);

        // Create a dialog showing incompatible properties and signals
        if (!incompatibleProperties.empty()) {
            QString detailedText = QString("<b>Incompatible properties:</b><br>");

            for (const auto &p : std::as_const(incompatibleProperties))
                detailedText.append("- " + QString::fromUtf8(p) + "<br>");

            detailedText.chop(QString("<br>").size());

            QMessageBox msgBox;
            msgBox.setTextFormat(Qt::RichText);
            msgBox.setIcon(QMessageBox::Question);
            msgBox.setWindowTitle("Change Type");
            msgBox.setText(QString("Changing the type from %1 to %2 can't be done without removing "
                                   "incompatible properties.<br><br>%3")
                               .arg(selectedNode.simplifiedTypeName(), typeName, detailedText));
            msgBox.setInformativeText(
                "Do you want to continue by removing incompatible properties?");
            msgBox.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
            msgBox.setDefaultButton(QMessageBox::Ok);

            if (msgBox.exec() == QMessageBox::Cancel)
                return;

            for (const auto &p : std::as_const(incompatibleProperties))
                selectedNode.removeProperty(p);
        }

        compatibleProps.copyMappedProperties(selectedNode);
#ifdef QDS_USE_PROJECTSTORAGE
        if (selectedNode.isRootNode())
            rewriterView->changeRootNodeType(typeName.toUtf8(), -1, -1);
        else
            selectedNode.changeType(typeName.toUtf8(), -1, -1);
#else
        if (selectedNode.isRootNode())
            rewriterView->changeRootNodeType(metaInfo.typeName(),
                                             metaInfo.majorVersion(),
                                             metaInfo.minorVersion());
        else
            selectedNode.changeType(metaInfo.typeName(),
                                    metaInfo.majorVersion(),
                                    metaInfo.minorVersion());
#endif
        compatibleProps.applyCompatibleProperties(selectedNode);
    };

    try {
        auto transaction = RewriterTransaction(rewriterView, "PropertyEditorContextObject:changeTypeName");

        ModelNodes selectedNodes = m_editorNodes;
        for (ModelNode &selectedNode : selectedNodes)
            changeNodeTypeName(selectedNode);

        transaction.commit();
    } catch (const Exception &e) {
        e.showException();
    }
}

void PropertyEditorContextObject::insertKeyframe(const QString &propertyName)
{
    QTC_ASSERT(m_model && m_model->rewriterView(), return);

    if (isBlocked(propertyName))
        return;

    /* Ideally we should not missuse the rewriterView
     * If we add more code here we have to forward the property editor view */
    RewriterView *rewriterView = m_model->rewriterView();

    QTC_ASSERT(!rewriterView->selectedModelNodes().isEmpty(), return);

    ModelNode selectedNode = rewriterView->selectedModelNodes().constFirst();

    QmlTimeline timeline = rewriterView->currentTimelineNode();

    QTC_ASSERT(timeline.isValid(), return );
    QTC_ASSERT(selectedNode.isValid(), return );

    rewriterView->executeInTransaction("PropertyEditorContextObject::insertKeyframe", [&]{
        timeline.insertKeyframe(selectedNode, propertyName.toUtf8());
    });
}

QString PropertyEditorContextObject::activeDragSuffix() const
{
    return m_activeDragSuffix;
}

void PropertyEditorContextObject::setActiveDragSuffix(const QString &suffix)
{
    if (m_activeDragSuffix != suffix) {
        m_activeDragSuffix = suffix;
        emit activeDragSuffixChanged();
    }
}

int PropertyEditorContextObject::majorVersion() const
{
    return m_majorVersion;

}

int PropertyEditorContextObject::majorQtQuickVersion() const
{
    return m_majorQtQuickVersion;
}

int PropertyEditorContextObject::minorQtQuickVersion() const
{
    return m_minorQtQuickVersion;
}

void PropertyEditorContextObject::setMajorVersion(int majorVersion)
{
    if (m_majorVersion == majorVersion)
        return;

    m_majorVersion = majorVersion;

    emit majorVersionChanged();
}

void PropertyEditorContextObject::setMajorQtQuickVersion(int majorVersion)
{
    if (m_majorQtQuickVersion == majorVersion)
        return;

    m_majorQtQuickVersion = majorVersion;

    emit majorQtQuickVersionChanged();

}

void PropertyEditorContextObject::setMinorQtQuickVersion(int minorVersion)
{
    if (m_minorQtQuickVersion == minorVersion)
        return;

    m_minorQtQuickVersion = minorVersion;

    emit minorQtQuickVersionChanged();
}

int PropertyEditorContextObject::minorVersion() const
{
    return m_minorVersion;
}

void PropertyEditorContextObject::setMinorVersion(int minorVersion)
{
    if (m_minorVersion == minorVersion)
        return;

    m_minorVersion = minorVersion;

    emit minorVersionChanged();
}

bool PropertyEditorContextObject::hasActiveTimeline() const
{
    return m_setHasActiveTimeline;
}

void PropertyEditorContextObject::setHasActiveTimeline(bool b)
{
    if (b == m_setHasActiveTimeline)
        return;

    m_setHasActiveTimeline = b;
    emit hasActiveTimelineChanged();
}

void PropertyEditorContextObject::insertInQmlContext(QQmlContext *context)
{
    m_qmlContext = context;
    m_qmlContext->setContextObject(this);
}

QQmlComponent *PropertyEditorContextObject::specificQmlComponent()
{
    if (m_qmlComponent)
        return m_qmlComponent;

    m_qmlComponent = new QQmlComponent(m_qmlContext->engine(), this);

    m_qmlComponent->setData(m_specificQmlData.toUtf8(), QUrl::fromLocalFile("specifics.qml"));

    const bool showError = qEnvironmentVariableIsSet(Constants::ENVIRONMENT_SHOW_QML_ERRORS);
    if (showError && !m_specificQmlData.isEmpty() && !m_qmlComponent->errors().isEmpty()) {
        const QString errMsg = m_qmlComponent->errors().constFirst().toString();
        Core::AsynchronousMessageBox::warning(tr("Invalid QML source"), errMsg);
    }

    return m_qmlComponent;
}

bool PropertyEditorContextObject::hasMultiSelection() const
{
    return m_hasMultiSelection;
}

void PropertyEditorContextObject::setHasMultiSelection(bool b)
{
    if (b == m_hasMultiSelection)
        return;

    m_hasMultiSelection = b;
    emit hasMultiSelectionChanged();
}

bool PropertyEditorContextObject::isSelectionLocked() const
{
    return m_isSelectionLocked;
}

void PropertyEditorContextObject::setIsSelectionLocked(bool lock)
{
    if (lock == m_isSelectionLocked)
        return;

    m_isSelectionLocked = lock;
    emit isSelectionLockedChanged();
}

void PropertyEditorContextObject::setInsightEnabled(bool value)
{
    if (value != m_insightEnabled) {
        m_insightEnabled = value;
        emit insightEnabledChanged();
    }
}

void PropertyEditorContextObject::setInsightCategories(const QStringList &categories)
{
    m_insightCategories = categories;
    emit insightCategoriesChanged();
}

bool PropertyEditorContextObject::hasQuick3DImport() const
{
    return m_hasQuick3DImport;
}

void PropertyEditorContextObject::setEditorNodes(const ModelNodes &nodes)
{
    m_editorNodes = nodes;
}

void PropertyEditorContextObject::setHasQuick3DImport(bool value)
{
    if (value == m_hasQuick3DImport)
        return;

    m_hasQuick3DImport = value;
    emit hasQuick3DImportChanged();
}

bool PropertyEditorContextObject::hasMaterialLibrary() const
{
    return m_hasMaterialLibrary;
}

void PropertyEditorContextObject::setHasMaterialLibrary(bool value)
{
    if (value == m_hasMaterialLibrary)
        return;

    m_hasMaterialLibrary = value;
    emit hasMaterialLibraryChanged();
}

bool PropertyEditorContextObject::isQt6Project() const
{
    return m_isQt6Project;
}

void PropertyEditorContextObject::setIsQt6Project(bool value)
{
    if (m_isQt6Project == value)
        return;

    m_isQt6Project = value;
    emit isQt6ProjectChanged();
}

bool PropertyEditorContextObject::has3DModelSelected() const
{
    return m_has3DModelSelected;
}

void PropertyEditorContextObject::setHas3DModelSelected(bool value)
{
    if (value == m_has3DModelSelected)
        return;

    m_has3DModelSelected = value;
    emit has3DModelSelectedChanged();
}

void PropertyEditorContextObject::setSpecificsUrl(const QUrl &newSpecificsUrl)
{
    if (newSpecificsUrl == m_specificsUrl)
        return;

    qCInfo(urlSpecifics) << Q_FUNC_INFO << newSpecificsUrl;

    m_specificsUrl = newSpecificsUrl;
    emit specificsUrlChanged();
}

void PropertyEditorContextObject::setSpecificQmlData(const QString &newSpecificQmlData)
{
    if (m_specificQmlData == newSpecificQmlData)
        return;

    m_specificQmlData = newSpecificQmlData;

    delete m_qmlComponent;
    m_qmlComponent = nullptr;

    emit specificQmlComponentChanged();
    emit specificQmlDataChanged();
}

void PropertyEditorContextObject::setStateName(const QString &newStateName)
{
    if (newStateName == m_stateName)
        return;

    m_stateName = newStateName;
    emit stateNameChanged();
}

void PropertyEditorContextObject::setAllStateNames(const QStringList &allStates)
{
    if (allStates == m_allStateNames)
            return;

    m_allStateNames = allStates;
    emit allStateNamesChanged();
}

void PropertyEditorContextObject::setIsBaseState(bool newIsBaseState)
{
    if (newIsBaseState ==  m_isBaseState)
        return;

    m_isBaseState = newIsBaseState;
    emit isBaseStateChanged();
}

void PropertyEditorContextObject::setSelectionChanged(bool newSelectionChanged)
{
    if (newSelectionChanged == m_selectionChanged)
        return;

    m_selectionChanged = newSelectionChanged;
    emit selectionChangedChanged();
}

void PropertyEditorContextObject::setBackendValues(QQmlPropertyMap *newBackendValues)
{
    if (newBackendValues == m_backendValues)
        return;

    m_backendValues = newBackendValues;
    emit backendValuesChanged();
}

void PropertyEditorContextObject::setModel(Model *model)
{
    m_model = model;
}

void PropertyEditorContextObject::triggerSelectionChanged()
{
    setSelectionChanged(!m_selectionChanged);
}

void PropertyEditorContextObject::setHasAliasExport(bool hasAliasExport)
{
    if (m_aliasExport == hasAliasExport)
        return;

    m_aliasExport = hasAliasExport;
    emit hasAliasExportChanged();
}

void PropertyEditorContextObject::hideCursor()
{
    if (QApplication::overrideCursor())
        return;

    QApplication::setOverrideCursor(QCursor(Qt::BlankCursor));

    if (QWidget *w = QApplication::activeWindow())
        m_lastPos = QCursor::pos(w->screen());
}

void PropertyEditorContextObject::restoreCursor()
{
    if (!QApplication::overrideCursor())
        return;

    QApplication::restoreOverrideCursor();

    if (QWidget *w = QApplication::activeWindow())
        QCursor::setPos(w->screen(), m_lastPos);
}

void PropertyEditorContextObject::holdCursorInPlace()
{
    if (!QApplication::overrideCursor())
        return;

    if (QWidget *w = QApplication::activeWindow())
        QCursor::setPos(w->screen(), m_lastPos);
}

int PropertyEditorContextObject::devicePixelRatio()
{
    if (QWidget *w = QApplication::activeWindow())
        return w->devicePixelRatio();

    return 1;
}

QStringList PropertyEditorContextObject::styleNamesForFamily(const QString &family)
{
    return QFontDatabase::styles(family);
}

QStringList PropertyEditorContextObject::allStatesForId(const QString &id)
{
      if (m_model && m_model->rewriterView()) {
          const QmlObjectNode node = m_model->rewriterView()->modelNodeForId(id);
          if (node.isValid())
              return node.allStateNames();
      }

      return {};
}

bool PropertyEditorContextObject::isBlocked(const QString &propName) const
{
    if (m_model && m_model->rewriterView()) {
        const QList<ModelNode> nodes = m_model->rewriterView()->selectedModelNodes();
        for (const auto &node : nodes) {
              if (Qml3DNode qml3DNode{node}; qml3DNode.isBlocked(propName.toUtf8()))
                return true;
        }
    }
    return false;
}

void PropertyEditorContextObject::verifyInsightImport()
{
    Import import = Import::createLibraryImport("QtInsightTracker", "1.0");

    if (!m_model->hasImport(import))
        m_model->changeImports({import}, {});
}

QRect PropertyEditorContextObject::screenRect() const
{
    if (m_quickWidget && m_quickWidget->screen())
        return m_quickWidget->screen()->availableGeometry();
    return  {};
}

QPoint PropertyEditorContextObject::globalPos(const QPoint &point) const
{
    if (m_quickWidget)
        return m_quickWidget->mapToGlobal(point);
    return point;
}

void PropertyEditorContextObject::handleToolBarAction(int action)
{
    emit toolBarAction(action);
}

void PropertyEditorContextObject::saveExpandedState(const QString &sectionName, bool expanded)
{
    s_expandedStateHash.insert(sectionName, expanded);
}

bool PropertyEditorContextObject::loadExpandedState(const QString &sectionName, bool defaultValue) const
{
    return s_expandedStateHash.value(sectionName, defaultValue);
}

void EasingCurveEditor::registerDeclarativeType()
{
     qmlRegisterType<EasingCurveEditor>("HelperWidgets", 2, 0, "EasingCurveEditor");
}

void EasingCurveEditor::runDialog()
{
    if (m_modelNode.isValid())
        EasingCurveDialog::runDialog({ m_modelNode }, Core::ICore::dialogParent());
}

void EasingCurveEditor::setModelNodeBackend(const QVariant &modelNodeBackend)
{
    if (!modelNodeBackend.isNull() && modelNodeBackend.isValid()) {
        m_modelNodeBackend = modelNodeBackend;

        const auto modelNodeBackendObject = m_modelNodeBackend.value<QObject*>();

        const auto backendObjectCasted =
                qobject_cast<const QmlDesigner::QmlModelNodeProxy *>(modelNodeBackendObject);

        if (backendObjectCasted) {
            m_modelNode = backendObjectCasted->qmlObjectNode().modelNode();
        }

        emit modelNodeBackendChanged();
    }
}

QVariant EasingCurveEditor::modelNodeBackend() const
{
    return m_modelNodeBackend;
}

} //QmlDesigner
