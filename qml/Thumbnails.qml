import QtQuick
import glance

Item {
    id: thumbsRoot

    property bool visibleColumn: true

    Rectangle {
        anchors.fill: parent
        color: Theme.darkerBackground
    }

    ListView {
        id: list
        anchors.fill: parent
        anchors.topMargin: 10
        anchors.leftMargin: 10
        model: Doc.pageCount
        clip: true
        spacing: 8
        cacheBuffer: 1600
        boundsBehavior: Flickable.StopAtBounds
        currentIndex: -1

        delegate: Item {
            width: list.width - 12
            height: box.height + caption.height + 6
            readonly property var pts: Doc.pageSizePt(index)

            Rectangle {
                id: box
                anchors.top: parent.top
                width: parent.width
                height: pts.width > 0 ? width * pts.height / pts.width : width * 1.3
                color: "white"
                border.color: list.currentIndex === index ? Theme.accent : "transparent"
                border.width: 2

                Image {
                    anchors.fill: parent
                    source: "image://pages/t" + index + "_" + Screen.devicePixelRatio
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    cache: true
                    asynchronous: true
                }
            }

            Text {
                id: caption
                anchors.top: box.bottom
                anchors.topMargin: 2
                anchors.horizontalCenter: parent.horizontalCenter
                text: index + 1
                color: list.currentIndex === index ? Theme.accent : Theme.mutedForeground
                font.pixelSize: 11
                renderType: Text.QtRendering
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.jumpTo(index + 1, false)
            }
        }

        onCountChanged: {
            if (count > 0 && root.currentPage >= 1)
                positionViewAtIndex(root.currentPage - 1, ListView.Beginning)
        }

        FastWheelScroll {
            flickable: list
        }
    }

    Connections {
        target: root
        function onCurrentPageChanged() {
            const i = root.currentPage - 1
            if (i < 0 || i >= list.count)
                return
            if (list.currentIndex === i)
                return
            list.currentIndex = i
            const item = list.itemAtIndex(i)
            if (item && (item.mapToItem(list, 0, 0).y < 0 ||
                         item.mapToItem(list, 0, item.height).y > list.height))
                list.positionViewAtIndex(i, ListView.Contain)
        }
    }
}
