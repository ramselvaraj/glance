import QtQuick
import glance

Item {
    id: pg

    property int page: 0
    property real zoom: 1.0
    property real ptW: 612
    property real ptH: 792
    property int rotationAngle: 0
    property real dpr: Screen.devicePixelRatio

    // PDF theme: recolor the page to the system theme (documents only; photos
    // are left alone).
    readonly property bool recolor: Theme.pdfTheme && !root.isImageDoc

    readonly property bool sideways: (rotationAngle === 90 || rotationAngle === 270)
    // Round UP so the texture is never smaller than what is displayed; a
    // bucket below the display scale is what makes pages look soft.
    readonly property int bucketExp: Math.max(-3, Math.min(10,
        Math.ceil(Math.log(Math.max(zoom, 0.2)) / Math.log(1.25) - 1e-6)))
    property int renderedExp: 2
    property bool ready: false

    width: parent ? parent.width : 0
    height: (sideways ? ptW : ptH) * zoom

    // The parent Loader virtualizes pages outside the viewport.
    readonly property string requestedSource: (page >= 0 && ready)
        ? "image://pages/p" + page + "_" + renderedExp + "_" + dpr + "_" + Doc.generation
        : ""
    property string shownSource: ""

    onRequestedSourceChanged: {
        if (requestedSource === "")
            shownSource = ""
    }

    // Re-render as the bucket changes, but only one render in flight per page:
    // if a render is still loading, remember the target and apply it when the
    // current one finishes. Keeps the page crisp during a pinch without
    // queueing a storm of renders.
    onZoomChanged: maybeUpgrade()
    property int pendingExp: -1

    function maybeUpgrade() {
        const target = bucketExp
        // Too small -> re-render immediately. Too large (over ~1.5x) -> shrink
        // to save memory; between the two keep the existing texture.
        if (target === renderedExp || (target < renderedExp && renderedExp - target < 2)) {
            pendingExp = -1
            return
        }
        if (sharp.status === Image.Loading) {
            pendingExp = target
        } else {
            renderedExp = target
            pendingExp = -1
        }
    }

    Item {
        id: holder
        anchors.centerIn: parent
        width: ptW * zoom
        height: ptH * zoom
        rotation: rotationAngle
        transformOrigin: Item.Center

        Rectangle {
            anchors.fill: parent
            color: pg.recolor ? "transparent" : "white"
            border.color: pg.recolor ? "transparent" : pg.darkerEdge
            border.width: 1
            antialiasing: true
        }

        // Page pixels. In PDF theme this item is rendered through the recolor
        // shader; otherwise it is drawn directly and the PDF is untouched.
        Item {
            id: pageContent
            anchors.fill: parent
            layer.enabled: pg.recolor
            layer.effect: ShaderEffect {
                property color lightColor: Theme.background
                property color darkColor: Theme.foreground
                property real keepHue: 1.0
                property real paperAlpha: Theme.pdfOpacity
                fragmentShader: "qrc:/shaders/recolor.frag.qsb"
            }

            // Bottom layer: the last texture that finished loading. Keeps the page
            // visible while the next bucket is being rendered, so zooming never
            // blanks. Both layers are visible so Qt keeps their pixmaps alive.
            Image {
                id: base
                anchors.fill: parent
                source: pg.shownSource
                fillMode: Image.PreserveAspectFit
                smooth: true
                cache: true
                asynchronous: true
            }

            // Top layer: the requested texture. Fades in when ready, then becomes
            // the base. Uses a status flag rather than comparing QUrl to string.
            Image {
                id: sharp
                anchors.fill: parent
                source: pg.requestedSource
                fillMode: Image.PreserveAspectFit
                smooth: true
                cache: true
                asynchronous: true
                property bool sharpReady: false
                opacity: sharpReady ? 1 : 0
                onSourceChanged: sharpReady = false
                onStatusChanged: {
                    sharpReady = (status === Image.Ready)
                    if (status !== Image.Loading && pg.pendingExp >= 0
                            && pg.pendingExp !== pg.renderedExp) {
                        pg.renderedExp = pg.pendingExp
                        pg.pendingExp = -1
                    }
                }
                onSharpReadyChanged: if (sharpReady) {
                    pg.shownSource = pg.requestedSource
                }
            }
        }

        // Pictures keep their own colours in PDF theme: the untouched page
        // texture, clipped to each picture's box, drawn over the recolored one.
        Repeater {
            model: pg.recolor ? pg.colorRects : []
            delegate: Item {
                x: modelData.x * pg.zoom
                y: modelData.y * pg.zoom
                width: modelData.w * pg.zoom
                height: modelData.h * pg.zoom
                clip: true
                Image {
                    x: -parent.x
                    y: -parent.y
                    width: holder.width
                    height: holder.height
                    source: pg.shownSource
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    cache: true
                }
            }
        }

        // Clickable links (page point coords -> item coords).
        Repeater {
            model: pg.links
            delegate: Rectangle {
                x: modelData.x * pg.zoom
                y: modelData.y * pg.zoom
                width: modelData.w * pg.zoom
                height: modelData.h * pg.zoom
                color: linkHover.hovered ? Theme.accent : "transparent"
                opacity: linkHover.hovered ? 0.22 : 1
                HoverHandler {
                    id: linkHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: root.followLink(modelData)
                }
            }
        }

        // Search hit highlights (page point coords -> item coords).
        Repeater {
            model: (root.searchPage === pg.page) ? root.searchBoxes : []
            delegate: Rectangle {
                x: modelData.x * pg.zoom
                y: modelData.y * pg.zoom
                width: modelData.w * pg.zoom
                height: modelData.h * pg.zoom
                color: Theme.accent
                opacity: 0.35
            }
        }

        // Read aloud: paragraphs that are skipped fade out; the paragraph being
        // read gets a soft band and the sentence a stronger one.
        Repeater {
            model: root.readActive && root.readMode === "doc" ? (root.readSkipBoxes[pg.page] || []) : []
            delegate: Rectangle {
                x: modelData.x * pg.zoom - 2
                y: modelData.y * pg.zoom - 1
                width: modelData.w * pg.zoom + 4
                height: modelData.h * pg.zoom + 2
                color: Theme.darkerBackground
                opacity: 0.4
            }
        }
        Repeater {
            model: root.readActive ? (root.readParaBoxes[pg.page] || []) : []
            delegate: Rectangle {
                x: modelData.x * pg.zoom - 3
                y: modelData.y * pg.zoom - 1
                width: modelData.w * pg.zoom + 6
                height: modelData.h * pg.zoom + 2
                radius: 2
                color: Theme.accent
                opacity: 0.13
            }
        }
        Repeater {
            model: root.readActive ? (root.readSentBoxes[pg.page] || []) : []
            delegate: Rectangle {
                x: modelData.x * pg.zoom - 1
                y: modelData.y * pg.zoom - 1
                width: modelData.w * pg.zoom + 2
                height: modelData.h * pg.zoom + 2
                radius: 2
                color: Theme.accent
                opacity: 0.4
            }
        }

        Repeater {
            model: root.selectionBoxesForPage(pg.page)
            delegate: Rectangle {
                x: modelData.x * pg.zoom
                y: modelData.y * pg.zoom
                width: modelData.w * pg.zoom
                height: modelData.h * pg.zoom
                color: Theme.accent
                opacity: 0.5
            }
        }


    }

    function mapViewPointToPage(viewPoint) {
        const p = holder.mapFromItem(view, viewPoint.x, viewPoint.y)
        return Qt.point(Math.max(0, Math.min(ptW, p.x / zoom)),
                        Math.max(0, Math.min(ptH, p.y / zoom)))
    }

    TapHandler {
        id: clickSelection
        acceptedButtons: Qt.LeftButton
        enabled: !root.spaceHeld
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: (eventPoint, button) => {
            const viewPoint = view.mapFromItem(pg, eventPoint.position.x,
                                              eventPoint.position.y)
            const point = pg.mapViewPointToPage(viewPoint)
            if (tapCount === 1 && root.readJumpAt(pg.page, point))
                return
            if (tapCount === 1)
                root.clearSelection()
            else if (tapCount === 2)
                root.selectAt(pg.page, point, "word")
            else if (tapCount >= 3)
                root.selectAt(pg.page, point, "line")
        }
    }

    property var links: []
    property var colorRects: []
    function loadColorRects() {
        const info = Doc.cachedPageInfo(page)
        colorRects = info.colorRects || []
    }
    Connections {
        target: Doc
        function onLinksReady(page, list) {
            if (page === pg.page)
                pg.links = list
        }
        function onPageInfoReady(page) {
            if (page === pg.page)
                pg.loadColorRects()
        }
    }

    property color darkerEdge: "#00000000"
    Component.onCompleted: {
        darkerEdge = Theme.darkerBackground
        links = Doc.cachedLinks(page)
        loadColorRects()
        Doc.requestLinks(page)
        renderedExp = bucketExp
        ready = true
        maybeUpgrade()
    }
}
