// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "timelinenotesrenderpass.h"
#include "timelinerenderstate.h"
#include "timelinenotesmodel.h"

#include <utils/theme/theme.h>

namespace Timeline {

struct Point2DWithDistanceFromTop {
    float x, y; // vec4 vertexCoord
    float d; // float distanceFromTop
    void set(float nx, float ny, float nd);
};

class NotesMaterial : public QSGMaterial
{
public:
    QSGMaterialType *type() const final;
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const final;
};

struct NotesGeometry
{
    static const int maxNotes;
    static const QSGGeometry::AttributeSet &point2DWithDistanceFromTop();

    static QSGGeometry *createGeometry(QList<int> &ids, const TimelineModel *model,
                                       const TimelineRenderState *parentState, bool collapsed);
};

const int NotesGeometry::maxNotes = 0xffff / 2;

class TimelineNotesRenderPassState : public TimelineRenderPass::State
{
public:
    TimelineNotesRenderPassState(int expandedRows);
    ~TimelineNotesRenderPassState();

    QSGNode *expandedRow(int row) const { return m_expandedRows[row]; }
    QSGNode *collapsedOverlay() const final { return m_collapsedOverlay; }
    const QList<QSGNode *> &expandedRows() const final { return m_expandedRows; }

    QSGGeometry *nullGeometry() { return &m_nullGeometry; }
    NotesMaterial *material() { return &m_material; }

private:
    QSGGeometryNode *createNode();

    NotesMaterial m_material;
    QSGGeometry m_nullGeometry;
    QSGGeometryNode *m_collapsedOverlay;
    QList<QSGNode *> m_expandedRows;
};

const QSGGeometry::AttributeSet &NotesGeometry::point2DWithDistanceFromTop()
{
    static const QSGGeometry::Attribute data[] = {
        // vec4 vertexCoord
        QSGGeometry::Attribute::createWithAttributeType(0, 2, QSGGeometry::FloatType,
                                                        QSGGeometry::PositionAttribute),
        // float distanceFromTop
        QSGGeometry::Attribute::createWithAttributeType(1, 1, QSGGeometry::FloatType,
                                                        QSGGeometry::UnknownAttribute),
    };
    static const QSGGeometry::AttributeSet attrs = {
        sizeof(data) / sizeof(data[0]),
        sizeof(Point2DWithDistanceFromTop),
        data
    };
    return attrs;
}

const TimelineNotesRenderPass *TimelineNotesRenderPass::instance()
{
    static const TimelineNotesRenderPass pass;
    return &pass;
}

TimelineNotesRenderPass::TimelineNotesRenderPass()
{
}

TimelineRenderPass::State *TimelineNotesRenderPass::update(const TimelineAbstractRenderer *renderer,
                                                           const TimelineRenderState *parentState,
                                                           State *oldState, int firstIndex,
                                                           int lastIndex, bool stateChanged,
                                                           float spacing) const
{
    Q_UNUSED(firstIndex)
    Q_UNUSED(lastIndex)
    Q_UNUSED(spacing)

    const TimelineNotesModel *notes = renderer->notes();
    const TimelineModel *model = renderer->model();

    if (!model || !notes)
        return oldState;

    TimelineNotesRenderPassState *state;
    if (oldState == nullptr) {
        state = new TimelineNotesRenderPassState(model->expandedRowCount());
    } else {
        if (!stateChanged && !renderer->notesDirty())
            return oldState;
        state = static_cast<TimelineNotesRenderPassState *>(oldState);
    }

    QList<QList<int> > expanded(model->expandedRowCount());
    QList<int> collapsed;

    for (int i = 0; i < qMin(notes->count(), NotesGeometry::maxNotes); ++i) {
        if (notes->timelineModel(i) != model->modelId())
            continue;
        int timelineIndex = notes->timelineIndex(i);
        if (model->startTime(timelineIndex) > parentState->end() ||
                model->endTime(timelineIndex) < parentState->start())
            continue;
        expanded[model->expandedRow(timelineIndex)] << timelineIndex;
        collapsed << timelineIndex;
    }

    QSGGeometryNode *collapsedNode = static_cast<QSGGeometryNode *>(state->collapsedOverlay());

    if (!collapsed.isEmpty()) {
        collapsedNode->setGeometry(NotesGeometry::createGeometry(collapsed, model, parentState,
                                                                 true));
        collapsedNode->setFlag(QSGGeometryNode::OwnsGeometry, true);
    } else {
        collapsedNode->setGeometry(state->nullGeometry());
        collapsedNode->setFlag(QSGGeometryNode::OwnsGeometry, false);
    }

    for (int row = 0; row < model->expandedRowCount(); ++row) {
        QSGGeometryNode *rowNode = static_cast<QSGGeometryNode *>(state->expandedRow(row));
        if (expanded[row].isEmpty()) {
            rowNode->setGeometry(state->nullGeometry());
            rowNode->setFlag(QSGGeometryNode::OwnsGeometry, false);
        } else {
            rowNode->setGeometry(NotesGeometry::createGeometry(expanded[row], model, parentState,
                                                               false));
            collapsedNode->setFlag(QSGGeometryNode::OwnsGeometry, true);
        }
    }

    return state;
}

TimelineNotesRenderPassState::TimelineNotesRenderPassState(int numExpandedRows) :
    m_nullGeometry(NotesGeometry::point2DWithDistanceFromTop(), 0)
{
    m_material.setFlag(QSGMaterial::Blending, true);
    m_material.setFlag(QSGMaterial::NoBatching, true);
    m_expandedRows.reserve(numExpandedRows);
    for (int i = 0; i < numExpandedRows; ++i)
        m_expandedRows << createNode();
    m_collapsedOverlay = createNode();
}

TimelineNotesRenderPassState::~TimelineNotesRenderPassState()
{
    qDeleteAll(m_expandedRows);
    delete m_collapsedOverlay;
}

QSGGeometryNode *TimelineNotesRenderPassState::createNode()
{
    QSGGeometryNode *node = new QSGGeometryNode;
    node->setGeometry(&m_nullGeometry);
    node->setMaterial(&m_material);
    node->setFlag(QSGNode::OwnedByParent, false);
    return node;
}

QSGGeometry *NotesGeometry::createGeometry(QList<int> &ids, const TimelineModel *model,
                                           const TimelineRenderState *parentState, bool collapsed)
{
    float rowHeight = TimelineModel::defaultRowHeight();
    QSGGeometry *geometry = new QSGGeometry(point2DWithDistanceFromTop(),
                                            ids.count() * 2);
    Q_ASSERT(geometry->vertexData());
    geometry->setDrawingMode(QSGGeometry::DrawLines);
    geometry->setLineWidth(3);
    Point2DWithDistanceFromTop *v =
            static_cast<Point2DWithDistanceFromTop *>(geometry->vertexData());
    for (int i = 0; i < ids.count(); ++i) {
        int timelineIndex = ids[i];
        float horizontalCenter = ((model->startTime(timelineIndex) +
                                   model->endTime(timelineIndex)) / (qint64)2 -
                                  parentState->start()) * parentState->scale();
        float verticalStart = (collapsed ? (model->collapsedRow(timelineIndex) + 0.1) : 0.1) *
                rowHeight;
        float verticalEnd = verticalStart + 0.8 * rowHeight;
        v[i * 2].set(horizontalCenter, verticalStart, 0);
        v[i * 2 + 1].set(horizontalCenter, verticalEnd, 1);
    }
    return geometry;
}

class NotesMaterialShader : public QSGMaterialShader
{
public:
    NotesMaterialShader();

    bool updateUniformData(RenderState &state, QSGMaterial *, QSGMaterial *) override;
};

NotesMaterialShader::NotesMaterialShader()
    : QSGMaterialShader()
{
    setShaderFileName(VertexStage, ":/qt/qml/QtCreator/Tracing/notes_qt6.vert.qsb");
    setShaderFileName(FragmentStage, ":/qt/qml/QtCreator/Tracing/notes_qt6.frag.qsb");
}

static QColor notesColor()
{
    return Utils::creatorTheme()
            ? Utils::creatorColor(Utils::Theme::Timeline_HighlightColor)
            : QColor(255, 165, 0);
}

bool NotesMaterialShader::updateUniformData(RenderState &state, QSGMaterial *, QSGMaterial *)
{
    QByteArray *buf = state.uniformData();

    // mat4 matrix
    if (state.isMatrixDirty()) {
        const QMatrix4x4 m = state.combinedMatrix();
        memcpy(buf->data(), m.constData(), 64);
    }

    // vec4 notesColor
    const QColor color = notesColor();
    const float colorArray[] = { color.redF(), color.greenF(), color.blueF(), color.alphaF() };
    memcpy(buf->data() + 64, colorArray, 16);

    return true;
}

QSGMaterialType *NotesMaterial::type() const
{
    static QSGMaterialType type;
    return &type;
}

QSGMaterialShader *NotesMaterial::createShader(QSGRendererInterface::RenderMode) const
{
    return new NotesMaterialShader;
}

void Point2DWithDistanceFromTop::set(float nx, float ny, float nd)
{
    x = nx; y = ny; d = nd;
}

} // namespace Timeline
