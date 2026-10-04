import QtQuick
import glance

Item {
    id: outline

    Rectangle {
        anchors.fill: parent
        color: Theme.darkerBackground
    }

    ListView {
        id: list
        anchors.fill: parent
        anchors.topMargin: 8
        anchors.bottomMargin: 8
        clip: true
        model: root.outlineModel
        boundsBehavior: Flickable.StopAtBounds
        cacheBuffer: 2000

        delegate: Rectangle {
            width: list.width
            height: label.implicitHeight + 10
            color: ma.containsMouse ? Theme.selection : "transparent"

            Text {
                id: label
                x: 10 + modelData.depth * 14
                width: parent.width - x - 10
                anchors.verticalCenter: parent.verticalCenter
                text: modelData.title !== "" ? modelData.title : "(untitled)"
                color: modelData.page >= 0 ? Theme.foreground : Theme.mutedForeground
                font.pixelSize: 12
                elide: Text.ElideRight
                renderType: Text.QtRendering
            }

            MouseArea {
                id: ma
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (modelData.page >= 0)
                        root.jumpTo(modelData.page + 1, false)
                }
            }
        }

        FastWheelScroll {
            flickable: list
        }
    }
}
