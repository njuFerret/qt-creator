// Copyright (C) 2024 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <coreplugin/dialogs/ioptionspage.h>

#include <QPointer>

namespace Coco::Internal {

class CocoSettings
{
    friend CocoSettings &cocoSettings();
    CocoSettings();

public:
    void read();
    void save();

    Utils::FilePath directory() const;
    Utils::FilePath coverageBrowserPath() const;
    void setDirectory(const Utils::FilePath &dir);
    void findDefaultDirectory();

    bool isValid() const;
    QString errorMessage() const;

private:
    Utils::FilePath coverageScannerPath(const Utils::FilePath &cocoDir) const;
    void logError(const QString &msg);
    bool isCocoDirectory(const Utils::FilePath &cocoDir) const;
    bool verifyCocoDirectory();
    void tryPath(const QString &path);
    QString envVar(const QString &var) const;

    Utils::FilePath m_cocoPath;
    bool m_isValid = false;
    QString m_errorMessage;
};

CocoSettings &cocoSettings();

class GlobalSettingsWidget : public QFrame
{
    Q_OBJECT

public:
    GlobalSettingsWidget(QFrame *parent = nullptr);

    void apply();
    void cancel();

signals:
    void updateCocoDir();

public slots:
    void setVisible(bool visible) override;

private:
    void onCocoPathChanged();

    Utils::FilePath widgetCocoDir() const;
    bool verifyCocoDirectory(const Utils::FilePath &cocoDir);

    Utils::FilePathAspect m_cocoPathAspect;
    Utils::TextDisplay m_messageLabel;

    Utils::FilePath m_previousCocoDir;
};

class GlobalSettingsPage : public Core::IOptionsPage
{
public:
    static GlobalSettingsPage &instance();

    GlobalSettingsWidget *widget() override;
    void apply() override;
    void cancel() override;
    void finish() override;

private:
    GlobalSettingsPage();

    QPointer<GlobalSettingsWidget> m_widget;
};

} // namespace Coco::Internal
