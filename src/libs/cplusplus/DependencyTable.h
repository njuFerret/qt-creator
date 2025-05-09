// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <cplusplus/CPlusPlusForwardDeclarations.h>

#include <utils/filepath.h>

#include <QBitArray>
#include <QHash>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
template <typename T>
class QFuture;
QT_END_NAMESPACE

namespace CPlusPlus {

class Snapshot;

class CPLUSPLUS_EXPORT DependencyTable
{
private:
    friend class Snapshot;
    void build(const std::optional<QFuture<void>> &future, const Snapshot &snapshot);
    Utils::FilePaths filesDependingOn(const Utils::FilePath &fileName) const;

    Utils::FilePaths files;
    QHash<Utils::FilePath, int> fileIndex;
    QHash<int, QList<int> > includes;
    QList<QBitArray> includeMap;
};

} // namespace CPlusPlus
