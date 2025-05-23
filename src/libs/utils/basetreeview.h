// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "utils_global.h"

#include "itemviews.h"

namespace Utils {

class QtcSettings;

namespace Internal { class BaseTreeViewPrivate; }

class QTCREATOR_UTILS_EXPORT BaseTreeView : public TreeView
{
    Q_OBJECT

public:
    enum {
        ExtraIndicesForColumnWidth = 12734,

        ItemViewEventRole = Qt::UserRole + 12735,

        ItemActivatedRole,
        ItemClickedRole,

        ItemDelegateRole,
    };

    BaseTreeView(QWidget *parent = nullptr);
    ~BaseTreeView() override;

    void setSettings(Utils::QtcSettings *settings, const QByteArray &key);

    void setModel(QAbstractItemModel *model) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void contextMenuEvent(QContextMenuEvent *ev) override;
    void showEvent(QShowEvent *ev) override;
    void keyPressEvent(QKeyEvent *ev) override;
    void dragEnterEvent(QDragEnterEvent *ev) override;
    void dropEvent(QDropEvent *ev) override;
    void dragMoveEvent(QDragMoveEvent *ev) override;
    void mouseDoubleClickEvent(QMouseEvent *ev) override;
    void resizeEvent(QResizeEvent *event) override;

    void showProgressIndicator();
    void hideProgressIndicator();
    void resizeColumns();

    int spanColumn() const;
    void setSpanColumn(int column);

    void enableColumnHiding();

    // In some situations this needs to be called when manually resizing columns when the span
    // column is set.
    void refreshSpanColumn();

    QString selectionAsText() const;

signals:
    void aboutToShow();

private:
    void rowActivated(const QModelIndex &index);
    void rowClicked(const QModelIndex &index);

    Internal::BaseTreeViewPrivate *d;
};

template <typename Event> struct EventCode;
template <> struct EventCode<QDragEnterEvent> { enum { code = QEvent::DragEnter }; };
template <> struct EventCode<QDragLeaveEvent> { enum { code = QEvent::DragLeave }; };
template <> struct EventCode<QDragMoveEvent> { enum { code = QEvent::DragMove }; };
template <> struct EventCode<QDropEvent> { enum { code = QEvent::Drop }; };
template <> struct EventCode<QContextMenuEvent> { enum { code = QEvent::ContextMenu }; };
template <> struct EventCode<QMouseEvent> { enum { code = QEvent::MouseButtonPress }; };
template <> struct EventCode<QKeyEvent> { enum { code = QEvent::KeyPress }; };

template <class T> T *checkEventType(QEvent *ev)
{
    const int cc = EventCode<T>::code;
    const int tt = ev->type();
    if (cc == tt)
        return static_cast<T *>(ev);
    if (cc == QEvent::MouseButtonPress) {
        if (tt == QEvent::MouseButtonDblClick || tt == QEvent::MouseButtonRelease || tt == QEvent::MouseMove)
            return static_cast<T *>(ev);
    }
    if (cc == QEvent::KeyPress && tt == QEvent::KeyRelease)
        return static_cast<T *>(ev);
    return nullptr;
}

class QTCREATOR_UTILS_EXPORT ItemViewEvent
{
public:
    ItemViewEvent() = default;
    ItemViewEvent(QEvent *ev, QAbstractItemView *view);

    template <class T> T *as() const {
        return checkEventType<T>(m_event);
    }

    template <class T> T *as(QEvent::Type t) const {
        return m_event->type() == t ? as<T>() : nullptr;
    }

    QEvent::Type type() const { return m_event->type(); }
    QWidget *view() const { return m_view; }
    QPoint pos() const { return m_pos; }
    QPoint globalPos() const { return m_view->mapToGlobal(m_pos); }
    QModelIndex index() const { return m_index; }
    QModelIndex sourceModelIndex() const { return m_sourceModelIndex; }
    QModelIndexList selectedRows() const { return m_selectedRows; }
    QModelIndexList currentOrSelectedRows() const;

private:
    QEvent *m_event = nullptr;
    QWidget *m_view = nullptr;
    QPoint m_pos;
    QModelIndex m_index;
    QModelIndex m_sourceModelIndex;
    QModelIndexList m_selectedRows;
};

} // namespace Utils

Q_DECLARE_METATYPE(Utils::ItemViewEvent);
