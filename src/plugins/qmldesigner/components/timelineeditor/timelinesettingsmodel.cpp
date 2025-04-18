// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "timelinesettingsmodel.h"

#include "timelineview.h"

#include <modelnode.h>
#include <qmldesignertr.h>
#include <qmlvisualnode.h>
#include <variantproperty.h>

#include <utils/qtcassert.h>

#include <QComboBox>
#include <QItemEditorFactory>
#include <QMessageBox>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTimer>

#include <optional>

namespace QmlDesigner {

static void setDataForFixedFrame(QStandardItem *item, std::optional<int> fixedValue)
{
    if (fixedValue)
        item->setData(fixedValue.value(), Qt::EditRole);
    else
        item->setData(Tr::tr("None"), Qt::EditRole);
}

class CustomDelegate : public QStyledItemDelegate
{
public:
    explicit CustomDelegate(QWidget *parent = nullptr);
    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
};

CustomDelegate::CustomDelegate(QWidget *parent)
    : QStyledItemDelegate(parent)
{}

void CustomDelegate::paint(QPainter *painter,
                           const QStyleOptionViewItem &option,
                           const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    opt.state &= ~QStyle::State_HasFocus;
    QStyledItemDelegate::paint(painter, opt, index);
}

class TimelineEditorDelegate : public CustomDelegate
{
public:
    TimelineEditorDelegate(QWidget *parent = nullptr);
    QWidget *createEditor(QWidget *parent,
                          const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override;
};

TimelineEditorDelegate::TimelineEditorDelegate(QWidget *parent)
    : CustomDelegate(parent)
{
    static QItemEditorFactory *factory = nullptr;
    if (factory == nullptr) {
        factory = new QItemEditorFactory;
        QItemEditorCreatorBase *creator = new QItemEditorCreator<QComboBox>("currentText");
        factory->registerEditor(QMetaType::QString, creator);
    }

    setItemEditorFactory(factory);
}

QSpinBox *createSpinBox(QWidget *parent)
{
    auto spinBox = new QSpinBox(parent);
    spinBox->setRange(-10000, 10000);
    return spinBox;
}

QWidget *TimelineEditorDelegate::createEditor(QWidget *parent,
                                              const QStyleOptionViewItem &option,
                                              const QModelIndex &index) const
{
    QWidget *widget = nullptr;

    if (index.column() ==  TimelineSettingsModel::FixedFrameRow)
        widget = createSpinBox(parent);
    else
        widget = QStyledItemDelegate::createEditor(parent, option, index);

    const auto timelineSettingsModel = qobject_cast<const TimelineSettingsModel *>(index.model());

    auto comboBox = qobject_cast<QComboBox *>(widget);

    QTC_ASSERT(timelineSettingsModel, return widget);
    QTC_ASSERT(timelineSettingsModel->timelineView(), return widget);

    QmlTimeline qmlTimeline = timelineSettingsModel->timelineForRow(index.row());

    switch (index.column()) {
    case TimelineSettingsModel::TimelineRow: {
        QTC_ASSERT(comboBox, return widget);
        comboBox->addItem(Tr::tr("None"));
        for (const auto &timeline : timelineSettingsModel->timelineView()->getTimelines()) {
            if (!timeline.modelNode().id().isEmpty())
                comboBox->addItem(timeline.modelNode().id());
        }
    } break;
    case TimelineSettingsModel::AnimationRow: {
        QTC_ASSERT(comboBox, return widget);
        comboBox->addItem(Tr::tr("None"));
        for (const auto &animation :
             timelineSettingsModel->timelineView()->getAnimations(qmlTimeline)) {
            if (!animation.id().isEmpty())
                comboBox->addItem(animation.id());
        }
    } break;
    case TimelineSettingsModel::FixedFrameRow: {
    } break;

    default:
        qWarning() << "TimelineEditorDelegate::createEditor column" << index.column();
    }

    if (comboBox) {
        connect(comboBox, &QComboBox::activated, this, [this, comboBox] {
            auto delegate = const_cast<TimelineEditorDelegate *>(this);
            emit delegate->commitData(comboBox);
        });
    }

    return widget;
}

TimelineSettingsModel::TimelineSettingsModel(QObject *parent, TimelineView *view)
    : QStandardItemModel(parent)
    , m_timelineView(view)
{
    connect(this, &QStandardItemModel::dataChanged, this, &TimelineSettingsModel::handleDataChanged);
}

void TimelineSettingsModel::resetModel()
{
    beginResetModel();
    clear();
    setHorizontalHeaderLabels(
        QStringList({tr("State"), tr("Timeline"), tr("Animation"), tr("Fixed Frame")}));

    if (timelineView()->isAttached()) {
        addState(ModelNode());
        QmlVisualNode visNode(timelineView()->rootModelNode());
        if (visNode.isValid()) {
            const auto allStates = visNode.states().allStates();
            for (const QmlModelState &state : allStates)
                addState(state);
        }
    }

    endResetModel();
}

TimelineView *TimelineSettingsModel::timelineView() const
{
    return m_timelineView;
}

void TimelineSettingsModel::setupDelegates(QAbstractItemView *view)
{
    view->setItemDelegate(new TimelineEditorDelegate);
}

static std::optional<int> propertyValueForState(const ModelNode &modelNode,
                                 QmlModelState state,
                                 const PropertyName &propertyName)
{
    if (!modelNode.isValid())
        return {};

    if (state.isBaseState()) {
        if (modelNode.hasVariantProperty(propertyName))
            return modelNode.variantProperty(propertyName).value().toInt();
        return {};
    }

    if (state.hasPropertyChanges(modelNode)) {
        QmlPropertyChanges propertyChanges(state.propertyChanges(modelNode));
        if (propertyChanges.modelNode().hasVariantProperty(propertyName))
            return propertyChanges.modelNode().variantProperty(propertyName).value().toInt();
    }

    return {};
}

static QStandardItem *createStateItem(const ModelNode &state)
{
    if (state.isValid())
        return new QStandardItem(state.variantProperty("name").value().toString());
    else
        return new QStandardItem(Tr::tr("Base State"));
}

void TimelineSettingsModel::addState(const ModelNode &state)
{
    QList<QStandardItem *> items;

    QmlTimeline timeline = timelineView()->timelineForState(state);
    const QString timelineId = timeline.modelNode().id();
    ModelNode animation = animationForTimelineAndState(timeline, state);
    const QString animationId = animation.id();

    QStandardItem *stateItem = createStateItem(state);
    auto *timelinelItem = new QStandardItem(timelineId);
    auto *animationItem = new QStandardItem(animationId);
    auto *fixedFrameItem = new QStandardItem("");

    stateItem->setData(state.internalId());
    stateItem->setFlags(Qt::ItemIsEnabled);

    auto fixedValue = propertyValueForState(timeline, state, "currentFrame");
    setDataForFixedFrame(fixedFrameItem, fixedValue);

    items.append(stateItem);
    items.append(timelinelItem);
    items.append(animationItem);
    items.append(fixedFrameItem);

    appendRow(items);
}

void TimelineSettingsModel::handleException()
{
    QMessageBox::warning(nullptr, tr("Error"), m_exceptionError);
    resetModel();
}

ModelNode TimelineSettingsModel::animationForTimelineAndState(const QmlTimeline &timeline,
                                                              const ModelNode &state)
{
    QmlModelState modelState(state);

    if (!timeline.isValid())
        return ModelNode();

    const QList<ModelNode> &animations = timelineView()->getAnimations(timeline);

    if (modelState.isBaseState()) {
        for (const auto &animation : animations) {
            if (animation.hasVariantProperty("running")
                && animation.variantProperty("running").value().toBool())
                return animation;
        }
        return ModelNode();
    }

    for (const auto &animation : animations) {
        if (modelState.affectsModelNode(animation)) {
            QmlPropertyChanges propertyChanges(modelState.propertyChanges(animation));

            if (propertyChanges.isValid() && propertyChanges.modelNode().hasProperty("running")
                && propertyChanges.modelNode().variantProperty("running").value().toBool())
                return animation;
        }
    }
    return ModelNode();
}

void TimelineSettingsModel::updateTimeline(int row)
{

    timelineView()->executeInTransaction("TimelineSettingsModel::updateTimeline", [this, row](){
        QmlModelState modelState(stateForRow(row));
        QmlTimeline timeline(timelineForRow(row));
        ModelNode animation(animationForRow(row));
        QmlTimeline oldTimeline = timelineView()->timelineForState(modelState);

        if (modelState.isBaseState()) {
            if (oldTimeline.isValid())
                oldTimeline.modelNode().variantProperty("enabled").setValue(false);
            if (timeline.isValid())
                timeline.modelNode().variantProperty("enabled").setValue(true);
        } else {
            if (oldTimeline.isValid() && modelState.affectsModelNode(oldTimeline)) {
                QmlPropertyChanges propertyChanges(modelState.propertyChanges(oldTimeline));
                if (propertyChanges.isValid() && propertyChanges.modelNode().hasProperty("enabled"))
                    propertyChanges.modelNode().removeProperty("enabled");
            }

            QmlTimeline baseTimeline(timelineForRow(0));

            if (baseTimeline.isValid()) {
                QmlPropertyChanges propertyChanges(modelState.propertyChanges(baseTimeline));
                if (propertyChanges.isValid())
                    propertyChanges.modelNode().variantProperty("enabled").setValue(false);
            }

            if (timeline.isValid()) { /* If timeline is invalid 'none' was selected */
                QmlPropertyChanges propertyChanges(modelState.propertyChanges(timeline));
                if (propertyChanges.isValid())
                    propertyChanges.modelNode().variantProperty("enabled").setValue(true);
            }
        }
    });

    resetRow(row);
}

void TimelineSettingsModel::updateAnimation(int row)
{
    timelineView()->executeInTransaction("TimelineSettingsModel::updateAnimation", [this, row](){
        QmlModelState modelState(stateForRow(row));
        QmlTimeline timeline(timelineForRow(row));
        ModelNode animation(animationForRow(row));
        QmlTimeline oldTimeline = timelineView()->timelineForState(modelState);
        ModelNode oldAnimation = animationForTimelineAndState(oldTimeline, modelState);

        if (modelState.isBaseState()) {
            oldAnimation.variantProperty("running").setValue(false);
            animation.variantProperty("running").setValue(true);
            timeline.modelNode().removeProperty("currentFrame");
        } else {
            if (modelState.affectsModelNode(oldAnimation)) {
                QmlPropertyChanges propertyChanges(modelState.propertyChanges(oldAnimation));
                propertyChanges.modelNode().removeProperty("running");
            }

            if (ModelNode baseAnimation = animationForRow(0)) {
                QmlPropertyChanges propertyChanges(modelState.propertyChanges(baseAnimation));
                propertyChanges.modelNode().variantProperty("running").setValue(false);
                propertyChanges.modelNode().removeProperty("currentFrame");
            }

            if (animation.isValid()) { /* If animation is invalid 'none' was selected */
                QmlPropertyChanges propertyChanges(modelState.propertyChanges(animation));
                propertyChanges.modelNode().variantProperty("running").setValue(true);
            }
        }
    });
    resetRow(row);
}

void TimelineSettingsModel::updateFixedFrameRow(int row)
{
    timelineView()->executeInTransaction("TimelineSettingsModel::updateFixedFrameRow", [this, row](){
        QmlModelState modelState(stateForRow(row));
        QmlTimeline timeline(timelineForRow(row));

        ModelNode animation = animationForTimelineAndState(timeline, modelState);

        int fixedFrame = fixedFrameForRow(row);

        if (modelState.isBaseState()) {
            animation.variantProperty("running").setValue(false);
            timeline.modelNode().variantProperty("currentFrame").setValue(fixedFrame);
        } else {
            if (modelState.affectsModelNode(animation)) {
                QmlPropertyChanges propertyChanges(modelState.propertyChanges(animation));
                if (propertyChanges.modelNode().hasProperty("running"))
                    propertyChanges.modelNode().removeProperty("running");
            }

            QmlPropertyChanges propertyChanges(modelState.propertyChanges(timeline));
            propertyChanges.modelNode().variantProperty("currentFrame").setValue(fixedFrame);
        }

    });

    resetRow(row);
}

void TimelineSettingsModel::resetRow(int row)
{
    m_lock = true;
    QStandardItem *animationItem = item(row, AnimationRow);
    QStandardItem *fixedFrameItem = item(row, FixedFrameRow);

    QmlModelState modelState(stateForRow(row));
    QmlTimeline timeline(timelineForRow(row));
    ModelNode animation = animationForTimelineAndState(timeline, modelState);

    if (animationItem)
        animationItem->setText(animation.id());

    if (fixedFrameItem) {
        auto fixedValue = propertyValueForState(timeline, modelState, "currentFrame");
        if (fixedFrameItem->data(Qt::EditRole).toInt() != fixedValue)
            setDataForFixedFrame(fixedFrameItem, fixedValue);
    }

    m_lock = false;
}

QmlTimeline TimelineSettingsModel::timelineForRow(int row) const
{
    QStandardItem *standardItem = item(row, TimelineRow);

    if (standardItem)
        return QmlTimeline(timelineView()->modelNodeForId(standardItem->text()));

    return QmlTimeline();
}

ModelNode TimelineSettingsModel::animationForRow(int row) const
{
    QStandardItem *standardItem = item(row, AnimationRow);

    if (standardItem)
        return timelineView()->modelNodeForId(standardItem->text());

    return ModelNode();
}

ModelNode TimelineSettingsModel::stateForRow(int row) const
{
    QStandardItem *standardItem = item(row, StateRow);

    if (standardItem)
        return timelineView()->modelNodeForInternalId(standardItem->data().toInt());

    return ModelNode();
}

int TimelineSettingsModel::fixedFrameForRow(int row) const
{
    QStandardItem *standardItem = item(row, FixedFrameRow);

    if (standardItem)
        return standardItem->data(Qt::EditRole).toInt();

    return -1;
}

void TimelineSettingsModel::handleDataChanged(const QModelIndex &topLeft,
                                              const QModelIndex &bottomRight)
{
    if (topLeft != bottomRight) {
        qWarning() << "TimelineSettingsModel::handleDataChanged multi edit?";
        return;
    }

    if (m_lock)
        return;

    m_lock = true;

    int currentColumn = topLeft.column();
    int currentRow = topLeft.row();

    switch (currentColumn) {
    case StateRow: {
        /* read only */
    } break;
    case TimelineRow: {
        updateTimeline(currentRow);
    } break;
    case AnimationRow: {
        updateAnimation(currentRow);
    } break;
    case FixedFrameRow: {
        updateFixedFrameRow(currentRow);
    } break;

    default:
        qWarning() << "ConnectionModel::handleDataChanged column" << currentColumn;
    }

    m_lock = false;
}

} // namespace QmlDesigner
