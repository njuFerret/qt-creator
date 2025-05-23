// Copyright (C) 2021 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

import QtQuick 2.15
import QtQuick.Layouts 1.15

RowLayout {
    id: root

    property bool searchNoMatch: false

    Layout.fillWidth: true
    spacing: 0

    states: [
        State {
            name: "searchNoMatch"
            when: searchNoMatch
            PropertyChanges {
                target: root
                visible: false
            }
        }
    ]
}
