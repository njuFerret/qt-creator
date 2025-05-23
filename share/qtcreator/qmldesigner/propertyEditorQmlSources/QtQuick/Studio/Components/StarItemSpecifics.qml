// Copyright (C) 2025 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

import QtQuick
import QtQuick.Layouts
import HelperWidgets
import StudioTheme as StudioTheme

Column {
    anchors.left: parent.left
    anchors.right: parent.right

    Section {
        anchors.left: parent.left
        anchors.right: parent.right
        caption: qsTr("Star Item")

        SectionLayout {
            PropertyLabel {
                text: qsTr("Fill color")
                tooltip: qsTr("Sets the color to fill the enclosed Star.")
            }

            ColorEditor {
                backendValue: backendValues.fillColor
                supportGradient: true
                shapeGradients: true
            }

            PropertyLabel {
                text: qsTr("Stroke color")
                tooltip: qsTr("Sets the stroke color of the boundary.")
            }

            ColorEditor {
                backendValue: backendValues.strokeColor
                supportGradient: false
            }

            PropertyLabel {
                text: qsTr("Stroke width")
                tooltip: qsTr("Sets the stroke thickness of the boundary.")
            }

            SecondColumnLayout {
                SpinBox {
                    id: strokeWidthSpinBox
                    implicitWidth: StudioTheme.Values.twoControlColumnWidth
                                   + StudioTheme.Values.actionIndicatorWidth
                    backendValue: backendValues.strokeWidth
                    decimals: 1
                    minimumValue: -1
                    maximumValue: 200
                    stepSize: 1

                    property real previousValue: 0

                    onValueChanged: {
                        if (strokeWidthSpinBox.value > 0)
                            strokeWidthSpinBox.previousValue = strokeWidthSpinBox.value
                    }

                    Component.onCompleted: strokeWidthSpinBox.previousValue
                                           = Math.max(1, backendValues.strokeWidth.value)
                }

                Spacer {
                    implicitWidth: StudioTheme.Values.twoControlColumnGap
                                   + StudioTheme.Values.actionIndicatorWidth
                }

                CheckBox {
                    id: strokeWidthCheckBox
                    text: qsTr("Hide")
                    implicitWidth: StudioTheme.Values.twoControlColumnWidth
                    checked: (backendValues.strokeWidth.value < 0)
                    actionIndicator.visible: false

                    onCheckedChanged: backendValues.strokeWidth.value
                                      = (strokeWidthCheckBox.checked ? -1 : strokeWidthSpinBox.previousValue)
                }

                ExpandingSpacer {}
            }

            PropertyLabel {
                text: qsTr("Count")
                tooltip: qsTr("Sets the number of TODO for the Star.")
            }

            SecondColumnLayout {
                SpinBox {
                    backendValue: backendValues.count
                    implicitWidth: StudioTheme.Values.twoControlColumnWidth
                                   + StudioTheme.Values.actionIndicatorWidth
                    decimals: 0
                    minimumValue: 3
                    maximumValue: 60
                    stepSize: 1
                }

                ExpandingSpacer {}
            }

            PropertyLabel {
                text: qsTr("Ratio")
                tooltip: qsTr("TODO")
            }

            SecondColumnLayout {
                SpinBox {
                    backendValue: backendValues.ratio
                    implicitWidth: StudioTheme.Values.twoControlColumnWidth
                                   + StudioTheme.Values.actionIndicatorWidth
                    decimals: 3
                    minimumValue: 0.001
                    maximumValue: 1.0
                    stepSize: 0.01
                }

                ExpandingSpacer {}
            }
        }
    }

    StrokeDetailsSection {
        showBorderMode: false
        showJoinStyle: true
        showCapStyle: false
    }

    Section {
        anchors.left: parent.left
        anchors.right: parent.right
        caption: qsTr("Radiuses")

        SectionLayout {
            PropertyLabel {
                text: qsTr("Radius")
                tooltip: qsTr("Sets the radius of the corners in the Regular Polygon.")
            }

            SecondColumnLayout {
                SpinBox {
                    backendValue: backendValues.radius
                    implicitWidth: StudioTheme.Values.twoControlColumnWidth
                                   + StudioTheme.Values.actionIndicatorWidth
                    decimals: 0
                    minimumValue: backendValues.minRadius.value
                    maximumValue: backendValues.maxRadius.value
                    stepSize: 1
                }

                Spacer { implicitWidth: StudioTheme.Values.controlLabelGap }

                MultiIconLabel {
                    icon0: StudioTheme.Constants.triangleCornerB
                    icon1: StudioTheme.Constants.triangleCornerA
                }

                ExpandingSpacer {}
            }
        }
    }
}
