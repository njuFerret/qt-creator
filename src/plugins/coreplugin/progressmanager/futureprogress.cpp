// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "futureprogress.h"

#include "progressbar.h"
#include "progressmanager_p.h"

#include <utils/stylehelper.h>
#include <utils/theme/theme.h>

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QPainter>
#include <QSequentialAnimationGroup>
#include <QTimer>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMouseEvent>

const int notificationTimeout = 8000;
const int shortNotificationTimeout = 1000;

using namespace Utils;

namespace Core {

class FutureProgressPrivate : public QObject
{
    Q_OBJECT

public:
    explicit FutureProgressPrivate(FutureProgress *q);

    void fadeAway();
    void tryToFadeAway();

    QFutureWatcher<void> m_watcher;
    Internal::ProgressBar *m_progress;
    QWidget *m_widget;
    QHBoxLayout *m_widgetLayout;
    QWidget *m_statusBarWidget;
    Id m_type;
    FutureProgress::KeepOnFinishType m_keep;
    bool m_waitingForUserInteraction;
    FutureProgress *m_q;
    bool m_fadeStarting;
    bool m_isFading;
    bool m_isSubtitleVisibleInStatusBar = false;
};

FutureProgressPrivate::FutureProgressPrivate(FutureProgress *q) :
    m_progress(new Internal::ProgressBar), m_widget(nullptr), m_widgetLayout(new QHBoxLayout),
    m_statusBarWidget(nullptr),
    m_keep(FutureProgress::HideOnFinish), m_waitingForUserInteraction(false),
    m_q(q), m_fadeStarting(false), m_isFading(false)
{
}

/*!
    \ingroup mainclasses
    \inheaderfile coreplugin/progressmanager/futureprogress.h
    \class Core::FutureProgress
    \inmodule QtCreator

    \brief The FutureProgress class is used to adapt the appearance of
    progress indicators that were created through the ProgressManager class.

    Use the instance of this class that was returned by
    ProgressManager::addTask() to define a widget that
    should be shown below the progress bar, or to change the
    progress title.
    Also use it to react on the event that the user clicks on
    the progress indicator (which can be used to e.g. open a more detailed
    view, or the results of the task).
*/

/*!
    \fn void Core::FutureProgress::clicked()
    Connect to this signal to get informed when the user clicks on the
    progress indicator.
*/

/*!
    \fn void Core::FutureProgress::canceled()
    Connect to this signal to get informed when the operation is canceled.
*/

/*!
    \fn void Core::FutureProgress::finished()
    Another way to get informed when the task has finished.
*/

/*!
    \fn QWidget Core::FutureProgress::widget() const
    Returns the custom widget that is shown below the progress indicator.
*/

/*!
    \internal
*/
FutureProgress::FutureProgress(QWidget *parent) :
    QWidget(parent), d(new FutureProgressPrivate(this))
{
    auto layout = new QVBoxLayout;
    setLayout(layout);
    layout->addWidget(d->m_progress);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(d->m_widgetLayout);
    d->m_widgetLayout->setContentsMargins(7, 0, 7, 2);
    d->m_widgetLayout->setSpacing(0);

    connect(&d->m_watcher, &QFutureWatcherBase::started, this, &FutureProgress::setStarted);
    connect(&d->m_watcher, &QFutureWatcherBase::finished, this, &FutureProgress::setFinished);
    connect(&d->m_watcher, &QFutureWatcherBase::canceled, this, &FutureProgress::canceled);
    connect(&d->m_watcher, &QFutureWatcherBase::progressRangeChanged,
            this, &FutureProgress::setProgressRange);
    connect(&d->m_watcher, &QFutureWatcherBase::progressValueChanged,
            this, &FutureProgress::setProgressValue);
    connect(&d->m_watcher, &QFutureWatcherBase::progressTextChanged,
            this, &FutureProgress::setProgressText);
    connect(d->m_progress, &Internal::ProgressBar::clicked, this, [this] {
        if (isCancelEnabled())
            cancel();
        else
            emit clicked();
    });
    setMinimumWidth(Internal::ProgressManagerPrivate::infoMinWidth());
    setMaximumWidth(Internal::ProgressManagerPrivate::infoMaxWidth());
}

/*!
    \internal
*/
FutureProgress::~FutureProgress()
{
    delete d->m_widget;
    delete d;
}

/*!
    Sets the \a widget to show below the progress bar.
    This will be destroyed when the progress indicator is destroyed.
    Default is to show no widget below the progress indicator.
*/
void FutureProgress::setWidget(QWidget *widget)
{
    delete d->m_widget;
    QSizePolicy sp = widget->sizePolicy();
    sp.setHorizontalPolicy(QSizePolicy::Ignored);
    widget->setSizePolicy(sp);
    d->m_widget = widget;
    if (d->m_widget)
        d->m_widgetLayout->addWidget(d->m_widget);
}

/*!
    Changes the \a title of the progress indicator.
*/
void FutureProgress::setTitle(const QString &title)
{
    d->m_progress->setTitle(title);
}

/*!
    Returns the title of the progress indicator.
*/
QString FutureProgress::title() const
{
    return d->m_progress->title();
}

void FutureProgress::setSubtitle(const QString &subtitle)
{
    if (subtitle != d->m_progress->subtitle()) {
        d->m_progress->setSubtitle(subtitle);
        if (d->m_isSubtitleVisibleInStatusBar)
            emit subtitleInStatusBarChanged();
    }
}

QString FutureProgress::subtitle() const
{
    return d->m_progress->subtitle();
}

void FutureProgress::setSubtitleVisibleInStatusBar(bool visible)
{
    if (visible != d->m_isSubtitleVisibleInStatusBar) {
        d->m_isSubtitleVisibleInStatusBar = visible;
        emit subtitleInStatusBarChanged();
    }
}

bool FutureProgress::isSubtitleVisibleInStatusBar() const
{
    return d->m_isSubtitleVisibleInStatusBar;
}

void FutureProgress::cancel()
{
    d->m_watcher.future().cancel();
}

void FutureProgress::updateToolTip(const QString &text)
{
    setToolTip(QLatin1String("<b>") + title() + QLatin1String("</b><br>") + text);
}

void FutureProgress::setStarted()
{
    d->m_progress->reset();
    d->m_progress->setError(false);
    d->m_progress->setRange(d->m_watcher.progressMinimum(), d->m_watcher.progressMaximum());
    d->m_progress->setValue(d->m_watcher.progressValue());
}


bool FutureProgress::eventFilter(QObject *, QEvent *e)
{
    if (d->m_keep != KeepOnFinish && d->m_waitingForUserInteraction
            && (e->type() == QEvent::MouseMove || e->type() == QEvent::KeyPress)) {
        qApp->removeEventFilter(this);
        QTimer::singleShot(notificationTimeout, d, &FutureProgressPrivate::fadeAway);
    }
    return false;
}

void FutureProgress::setFinished()
{
    updateToolTip(d->m_watcher.future().progressText());

    d->m_progress->setFinished(true);

    if (d->m_watcher.future().isCanceled()) {
        d->m_progress->setError(true);
        emit hasErrorChanged();
    } else {
        d->m_progress->setError(false);
    }
    emit finished();
    d->tryToFadeAway();
}

void FutureProgressPrivate::tryToFadeAway()
{
    if (m_fadeStarting)
        return;
    if (m_keep == FutureProgress::KeepOnFinishTillUserInteraction
            || (m_keep == FutureProgress::HideOnFinish && m_progress->hasError())) {
        m_waitingForUserInteraction = true;
        //eventfilter is needed to get user interaction
        //events to start QTimer::singleShot later
        qApp->installEventFilter(m_q);
        m_fadeStarting = true;
    } else if (m_keep == FutureProgress::HideOnFinish) {
        QTimer::singleShot(shortNotificationTimeout, this, &FutureProgressPrivate::fadeAway);
        m_fadeStarting = true;
    }
}


void FutureProgress::setProgressRange(int min, int max)
{
    d->m_progress->setRange(min, max);
}

void FutureProgress::setProgressValue(int val)
{
    d->m_progress->setValue(val);
}

void FutureProgress::setProgressText(const QString &text)
{
    updateToolTip(text);
}

/*!
    \internal
*/
void FutureProgress::setFuture(const QFuture<void> &future)
{
    d->m_watcher.setFuture(future);
}

/*!
    Returns a QFuture object that represents this running task.
*/
QFuture<void> FutureProgress::future() const
{
    return d->m_watcher.future();
}

/*!
    \internal
*/
void FutureProgress::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        emit clicked();
    QWidget::mouseReleaseEvent(event);
}

void FutureProgress::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    if (creatorTheme()->flag(Theme::FlatToolBars)) {
        p.fillRect(rect(), StyleHelper::baseColor());
        p.fillRect(rect(), creatorColor(Theme::FancyToolButtonSelectedColor));
    } else {
        QLinearGradient grad = StyleHelper::statusBarGradient(rect());
        p.fillRect(rect(), grad);
    }
}

/*!
    Returns the error state of this progress indicator.
*/
bool FutureProgress::hasError() const
{
    return d->m_progress->hasError();
}

void FutureProgress::setType(Id type)
{
    d->m_type = type;
}

Id FutureProgress::type() const
{
    return d->m_type;
}

void FutureProgress::setKeepOnFinish(KeepOnFinishType keepType)
{
    if (d->m_keep == keepType)
        return;
    d->m_keep = keepType;

    //if it is not finished tryToFadeAway is called by setFinished at the end
    if (d->m_watcher.isFinished())
        d->tryToFadeAway();
}

bool FutureProgress::keepOnFinish() const
{
    return d->m_keep;
}

QWidget *FutureProgress::widget() const
{
    return d->m_widget;
}

void FutureProgress::setStatusBarWidget(QWidget *widget)
{
    if (widget == d->m_statusBarWidget)
        return;
    delete d->m_statusBarWidget;
    d->m_statusBarWidget = widget;
    emit statusBarWidgetChanged();
}

QWidget *FutureProgress::statusBarWidget() const
{
    return d->m_statusBarWidget;
}

bool FutureProgress::isFading() const
{
    return d->m_isFading;
}

QSize FutureProgress::sizeHint() const
{
    return QSize(QWidget::sizeHint().width(), minimumHeight());
}

bool FutureProgress::isCancelEnabled() const
{
    return d->m_progress->isCancelEnabled();
}

void FutureProgress::setCancelEnabled(bool enabled)
{
    d->m_progress->setCancelEnabled(enabled);
}

void FutureProgressPrivate::fadeAway()
{
    m_isFading = true;

    auto opacityEffect = new QGraphicsOpacityEffect;
    opacityEffect->setOpacity(.999);
    m_q->setGraphicsEffect(opacityEffect);

    auto group = new QSequentialAnimationGroup(this);
    QPropertyAnimation *animation = new QPropertyAnimation(opacityEffect, "opacity");
    animation->setDuration(StyleHelper::progressFadeAnimationDuration);
    animation->setEndValue(0.);
    group->addAnimation(animation);
    animation = new QPropertyAnimation(m_q, "maximumHeight");
    animation->setDuration(120);
    animation->setEasingCurve(QEasingCurve::InCurve);
    animation->setStartValue(m_q->sizeHint().height());
    animation->setEndValue(0.0);
    group->addAnimation(animation);

    connect(group, &QAbstractAnimation::finished, m_q, &FutureProgress::removeMe);
    group->start(QAbstractAnimation::DeleteWhenStopped);
    emit m_q->fadeStarted();
}

} // namespace Core

#include "futureprogress.moc"
