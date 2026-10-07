import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Dialogs
import glance

Window {
    id: root

    readonly property int pageCount: Doc.pageCount
    readonly property string fileName: Doc.fileName
    readonly property real dpr: Screen.devicePixelRatio

    property real zoom: 1.0
    property int rotation: 0
    property int currentPage: 1
    property bool showThumbs: false
    property bool showOutline: false
    property var outlineModel: []
    property bool searchActive: false
    property bool showKeys: false
    property bool showMenu: false
    property point menuPos: Qt.point(0, 0)
    property var menuItems: []
    // "Select All" spans every page: boxes are produced lazily per visible page,
    // the full text is gathered in the background (Doc.requestAllText).
    property bool selectAllDoc: false
    property bool copyAfterAllText: false
    property var allDocBoxes: ({})
    property bool searchBackward: false
    property bool cmdActive: false
    property int vimCount: 0
    property bool gPending: false
    property string searchText: ""
    property int searchPage: -1
    property var searchBoxes: []
    property var searchResults: []
    property int searchResultIndex: -1
    property int searchMatchCount: 0
    property int searchGeneration: 0
    property bool searchPending: false
    property int searchInitialDirection: 1
    property var selectionPages: ({})
    // Read aloud
    property bool readActive: false
    property string readMode: "doc"          // "doc" = read the document, "selection" = read the selected text
    property bool readAskInstall: false
    property var readAll: null               // every paragraph in reading order (Doc.readingParagraphs)
    property var readList: []                // the paragraphs that are spoken
    property var readSkips: ({ headers: true, footnotes: true, captions: false,
                               tables: false, references: false, math: true })
    property var readParaBoxes: ({})         // page -> boxes of the paragraph being read
    property var readSentBoxes: ({})         // page -> boxes of the sentence being read
    property var readSkipBoxes: ({})         // page -> boxes of paragraphs that are skipped
    property var readSkippedList: []         // the skipped paragraphs themselves (click to include)
    property var readOverrides: ({})         // paragraph key -> "read" | "skip", from the user
    property bool readFollow: true
    property double readScrollStamp: 0       // when the reader last scrolled the view itself
    property bool showSettings: false
    property bool confirmRemoveModel: false
    readonly property bool aiOn: Features.ai
    property bool showVoices: false
    property bool showLyrics: false          // big scrolling sentence view over the document
    property var lyricsModel: []
    property bool showLens: false            // magnifier that follows the word being read
    property var lensWord: null              // {page, x, y, w, h, lineX, lineW, ...} in page points
    property var lensBand: ({ x: 0, y: 0, w: 1, h: 1 })
    property string lensKey: ""
    property string lensSource: ""
    property real lensZ: 3.0
    property string readInstallLog: ""
    property string selectionText: ""
    readonly property bool spaceHeld: Input.spaceHeld && !typing
    property bool pageLayoutReady: false
    property bool pinchPreviewActive: false
    property real pinchPreviewScale: 1.0
    property real pinchAnchorContentX: 0
    property real pinchAnchorContentY: 0
    property real pinchAnchorViewportX: 0
    property real pinchAnchorViewportY: 0
    property int pinchAnchorPage: 0
    property real pinchAnchorPageFraction: 0
    property real pinchAnchorColumnFraction: 0.5
    property var pageTextCapabilities: ({})
    property int ocrGeneration: 0
    property var ocrPages: ({})
    property string ocrStatus: ""
    // A standalone image is recognised whole, once, as soon as it opens (like
    // Live Text); regional hover OCR is only for scanned regions inside PDFs.
    readonly property bool isImageDoc: Doc.filePath !== ""
        && !Doc.filePath.toLowerCase().endsWith(".pdf")
    property int ocrRequestedPage: -1
    property bool ocrExplicit: false
    property var ocrRegionKeys: ({})
    property var pendingOcrRegion: null
    property var nativeHoverCache: ({})
    property var nativePageBoxes: ({})
    property point viewportPointer: Qt.point(-1, -1)
    property var viewportSelection: null
    property bool viewportHasNativeText: false
    property int resumeFullPageOcr: -1
    property string hoverRegionKey: ""
    property int perfFrames: 0
    property var pageSizes: []
    // Layout: the document is a list of rows of 1 (single) or 2 (two-page) pages.
    property int pagesPerRow: 1
    property bool coverAlone: true          // two-page: page 1 sits alone, then pairs
    property var rowOfPage: []              // page -> row
    property var rowFirstPage: []           // row -> first page
    property var rowHUp: []                 // row height in points (upright)
    property var rowHSide: []               // row height in points (rotated 90/270)
    property var rowPrefixHUp: []           // cumulative row heights before row r
    property var rowPrefixHSide: []
    property real maxRowWUp: 612
    property real maxRowWSide: 792
    property real maxRowHUp: 792
    property real maxRowHSide: 612
    readonly property real spreadGap: 8     // points between the two pages of a row

    readonly property real pageGap: 12
    readonly property real pageTopPadding: 12
    readonly property real pageBottomPadding: 16

    color: Theme.pdfTheme ? Qt.rgba(Theme.background.r, Theme.background.g,
                                    Theme.background.b, Theme.pdfOpacity)
                          : Theme.background
    title: fileName !== "" ? fileName + " — glance" : "glance"
    width: InitialWindowWidth
    height: InitialWindowHeight
    minimumWidth: InitialIsImage ? 320 : 480
    minimumHeight: InitialIsImage ? 220 : 300
    visible: true

    // ---------- helpers ----------

    function computeExp(z) {
        return Math.max(-3, Math.min(10, Math.round(Math.log(z) / Math.log(1.25))))
    }

    function prefetchAround() {
        if (pageCount === 0)
            return
        const bucket = Math.pow(1.25, computeExp(zoom)) * dpr
        Doc.prefetch(currentPage - 2, bucket)
        Doc.prefetch(currentPage, bucket)
    }

    function pageTop(page) { // page is 1-based
        return pageTopAt(page - 1, zoom)
    }

    function nowMs() {
        try {
            return performance.now()
        } catch (e) {
            return Date.now()
        }
    }

    // Page under the 40% line of the viewport (the first page of its row).
    function detectPage() {
        const rows = rowFirstPage.length
        if (rows === 0)
            return
        const midY = view.contentY + view.height * 0.4
        let lo = 0, hi = rows - 1
        while (lo < hi) {
            const mid = (lo + hi + 1) >> 1
            if (rowTopAt(mid, zoom) <= midY)
                lo = mid
            else
                hi = mid - 1
        }
        const page = rowFirstPage[lo] + 1
        if (currentPage !== page)
            currentPage = page
    }

    // Jump list so a followed link (or a :goto) can be undone with Ctrl+o.
    property var navBack: []

    function pushNav() {
        if (pageCount === 0)
            return
        navBack = navBack.concat([viewPosition()]).slice(-50)
    }

    function goBack() {
        if (navBack.length === 0)
            return
        const pos = navBack[navBack.length - 1]
        navBack = navBack.slice(0, -1)
        const y = pageTopAt(pos.page, zoom) + pos.offset * sheetHpt(pos.page) * zoom
        view.contentY = Math.max(0, Math.min(y, Math.max(0, view.contentHeight - view.height)))
    }

    function followLink(link) {
        if (link.external) {
            Doc.openExternal(link.uri)
            return
        }
        pushNav()
        const lead = rotation === 0 ? Math.max(0, link.destY - 28) : 0
        const y = pageTopAt(link.page, zoom) + lead * zoom
        view.contentY = Math.max(0, Math.min(y, Math.max(0, view.contentHeight - view.height)))
    }

    function jumpTo(page, animate) {
        if (page < 1 || page > pageCount)
            return
        const y = Math.max(0, Math.min(pageTop(page),
                                       Math.max(0, view.contentHeight - view.height)))
        if (animate) {
            jumpAnim.to = y
            jumpAnim.from = view.contentY
            jumpAnim.restart()
        } else {
            view.contentY = y
        }
    }

    function sideways() {
        return rotation === 90 || rotation === 270
    }
    function sheetHpt(i) { // page height in points, rotation-aware
        const s = pageSizes[i]
        if (!s)
            return 792
        return sideways() ? s.width : s.height
    }
    function sheetWpt(i) { // page width in points, rotation-aware
        const s = pageSizes[i]
        if (!s)
            return 612
        return sideways() ? s.height : s.width
    }
    function colWpt() { return sideways() ? maxRowWSide : maxRowWUp }
    function colHpt() { return sideways() ? maxRowHSide : maxRowHUp }
    function colW(z) { // width of the page column at zoom z
        return colWpt() * z
    }
    function colX(z) { // left edge of the page column in content coords
        const cw = colW(z)
        return (Math.max(view.width, cw) - cw) / 2
    }
    function rowOf(i) {
        return rowOfPage[i] || 0
    }
    function rowHeightPt(r) {
        return (sideways() ? rowHSide[r] : rowHUp[r]) || 792
    }
    function rowPrefixAt(r) { // cumulative row height (pt) before row r
        const arr = sideways() ? rowPrefixHSide : rowPrefixHUp
        return arr[r] || 0
    }
    function rowTopAt(r, z) {
        return pageTopPadding + pageGap * r + rowPrefixAt(r) * z
    }
    function pageTopAt(i, z) { // top of the page's row
        return rowTopAt(rowOf(i), z)
    }
    function rowPages(r) { // [first, endExclusive)
        const first = rowFirstPage[r] || 0
        return [first, r + 1 < rowFirstPage.length ? rowFirstPage[r + 1] : pageCount]
    }
    function pageXAt(i) { // left of the page within the column, in points
        const r = rowOf(i)
        const span = rowPages(r)
        let rowW = spreadGap * (span[1] - span[0] - 1)
        let before = 0
        for (let p = span[0]; p < span[1]; ++p) {
            rowW += sheetWpt(p)
            if (p < i)
                before += sheetWpt(p) + spreadGap
        }
        return (colWpt() - rowW) / 2 + before
    }
    function pageYOffsetPt(i) { // vertical centring inside a row, in points
        return (rowHeightPt(rowOf(i)) - sheetHpt(i)) / 2
    }
    function totalH(z) {
        const rows = rowFirstPage.length
        if (rows === 0)
            return 0
        return pageTopPadding + pageGap * (rows - 1) + rowPrefixAt(rows) * z + pageBottomPadding
    }

    function rebuildLayout() {
        const n = pageCount
        const rows = []
        let i = 0
        if (pagesPerRow === 2 && coverAlone && n > 0) {
            rows.push([0])
            i = 1
        }
        while (i < n) {
            const row = pagesPerRow === 2 && i + 1 < n ? [i, i + 1] : [i]
            rows.push(row)
            i += row.length
        }
        const ofPage = [], first = [], hUp = [], hSide = [], pUp = [0], pSide = [0]
        let wUp = pagesPerRow === 1 ? 612 : 0, wSide = pagesPerRow === 1 ? 792 : 0
        let mhUp = pagesPerRow === 1 ? 792 : 0, mhSide = pagesPerRow === 1 ? 612 : 0
        for (let r = 0; r < rows.length; ++r) {
            let rhUp = 0, rhSide = 0, rwUp = spreadGap * (rows[r].length - 1)
            let rwSide = rwUp
            for (const p of rows[r]) {
                const s = pageSizes[p] || { width: 612, height: 792 }
                ofPage[p] = r
                rhUp = Math.max(rhUp, s.height)
                rhSide = Math.max(rhSide, s.width)
                rwUp += s.width
                rwSide += s.height
            }
            first.push(rows[r][0])
            hUp.push(rhUp)
            hSide.push(rhSide)
            pUp.push(pUp[r] + rhUp)
            pSide.push(pSide[r] + rhSide)
            wUp = Math.max(wUp, rwUp)
            wSide = Math.max(wSide, rwSide)
            mhUp = Math.max(mhUp, rhUp)
            mhSide = Math.max(mhSide, rhSide)
        }
        rowOfPage = ofPage
        rowFirstPage = first
        rowHUp = hUp
        rowHSide = hSide
        rowPrefixHUp = pUp
        rowPrefixHSide = pSide
        maxRowWUp = wUp
        maxRowWSide = wSide
        maxRowHUp = mhUp
        maxRowHSide = mhSide
    }

    // single -> two-page (cover alone) -> two-page (no cover offset) -> single
    function cycleLayout() {
        if (pageCount < 2)
            return
        const pos = viewPosition()
        if (pagesPerRow === 1) {
            pagesPerRow = 2
            coverAlone = true
        } else if (coverAlone) {
            coverAlone = false
        } else {
            pagesPerRow = 1
        }
        rebuildLayout()
        autoFitSinglePage = false
        zoom = fitWidthZoom()
        scrollTimer.page = pos.page
        scrollTimer.offset = pos.offset
        scrollTimer.start()
    }

    function clampZoom(z) {
        return Math.min(8.0, Math.max(0.2, z))
    }
    function anchorFrom(scenePoint, fallbackPoint) {
        // Scene -> view(Flickable) coordinates; falls back to the handler's
        // own reported position if scene mapping is unavailable.
        try {
            const p = view.mapFromItem(null, scenePoint)
            if (p && isFinite(p.x) && isFinite(p.y))
                return p
        } catch (e) {
        }
        return fallbackPoint
    }

    // Zoom keeping the content under (ax, ay) — viewport coordinates — fixed.
    function zoomAt(ax, ay, f) {
        const z0 = zoom
        const z1 = clampZoom(z0 * f)
        if (z1 === z0)
            return

        const cx = view.contentX + ax
        const cy = view.contentY + ay

        // Horizontal: the page column scales uniformly, so preserve the
        // fraction across the column (accounts for centering when the column
        // is narrower than the viewport).
        const cw0 = colW(z0)
        const fx = cw0 > 0 ? (cx - colX(z0)) / cw0 : 0.5

        // Vertical: gaps and padding do not scale, so preserve the page index
        // and the fraction within that page.
        const n = pageCount
        let page = 0
        let u = 0
        if (n > 0) {
            page = n - 1
            for (let i = 0; i < n; ++i) {
                const top = pageTopAt(i, z0)
                const h = sheetHpt(i) * z0
                if (cy < top + h) {
                    page = i
                    u = h > 0 ? (cy - top) / h : 0
                    break
                }
                if (i === n - 1)
                    u = 1
            }
        }

        zoom = z1

        const cw1 = colW(z1)
        const newCx = colX(z1) + fx * cw1
        const newCy = pageTopAt(page, z1) + u * sheetHpt(page) * z1
        const maxX = Math.max(0, Math.max(view.width, cw1) - view.width)
        const maxY = Math.max(0, totalH(z1) - view.height)
        view.contentX = Math.min(Math.max(0, newCx - ax), maxX)
        view.contentY = Math.min(Math.max(0, newCy - ay), maxY)
    }

    property bool initialFitDone: false
    // View state remembered per file: restored once after the first fit, and
    // saving stays off until then so we never overwrite it with page 1.
    property var pendingViewState: Doc.loadViewState()
    property bool viewStateReady: false
    property bool autoFitSinglePage: false
    function fitWidthZoom() {
        return Math.min(8.0, Math.max(0.2, (view.width - 24) / root.colWpt()))
    }

    // Top-of-viewport page index and fraction of that page already scrolled past.
    function viewPosition() {
        let lo = 0, hi = pageCount - 1
        const y = view.contentY
        while (lo < hi) {
            const mid = (lo + hi + 1) >> 1
            if (pageTopAt(mid, zoom) <= y)
                lo = mid
            else
                hi = mid - 1
        }
        const h = sheetHpt(lo) * zoom
        return { page: lo, offset: h > 0 ? Math.max(0, Math.min(1, (y - pageTopAt(lo, zoom)) / h)) : 0 }
    }

    function saveViewState() {
        if (!viewStateReady || pageCount === 0)
            return
        const pos = viewPosition()
        Doc.saveViewState({ page: pos.page, offset: pos.offset, zoom: zoom,
                            fit: Math.abs(zoom - fitWidthZoom()) < 0.005,
                            rotation: rotation, perRow: pagesPerRow, cover: coverAlone })
    }

    function beginViewRestore() {
        const state = pendingViewState
        pendingViewState = null
        if (!state || state.page === undefined) {
            viewStateReady = true
            return
        }
        restoreTimer.state = state
        restoreTimer.start()
    }

    function fitWidth(preserveAutoFit) {
        if (pageCount === 0 || view.width < 100)
            return
        zoom = fitWidthZoom()
        if (!initialFitDone)
            beginViewRestore()
        initialFitDone = true
        if (!preserveAutoFit)
            autoFitSinglePage = false
    }

    function fitPage() {
        if (pageCount === 0)
            return
        const w = (view.width - 24) / colWpt()
        const h = (view.height - 24) / colHpt()
        zoom = Math.min(8.0, Math.max(0.2, Math.min(w, h)))
    }

    function rotateCW() {
        rotation = (rotation + 90) % 360
        view.contentY = Math.max(0, Math.min(view.contentY,
                                             Math.max(0, view.contentHeight - view.height)))
    }

    function toggleFullscreen() {
        if (visibility === Window.FullScreen)
            showNormal()
        else
            showFullScreen()
    }

    function toggleOutline() {
        showOutline = !showOutline
        if (showOutline && outlineModel.length === 0)
            outlineModel = Doc.outline()
    }

    function takeCount() {
        const n = Math.max(1, vimCount)
        vimCount = 0
        return n
    }

    function scrollBy(dx, dy) {
        view.contentY = Math.max(0, Math.min(view.contentY + dy,
                                             Math.max(0, view.contentHeight - view.height)))
        view.contentX = Math.max(0, Math.min(view.contentX + dx,
                                             Math.max(0, view.contentWidth - view.width)))
    }

    function openSearch(backward) {
        searchBackward = backward
        searchActive = true
        searchField.forceActiveFocus()
        searchField.selectAll()
    }

    function openCommand() {
        cmdActive = true
        cmdField.text = ""
        cmdField.forceActiveFocus()
    }

    function closeCommand() {
        cmdActive = false
        cmdField.text = ""
        view.forceActiveFocus()
    }

    // Accepts a printed page label ("40", "iv"), a plain page number, or a
    // relative move (+5 / -3). Printed labels win, so ":40" matches the number
    // printed on the page and the document's own contents list.
    function gotoPage(text) {
        const t = text.trim()
        if (t === "" || pageCount === 0)
            return false
        let target = -1
        if (/^[+-]\d+$/.test(t)) {
            target = currentPage - 1 + parseInt(t)
        } else if (t === "$") {
            target = pageCount - 1
        } else {
            target = Doc.pageForLabel(t)
            if (target < 0 && /^\d+$/.test(t))
                target = parseInt(t) - 1
        }
        if (target < 0 || target >= pageCount)
            return false
        pushNav()
        jumpTo(target + 1, false)
        return true
    }

    function pageIndicator() {
        if (pageCount <= 0)
            return ""
        const labelsVersion = Doc.labelsVersion      // re-evaluate when labels finish loading
        const label = Doc.pageLabel(currentPage - 1)
        const position = currentPage + " / " + pageCount
        return label !== "" && label !== String(currentPage)
            ? label + "  ·  " + position : position
    }

    function closeSearch() {
        searchActive = false
        searchText = ""
        searchPage = -1
        searchBoxes = []
        searchResults = []
        searchResultIndex = -1
        searchMatchCount = 0
        searchPending = false
        searchGeneration += 1
        Doc.cancelSearch()
    }

    function showSearchResult(index) {
        if (searchResults.length === 0)
            return
        searchResultIndex = (index + searchResults.length) % searchResults.length
        const result = searchResults[searchResultIndex]
        searchPage = result.page
        searchBoxes = result.boxes
        jumpTo(result.page + 1, false)
    }

    function runSearch(reset, direction) {
        if (Doc.pageCount === 0 || searchText.length < 1)
            return
        if (reset || searchResults.length === 0) {
            searchPending = true
            searchInitialDirection = direction || 1
            searchGeneration += 1
            Doc.cancelSearch()
            Doc.searchAllAsync(searchText, searchGeneration)
        } else {
            showSearchResult(searchResultIndex + (direction || 1))
        }
    }

    Connections {
        target: Doc
        function onSearchFinished(generation, results) {
            if (generation !== root.searchGeneration || !root.searchActive)
                return
            root.searchPending = false
            const merged = results.slice()
            const query = root.searchText.toLowerCase()
            const ocrPageKeys = Object.keys(root.ocrPages)
            for (let i = 0; i < ocrPageKeys.length; ++i) {
                const page = Number(ocrPageKeys[i])
                if (root.ocrPages[page].embeddedText)
                    continue
                const words = root.ocrPages[page].words.filter(
                    word => word.text.toLowerCase().includes(query))
                if (words.length > 0)
                    merged.push({ page: page, boxes: words, count: words.length })
            }
            merged.sort((a, b) => a.page - b.page)
            root.searchResults = merged
            root.searchMatchCount = 0
            for (let i = 0; i < merged.length; ++i)
                root.searchMatchCount += merged[i].count
            if (merged.length === 0) {
                root.searchPage = -1
                root.searchBoxes = []
                root.searchResultIndex = -1
                return
            }
            let nearest = root.searchInitialDirection < 0 ? merged.length - 1 : 0
            if (root.searchInitialDirection < 0) {
                for (let i = merged.length - 1; i >= 0; --i) {
                    if (merged[i].page <= root.currentPage - 1) {
                        nearest = i
                        break
                    }
                }
            } else {
                for (let i = 0; i < merged.length; ++i) {
                    if (merged[i].page >= root.currentPage - 1) {
                        nearest = i
                        break
                    }
                }
            }
            root.showSearchResult(nearest)
        }
    }

    function copyPage() {
        if (selectAllDoc && selectionText === "") {
            // Whole-document text is still being gathered; copy as soon as it's ready.
            copyAfterAllText = true
            setStatus("preparing text of all pages…")
            return
        }
        if (selectionText !== "" && Doc.copyTextToClipboard(selectionText)) {
            if (selectAllDoc)
                setStatus("copied " + selectionText.length + " characters")
            return
        }
        if (pageCount > 0 && Doc.copyPageToClipboard(currentPage - 1))
            console.log("glance: page copied to clipboard")
    }

    // Header status line, cleared after a few seconds.
    function setStatus(text) {
        ocrStatus = text
        statusClearTimer.restart()
    }


    // ---------- read aloud ----------

    function readKindSkipped(kind) {
        switch (kind) {
        case "header": case "footer": case "pagenum": case "aside": return readSkips.headers
        case "footnote": return readSkips.footnotes
        case "caption": return readSkips.captions
        case "table": return readSkips.tables
        case "reference": return readSkips.references
        case "math": return readSkips.math
        }
        return false
    }

    function readPush(map, key, value) {
        if (!map[key])
            map[key] = []
        map[key].push(value)
    }

    function readKey(p) {
        return p.page + ":" + Math.round(p.boxes[0].y) + ":" + p.text.slice(0, 16)
    }

    function readLoadAll() {
        const all = Doc.readingParagraphs({})
        for (let i = 0; i < all.length; ++i)
            all[i].id = i
        readAll = all
    }

    // Splits the document's paragraphs into what is spoken and what is skipped.
    // The user's own include/skip choices win over the classifier.
    function readRebuildList() {
        const list = []
        const skippedList = []
        const skipped = {}
        for (const p of readAll) {
            const choice = readOverrides[readKey(p)]
            const skip = choice ? choice === "skip" : readKindSkipped(p.kind)
            if (skip) {
                skippedList.push(p)
                for (const b of p.boxes)
                    readPush(skipped, b.page, b)
            } else {
                list.push(p)
            }
        }
        readList = list
        readSkippedList = skippedList
        readSkipBoxes = skipped
    }

    // First spoken paragraph at or after a position (page, y in points).
    function readIndexAt(page, y) {
        for (let i = 0; i < readList.length; ++i) {
            const b = readList[i].boxes[0]
            if (b.page > page || (b.page === page && b.y >= y - 1))
                return i
        }
        return Math.max(0, readList.length - 1)
    }

    // The user says a paragraph is (not) worth reading: remember it for this
    // document and teach the classifier, which re-judges the whole document.
    function readCorrect(p, read) {
        const next = read ? p.boxes[0] : (readList[readList.indexOf(p) + 1]
                                          ? readList[readList.indexOf(p) + 1].boxes[0] : null)
        const overrides = Object.assign({}, readOverrides)
        overrides[readKey(p)] = read ? "read" : "skip"
        readOverrides = overrides
        // Only free-floating text (margin notes and the like) is the classifier's call.
        if (p.features && (!read || p.kind === "aside"))
            Doc.teachReading(p.features, !read)
        readLoadAll()
        readRebuildList()
        if (readList.length === 0) {
            closeReader()
            return
        }
        const idx = next ? readIndexAt(next.page, next.y) : readList.length - 1
        readFollow = true
        Reader.start(readList.map(x => x.text), idx)
    }

    function readSkipCurrent() {
        if (!readActive || readMode !== "doc")
            return
        const p = readList[Reader.paragraph]
        if (p)
            readCorrect(p, false)
    }

    // First spoken paragraph at or below the top of the viewport.
    function readStartIndex() {
        const pos = viewPosition()
        const size = pageSizes[pos.page]
        const y0 = pos.offset * (size ? size.height : 792)
        for (let i = 0; i < readList.length; ++i) {
            const boxes = readList[i].boxes
            const last = boxes[boxes.length - 1]
            if (last.page > pos.page || (last.page === pos.page && last.y + last.h > y0 + 4))
                return i
        }
        return 0
    }

    function startReading() {
        if (pageCount === 0)
            return
        if (!aiOn) {
            showSettings = true
            return
        }
        if (!Reader.available) {
            readActive = true
            readAskInstall = true
            return
        }
        readAskInstall = false
        showVoices = false
        if (selectionText !== "" && !selectAllDoc) {
            const text = selectionText.replace(/-\n(?=[a-z])/g, "").replace(/\s+/g, " ").trim()
            if (text === "")
                return
            readMode = "selection"
            readList = [{ text: text, kind: "body", page: 0, boxes: [] }]
            readSkipBoxes = ({})
            readActive = true
            Reader.start([text], 0)
            return
        }
        readMode = "doc"
        if (readAll === null)
            readLoadAll()
        readRebuildList()
        if (readList.length === 0) {
            setStatus("no readable text in this document")
            readActive = false
            return
        }
        readActive = true
        readFollow = true
        Reader.start(readList.map(p => p.text), selectAllDoc ? 0 : readStartIndex())
    }

    function closeReader() {
        Reader.stop()
        showLyrics = false
        showLens = false
        readActive = false
        readAskInstall = false
        showVoices = false
        readParaBoxes = ({})
        readSentBoxes = ({})
        readSkipBoxes = ({})
        readSkippedList = []
        readOverrides = ({})
    }

    function toggleReading() {
        if (!aiOn) {
            showSettings = true
            return
        }
        if (!readActive)
            startReading()
        else if (Reader.state === 0)
            startReading()
        else
            Reader.toggle()
    }

    function readToggleSkip(key) {
        const s = Object.assign({}, readSkips)
        s[key] = !s[key]
        readSkips = s
        if (!readActive || readMode !== "doc" || readAll === null)
            return
        const cur = readList[Reader.paragraph]
        readRebuildList()
        let idx = 0
        if (cur)
            idx = readIndexAt(cur.boxes[0].page, cur.boxes[0].y)
        if (readList.length === 0) {
            closeReader()
            return
        }
        Reader.start(readList.map(p => p.text), idx)
    }

    // A click on a paragraph while reading continues from there.
    function readJumpAt(page, pt) {
        if (!readActive || readMode !== "doc")
            return false
        for (const p of readSkippedList) {
            for (const b of p.boxes) {
                if (b.page === page && pt.x >= b.x - 3 && pt.x <= b.x + b.w + 3
                        && pt.y >= b.y - 2 && pt.y <= b.y + b.h + 2) {
                    readCorrect(p, true)
                    return true
                }
            }
        }
        for (let i = 0; i < readList.length; ++i) {
            for (const b of readList[i].boxes) {
                if (b.page === page && pt.x >= b.x - 3 && pt.x <= b.x + b.w + 3
                        && pt.y >= b.y - 2 && pt.y <= b.y + b.h + 2) {
                    readFollow = true
                    Reader.jumpToParagraph(i)
                    return true
                }
            }
        }
        return false
    }

    function readUpdateHighlight() {
        if (!readActive || readMode !== "doc" || Reader.paragraph < 0
                || Reader.paragraph >= readList.length) {
            readParaBoxes = ({})
            readSentBoxes = ({})
            return
        }
        const p = readList[Reader.paragraph]
        const para = {}
        for (const b of p.boxes)
            readPush(para, b.page, b)
        const ranges = Reader.sentenceRanges(p.text)
        const r = ranges[Reader.sentence]
        const sent = {}
        let first = null
        if (r) {
            for (const b of p.boxes) {
                const s = Math.max(r.start, b.start)
                const e = Math.min(r.end, b.end)
                if (e <= s)
                    continue
                const len = Math.max(1, b.end - b.start)
                const piece = { x: b.x + b.w * (s - b.start) / len, y: b.y,
                                w: b.w * (e - s) / len, h: b.h }
                readPush(sent, b.page, piece)
                if (first === null)
                    first = { page: b.page, y: b.y }
            }
        }
        readParaBoxes = para
        readSentBoxes = sent
        if (readFollow && first !== null)
            readScrollTo(first.page, first.y)
    }

    function readScrollTo(page, boxY) {
        const y = pageTopAt(page, zoom) + (rotation === 0 ? boxY * zoom : 0)
        const top = view.contentY
        const h = view.height
        if (y > top + h * 0.1 && y < top + h * 0.65)
            return
        const maxY = Math.max(0, view.contentHeight - h)
        const target = Math.max(0, Math.min(y - h * 0.22, maxY))
        if (Math.abs(target - view.contentY) < 2)
            return
        readScrollStamp = Date.now()
        readScrollAnim.stop()
        readScrollAnim.from = view.contentY
        readScrollAnim.to = target
        readScrollAnim.start()
    }

    function readFinished() {
        if (readMode === "doc")
            setStatus("finished reading")
        closeReader()
    }

    function readVoiceLabel(v) {
        const m = /^([ab])([fm])_(.+)$/.exec(v)
        if (!m)
            return v
        return m[3].charAt(0).toUpperCase() + m[3].slice(1) + " · "
               + (m[1] === "a" ? "US" : "UK") + " " + (m[2] === "f" ? "F" : "M")
    }

    function readVoiceList() {
        const out = []
        for (const v of Reader.voices)
            if (/^[ab][fm]_/.test(v))
                out.push(v)
        return out.length > 0 ? out : ["af_heart", "af_bella", "am_adam", "bf_emma", "bm_george"]
    }


    // Lyrics view: the spoken sentences, one line each, with the current one large.
    function lyricsRebuild() {
        lyricsModel = Reader.units()
    }

    function lyricsAccentB() {
        const c = Theme.accent
        const h = c.hslHue < 0 ? 0 : c.hslHue
        return Qt.hsla((h + 0.12) % 1, Math.min(1, c.hslSaturation * 1.05),
                       Math.min(0.82, c.hslLightness + 0.14), 1)
    }

    // The current sentence, coloured character by character from the accent to a
    // lighter neighbouring hue, so it reads as a gradient along the reading order.
    function lyricsGradientHtml(text) {
        const a = Theme.accent
        const b = lyricsAccentB()
        const n = text.length
        let out = ""
        for (let i = 0; i < n; ++i) {
            const ch = text[i]
            if (ch === " ") {
                out += " "
                continue
            }
            const t = n > 1 ? i / (n - 1) : 0
            const c = Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                              a.b + (b.b - a.b) * t, 1)
            const esc = ch === "&" ? "&amp;" : (ch === "<" ? "&lt;" : (ch === ">" ? "&gt;" : ch))
            out += '<font color="' + c.toString() + '">' + esc + "</font>"
        }
        return out
    }

    function lyricsFollow(animate) {
        const idx = Reader.unit
        if (idx < 0 || idx >= lyricsModel.length)
            return
        const it = lyricsList.itemAtIndex(idx)
        if (!it) {
            lyricsList.positionViewAtIndex(idx, ListView.Center)
            return
        }
        const target = Math.max(0, it.y + it.height / 2 - lyricsList.height * 0.42)
        if (!animate) {
            lyricsList.contentY = target
            return
        }
        lyricsScroll.stop()
        lyricsScroll.from = lyricsList.contentY
        lyricsScroll.to = target
        lyricsScroll.start()
    }

    function toggleLyrics() {
        if (!readActive || readAskInstall)
            return
        showLyrics = !showLyrics
        if (showLyrics) {
            lyricsRebuild()
            lyricsFollow(false)
        }
    }


    // ---------- magnifier ----------

    // Where in a sentence the voice is: characters weighted so that commas and
    // full stops, which the voice pauses on, take longer than letters.
    function lensCharAt(text, progress) {
        const n = text.length
        if (n === 0)
            return 0
        const cumulative = []
        let total = 0
        for (let i = 0; i < n; ++i) {
            const c = text[i]
            let w = 1
            if (c === " ")
                w = 0.6
            else if (",;:".indexOf(c) >= 0)
                w = 4
            else if (".!?".indexOf(c) >= 0 && (i === n - 1 || text[i + 1] === " "))
                w = 7
            total += w
            cumulative.push(total)
        }
        const target = Math.max(0, Math.min(1, (progress - 0.03) / 0.93)) * total
        for (let i = 0; i < n; ++i)
            if (cumulative[i] >= target)
                return i
        return n - 1
    }

    function lensUpdate() {
        if (!showLens || !readActive || readMode !== "doc" || Reader.state === 0)
            return
        const p = readList[Reader.paragraph]
        if (!p || p.id === undefined)
            return
        const r = Reader.sentenceRanges(p.text)[Reader.sentence]
        if (!r)
            return
        const offset = r.start + lensCharAt(p.text.slice(r.start, r.end), Reader.unitProgress)
        const w = Doc.readingWordAt(p.id, offset)
        if (w.page === undefined)
            return
        lensWord = w
        const key = p.id + ":" + w.line
        if (key === lensKey)
            return
        lensKey = key
        const size = pageSizes[w.page] || { width: 612, height: 792 }
        const bx = 0
        const by = Math.max(0, w.y - w.h * 1.9)
        const bw = size.width
        const bh = Math.min(size.height - by, w.h * 4.8)
        lensBand = { x: bx, y: by, w: bw, h: bh }
        lensSource = "image://pages/r" + w.page + "_" + (lensZ * dpr).toFixed(2) + "_"
                     + bx.toFixed(1) + "_" + by.toFixed(1) + "_" + bw.toFixed(1) + "_"
                     + bh.toFixed(1) + "_" + Doc.generation
    }

    function toggleLens() {
        if (!readActive || readAskInstall)
            return
        showLens = !showLens
        lensKey = ""
        if (showLens)
            lensUpdate()
    }

    function readStepSpeed(dir) {
        const steps = [0.75, 1.0, 1.25, 1.5, 1.75, 2.0]
        let i = 0
        for (let k = 0; k < steps.length; ++k)
            if (Math.abs(steps[k] - Reader.speed) < 0.01)
                i = k
        Reader.speed = steps[Math.max(0, Math.min(steps.length - 1, i + dir))]
    }

    function readRemainingText() {
        if (readMode !== "doc")
            return ""
        let chars = 0
        for (let i = Math.max(0, Reader.paragraph); i < readList.length; ++i)
            chars += readList[i].text.length
        const mins = Math.round(chars / (900 * Reader.speed))
        return mins < 1 ? "<1 min left" : "~" + mins + " min left"
    }

    NumberAnimation {
        id: readScrollAnim
        target: view
        property: "contentY"
        duration: 380
        easing.type: Easing.OutCubic
    }

    Connections {
        target: Features
        function onAiChanged() {
            if (!Features.ai) {
                root.closeReader()
                root.showLyrics = false
                root.showLens = false
                root.readAll = null
            }
        }
    }

    Connections {
        target: Reader
        function onProgressChanged() { root.lensUpdate() }
        function onPositionChanged() {
            root.readUpdateHighlight()
            root.lensUpdate()
            if (root.showLyrics)
                root.lyricsFollow(true)
        }
        function onUnitsChanged() {
            if (root.showLyrics) {
                root.lyricsRebuild()
                root.lyricsFollow(false)
            }
        }
        function onFinished() { root.readFinished() }
        function onErrorOccurred(message) { root.setStatus("read aloud: " + message) }
        function onInstallOutput(line) { root.readInstallLog = line }
        function onInstallFinished(ok) {
            if (ok) {
                root.readAskInstall = false
                root.startReading()
            } else {
                root.readInstallLog = "Install failed. " + root.readInstallLog
            }
        }
    }

    // Select All: every text run on every page (native text, or OCR text on
    // pages that have none).
    function selectAllDocument() {
        if (pageCount === 0)
            return
        allDocBoxes = ({})
        selectAllDoc = true
        selectionText = ""
        const marker = {}
        marker["*"] = true          // changes selectionPages so page overlays refresh
        selectionPages = marker
        Doc.requestAllText()
    }

    function composeAllText(pages) {
        const out = []
        for (let p = 0; p < pages.length; ++p) {
            let text = pages[p]
            if (text === "" && ocrPages[p])
                text = ocrText(sortedOcrWords(p))
            if (text !== "")
                out.push(text)
        }
        return out.join("\n\n")
    }

    // Highlight boxes of one page while Select All spans the document.
    function allDocBoxesForPage(page) {
        if (allDocBoxes[page] !== undefined)
            return allDocBoxes[page]
        const info = Doc.cachedPageInfo(page)
        if (info.hasText === undefined) {
            Doc.requestPageInfo(page)        // pageInfoReady refreshes the highlight
            return ocrPages[page] ? sortedOcrWords(page) : []
        }
        let boxes = info.boxes
        if (boxes.length === 0 && ocrPages[page])
            boxes = sortedOcrWords(page)
        allDocBoxes[page] = boxes     // plain cache; no binding depends on it
        return boxes
    }

    // Select All OCR Text: the words recognised by OCR on the current page, e.g.
    // a screenshot inside a PDF, or a whole standalone image.
    function selectAllOcrOnPage() {
        if (pageCount === 0)
            return
        const page = Math.max(0, currentPage - 1)
        const words = sortedOcrWords(page)
        if (words.length === 0) {
            setStatus(Ocr.running ? "still reading this page…"
                                  : "no OCR text on this page yet")
            return
        }
        selectAllDoc = false
        const pages = {}
        pages[page] = words
        selectionPages = pages
        selectionText = ocrText(words)
    }

    function clearSelection() {
        selectAllDoc = false
        copyAfterAllText = false
        selectionPages = ({})
        selectionText = ""
    }

    function pointInsideOcrWord(page, point) {
        const words = sortedOcrWords(page)
        for (let i = 0; i < words.length; ++i) {
            const word = words[i]
            if (point.x >= word.x && point.x <= word.x + word.w
                    && point.y >= word.y && point.y <= word.y + word.h)
                return i
        }
        return -1
    }

    function updateSelection(anchorPage, anchor, focusPage, focus, source) {
        selectAllDoc = false
        if (source === "ocr" && anchorPage === focusPage && ocrPages[anchorPage]) {
            const characters = ocrCharacters(anchorPage)
            if (characters.length > 0) {
                const startChar = nearestOcrCharacter(characters, anchor)
                const endChar = nearestOcrCharacter(characters, focus)
                if (startChar.index >= 0 && endChar.index >= 0) {
                    const firstChar = Math.min(startChar.index, endChar.index)
                    const lastChar = Math.max(startChar.index, endChar.index)
                    const selectedChars = characters.slice(firstChar, lastChar + 1)
                    const charPages = {}
                    charPages[anchorPage] = selectedChars
                    selectionPages = charPages
                    selectionText = ocrCharacterText(selectedChars)
                    return
                }
            }
            const words = sortedOcrWords(anchorPage)
            const start = nearestOcrWord(words, anchor)
            const end = nearestOcrWord(words, focus)
            if (start.index >= 0 && end.index >= 0) {
                const first = Math.min(start.index, end.index)
                const last = Math.max(start.index, end.index)
                const selected = words.slice(first, last + 1)
                const pages = {}
                pages[anchorPage] = selected
                selectionPages = pages
                selectionText = ocrText(selected)
                return
            }
        }
        const result = Doc.selectTextRange(anchorPage, anchor, focusPage, focus)
        const pages = {}
        for (let i = 0; i < result.pages.length; ++i)
            pages[result.pages[i].page] = result.pages[i].boxes
        selectionPages = pages
        selectionText = result.text
    }

    function selectionBoxesForPage(page) {
        if (selectAllDoc && selectionPages["*"] === true)
            return allDocBoxesForPage(page)
        return selectionPages[page] || []
    }

    function sortedOcrWords(page) {
        if (!ocrPages[page])
            return []
        return ocrPages[page].words.slice().sort((a, b) =>
            a.block !== b.block ? a.block - b.block
            : a.paragraph !== b.paragraph ? a.paragraph - b.paragraph
            : a.line !== b.line ? a.line - b.line
            : a.word - b.word)
    }

    function nearestOcrWord(words, point) {
        let nearest = -1
        let best = Number.MAX_VALUE
        for (let i = 0; i < words.length; ++i) {
            const word = words[i]
            const dx = point.x < word.x ? word.x - point.x
                     : point.x > word.x + word.w ? point.x - word.x - word.w : 0
            const dy = point.y < word.y ? word.y - point.y
                     : point.y > word.y + word.h ? point.y - word.y - word.h : 0
            const distance = dx * dx + dy * dy * 2
            if (distance < best) {
                best = distance
                nearest = i
            }
        }
        return { index: nearest, distance: best }
    }

    function ocrCharacters(page) {
        const words = sortedOcrWords(page)
        const characters = []
        for (let i = 0; i < words.length; ++i) {
            const chars = words[i].chars || []
            for (let j = 0; j < chars.length; ++j) {
                const character = Object.assign({}, chars[j])
                character.wordIndex = i
                character.block = words[i].block
                character.paragraph = words[i].paragraph
                character.line = words[i].line
                character.charIndex = j
                characters.push(character)
            }
        }
        return characters
    }

    function nearestOcrCharacter(characters, point) {
        let nearest = -1
        let best = Number.MAX_VALUE
        for (let i = 0; i < characters.length; ++i) {
            const character = characters[i]
            const dx = point.x < character.x ? character.x - point.x
                     : point.x > character.x + character.w
                       ? point.x - character.x - character.w : 0
            const dy = point.y < character.y ? character.y - point.y
                     : point.y > character.y + character.h
                       ? point.y - character.y - character.h : 0
            const distance = dx * dx + dy * dy * 2
            if (distance < best) {
                best = distance
                nearest = i
            }
        }
        return { index: nearest, distance: best }
    }

    function ocrCharacterText(characters) {
        let text = ""
        for (let i = 0; i < characters.length; ++i) {
            if (i > 0 && characters[i - 1].wordIndex !== characters[i].wordIndex) {
                const previous = characters[i - 1]
                text += previous.block !== characters[i].block
                     || previous.paragraph !== characters[i].paragraph
                     || previous.line !== characters[i].line ? "\n" : " "
            }
            text += characters[i].text
        }
        return text
    }

    function ocrText(words) {
        let text = ""
        for (let i = 0; i < words.length; ++i) {
            if (i > 0) {
                const previous = words[i - 1]
                text += previous.block !== words[i].block
                     || previous.paragraph !== words[i].paragraph
                     || previous.line !== words[i].line ? "\n" : " "
            }
            text += words[i].text
        }
        return text
    }

    // Double-click granularity: the run of identifier characters (or of
    // punctuation) under the pointer, not the whole whitespace-separated token.
    // An apostrophe between letters and a '.' between digits stay inside a word.
    function charClass(c, prev, next) {
        if (/[\p{L}\p{N}_]/u.test(c))
            return 1
        if ((c === "'" || c === "\u2019") && prev && next
                && /[\p{L}]/u.test(prev) && /[\p{L}]/u.test(next))
            return 1
        if (c === "." && prev && next && /\d/.test(prev) && /\d/.test(next))
            return 1
        return /\s/.test(c) ? 0 : 2
    }

    function subWordAt(word, point) {
        const chars = word.chars || []
        if (chars.length === 0)
            return word
        let hit = 0
        let best = Number.MAX_VALUE
        for (let i = 0; i < chars.length; ++i) {
            const dx = point.x < chars[i].x ? chars[i].x - point.x
                     : point.x > chars[i].x + chars[i].w ? point.x - chars[i].x - chars[i].w : 0
            if (dx < best) {
                best = dx
                hit = i
            }
        }
        const cls = i => charClass(chars[i].text, i > 0 ? chars[i - 1].text : "",
                                   i < chars.length - 1 ? chars[i + 1].text : "")
        const kind = cls(hit)
        let from = hit
        let to = hit
        while (from > 0 && cls(from - 1) === kind)
            --from
        while (to < chars.length - 1 && cls(to + 1) === kind)
            ++to
        if (from === 0 && to === chars.length - 1)
            return word
        const run = chars.slice(from, to + 1)
        const left = Math.min(...run.map(c => c.x))
        const top = Math.min(...run.map(c => c.y))
        const right = Math.max(...run.map(c => c.x + c.w))
        const bottom = Math.max(...run.map(c => c.y + c.h))
        return { text: run.map(c => c.text).join(""), x: left, y: top, w: right - left,
                 h: bottom - top, block: word.block, paragraph: word.paragraph,
                 line: word.line, word: word.word, chars: run }
    }

    function selectAt(page, point, mode) {
        if (ocrPages[page]) {
            const words = sortedOcrWords(page)
            const nearest = nearestOcrWord(words, point)
            const hit = nearest.index
            if (hit < 0 && !ocrPages[page].embeddedText) {
                clearSelection()
                return
            }
            const closeEnough = hit >= 0 && nearest.distance <= 64
            if (closeEnough) {
                const chosen = mode === "line"
                ? words.filter(word => {
                    const center = word.y + word.h / 2
                    const target = words[hit]
                    return center >= target.y && center <= target.y + target.h
                })
                    : [subWordAt(words[hit], point)]
                const pages = {}
                pages[page] = chosen
                selectionPages = pages
                selectionText = ocrText(chosen)
                return
            }
        }
        const result = Doc.selectTextAt(page, point, mode)
        const capabilities = Object.assign({}, pageTextCapabilities)
        capabilities[page] = result.text !== ""
        pageTextCapabilities = capabilities
        const pages = {}
        pages[page] = result.boxes
        selectionPages = pages
        selectionText = result.text
    }

    function selectionPoint(viewPoint) {
        if (pageCount === 0)
            return null
        const contentY = view.contentY + viewPoint.y
        const contentX = view.contentX + viewPoint.x
        const rows = rowFirstPage.length
        // Row under (or nearest to) the pointer.
        let lo = 0, hi = rows - 1
        while (lo < hi) {
            const mid = (lo + hi + 1) >> 1
            if (rowTopAt(mid, zoom) <= contentY)
                lo = mid
            else
                hi = mid - 1
        }
        let row = lo
        const rowBottom = rowTopAt(row, zoom) + rowHeightPt(row) * zoom
        if (contentY > rowBottom && row + 1 < rows
                && rowTopAt(row + 1, zoom) - contentY < contentY - rowBottom)
            row += 1
        // Page within the row: nearest horizontally.
        const span = rowPages(row)
        let page = span[0]
        let best = Number.MAX_VALUE
        for (let p = span[0]; p < span[1]; ++p) {
            const it = pageRepeater.itemAt(p)
            if (!it)
                continue
            const left = pagesCol.x + it.x
            const right = left + it.width
            const dx = contentX < left ? left - contentX : (contentX > right ? contentX - right : 0)
            if (dx < best) {
                best = dx
                page = p
            }
        }
        const item = pageRepeater.itemAt(page)
        if (!item)
            return null
        return { page: page, point: item.mapViewPointToPage(viewPoint) }
    }

    function pageCanSelect(page) {
        return pageTextCapabilities[page] !== false || ocrPages[page] !== undefined
    }

    function selectionCanSelect(selection) {
        if (!selection)
            return false
        if (viewportHasNativeText)
            return true
        if (!ocrPages[selection.page])
            return false
        const point = selection.point
        const words = ocrPages[selection.page].words
        for (let i = 0; i < words.length; ++i) {
            const word = words[i]
            if (point.x >= word.x - 8 && point.x <= word.x + word.w + 8
                    && point.y >= word.y - 8 && point.y <= word.y + word.h + 8)
                return true
        }
        return false
    }

    function requestOcr(page, explicit) {
        if (pageCount === 0)
            return
        if (Ocr.running) {
            if (explicit) {
                Ocr.cancel()
                ocrGeneration += 1
                ocrRequestedPage = -1
                ocrStatus = "recognition cancelled"
            } else if (ocrRequestedPage < 0) {
                resumeFullPageOcr = page
            }
            return false
        }
        if (page < 0 || page >= pageCount
                || (ocrPages[page] !== undefined && !ocrPages[page].partial))
            return false
        // Native text and image facts come from the background page-info cache, so
        // deciding about OCR never makes the UI wait on the MuPDF lock.
        let info = Doc.cachedPageInfo(page)
        if (info.hasText === undefined) {
            if (!explicit) {
                Doc.requestPageInfo(page)      // onPageInfoReady re-schedules OCR
                return false
            }
            info = Doc.pageInfo(page)          // the user asked: waiting is fine
        }
        const hasText = info.hasText
        const capabilities = Object.assign({}, pageTextCapabilities)
        capabilities[page] = hasText
        pageTextCapabilities = capabilities
        // With RapidOCR a PDF page is read as crops: each embedded image when the
        // page already has native text, or the whole page when it has none.
        const cropMode = Ocr.rapidAvailable && !isImageDoc
        const size = Doc.pageSizePt(page)
        let regions = []
        const nativeChars = info.chars
        if (hasText) {
            const boxes = Object.assign({}, nativePageBoxes)
            boxes[page] = info.boxes
            nativePageBoxes = boxes
            if (cropMode) {
                // A full-page backdrop behind a text-rich page is not worth reading.
                regions = info.imageRects.filter(r =>
                    nativeChars < 200 || r.w * r.h < 0.85 * size.width * size.height)
            }
        }
        if (hasText && explicit && regions.length === 0) {
            ocrStatus = "page already has selectable text"
            return false
        }
        if (hasText) {
            if (cropMode) {
                if (regions.length === 0)
                    return false
            } else if (!info.hasImages || nativeChars >= 200) {
                return false
            }
        }
        ocrGeneration += 1
        ocrRequestedPage = page
        ocrExplicit = explicit
        if (explicit)
            ocrStatus = "recognizing page " + (page + 1)
        Ocr.recognize(Doc.ocrPath, page, size, ocrGeneration, regions)
        return true
    }

    function recognizeCurrentPage() {
        requestOcr(Math.max(0, currentPage - 1), true)
    }

    function scheduleVisibleOcr() {
        autoOcrTimer.restart()
    }

    function refreshViewportSelection() {
        viewportSelection = viewportPointer.x >= 0
            ? selectionPoint(viewportPointer) : null
        viewportHasNativeText = viewportSelection
            ? nativePointHasText(viewportSelection.page, viewportSelection.point) : false
    }

    function nativePointHasText(page, point) {
        if (pageTextCapabilities[page] !== true)
            return false
        const key = page + ":" + Math.round(point.x / 3) + ":" + Math.round(point.y / 3)
        if (nativeHoverCache[key] === true)
            return true
        const result = Doc.hoverTextAt(page, point)
        if (result.busy)
            return false      // lock busy / text not ready: skip this frame, never stall
        let hit = false
        for (let i = 0; i < result.boxes.length; ++i) {
            const box = result.boxes[i]
            if (point.x >= box.x - 5 && point.x <= box.x + box.w + 5
                    && point.y >= box.y - 5 && point.y <= box.y + box.h + 5) {
                hit = true
                break
            }
        }
        const cache = Object.assign({}, nativeHoverCache)
        if (hit) {
            cache[key] = true
            nativeHoverCache = cache
        }
        return hit
    }

    onPageTextCapabilitiesChanged: refreshViewportSelection()
    onZoomChanged: {
        refreshViewportSelection()
        saveViewTimer.restart()
    }

    function requestRegionalOcr(selection) {
        // Hover OCR is only the Tesseract fallback; with RapidOCR whole pages
        // (or their images) are read up front instead.
        if (!selection || isImageDoc || Ocr.rapidAvailable)
            return false
        const hasNativeText = pageTextCapabilities[selection.page] === true
        if (hasNativeText && nativePointHasText(selection.page, selection.point))
            return false
        if (Ocr.running) {
            if (ocrRequestedPage < 0)
                return false
            if (ocrRequestedPage === selection.page) {
                root.resumeFullPageOcr = selection.page
                Ocr.cancel()
                ocrGeneration += 1
                ocrRequestedPage = -1
            } else {
                Ocr.cancel()
                ocrGeneration += 1
                ocrRequestedPage = -1
            }
        }
        const point = selection.point
        const words = ocrPages[selection.page] ? ocrPages[selection.page].words : []
        for (let i = 0; i < words.length; ++i) {
            const word = words[i]
            if (point.x >= word.x - 6 && point.x <= word.x + word.w + 6
                    && point.y >= word.y - 6 && point.y <= word.y + word.h + 6)
                return false
        }
        const geometry = regionalOcrGeometry(selection)
        const size = geometry.size
        const region = geometry.region
        const key = geometry.key
        if (ocrRegionKeys[key])
            return false
        const keys = Object.assign({}, ocrRegionKeys)
        keys[key] = true
        ocrRegionKeys = keys
        pendingOcrRegion = { page: selection.page, region: region }
        ocrGeneration += 1
        Ocr.recognizeRegion(Doc.ocrPath, selection.page, size, region, ocrGeneration)
        return true
    }

    function regionalOcrGeometry(selection) {
        if (!selection)
            return { key: "", region: Qt.rect(0, 0, 0, 0), size: Qt.size(0, 0) }
        const size = Doc.pageSizePt(selection.page)
        const width = Math.min(100, size.width)
        const height = Math.min(40, size.height)
        const rawX = Math.max(0, Math.min(size.width - width,
                                         selection.point.x - width / 2))
        const rawY = Math.max(0, Math.min(size.height - height,
                                         selection.point.y - height / 2))
        const x = Math.max(0, Math.min(size.width - width, Math.round(rawX / 50) * 50))
        const y = Math.max(0, Math.min(size.height - height, Math.round(rawY / 20) * 20))
        return { key: selection.page + ":" + x + ":" + y,
                 region: Qt.rect(x, y, width, height), size: size }
    }

    Connections {
        target: Doc
        function onPageInfoReady(page) {
            const info = Doc.cachedPageInfo(page)
            const capabilities = Object.assign({}, root.pageTextCapabilities)
            capabilities[page] = info.hasText === true
            root.pageTextCapabilities = capabilities
            if (info.hasText === true) {
                const boxes = Object.assign({}, root.nativePageBoxes)
                boxes[page] = info.boxes
                root.nativePageBoxes = boxes
            }
            if (root.selectAllDoc && root.selectionPages["*"] === true)
                root.selectionPages = Object.assign({}, root.selectionPages)
            if (Math.abs(page - (root.currentPage - 1)) <= 4) {
                root.scheduleVisibleOcr()       // now we know whether it needs OCR
                if (!Ocr.running)
                    nearbyOcrTimer.restart()    // and re-arm the read-ahead
            }
        }
        function onAllTextReady(generation, pages) {
            if (!root.selectAllDoc || generation !== Doc.generation)
                return
            root.selectionText = root.composeAllText(pages)
            if (root.copyAfterAllText) {
                root.copyAfterAllText = false
                root.copyPage()
            }
        }
        function onFileChangedOnDisk() {
            root.reloadFile(true)
        }
    }

    Timer {
        id: statusClearTimer
        interval: 4000
        onTriggered: if (!Ocr.running) root.ocrStatus = ""
    }

    Connections {
        target: Ocr
        function onFinished(generation, page, text, words, error) {
            if (generation !== root.ocrGeneration)
                return
            if (error !== "") {
                root.ocrStatus = error
                return
            }
            const embeddedText = root.pageTextCapabilities[page] === true
            let filteredWords = words
            if (embeddedText && root.nativePageBoxes[page]) {
                const nativeBoxes = root.nativePageBoxes[page]
                filteredWords = words.filter(word => {
                    const cx = word.x + word.w / 2
                    const cy = word.y + word.h / 2
                    for (let i = 0; i < nativeBoxes.length; ++i) {
                        const box = nativeBoxes[i]
                        if (cx >= box.x && cx <= box.x + box.w
                                && cy >= box.y && cy <= box.y + box.h)
                            return false
                    }
                    return true
                })
            }
            const pages = Object.assign({}, root.ocrPages)
            pages[page] = { text: root.ocrText(filteredWords), words: filteredWords,
                            embeddedText: embeddedText, partial: false }
            root.ocrPages = pages
            root.refreshViewportSelection()
            const capabilities = Object.assign({}, root.pageTextCapabilities)
            capabilities[page] = true
            root.pageTextCapabilities = capabilities
            if (root.ocrExplicit)
                root.ocrStatus = words.length + " words recognized"
            root.ocrRequestedPage = -1
            nearbyOcrTimer.restart()
            if (root.viewportPointer.x >= 0)
                regionHoverTimer.restart()
        }
        function onRegionFinished(generation, page, region, words, error) {
            if (generation !== root.ocrGeneration)
                return
            root.pendingOcrRegion = null
            if (root.resumeFullPageOcr >= 0) {
                const resumePage = root.resumeFullPageOcr
                root.resumeFullPageOcr = -1
                fullPageOcrResume.page = resumePage
                fullPageOcrResume.restart()
            }
            if (error !== "")
            {
                const geometry = root.regionalOcrGeometry(
                    { page: page, point: Qt.point(region.x + region.width / 2,
                                                   region.y + region.height / 2) })
                const keys = Object.assign({}, root.ocrRegionKeys)
                delete keys[geometry.key]
                root.ocrRegionKeys = keys
                return
            }
            const previous = root.ocrPages[page]
            const merged = previous ? previous.words.slice() : []
            for (let i = 0; i < words.length; ++i) {
                const candidate = words[i]
                let duplicate = false
                for (let j = 0; j < merged.length; ++j) {
                    const existing = merged[j]
                    const cx = candidate.x + candidate.w / 2
                    const cy = candidate.y + candidate.h / 2
                    if (cx >= existing.x && cx <= existing.x + existing.w
                            && cy >= existing.y && cy <= existing.y + existing.h) {
                        duplicate = true
                        break
                    }
                }
                if (!duplicate)
                    merged.push(candidate)
            }
            merged.sort((a, b) => Math.abs(a.y - b.y) > Math.max(a.h, b.h) * 0.5
                        ? a.y - b.y : a.x - b.x)
            let line = 0
            for (let i = 0; i < merged.length; ++i) {
                if (i > 0 && Math.abs(merged[i].y - merged[i - 1].y)
                        > Math.max(merged[i].h, merged[i - 1].h) * 0.5)
                    line += 1
                merged[i].block = 0
                merged[i].paragraph = 0
                merged[i].line = line
                merged[i].word = i
            }
            const pages = Object.assign({}, root.ocrPages)
            pages[page] = { text: root.ocrText(merged),
                            words: merged,
                            embeddedText: root.pageTextCapabilities[page] === true,
                            partial: !previous || previous.partial === true }
            root.ocrPages = pages
            root.refreshViewportSelection()
            if (root.viewportPointer.x >= 0)
                regionHoverTimer.restart()
        }
    }

    Timer {
        id: autoOcrTimer
        interval: 120
        onTriggered: root.requestOcr(Math.max(0, root.currentPage - 1), false)
    }

    Timer {
        id: nearbyOcrTimer
        interval: 800
        onTriggered: {
            // Read-ahead: the next few pages first (people mostly read forward),
            // then the previous one. Pages already done or without images are
            // skipped by requestOcr, so this walks on to the next candidate.
            const page = Math.max(0, root.currentPage - 1)
            for (const offset of [1, 2, 3, -1]) {
                if (root.requestOcr(page + offset, false))
                    break
            }
        }
    }

    Timer {
        id: fullPageOcrResume
        interval: 300
        property int page: -1
        onTriggered: {
            if (page >= 0 && !Ocr.running
                    && (root.ocrPages[page] === undefined || root.ocrPages[page].partial))
                root.requestOcr(page, false)
            page = -1
        }
    }

    // Re-read the open file from disk, keeping page, position, zoom and layout.
    // If the file can't be opened yet (still being written), keep retrying.
    property string reloadPath: ""
    property int reloadTries: 0
    function reloadFile(automatic) {
        const path = Doc.filePath !== "" ? Doc.filePath : reloadPath
        if (path === "")
            return
        reloadRetry.stop()          // a signal-driven reload supersedes a pending retry
        reloadPath = path
        reloadAutomatic = automatic
        if (pageCount > 0)
            saveViewState()
        openFromUrl(path)
        if (pageCount === 0 && reloadTries < 15) {
            reloadTries += 1
            reloadRetry.start()         // open failed: try again shortly
            return
        }
        reloadTries = 0
        reloadNote.text = automatic ? "file changed \u2014 reloaded" : "reloaded"
        reloadNote.start()
    }
    property bool reloadAutomatic: false

    Timer {
        id: reloadRetry
        interval: 1000
        onTriggered: root.reloadFile(root.reloadAutomatic)
    }

    Timer {
        id: reloadNote
        property string text: ""
        interval: 350      // openFromUrl clears the status; set ours just after
        onTriggered: root.setStatus(text)
    }

    function openFromUrl(url) {
        if (!url)
            return
        const path = url.toString().startsWith("file://")
                    ? decodeURIComponent(url.toString().substring(7))
                    : url.toString()
        saveViewState()
        if (Doc.open(path)) {
            closeReader()
            readAll = null
            navBack = []
            viewStateReady = false
            initialFitDone = false
            pendingViewState = Doc.loadViewState()
            clearSelection()
            closeSearch()
            pageTextCapabilities = ({})
            showThumbs = false
            currentPage = 1
            view.contentY = 0
        }
    }

    function computeSizes() {
        const n = Doc.pageCount
        const sizes = []
        for (let i = 0; i < n; ++i)
            sizes.push(Doc.pageSizePt(i))
        pageSizes = sizes
        rebuildLayout()
        if (n > 0) {
            autoFitSinglePage = (n === 1)
            fitWidth(true)
        }
    }

    // ---------- header ----------

    component HeaderButton: Rectangle {
        id: btn
        property string label
        property string tip
        property bool activeFlag: false
        signal activated()

        width: 26
        height: 26
        radius: 6
        color: ma.containsMouse ? Theme.selection : (activeFlag ? Theme.selection : "transparent")
        border.color: activeFlag ? Theme.accent : "transparent"
        border.width: 1

        Text {
            anchors.centerIn: parent
            text: parent.label
            color: ma.containsMouse ? Theme.accent : Theme.mutedForeground
            font.pixelSize: 12
            renderType: Text.QtRendering
        }
        MouseArea {
            id: ma
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: btn.activated()
        }
    }

    Rectangle {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 40
        color: Theme.darkerBackground

        // Drag the bar to move the window: hand the move to the compositor.
        // TakeOverForbidden keeps the search field, buttons and page-number
        // click working; only the bar's own empty space starts a move.
        DragHandler {
            target: null
            acceptedButtons: Qt.LeftButton
            grabPermissions: PointerHandler.TakeOverForbidden
            onActiveChanged: if (active) root.startSystemMove()
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 8
            spacing: 10

            Text {
                text: "glance"
                color: Theme.accent
                font.pixelSize: 14
                font.weight: Font.DemiBold
                font.letterSpacing: 0.5
                renderType: Text.QtRendering
            }
            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: 18
                color: Theme.mutedForeground
                opacity: 0.28
            }
            Text {
                text: fileName !== "" ? fileName : "no file"
                color: fileName !== "" ? Theme.foreground : Theme.mutedForeground
                font.pixelSize: 13
                elide: Text.ElideMiddle
                Layout.maximumWidth: parent.width * 0.4
                renderType: Text.QtRendering
            }
            Text {
                text: root.pageIndicator()
                color: Theme.foreground
                opacity: 0.75
                font.pixelSize: 12
                renderType: Text.QtRendering
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -4
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.openCommand()
                }
            }
            Text {
                text: Math.round(zoom * 100) + "%"
                color: Theme.mutedForeground
                font.pixelSize: 12
                Layout.preferredWidth: 46
                renderType: Text.QtRendering
            }
            Item { Layout.fillWidth: true }

            TextInput {
                id: cmdField
                visible: root.cmdActive
                Layout.preferredWidth: visible ? 150 : 0
                Layout.alignment: Qt.AlignVCenter
                color: Theme.foreground
                selectionColor: Theme.accent
                selectedTextColor: Theme.darkerBackground
                font.pixelSize: 12
                renderType: Text.QtRendering
                verticalAlignment: TextInput.AlignVCenter
                leftPadding: 10
                clip: true
                Text {
                    x: 0
                    anchors.verticalCenter: parent.verticalCenter
                    text: ":"
                    color: Theme.accent
                    font.pixelSize: 12
                    renderType: Text.QtRendering
                }
                Text {
                    x: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: "page (40, iv, +5)"
                    visible: cmdField.text === ""
                    color: Theme.mutedForeground
                    font.pixelSize: 12
                    renderType: Text.QtRendering
                }
                onAccepted: {
                    root.gotoPage(text)
                    root.closeCommand()
                }
                Keys.onEscapePressed: root.closeCommand()
                onVisibleChanged: if (visible) forceActiveFocus()
            }

            TextInput {
                id: searchField
                visible: root.searchActive
                Layout.preferredWidth: visible ? 180 : 0
                Layout.alignment: Qt.AlignVCenter
                color: Theme.foreground
                selectionColor: Theme.accent
                selectedTextColor: Theme.darkerBackground
                font.pixelSize: 12
                renderType: Text.QtRendering
                verticalAlignment: TextInput.AlignVCenter
                clip: true
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.searchBackward ? "search backward…" : "search…"
                    visible: searchField.text === ""
                    color: Theme.mutedForeground
                    font.pixelSize: 12
                    renderType: Text.QtRendering
                }
                onTextChanged: {
                    root.searchGeneration += 1
                    Doc.cancelSearch()
                    root.searchPending = false
                    root.searchText = text
                    root.searchResults = []
                    root.searchResultIndex = -1
                    root.searchMatchCount = 0
                    root.searchPage = -1
                    root.searchBoxes = []
                }
                onAccepted: { root.runSearch(true, root.searchBackward ? -1 : 1); view.forceActiveFocus() }
                Keys.onEscapePressed: root.closeSearch()
                onVisibleChanged: if (visible) forceActiveFocus()
            }

            HeaderButton {
                visible: root.searchActive
                label: "‹"
                tip: "previous match page"
                onActivated: root.runSearch(root.searchResults.length === 0, -1)
            }
            Text {
                visible: root.searchActive
                text: root.searchResults.length === 0
                      ? (root.searchPending ? "…" : (root.searchText === "" ? "" : "0"))
                      : ((root.searchResultIndex + 1) + " / " + root.searchResults.length
                         + " pages · " + root.searchMatchCount + " highlights")
                color: Theme.mutedForeground
                font.pixelSize: 11
                renderType: Text.QtRendering
            }
            HeaderButton {
                visible: root.searchActive
                label: "›"
                tip: "next match page"
                onActivated: root.runSearch(root.searchResults.length === 0, 1)
            }

            Text {
                visible: Ocr.running || root.ocrStatus !== ""
                text: Ocr.running ? (Ocr.progress + "%") : root.ocrStatus
                color: Theme.mutedForeground
                font.pixelSize: 10
                elide: Text.ElideRight
                Layout.maximumWidth: 130
                renderType: Text.QtRendering
            }
            HeaderButton {
                label: "\u2699"
                activeFlag: root.showSettings
                onActivated: root.showSettings = !root.showSettings
            }
            Rectangle {
                id: keysButton
                Layout.preferredWidth: keysLabel.implicitWidth + 24
                Layout.preferredHeight: 26
                radius: Theme.cornerRadius
                color: keysMouse.containsMouse || root.showKeys ? Theme.selection : "transparent"
                border.color: root.showKeys ? Theme.accent : Theme.mutedForeground
                border.width: 1
                Text {
                    id: keysLabel
                    anchors.centerIn: parent
                    text: "Keybindings"
                    color: keysMouse.containsMouse || root.showKeys
                           ? Theme.accent : Theme.foreground
                    font.pixelSize: 12
                    renderType: Text.QtRendering
                }
                MouseArea {
                    id: keysMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.showKeys = !root.showKeys
                }
            }
        }
    }

    // ---------- content ----------

    RowLayout {
        id: contentArea
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        spacing: 0

        Loader {
            active: root.showOutline
            visible: active
            Layout.preferredWidth: active ? 280 : 0
            Layout.fillHeight: true
            sourceComponent: Outline { }
        }

        Flickable {
            id: view
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            interactive: !root.spaceHeld && !root.selectionCanSelect(root.viewportSelection)
            boundsBehavior: Flickable.StopAtBounds
            contentWidth: Math.max(width, colW(zoom))
            contentHeight: pagesCol.implicitHeight
            onContentXChanged: root.refreshViewportSelection()
            onContentYChanged: {
                if (root.readActive && root.readMode === "doc" && Reader.state !== 0
                        && Date.now() - root.readScrollStamp > 700)
                    root.readFollow = false
                root.refreshViewportSelection()
                root.detectPage()
            }
            onHeightChanged: root.detectPage()
            onContentHeightChanged: root.detectPage()

            Item {
                id: pagesCol
                x: colX(root.zoom)
                width: colW(root.zoom)
                height: root.totalH(root.zoom)
                implicitHeight: height
                transform: Scale {
                    origin.x: root.pinchAnchorContentX - pagesCol.x
                    origin.y: root.pinchAnchorContentY - pagesCol.y
                    xScale: root.pinchPreviewActive ? root.pinchPreviewScale : 1
                    yScale: root.pinchPreviewActive ? root.pinchPreviewScale : 1
                }

                Repeater {
                    id: pageRepeater
                    model: Doc.pageCount
                    delegate: Item {
                        id: pageSlot
                        x: root.pagesPerRow === 1 ? 0 : root.pageXAt(index) * root.zoom
                        y: root.pageTopAt(index, root.zoom)
                           + (root.pagesPerRow === 1 ? 0 : root.pageYOffsetPt(index) * root.zoom)
                        width: root.pagesPerRow === 1 ? pagesCol.width
                                                      : root.sheetWpt(index) * root.zoom
                        height: root.sheetHpt(index) * root.zoom
                        readonly property bool nearView: y + height >= view.contentY - 1000
                                                         && y <= view.contentY + view.height
                                                                 + (root.pinchPreviewActive ? 5000 : 2600)
                        function mapViewPointToPage(viewPoint) {
                            if (pageLoader.item)
                                return pageLoader.item.mapViewPointToPage(viewPoint)
                            const local = pageSlot.mapFromItem(view, viewPoint.x, viewPoint.y)
                            const size = root.pageSizes[index]
                            return Qt.point(Math.max(0, Math.min(size ? size.width : 612,
                                                               local.x / root.zoom)),
                                            Math.max(0, Math.min(size ? size.height : 792,
                                                               local.y / root.zoom)))
                        }

                        Loader {
                            id: pageLoader
                            anchors.fill: parent
                            active: root.pageLayoutReady && pageSlot.nearView
                            sourceComponent: PageImage {
                                page: index
                                zoom: root.zoom
                                rotationAngle: root.rotation
                                dpr: root.dpr
                                ptW: root.pageSizes[index] ? root.pageSizes[index].width : 612
                                ptH: root.pageSizes[index] ? root.pageSizes[index].height : 792
                            }
                            onLoaded: Doc.requestPageInfo(index)
                        }
                    }
                }
            }

            WheelHandler {
                id: wheel
                target: null
                blocking: true
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: (event) => {
                    scrollMomentum.stop()
                    if (event.modifiers & Qt.ControlModifier) {
                        const d = event.angleDelta.y
                        if (d === 0)
                            return
                        const a = root.anchorFrom(wheel.point.scenePosition,
                                                  wheel.point.position)
                        root.zoomAt(a.x, a.y, Math.pow(1.25, d / 120))
                        return
                    }

                    // Direct scroll: touchpad pixel deltas are multiplied so a
                    // small finger movement covers more ground; a mouse wheel
                    // moves a fixed number of pixels per notch.
                    const pixelFactor = 5.0
                    const notchPx = 120.0
                    let dx = event.pixelDelta.x * pixelFactor
                    let dy = event.pixelDelta.y * pixelFactor
                    if (dx === 0 && dy === 0) {
                        dx = event.angleDelta.x / 120 * notchPx
                        dy = event.angleDelta.y / 120 * notchPx
                    }
                    const maxX = Math.max(0, view.contentWidth - view.width)
                    const maxY = Math.max(0, view.contentHeight - view.height)
                    if (dx !== 0)
                        view.contentX = Math.max(0, Math.min(view.contentX - dx, maxX))
                    if (dy !== 0)
                        view.contentY = Math.max(0, Math.min(view.contentY - dy, maxY))

                    if (event.pixelDelta.x !== 0 || event.pixelDelta.y !== 0) {
                        scrollMomentum.velocityX = scrollMomentum.velocityX * 0.4 - dx * 0.6
                        scrollMomentum.velocityY = scrollMomentum.velocityY * 0.4 - dy * 0.6
                        scrollEndDelay.restart()
                        if (event.phase === Qt.ScrollEnd) {
                            scrollEndDelay.stop()
                            scrollMomentum.start()
                        }
                    }
                }
            }

            Timer {
                id: scrollEndDelay
                interval: 55
                onTriggered: scrollMomentum.start()
            }

            Timer {
                id: scrollMomentum
                interval: 16
                repeat: true
                property real velocityX: 0
                property real velocityY: 0

                function start() {
                    if (Math.abs(velocityX) >= 0.5 || Math.abs(velocityY) >= 0.5)
                        restart()
                }

                onTriggered: {
                    const maxX = Math.max(0, view.contentWidth - view.width)
                    const maxY = Math.max(0, view.contentHeight - view.height)
                    const nextX = Math.max(0, Math.min(view.contentX + velocityX, maxX))
                    const nextY = Math.max(0, Math.min(view.contentY + velocityY, maxY))
                    const hitX = nextX === view.contentX && velocityX !== 0
                    const hitY = nextY === view.contentY && velocityY !== 0
                    view.contentX = nextX
                    view.contentY = nextY
                    velocityX = hitX ? 0 : velocityX * 0.95
                    velocityY = hitY ? 0 : velocityY * 0.95
                    if (Math.abs(velocityX) < 0.5 && Math.abs(velocityY) < 0.5)
                        stop()
                }
            }

            PinchHandler {
                id: pinch
                target: null
                property real startZoom: 1
                onActiveChanged: {
                    if (active) {
                        scrollEndDelay.stop()
                        scrollMomentum.stop()
                        scrollMomentum.velocityX = 0
                        scrollMomentum.velocityY = 0
                        const a = root.anchorFrom(centroid.scenePosition, centroid.position)
                        startZoom = root.zoom
                        root.pinchAnchorViewportX = a.x
                        root.pinchAnchorViewportY = a.y
                        root.pinchAnchorContentX = view.contentX + a.x
                        root.pinchAnchorContentY = view.contentY + a.y
                        const cw = root.colW(startZoom)
                        root.pinchAnchorColumnFraction = cw > 0
                            ? (root.pinchAnchorContentX - root.colX(startZoom)) / cw : 0.5
                        root.pinchAnchorPage = root.pageCount > 0
                            ? Math.max(0, root.currentPage - 1) : 0
                        for (let i = 0; i < root.pageCount; ++i) {
                            const top = root.pageTopAt(i, startZoom)
                            const h = root.sheetHpt(i) * startZoom
                            if (root.pinchAnchorContentY < top + h) {
                                root.pinchAnchorPage = i
                                root.pinchAnchorPageFraction = h > 0
                                    ? Math.max(0, Math.min(1,
                                        (root.pinchAnchorContentY - top) / h)) : 0
                                break
                            }
                        }
                        root.pinchPreviewScale = 1
                        root.pinchPreviewActive = true
                    } else if (root.pinchPreviewActive) {
                        const targetZoom = root.clampZoom(startZoom * root.pinchPreviewScale)
                        const cw = root.colW(targetZoom)
                        const newCx = root.colX(targetZoom)
                                    + root.pinchAnchorColumnFraction * cw
                        const newCy = root.pageTopAt(root.pinchAnchorPage, targetZoom)
                                    + root.pinchAnchorPageFraction
                                      * root.sheetHpt(root.pinchAnchorPage) * targetZoom
                        root.zoom = targetZoom
                        view.contentX = Math.max(0, Math.min(
                            newCx - root.pinchAnchorViewportX,
                            Math.max(0, Math.max(view.width, cw) - view.width)))
                        view.contentY = Math.max(0, Math.min(
                            newCy - root.pinchAnchorViewportY,
                            Math.max(0, root.totalH(targetZoom) - view.height)))
                        root.pinchPreviewActive = false
                        root.pinchPreviewScale = 1
                    }
                }
                onActiveScaleChanged: {
                    if (!active)
                        return
                    const a = root.anchorFrom(centroid.scenePosition, centroid.position)
                    root.pinchAnchorViewportX = a.x
                    root.pinchAnchorViewportY = a.y
                    const targetZoom = root.clampZoom(startZoom * activeScale)
                    root.pinchPreviewScale = targetZoom / startZoom
                    const maxX = Math.max(0, Math.max(view.width, root.colW(targetZoom))
                                                   - view.width)
                    const maxY = Math.max(0, root.totalH(targetZoom) - view.height)
                    view.contentX = Math.max(0, Math.min(
                        root.pinchAnchorContentX - a.x, maxX))
                    view.contentY = Math.max(0, Math.min(
                        root.pinchAnchorContentY - a.y, maxY))
                }
            }

            DragHandler {
                id: rangeSelection
                property var anchorSelection: null
                property point lastPoint: Qt.point(0, 0)
                property string selectionSource: "native"
                acceptedButtons: Qt.LeftButton
                enabled: !root.spaceHeld
                         && (active || root.selectionCanSelect(root.viewportSelection))
                target: null

                function refresh(point) {
                    const viewportPoint = view.mapFromItem(rangeSelection.parent,
                                                           point.x, point.y)
                    refreshViewport(viewportPoint)
                }

                function refreshViewport(viewportPoint) {
                    lastPoint = viewportPoint
                    const focus = root.selectionPoint(viewportPoint)
                    if (active && anchorSelection && focus)
                        root.updateSelection(anchorSelection.page, anchorSelection.point,
                                             focus.page, focus.point, selectionSource)
                }

                onActiveChanged: {
                    if (active) {
                        const viewportPoint = view.mapFromItem(rangeSelection.parent,
                                                               centroid.pressPosition.x,
                                                               centroid.pressPosition.y)
                        lastPoint = view.mapFromItem(rangeSelection.parent,
                                                     centroid.position.x,
                                                     centroid.position.y)
                        anchorSelection = root.selectionPoint(viewportPoint)
                        selectionSource = anchorSelection && root.ocrPages[anchorSelection.page]
                            && (!root.ocrPages[anchorSelection.page].embeddedText
                                || root.pointInsideOcrWord(anchorSelection.page,
                                                           anchorSelection.point) >= 0)
                            ? "ocr" : "native"
                        root.clearSelection()
                        root.requestRegionalOcr(anchorSelection)
                    } else {
                        const focus = root.selectionPoint(lastPoint)
                        if (anchorSelection && focus)
                            root.updateSelection(anchorSelection.page, anchorSelection.point,
                                                 focus.page, focus.point, selectionSource)
                        anchorSelection = null
                    }
                }
                onCentroidChanged: if (active) refresh(centroid.position)
            }

            DragHandler {
                id: spacePan
                target: null
                acceptedButtons: Qt.LeftButton
                enabled: root.spaceHeld
                property real startContentX: 0
                property real startContentY: 0

                onActiveChanged: if (active) {
                    scrollEndDelay.stop()
                    scrollMomentum.stop()
                    startContentX = view.contentX
                    startContentY = view.contentY
                }
                onTranslationChanged: if (active) {
                    const maxX = Math.max(0, view.contentWidth - view.width)
                    const maxY = Math.max(0, view.contentHeight - view.height)
                    view.contentX = Math.max(0, Math.min(startContentX - translation.x, maxX))
                    view.contentY = Math.max(0, Math.min(startContentY - translation.y, maxY))
                }
            }

            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: (eventPoint) => root.openContextMenu(eventPoint.scenePosition.x,
                                                               eventPoint.scenePosition.y)
            }

            Timer {
                id: selectionAutoScroll
                interval: 16
                repeat: true
                running: rangeSelection.active
                         && (rangeSelection.lastPoint.x < 48
                             || rangeSelection.lastPoint.x > view.width - 48
                             || rangeSelection.lastPoint.y < 48
                             || rangeSelection.lastPoint.y > view.height - 48)
                onTriggered: {
                    const edge = 48
                    let deltaX = 0
                    let deltaY = 0
                    if (rangeSelection.lastPoint.x < edge)
                        deltaX = -Math.min(24, edge - rangeSelection.lastPoint.x)
                    else if (rangeSelection.lastPoint.x > view.width - edge)
                        deltaX = Math.min(24, rangeSelection.lastPoint.x - (view.width - edge))
                    if (rangeSelection.lastPoint.y < edge)
                        deltaY = -Math.min(24, edge - rangeSelection.lastPoint.y)
                    else if (rangeSelection.lastPoint.y > view.height - edge)
                        deltaY = Math.min(24, rangeSelection.lastPoint.y - (view.height - edge))
                    const maxX = Math.max(0, view.contentWidth - view.width)
                    const maxY = Math.max(0, view.contentHeight - view.height)
                    view.contentX = Math.max(0, Math.min(view.contentX + deltaX, maxX))
                    view.contentY = Math.max(0, Math.min(view.contentY + deltaY, maxY))
                    rangeSelection.refreshViewport(rangeSelection.lastPoint)
                }
            }

            HoverHandler {
                id: hoveredPage
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onPointChanged: {
                    root.viewportPointer = view.mapFromItem(
                        null, point.scenePosition.x, point.scenePosition.y)
                    root.refreshViewportSelection()
                    if (root.viewportSelection && !root.viewportHasNativeText)
                        Ocr.prewarm()
                    const nextRegionKey = root.regionalOcrGeometry(
                        root.viewportSelection).key
                    if (nextRegionKey !== root.hoverRegionKey) {
                        root.hoverRegionKey = nextRegionKey
                        regionHoverTimer.restart()
                    }
                    if (point.pressedButtons & Qt.LeftButton)
                        root.requestRegionalOcr(root.viewportSelection)
                }
                onHoveredChanged: if (!hovered) {
                    root.viewportPointer = Qt.point(-1, -1)
                    root.viewportSelection = null
                    root.viewportHasNativeText = false
                    root.hoverRegionKey = ""
                }
            }

            Timer {
                id: regionHoverTimer
                interval: 40
                onTriggered: root.requestRegionalOcr(root.viewportSelection)
            }

            HoverHandler {
                id: viewportCursor
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                cursorShape: spacePan.active || view.dragging ? Qt.ClosedHandCursor
                                          : root.spaceHeld ? Qt.OpenHandCursor
                                          : root.selectionCanSelect(root.viewportSelection)
                                            ? Qt.IBeamCursor
                                            : root.viewportSelection ? Qt.OpenHandCursor
                                            : Qt.ArrowCursor
            }
        }

        Loader {
            active: root.showThumbs && root.pageCount > 1
            visible: active
            Layout.preferredWidth: active ? 148 : 0
            Layout.fillHeight: true
            sourceComponent: Thumbnails { }
        }
    }

    NumberAnimation {
        id: jumpAnim
        target: view
        property: "contentY"
        duration: 300
        easing.type: Easing.OutCubic
    }

    // ---------- empty state ----------

    Rectangle {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: root.pageCount === 0
        color: Theme.background

        Column {
            anchors.centerIn: parent
            spacing: 8

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "glance"
                color: Theme.foreground
                font.pixelSize: 28
                font.weight: Font.DemiBold
                font.letterSpacing: 1
                renderType: Text.QtRendering
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "press o or drop a file here"
                color: Theme.mutedForeground
                font.pixelSize: 14
                renderType: Text.QtRendering
            }
        }
        MouseArea {
            anchors.fill: parent
            onClicked: picker.open()
        }
    }

    DropArea {
        anchors.fill: parent
        onDropped: (drop) => {
            if (root.pageCount > 0)
                return // one file per window, per spec
            if (drop.urls.length > 0)
                root.openFromUrl(drop.urls[0])
        }
    }

    FileDialog {
        id: picker
        nameFilters: ["Documents (*.pdf *.png *.jpeg *.jpg *.gif *.webp *.bmp *.tiff *.tif)"]
        onAccepted: root.openFromUrl(currentFile)
    }


    // ---------- right-click menu ----------
    function buildMenu() {
        const page = Math.max(0, currentPage - 1)
        const hasPages = pageCount > 0
        const ocrWords = hasPages && ocrPages[page] ? ocrPages[page].words.length : 0
        const lastRow = rowFirstPage.length - 1
        const items = []
        items.push({ label: selectionText !== "" ? "Copy" : "Copy Page as Image",
                     shortcut: "Ctrl+C", enabled: hasPages, action: function() { copyPage() } })
        items.push({ label: "Select All", shortcut: "Ctrl+A", enabled: hasPages,
                     action: function() { selectAllDocument() } })
        items.push({ label: "Select All OCR Text", shortcut: "Ctrl+Shift+A", enabled: ocrWords > 0,
                     action: function() { selectAllOcrOnPage() } })
        items.push({ separator: true })
        items.push({ label: "Previous Page", shortcut: "K", enabled: hasPages && currentPage > 1,
                     action: function() { jumpTo(Math.max(1, currentPage - pagesPerRow), false) } })
        items.push({ label: "Next Page", shortcut: "J",
                     enabled: hasPages && rowOf(page) < lastRow,
                     action: function() { jumpTo(currentPage + pagesPerRow, false) } })
        items.push({ label: "Back", shortcut: "Ctrl+O", enabled: navBack.length > 0,
                     action: function() { goBack() } })
        items.push({ separator: true })
        items.push({ label: "Reload", shortcut: "Ctrl+R", enabled: hasPages,
                     action: function() { reloadFile(false) } })
        items.push({ label: "Open Containing Folder", shortcut: "", enabled: hasPages,
                     action: function() { Doc.openContainingFolder() } })
        items.push({ separator: true })
        items.push({ label: "Fit Width", shortcut: "W", enabled: hasPages, action: function() { fitWidth() } })
        items.push({ label: "Fit Page", shortcut: "P", enabled: hasPages, action: function() { fitPage() } })
        items.push({ label: "Actual Size", shortcut: "Ctrl+0", enabled: hasPages,
                     action: function() { zoom = 1.0 } })
        items.push({ separator: true })
        if (aiOn)
            items.push({ label: selectionText !== "" ? "Read Selection Aloud" : "Read Aloud",
                         shortcut: "S", enabled: hasPages, action: function() { startReading() } })
        items.push({ label: "Read Text on This Page", shortcut: "Ctrl+Shift+O", enabled: hasPages,
                     action: function() { recognizeCurrentPage() } })
        return items
    }

    function openContextMenu(sceneX, sceneY) {
        showKeys = false
        menuItems = buildMenu()
        let h = 12
        for (const item of menuItems)
            h += item.separator ? 9 : 30
        menuPos = Qt.point(Math.max(4, Math.min(sceneX, width - 288)),
                           Math.max(header.height + 2, Math.min(sceneY, height - h - 4)))
        showMenu = true
    }

    MouseArea {   // click anywhere outside the menu to dismiss it
        anchors.fill: parent
        visible: root.showMenu
        z: 95
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: root.showMenu = false
    }

    Rectangle {
        id: contextMenu
        visible: root.showMenu
        z: 100
        x: root.menuPos.x
        y: root.menuPos.y
        width: 280
        height: menuColumn.implicitHeight + 12
        radius: Theme.cornerRadius
        color: Theme.darkerBackground
        border.color: Theme.mutedForeground
        border.width: 1

        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }   // swallow clicks

        Column {
            id: menuColumn
            x: 6
            y: 6
            width: parent.width - 12
            Repeater {
                model: root.menuItems
                delegate: Item {
                    width: menuColumn.width
                    height: modelData.separator ? 9 : 30

                    Rectangle {      // separator line
                        visible: !!modelData.separator
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        height: 1
                        color: Theme.mutedForeground
                        opacity: 0.35
                    }
                    Rectangle {      // row
                        visible: !modelData.separator
                        anchors.fill: parent
                        radius: Math.max(0, Theme.cornerRadius - 4)
                        color: rowArea.containsMouse && modelData.enabled
                               ? Theme.selection : "transparent"
                        Text {
                            x: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.label || ""
                            color: modelData.enabled ? Theme.foreground : Theme.mutedForeground
                            opacity: modelData.enabled ? 1 : 0.55
                            font.pixelSize: 13
                            renderType: Text.QtRendering
                        }
                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.shortcut || ""
                            color: Theme.mutedForeground
                            opacity: modelData.enabled ? 1 : 0.55
                            font.pixelSize: 12
                            renderType: Text.QtRendering
                        }
                        MouseArea {
                            id: rowArea
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: !!modelData.enabled
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.showMenu = false
                                modelData.action()
                            }
                        }
                    }
                }
            }
        }
    }



    // ---------- lyrics view ----------

    Rectangle {
        id: lyricsPanel
        visible: root.readActive && root.showLyrics
        z: 55
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 10
        anchors.topMargin: 8
        radius: Theme.cornerRadius + 6
        // Opaque, whatever alpha the theme gives its darker background.
        readonly property color solid: Qt.rgba(Theme.darkerBackground.r, Theme.darkerBackground.g,
                                               Theme.darkerBackground.b, 1)
        color: solid
        border.color: Theme.mutedForeground
        border.width: 1
        clip: true

        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }   // keep clicks off the page

        ListView {
            id: lyricsList
            anchors.fill: parent
            anchors.topMargin: 56
            anchors.bottomMargin: 140
            model: root.lyricsModel
            interactive: true
            boundsBehavior: Flickable.StopAtBounds
            cacheBuffer: 2000
            NumberAnimation {
                id: lyricsScroll
                target: lyricsList
                property: "contentY"
                duration: 520
                easing.type: Easing.OutCubic
            }

            delegate: Item {
                id: lyricRow
                required property var modelData
                required property int index
                readonly property bool isCurrent: index === Reader.unit
                readonly property bool silent: modelData.text === ""
                readonly property bool paragraphStart: index === 0
                    || root.lyricsModel[index - 1].paragraph !== modelData.paragraph
                width: lyricsList.width
                height: silent ? 0 : lineCell.height + (paragraphStart ? 34 : 12)
                visible: !silent

                Item {
                    id: lineCell
                    x: 44
                    y: lyricRow.paragraphStart ? 22 : 0
                    width: parent.width - 88
                    height: plainText.implicitHeight
                    transformOrigin: Item.Left
                    scale: lyricRow.isCurrent ? 1.0 : 0.72
                    opacity: lyricRow.isCurrent ? 1.0
                             : Math.max(0.14, 0.5 - 0.09 * Math.abs(lyricRow.index - Reader.unit))
                    Behavior on scale { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }
                    Behavior on opacity { NumberAnimation { duration: 320 } }

                    Text {
                        id: plainText
                        width: parent.width
                        text: lyricRow.isCurrent ? root.lyricsGradientHtml(lyricRow.modelData.text)
                                                 : lyricRow.modelData.text
                        textFormat: lyricRow.isCurrent ? Text.StyledText : Text.PlainText
                        wrapMode: Text.WordWrap
                        font.pixelSize: 40
                        font.weight: Font.Bold
                        lineHeight: 1.12
                        color: Theme.foreground
                        renderType: Text.QtRendering
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: Reader.jumpToParagraph(lyricRow.modelData.paragraph)
                    }
                }
            }
        }

        // Soften the top and bottom edges of the scrolling lines. The transparent
        // end keeps the panel's own colour so the gradient does not pass through black.
        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 150
            gradient: Gradient {
                GradientStop { position: 0.0; color: lyricsPanel.solid }
                GradientStop { position: 0.45; color: lyricsPanel.solid }
                GradientStop { position: 1.0; color: Qt.rgba(lyricsPanel.solid.r,
                    lyricsPanel.solid.g, lyricsPanel.solid.b, 0) }
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 250
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(lyricsPanel.solid.r,
                    lyricsPanel.solid.g, lyricsPanel.solid.b, 0) }
                GradientStop { position: 1.0; color: lyricsPanel.solid }
            }
        }

        Text {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 20
            text: "♫  " + (Doc.fileName !== "" ? Doc.fileName : "Reading")
            color: Theme.mutedForeground
            font.pixelSize: 12
            renderType: Text.QtRendering
        }
        HeaderButton {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 14
            label: "✕"
            onActivated: root.showLyrics = false
        }
    }


    // ---------- magnifier ----------

    Rectangle {
        id: lens
        visible: root.readActive && root.showLens && root.readMode === "doc" && !root.readAskInstall
        z: 58
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: readBar.top
        anchors.bottomMargin: 10
        width: Math.min(820, parent.width - 32)
        height: 210
        radius: Theme.cornerRadius + 6
        color: Qt.rgba(Theme.darkerBackground.r, Theme.darkerBackground.g, Theme.darkerBackground.b, 1)
        border.color: Theme.accent
        border.width: 2
        onWidthChanged: root.lensZ = Math.max(2.2, Math.min(4.0, (width - 12) / 300))

        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

        Item {
            id: lensView
            anchors.fill: parent
            anchors.margins: 6
            clip: true

            readonly property bool themed: Theme.pdfTheme
            Rectangle {
                anchors.fill: parent
                color: lensView.themed ? Theme.background : "white"
                radius: Math.max(0, lens.radius - 6)
            }

            Image {
                id: bandImage
                source: root.lensSource
                asynchronous: true
                retainWhileLoading: true
                cache: false
                smooth: true
                fillMode: Image.Stretch
                // Same recolor as the pages, so the lens matches PDF theme
                // (opaque here: the lens panel itself is solid).
                layer.enabled: lensView.themed
                layer.effect: ShaderEffect {
                    property color lightColor: Theme.background
                    property color darkColor: Theme.foreground
                    property real keepHue: 1.0
                    property real paperAlpha: 1.0
                    fragmentShader: "qrc:/shaders/recolor.frag.qsb"
                }
                width: root.lensBand.w * root.lensZ
                height: root.lensBand.h * root.lensZ
                // Keep the current word under the lens centre; the line slides past.
                readonly property real wordCx: root.lensWord ? root.lensWord.x + root.lensWord.w / 2 : 0
                readonly property real wordCy: root.lensWord ? root.lensWord.y + root.lensWord.h / 2 : 0
                x: lensView.width / 2 - (wordCx - root.lensBand.x) * root.lensZ
                y: lensView.height / 2 - (wordCy - root.lensBand.y) * root.lensZ
            }

            // The word being spoken stays fixed in the middle of the lens; only the
            // page slides underneath it.
            Rectangle {
                visible: root.lensWord !== null
                width: root.lensWord ? root.lensWord.w * root.lensZ + 10 : 0
                height: root.lensWord ? root.lensWord.h * root.lensZ + 8 : 0
                x: (lensView.width - width) / 2
                y: (lensView.height - height) / 2
                radius: 5
                color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.28)
                border.color: Theme.accent
                border.width: 2
            }

            Rectangle {      // soft edges, like looking through glass
                anchors.left: parent.left
                width: 60
                height: parent.height
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: lensView.themed ? Qt.rgba(Theme.background.r, Theme.background.g, Theme.background.b, 0.8) : "#ccffffff" }
                    GradientStop { position: 1.0; color: lensView.themed ? Qt.rgba(Theme.background.r, Theme.background.g, Theme.background.b, 0) : "#00ffffff" }
                }
            }
            Rectangle {
                anchors.right: parent.right
                width: 60
                height: parent.height
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: lensView.themed ? Qt.rgba(Theme.background.r, Theme.background.g, Theme.background.b, 0) : "#00ffffff" }
                    GradientStop { position: 1.0; color: lensView.themed ? Qt.rgba(Theme.background.r, Theme.background.g, Theme.background.b, 0.8) : "#ccffffff" }
                }
            }
        }

        HeaderButton {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            width: 24
            height: 24
            label: "✕"
            onActivated: root.showLens = false
        }
    }

    // ---------- read aloud bar ----------

    component ReadChip: Rectangle {
        id: chip
        property string label
        property bool on: false
        signal toggled()
        height: 24
        width: chipText.implicitWidth + 20
        radius: 12
        color: on ? Theme.selection : "transparent"
        border.color: on ? Theme.accent : Theme.mutedForeground
        border.width: 1
        Text {
            id: chipText
            anchors.centerIn: parent
            text: (chip.on ? "✓ " : "") + chip.label
            color: chip.on ? Theme.foreground : Theme.mutedForeground
            font.pixelSize: 11
            renderType: Text.QtRendering
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: chip.toggled()
        }
    }

    Rectangle {      // "follow reading" chip after the user scrolls away
        visible: root.readActive && root.readMode === "doc" && !root.readFollow
                 && Reader.state !== 0 && !root.readAskInstall
        z: 61
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: lens.visible ? lens.top : readBar.top
        anchors.bottomMargin: 8
        width: followText.implicitWidth + 24
        height: 28
        radius: 14
        color: Theme.accent
        Text {
            id: followText
            anchors.centerIn: parent
            text: "Jump back to reading"
            color: Theme.darkerBackground
            font.pixelSize: 12
            font.weight: Font.DemiBold
            renderType: Text.QtRendering
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                root.readFollow = true
                root.readUpdateHighlight()
            }
        }
    }

    Rectangle {      // voice picker
        visible: root.readActive && root.showVoices
        z: 62
        anchors.right: readBar.right
        anchors.rightMargin: 44
        anchors.bottom: readBar.top
        anchors.bottomMargin: 8
        width: 190
        height: Math.min(voiceCol.implicitHeight, Math.max(120, root.height - readBar.height - 90)) + 12
        radius: Theme.cornerRadius
        color: Theme.darkerBackground
        border.color: Theme.mutedForeground
        border.width: 1
        MouseArea { anchors.fill: parent }
        Flickable {
            x: 6
            y: 6
            width: parent.width - 12
            height: parent.height - 12
            clip: true
            contentWidth: width
            contentHeight: voiceCol.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            Column {
                id: voiceCol
                width: parent.width
                Repeater {
                    model: root.showVoices ? root.readVoiceList() : []
                    delegate: Rectangle {
                        width: voiceCol.width
                        height: 28
                        radius: Math.max(0, Theme.cornerRadius - 4)
                        color: voiceMouse.containsMouse ? Theme.selection : "transparent"
                        Text {
                            x: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.readVoiceLabel(modelData)
                            color: modelData === Reader.voice ? Theme.accent : Theme.foreground
                            font.pixelSize: 13
                            renderType: Text.QtRendering
                        }
                        MouseArea {
                            id: voiceMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                Reader.voice = modelData
                                root.showVoices = false
                            }
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        id: readBar
        visible: root.readActive
        z: 60
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 14
        width: Math.min(700, parent.width - 24)
        height: barColumn.implicitHeight + 22
        radius: Theme.cornerRadius + 4
        color: Theme.darkerBackground
        border.color: Theme.mutedForeground
        border.width: 1

        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }   // swallow clicks

        Column {
            id: barColumn
            x: 14
            y: 11
            width: parent.width - 28
            spacing: 9

            // One-time download prompt (read aloud is opt-in).
            Column {
                visible: root.readAskInstall
                width: parent.width
                spacing: 8
                Text {
                    width: parent.width
                    text: Reader.installing ? "Setting up read aloud…"
                                            : "Read aloud runs a speech model on your computer"
                    color: Theme.foreground
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    renderType: Text.QtRendering
                }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: Reader.installing ? (root.readInstallLog !== "" ? root.readInstallLog : "Starting…")
                          : (root.readInstallLog !== "" ? root.readInstallLog
                             : "It needs a one-time download of about 600 MB (the Kokoro voice model and a small Python runtime). Nothing is sent anywhere; everything stays local.")
                    color: Theme.mutedForeground
                    font.pixelSize: 12
                    renderType: Text.QtRendering
                }
                Row {
                    spacing: 8
                    Rectangle {
                        visible: !Reader.installing
                        width: dlText.implicitWidth + 24
                        height: 28
                        radius: 14
                        color: Theme.accent
                        Text {
                            id: dlText
                            anchors.centerIn: parent
                            text: "Download and enable"
                            color: Theme.darkerBackground
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            renderType: Text.QtRendering
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { root.readInstallLog = ""; Reader.install() }
                        }
                    }
                    Rectangle {
                        visible: !Reader.installing
                        width: cancelText.implicitWidth + 24
                        height: 28
                        radius: 14
                        color: "transparent"
                        border.color: Theme.mutedForeground
                        border.width: 1
                        Text {
                            id: cancelText
                            anchors.centerIn: parent
                            text: "Not now"
                            color: Theme.mutedForeground
                            font.pixelSize: 12
                            renderType: Text.QtRendering
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.closeReader()
                        }
                    }
                }
            }

            // Transport row.
            RowLayout {
                visible: !root.readAskInstall
                width: parent.width
                spacing: 4

                HeaderButton { width: 30; height: 30; label: "«"; onActivated: Reader.previousParagraph() }
                HeaderButton { width: 30; height: 30; label: "‹"; onActivated: Reader.previousSentence() }
                Rectangle {
                    Layout.preferredWidth: 38
                    Layout.preferredHeight: 38
                    radius: 19
                    color: Theme.accent
                    Text {
                        anchors.centerIn: parent
                        text: Reader.state === 2 ? "❚❚" : (Reader.state === 1 ? "…" : "▶")
                        color: Theme.darkerBackground
                        font.pixelSize: 14
                        renderType: Text.QtRendering
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.toggleReading()
                    }
                }
                HeaderButton { width: 30; height: 30; label: "›"; onActivated: Reader.nextSentence() }
                HeaderButton { width: 30; height: 30; label: "»"; onActivated: Reader.nextParagraph() }

                Item { Layout.fillWidth: true }

                HeaderButton { width: 26; height: 26; label: "−"; onActivated: root.readStepSpeed(-1) }
                Text {
                    text: Reader.speed.toFixed(2).replace(/0$/, "") + "×"
                    Layout.preferredWidth: 38
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.foreground
                    font.pixelSize: 12
                    renderType: Text.QtRendering
                }
                HeaderButton { width: 26; height: 26; label: "+"; onActivated: root.readStepSpeed(1) }

                Rectangle {
                    Layout.preferredWidth: voiceText.implicitWidth + 22
                    Layout.preferredHeight: 26
                    Layout.leftMargin: 8
                    radius: 6
                    color: voiceBtn.containsMouse || root.showVoices ? Theme.selection : "transparent"
                    border.color: root.showVoices ? Theme.accent : Theme.mutedForeground
                    border.width: 1
                    Text {
                        id: voiceText
                        anchors.centerIn: parent
                        text: root.readVoiceLabel(Reader.voice) + " ▾"
                        color: Theme.foreground
                        font.pixelSize: 12
                        renderType: Text.QtRendering
                    }
                    MouseArea {
                        id: voiceBtn
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.showVoices = !root.showVoices
                    }
                }
                HeaderButton { width: 26; height: 26; Layout.leftMargin: 4; label: "✕"; onActivated: root.closeReader() }
            }

            // Progress row.
            Item {
                visible: !root.readAskInstall && root.readMode === "doc"
                width: parent.width
                height: 18
                Rectangle {
                    id: progressTrack
                    anchors.left: parent.left
                    anchors.right: infoText.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    height: 4
                    radius: 2
                    color: Theme.mutedForeground
                    opacity: 1
                    Rectangle {
                        height: parent.height
                        radius: 2
                        color: Theme.accent
                        width: root.readList.length > 0
                               ? parent.width * Math.max(0, Reader.paragraph + 1) / root.readList.length : 0
                    }
                    MouseArea {
                        anchors.fill: parent
                        anchors.topMargin: -7
                        anchors.bottomMargin: -7
                        cursorShape: Qt.PointingHandCursor
                        onClicked: (mouse) => {
                            root.readFollow = true
                            Reader.jumpToParagraph(Math.floor(mouse.x / progressTrack.width * root.readList.length))
                        }
                    }
                }
                Text {
                    id: infoText
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: Reader.state === 1 ? "Loading voice…"
                          : "¶ " + (Reader.paragraph + 1) + " of " + root.readList.length
                            + " · p." + (Reader.paragraph >= 0 && root.readList[Reader.paragraph]
                                              ? root.readList[Reader.paragraph].page + 1 : "")
                            + " · " + root.readRemainingText()
                    color: Theme.mutedForeground
                    font.pixelSize: 11
                    renderType: Text.QtRendering
                }
            }
            Text {
                visible: !root.readAskInstall && root.readMode === "selection"
                text: Reader.state === 1 ? "Loading voice…" : "Reading the selection"
                color: Theme.mutedForeground
                font.pixelSize: 11
                renderType: Text.QtRendering
            }

            // What to leave out.
            Flow {
                visible: !root.readAskInstall && root.readMode === "doc"
                width: parent.width
                spacing: 6
                ReadChip { label: "Skip headers, footers & margin text"; on: root.readSkips.headers
                           onToggled: root.readToggleSkip("headers") }
                ReadChip { label: "Skip footnotes"; on: root.readSkips.footnotes
                           onToggled: root.readToggleSkip("footnotes") }
                ReadChip { label: "Skip captions"; on: root.readSkips.captions
                           onToggled: root.readToggleSkip("captions") }
                ReadChip { label: "Skip tables"; on: root.readSkips.tables
                           onToggled: root.readToggleSkip("tables") }
                ReadChip { label: "Skip equations"; on: root.readSkips.math
                           onToggled: root.readToggleSkip("math") }
                ReadChip { label: "Skip references"; on: root.readSkips.references
                           onToggled: root.readToggleSkip("references") }
                ReadChip { label: "\u2715 Don't read this"; on: false
                           onToggled: root.readSkipCurrent() }
            }
        }
    }


    // ---------- settings ----------

    MouseArea {   // click outside the panel to close it
        anchors.fill: parent
        visible: root.showSettings
        z: 90
        onClicked: root.showSettings = false
    }

    Rectangle {
        id: settingsPanel
        visible: root.showSettings
        z: 100
        anchors.top: parent.top
        anchors.topMargin: header.height + 6
        anchors.right: parent.right
        anchors.rightMargin: 8
        width: Math.min(440, parent.width - 16)
        height: settingsColumn.implicitHeight + 32
        radius: Theme.cornerRadius
        color: Theme.darkerBackground
        border.color: Theme.mutedForeground
        border.width: 1
        onVisibleChanged: root.confirmRemoveModel = false

        MouseArea { anchors.fill: parent }   // swallow clicks on the panel itself

        Column {
            id: settingsColumn
            x: 18
            y: 16
            width: parent.width - 36
            spacing: 14

            Text {
                text: "Settings"
                color: Theme.foreground
                font.pixelSize: 15
                font.weight: Font.DemiBold
                renderType: Text.QtRendering
            }

            Item {
                width: parent.width
                height: ptText.implicitHeight

                Column {
                    id: ptText
                    anchors.left: parent.left
                    anchors.right: ptSwitch.left
                    anchors.rightMargin: 16
                    spacing: 5
                    Text {
                        text: "PDF theme"
                        color: Theme.foreground
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        renderType: Text.QtRendering
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "Off shows the PDF exactly as it was made. On recolors pages to your system theme and makes the window translucent so your desktop blur shows through. Photos are never recolored. Toggle with Ctrl+I."
                        color: Theme.mutedForeground
                        font.pixelSize: 12
                        lineHeight: 1.15
                        renderType: Text.QtRendering
                    }
                }

                Rectangle {
                    id: ptSwitch
                    anchors.right: parent.right
                    anchors.top: parent.top
                    width: 44
                    height: 24
                    radius: 12
                    color: Theme.pdfTheme ? Theme.accent : Theme.selection
                    border.color: Theme.pdfTheme ? Theme.accent : Theme.mutedForeground
                    border.width: 1
                    Rectangle {
                        width: 18
                        height: 18
                        radius: 9
                        y: 3
                        x: Theme.pdfTheme ? parent.width - width - 3 : 3
                        color: Theme.pdfTheme ? Theme.darkerBackground : Theme.foreground
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: Theme.pdfTheme = !Theme.pdfTheme
                    }
                }
            }

            Item {
                visible: Theme.pdfTheme
                width: parent.width
                height: 24

                Text {
                    id: opLabel
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Opacity"
                    color: Theme.foreground
                    font.pixelSize: 12
                    renderType: Text.QtRendering
                }
                Text {
                    id: opValue
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: Math.round(Theme.pdfOpacity * 100) + "%"
                    color: Theme.mutedForeground
                    font.pixelSize: 12
                    renderType: Text.QtRendering
                }
                Rectangle {
                    id: opTrack
                    anchors.left: opLabel.right
                    anchors.leftMargin: 14
                    anchors.right: opValue.left
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    height: 4
                    radius: 2
                    color: Theme.selection
                    Rectangle {
                        width: parent.width * (Theme.pdfOpacity - 0.4) / 0.6
                        height: parent.height
                        radius: 2
                        color: Theme.accent
                    }
                    Rectangle {
                        width: 14
                        height: 14
                        radius: 7
                        anchors.verticalCenter: parent.verticalCenter
                        x: parent.width * (Theme.pdfOpacity - 0.4) / 0.6 - width / 2
                        color: Theme.foreground
                    }
                    MouseArea {
                        anchors.fill: parent
                        anchors.topMargin: -10
                        anchors.bottomMargin: -10
                        cursorShape: Qt.PointingHandCursor
                        function setFrom(mx) {
                            Theme.pdfOpacity = 0.4 + 0.6 * Math.max(0, Math.min(1, mx / opTrack.width))
                        }
                        onPressed: (m) => setFrom(m.x)
                        onPositionChanged: (m) => { if (pressed) setFrom(m.x) }
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.mutedForeground
                opacity: 0.35
            }

            Item {
                width: parent.width
                height: aiText.implicitHeight

                Column {
                    id: aiText
                    anchors.left: parent.left
                    anchors.right: aiSwitch.left
                    anchors.rightMargin: 16
                    spacing: 5
                    Text {
                        text: "AI features"
                        color: Theme.foreground
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        renderType: Text.QtRendering
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "Read aloud with a natural local voice, its lyrics and magnifier views, and the smart filter that skips headers, footers, tables and margin text. It all runs on your computer and nothing is uploaded. Switched off, none of it exists: no keys, menu entries, background processes or downloads."
                        color: Theme.mutedForeground
                        font.pixelSize: 12
                        lineHeight: 1.15
                        renderType: Text.QtRendering
                    }
                }

                Rectangle {
                    id: aiSwitch
                    anchors.right: parent.right
                    anchors.top: parent.top
                    width: 44
                    height: 24
                    radius: 12
                    color: root.aiOn ? Theme.accent : Theme.selection
                    border.color: root.aiOn ? Theme.accent : Theme.mutedForeground
                    border.width: 1
                    Rectangle {
                        width: 18
                        height: 18
                        radius: 9
                        y: 3
                        x: root.aiOn ? parent.width - width - 3 : 3
                        color: root.aiOn ? Theme.darkerBackground : Theme.foreground
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: Features.ai = !Features.ai
                    }
                }
            }

            Rectangle {
                visible: root.aiOn
                width: parent.width
                height: 1
                color: Theme.mutedForeground
                opacity: 0.35
            }

            Column {
                visible: root.aiOn
                width: parent.width
                spacing: 10

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: Reader.available
                          ? "Voice model: installed" + (Reader.diskUsage() !== "" ? " (" + Reader.diskUsage() + " on disk)" : "")
                          : "Voice model: not downloaded yet. It downloads the first time you press s."
                    color: Theme.foreground
                    font.pixelSize: 12
                    renderType: Text.QtRendering
                }

                Flow {
                    width: parent.width
                    spacing: 8

                    Rectangle {
                        visible: Reader.available
                        width: removeText.implicitWidth + 24
                        height: 28
                        radius: 14
                        color: root.confirmRemoveModel ? Theme.accent : "transparent"
                        border.color: root.confirmRemoveModel ? Theme.accent : Theme.mutedForeground
                        border.width: 1
                        Text {
                            id: removeText
                            anchors.centerIn: parent
                            text: root.confirmRemoveModel ? "Click again to delete it" : "Remove downloaded voice model"
                            color: root.confirmRemoveModel ? Theme.darkerBackground : Theme.foreground
                            font.pixelSize: 12
                            renderType: Text.QtRendering
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (!root.confirmRemoveModel) {
                                    root.confirmRemoveModel = true
                                } else {
                                    root.confirmRemoveModel = false
                                    root.closeReader()
                                    Reader.removeModel()
                                    root.setStatus("voice model removed")
                                }
                            }
                        }
                    }

                    Rectangle {
                        width: resetText.implicitWidth + 24
                        height: 28
                        radius: 14
                        color: "transparent"
                        border.color: Theme.mutedForeground
                        border.width: 1
                        Text {
                            id: resetText
                            anchors.centerIn: parent
                            text: "Reset what Glance learned"
                            color: Theme.foreground
                            font.pixelSize: 12
                            renderType: Text.QtRendering
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                Doc.resetReadingModel()
                                root.readAll = null
                                root.setStatus("learned filtering reset")
                            }
                        }
                    }
                }
            }

            Text {
                width: parent.width
                text: "Ctrl+, opens this panel."
                color: Theme.mutedForeground
                font.pixelSize: 11
                renderType: Text.QtRendering
            }
        }
    }

    // ---------- keybindings panel ----------
    // Each row: keys to show, a plain-language explanation, and (optionally) the
    // action to run when the row is clicked.
    readonly property var keySections: [
        { title: "Move around", items: [
            { keys: ["j", "k"], desc: "Scroll down / up. Put a number first to repeat: 5j scrolls five steps." },
            { keys: ["h", "l"], desc: "Scroll left / right. Only has an effect when the page is wider than the window, e.g. after zooming in." },
            { keys: ["J", "K"], desc: "Next / previous page (a whole spread in two-page view). 3J jumps three." },
            { keys: ["d", "u"], desc: "Half a screen down / up.  Ctrl+d and Ctrl+u do the same." },
            { keys: ["PgDn", "PgUp"], desc: "Almost a full screen down / up." },
            { keys: ["gg", "G"], desc: "Jump to the first / last page." },
            { keys: ["40G", ":"], desc: "Go to a page. Type 40 for the number printed on the page (the PDF's own numbering), or iv, +5, -3, $ for the last page.",
              action: function() { root.openCommand() } },
            { keys: ["Ctrl+o", "Alt+←"], desc: "Jump back to where you were before following a link or a go-to.",
              action: function() { root.goBack() } }
        ] },
        { title: "Search", items: [
            { keys: ["/", "Ctrl+f"], desc: "Search the document, forward. Enter runs it.",
              action: function() { root.openSearch(false) } },
            { keys: ["?"], desc: "Search backward.", action: function() { root.openSearch(true) } },
            { keys: ["n", "N"], desc: "Next / previous match (N goes the other way)." }
        ] },
        { title: "View", items: [
            { keys: ["w"], desc: "Fit the page width to the window.", action: function() { root.fitWidth() } },
            { keys: ["p"], desc: "Fit a whole page in the window.", action: function() { root.fitPage() } },
            { keys: ["Ctrl+0"], desc: "Actual size (100%).", action: function() { root.zoom = 1.0 } },
            { keys: ["=", "-"], desc: "Zoom in / out.", action: function() { root.zoomAt(view.width / 2, view.height / 2, 1.25) } },
            { keys: ["D"], desc: "Cycle the layout: single page, two pages with the cover alone, two pages side by side from page 1.",
              action: function() { root.cycleLayout() } },
            { keys: ["r"], desc: "Rotate 90 degrees.", action: function() { root.rotateCW() } },
            { keys: ["t"], desc: "Show page thumbnails.", action: function() { root.showThumbs = !root.showThumbs } },
            { keys: ["T"], desc: "Show the table of contents (outline).", action: function() { root.toggleOutline() } },
            { keys: ["f", "F11"], desc: "Full screen.", action: function() { root.toggleFullscreen() } }
        ] },
        { title: "Selecting and copying text", items: [
            { keys: ["Drag"], desc: "Select text. This works on scanned pages and screenshots too: Glance reads them in the background as you browse." },
            { keys: ["Double-click", "Triple-click"], desc: "Select a word (or a code identifier) / a whole line." },
            { keys: ["Ctrl+a"], desc: "Select all the text in the whole document, every page. A very long document takes a moment before it can be copied.",
              action: function() { root.selectAllDocument() } },
            { keys: ["Ctrl+Shift+a"], desc: "Select only the text read by OCR on this page, such as a screenshot or scan.",
              action: function() { root.selectAllOcrOnPage() } },
            { keys: ["c", "Ctrl+c"], desc: "Copy the selection, or the page as an image when nothing is selected.",
              action: function() { root.copyPage() } },
            { keys: ["Ctrl+Shift+o"], desc: "Read this page's text right now. Normally automatic.",
              action: function() { root.recognizeCurrentPage() } }
        ] },
        { title: "Read aloud", items: [
            { keys: ["s"], desc: "Read aloud. With text selected it reads the selection; otherwise it reads the document from the top of the window, skipping headers, footers, page numbers and footnotes. While reading, s pauses and resumes.",
              action: function() { root.toggleReading() } },
            { keys: ["[", "]"], desc: "Previous / next paragraph while reading." },
            { keys: ["{", "}"], desc: "Previous / next sentence while reading." },
            { keys: ["Click a paragraph"], desc: "While reading, continue from the paragraph you click. Click a dimmed (skipped) one to include it." },
            { keys: ["m"], desc: "Magnifier: a zoomed view of the page above the player that follows the word being spoken. Word position is estimated from the speech timing, so it can drift by a word or so.",
              action: function() { root.toggleLens() } },
            { keys: ["v"], desc: "Lyrics view: the sentences scroll past in large type over the document, current one in your accent colour. Click a line to jump there; Esc closes it.",
              action: function() { root.toggleLyrics() } },
            { keys: ["x"], desc: "Don't read this paragraph. Glance learns from it: similar margin text, figure labels and so on are skipped from then on, in every document." },
            { keys: ["Esc"], desc: "Stop reading." }
        ] },
        { title: "Mouse and trackpad", items: [
            { keys: ["Right-click"], desc: "Menu: copy, select all, previous / next page, back, reload, open the file's folder, zoom, read text." },
            { keys: ["Click a link"], desc: "Follow a PDF link. Web links open in your browser." },
            { keys: ["Pinch", "Ctrl+wheel"], desc: "Zoom around the pointer." },
            { keys: ["Space + drag"], desc: "Pan the page." },
            { keys: ["Click the page number"], desc: "Same as pressing :  to go to a page.", action: function() { root.openCommand() } }
        ] },
        { title: "File", items: [
            { keys: ["Ctrl+r"], desc: "Reload the file from disk and keep your place. Glance also reloads by itself when the file changes on disk (for example after you rebuild a PDF).",
              action: function() { root.reloadFile(false) } },
            { keys: ["o"], desc: "Open a file.", action: function() { picker.open() } },
            { keys: ["q", "Ctrl+q"], desc: "Quit.", action: function() { Qt.quit() } },
            { keys: ["F1"], desc: "Show or hide this list." }
        ] }
    ]

    MouseArea {   // click outside the panel to close it
        anchors.fill: parent
        visible: root.showKeys
        z: 90
        onClicked: root.showKeys = false
    }

    Rectangle {
        id: keysPanel
        visible: root.showKeys
        z: 100
        anchors.top: parent.top
        anchors.topMargin: header.height + 6
        anchors.right: parent.right
        anchors.rightMargin: 8
        width: Math.min(560, parent.width - 16)
        height: Math.min(parent.height - header.height - 20, keysColumn.implicitHeight + 24)
        radius: Theme.cornerRadius
        color: Theme.darkerBackground
        border.color: Theme.mutedForeground
        border.width: 1

        MouseArea { anchors.fill: parent }   // swallow clicks on the panel itself
        onVisibleChanged: if (visible) keysFlick.contentY = 0

        Flickable {
            id: keysFlick
            anchors.fill: parent
            anchors.margins: 12
            contentWidth: width
            contentHeight: keysColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: keysColumn
                width: keysFlick.width
                spacing: 4

                Text {
                    text: "Keybindings"
                    color: Theme.foreground
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    renderType: Text.QtRendering
                }
                Text {
                    width: parent.width
                    text: "Click any entry that has a pointing hand to run it. Esc or F1 closes this list."
                    color: Theme.mutedForeground
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                    renderType: Text.QtRendering
                    bottomPadding: 6
                }

                Repeater {
                    model: root.keySections.filter(s => root.aiOn || s.title !== "Read aloud")
                    delegate: Column {
                        width: keysColumn.width
                        spacing: 2
                        Text {
                            text: modelData.title.toUpperCase()
                            color: Theme.accent
                            font.pixelSize: 10
                            font.letterSpacing: 1
                            font.weight: Font.DemiBold
                            topPadding: 10
                            bottomPadding: 2
                            renderType: Text.QtRendering
                        }
                        Repeater {
                            model: modelData.items
                            delegate: Rectangle {
                                id: keyRow
                                width: parent.width
                                height: Math.max(chips.implicitHeight, descText.implicitHeight) + 12
                                radius: 6
                                color: rowMouse.containsMouse && modelData.action
                                       ? Theme.selection : "transparent"

                                Flow {
                                    id: chips
                                    x: 6
                                    y: 6
                                    width: 150
                                    spacing: 4
                                    Repeater {
                                        model: modelData.keys
                                        delegate: Rectangle {
                                            width: chipText.implicitWidth + 12
                                            height: chipText.implicitHeight + 6
                                            radius: 4
                                            color: Theme.background
                                            border.color: Theme.mutedForeground
                                            border.width: 1
                                            Text {
                                                id: chipText
                                                anchors.centerIn: parent
                                                text: modelData
                                                color: Theme.foreground
                                                font.pixelSize: 11
                                                font.family: "monospace"
                                                renderType: Text.QtRendering
                                            }
                                        }
                                    }
                                }
                                Text {
                                    id: descText
                                    x: 166
                                    y: 6
                                    width: parent.width - 166 - 8
                                    text: modelData.desc
                                    color: Theme.foreground
                                    font.pixelSize: 12
                                    wrapMode: Text.WordWrap
                                    renderType: Text.QtRendering
                                }
                                MouseArea {
                                    id: rowMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    enabled: !!modelData.action
                                    cursorShape: modelData.action ? Qt.PointingHandCursor : Qt.ArrowCursor
                                    onClicked: {
                                        root.showKeys = false
                                        modelData.action()
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ---------- shortcuts ----------
    // Single-key shortcuts are disabled while the search field has focus so
    // typing doesn't trigger navigation (or quit!).
    readonly property bool typing: searchField.activeFocus || cmdField.activeFocus

    Shortcut { sequence: "Ctrl+q"; onActivated: Qt.quit() }
    Shortcut { sequence: "F1"; onActivated: root.showKeys = !root.showKeys }
    Shortcut { sequence: "Ctrl+c"; enabled: !root.typing; onActivated: root.copyPage() }
    Shortcut { sequence: "Ctrl+a"; enabled: !root.typing; onActivated: root.selectAllDocument() }
    Shortcut { sequence: "Ctrl+Shift+a"; enabled: !root.typing; onActivated: root.selectAllOcrOnPage() }
    Shortcut { sequence: "Ctrl+r"; enabled: !root.typing; onActivated: root.reloadFile(false) }
    Shortcut { sequence: "Ctrl+Shift+o"; enabled: !root.typing;
               onActivated: root.recognizeCurrentPage() }
    Shortcut { sequence: "Ctrl+I"; onActivated: Theme.pdfTheme = !Theme.pdfTheme }
    Shortcut { sequence: "Ctrl+,"; onActivated: root.showSettings = !root.showSettings }
    Shortcut { sequence: "s"; enabled: !root.typing; onActivated: root.toggleReading() }
    Shortcut { sequence: "]"; enabled: !root.typing && root.readActive; onActivated: Reader.nextParagraph() }
    Shortcut { sequence: "["; enabled: !root.typing && root.readActive; onActivated: Reader.previousParagraph() }
    Shortcut { sequence: "m"; enabled: !root.typing && root.readActive; onActivated: root.toggleLens() }
    Shortcut { sequence: "v"; enabled: !root.typing && root.readActive; onActivated: root.toggleLyrics() }
    Shortcut { sequence: "x"; enabled: !root.typing && root.readActive; onActivated: root.readSkipCurrent() }
    Shortcut { sequence: "}"; enabled: !root.typing && root.readActive; onActivated: Reader.nextSentence() }
    Shortcut { sequence: "{"; enabled: !root.typing && root.readActive; onActivated: Reader.previousSentence() }
    Shortcut { sequence: "Escape"; onActivated: {
        root.vimCount = 0
        root.gPending = false
        if (root.showSettings)
            root.showSettings = false
        else if (root.showVoices)
            root.showVoices = false
        else if (root.showLens)
            root.showLens = false
        else if (root.showLyrics)
            root.showLyrics = false
        else if (root.showMenu)
            root.showMenu = false
        else if (root.showKeys)
            root.showKeys = false
        else if (root.cmdActive)
            root.closeCommand()
        else if (root.searchActive)
            root.closeSearch()
        else if (root.readActive)
            root.closeReader()
        else
            root.clearSelection()
    } }
    Shortcut { sequence: "q"; enabled: !root.typing; onActivated: Qt.quit() }
    Shortcut { sequence: "Ctrl+f"; onActivated: root.openSearch(false) }

    // vim-style navigation: counts, motions, / ? : and n N.
    Shortcut { sequence: "/"; enabled: !root.typing; onActivated: root.openSearch(false) }
    Shortcut { sequence: "?"; enabled: !root.typing; onActivated: root.openSearch(true) }
    Shortcut { sequence: ":"; enabled: !root.typing; onActivated: root.openCommand() }
    Shortcut { sequence: "Ctrl+g"; onActivated: root.openCommand() }
    Shortcut { sequence: "Ctrl+o"; enabled: !root.typing; onActivated: root.goBack() }
    Shortcut { sequence: "Alt+Left"; enabled: !root.typing; onActivated: root.goBack() }
    Shortcut { sequence: "n"; enabled: root.searchActive && !root.typing;
               onActivated: root.runSearch(false, root.searchBackward ? -1 : 1) }
    Shortcut { sequence: "Shift+n"; enabled: root.searchActive && !root.typing;
               onActivated: root.runSearch(false, root.searchBackward ? 1 : -1) }
    Shortcut { sequence: "j"; enabled: !root.typing
               onActivated: root.scrollBy(0, 80 * root.takeCount()) }
    Shortcut { sequence: "k"; enabled: !root.typing
               onActivated: root.scrollBy(0, -80 * root.takeCount()) }
    Shortcut { sequence: "h"; enabled: !root.typing
               onActivated: root.scrollBy(-80 * root.takeCount(), 0) }
    Shortcut { sequence: "l"; enabled: !root.typing
               onActivated: root.scrollBy(80 * root.takeCount(), 0) }
    Shortcut { sequence: "Shift+j"; enabled: !root.typing
               onActivated: root.jumpTo(root.currentPage + root.takeCount() * root.pagesPerRow, false) }
    Shortcut { sequence: "Shift+k"; enabled: !root.typing
               onActivated: root.jumpTo(Math.max(1, root.currentPage - root.takeCount() * root.pagesPerRow), false) }
    Shortcut { sequence: "d"; enabled: !root.typing
               onActivated: root.scrollBy(0, view.height / 2 * root.takeCount()) }
    Shortcut { sequence: "u"; enabled: !root.typing
               onActivated: root.scrollBy(0, -view.height / 2 * root.takeCount()) }
    Shortcut { sequence: "Ctrl+d"; enabled: !root.typing
               onActivated: root.scrollBy(0, view.height / 2 * root.takeCount()) }
    Shortcut { sequence: "Ctrl+u"; enabled: !root.typing
               onActivated: root.scrollBy(0, -view.height / 2 * root.takeCount()) }
    Shortcut { sequence: "PgDown"; enabled: !root.typing
               onActivated: root.scrollBy(0, view.height * 0.9 * root.takeCount()) }
    Shortcut { sequence: "PgUp"; enabled: !root.typing
               onActivated: root.scrollBy(0, -view.height * 0.9 * root.takeCount()) }
    // gg = top; NG / :N = page N (printed label first); G alone = last page.
    Shortcut { sequence: "g"; enabled: !root.typing; onActivated: {
        if (root.gPending) {
            root.gPending = false
            if (root.vimCount > 0) {
                root.gotoPage(String(root.takeCount()))
            } else {
                root.pushNav()
                root.jumpTo(1, false)
            }
        } else {
            root.gPending = true
            gPendingTimer.restart()
        }
    } }
    Shortcut { sequence: "Shift+g"; enabled: !root.typing; onActivated: {
        if (root.vimCount > 0) {
            root.gotoPage(String(root.takeCount()))
        } else {
            root.pushNav()
            root.jumpTo(root.pageCount, false)
        }
    } }
    Timer { id: gPendingTimer; interval: 800; onTriggered: root.gPending = false }
    Item {
        visible: false
        Repeater {
            model: 10
            delegate: Item {
                Shortcut {
                    sequence: String(index)
                    enabled: !root.typing && (index > 0 || root.vimCount > 0)
                    onActivated: root.vimCount = Math.min(99999, root.vimCount * 10 + index)
                }
            }
        }
    }

    Shortcut { sequence: "Shift+t"; enabled: !root.typing; onActivated: root.toggleOutline() }
    Shortcut { sequence: "c"; enabled: !root.typing; onActivated: root.copyPage() }
    Shortcut { sequence: "f"; enabled: !root.typing; onActivated: root.toggleFullscreen() }
    Shortcut { sequence: "F11"; onActivated: root.toggleFullscreen() }
    Shortcut { sequence: "="; enabled: !root.typing; onActivated: root.zoomAt(view.width / 2, view.height / 2, 1.25) }
    Shortcut { sequence: "-"; enabled: !root.typing; onActivated: root.zoomAt(view.width / 2, view.height / 2, 0.8) }
    Shortcut { sequence: "w"; enabled: !root.typing; onActivated: root.fitWidth() }
    Shortcut { sequence: "p"; enabled: !root.typing; onActivated: root.fitPage() }
    Shortcut { sequence: "Ctrl+0"; onActivated: root.zoom = 1.0 }
    Shortcut { sequence: "r"; enabled: !root.typing; onActivated: root.rotateCW() }
    Shortcut { sequence: "Shift+d"; enabled: !root.typing; onActivated: root.cycleLayout() }
    Shortcut { sequence: "t"; enabled: !root.typing; onActivated: root.showThumbs = !root.showThumbs }
    Shortcut { sequence: "o"; enabled: !root.typing; onActivated: picker.open() }

    // ---------- remembered view ----------
    Timer {
        id: restoreTimer
        property var state: null
        interval: 40
        onTriggered: {
            const st = state
            state = null
            if (!st) {
                root.viewStateReady = true
                return
            }
            if (st.rotation !== undefined)
                root.rotation = Number(st.rotation)
            let layoutChanged = false
            if (st.perRow !== undefined && Number(st.perRow) === 2 && root.pageCount > 1) {
                root.pagesPerRow = 2
                root.coverAlone = st.cover !== false
                root.rebuildLayout()
                layoutChanged = true
            }
            if (st.fit !== true && Number(st.zoom) > 0) {
                root.autoFitSinglePage = false
                root.zoom = root.clampZoom(Number(st.zoom))
            } else if (layoutChanged) {
                root.autoFitSinglePage = false
                root.zoom = root.fitWidthZoom()
            }
            scrollTimer.page = Math.max(0, Math.min(root.pageCount - 1, Number(st.page)))
            scrollTimer.offset = Number(st.offset) || 0
            scrollTimer.start()
        }
    }
    Timer {
        id: scrollTimer
        property int page: 0
        property real offset: 0
        interval: 40
        onTriggered: {
            const y = root.pageTopAt(page, root.zoom) + offset * root.sheetHpt(page) * root.zoom
            view.contentY = Math.max(0, Math.min(y, Math.max(0, view.contentHeight - view.height)))
            root.viewStateReady = true
            root.detectPage()
        }
    }
    Timer {
        id: saveViewTimer
        interval: 1500
        onTriggered: root.saveViewState()
    }
    Connections {
        target: view
        function onContentYChanged() { saveViewTimer.restart() }
    }
    onRotationChanged: saveViewTimer.restart()
    Component.onDestruction: saveViewState()
    onClosing: saveViewState()
    Connections {
        target: Qt.application
        function onAboutToQuit() { root.saveViewState() }
    }

    // ---------- lifecycle ----------

    Component.onCompleted: {
        computeSizes()
        pageLayoutReadyTimer.start()
        if (isImageDoc)
            Ocr.prewarm()
        scheduleVisibleOcr()
        if (Qt.application.arguments.indexOf("--perf") !== -1)
            perfStart.start()
    }
    onPageCountChanged: {
        Ocr.cancel()
        ocrGeneration += 1
        ocrPages = ({})
        ocrRegionKeys = ({})
        nativeHoverCache = ({})
        nativePageBoxes = ({})
        pendingOcrRegion = null
        hoverRegionKey = ""
        ocrStatus = ""
        pageLayoutReady = false
        computeSizes()
        pageLayoutReadyTimer.restart()
        prefetchAround()
        if (isImageDoc)
            Ocr.prewarm()
        scheduleVisibleOcr()
    }

    Timer {
        id: pageLayoutReadyTimer
        interval: 0
        onTriggered: root.pageLayoutReady = true
    }
    Connections {
        target: view
        function onWidthChanged(w) {
            if ((!root.initialFitDone || root.autoFitSinglePage) && root.pageCount > 0)
                root.fitWidth(true)
        }
    }
    onCurrentPageChanged: {
        // Learn about the pages OCR read-ahead will want, off the GUI thread.
        for (const offset of [0, 1, 2, 3, -1])
            Doc.requestPageInfo(currentPage - 1 + offset)
        prefetchAround()
        nearbyOcrTimer.stop()
        if (Ocr.running && ocrRequestedPage !== currentPage - 1) {
            Ocr.cancel()
            ocrGeneration += 1
            ocrRequestedPage = -1
        }
        scheduleVisibleOcr()
    }

    // ---------- temporary perf probe (--perf) ----------
    FrameAnimation {
        id: perfFramesAnim
        running: false
        property real lastT: 0
        onTriggered: {
            root.perfFrames += 1
            if (lastT > 0) {
                const dt = nowMs() - lastT
                if (dt > 20)
                    console.log("GLANCE DROP " + Math.round(dt) + "ms")
            }
            lastT = nowMs()
        }
    }
    NumberAnimation {
        id: perfZoomAnim
        target: root
        property: "zoom"
        from: 1.0
        to: 3.0
        duration: 3000
        easing.type: Easing.InOutSine
    }
    FrameAnimation {
        id: perfPinchAnim
        running: false
        property real startedAt: 0
        onTriggered: {
            const elapsed = nowMs() - startedAt
            root.pinchPreviewScale = 1 + Math.min(1, elapsed / 3000)
        }
    }
    NumberAnimation {
        id: perfScrollAnim
        target: view
        property: "contentY"
        from: 0
        to: 20000
        duration: 3000
        easing.type: Easing.Linear
    }
    Timer { id: perfStart; interval: 1500; onTriggered: {
        root.perfFrames = 0
        perfFramesAnim.running = true
        root.pinchAnchorViewportX = view.width / 2
        root.pinchAnchorViewportY = view.height / 2
        root.pinchAnchorContentX = view.contentX + view.width / 2
        root.pinchAnchorContentY = view.contentY + view.height / 2
        root.pinchPreviewScale = 1
        root.pinchPreviewActive = true
        perfPinchAnim.startedAt = nowMs()
        perfPinchAnim.running = true
        perfStopZoom.restart()
    } }
    Timer { id: perfStopZoom; interval: 3000; onTriggered: {
        console.log("PERF pinch fps=" + (root.perfFrames / 3.0).toFixed(1))
        perfPinchAnim.running = false
        const factor = root.pinchPreviewScale
        root.pinchPreviewActive = false
        root.pinchPreviewScale = 1
        root.zoomAt(root.pinchAnchorViewportX, root.pinchAnchorViewportY, factor)
        root.perfFrames = 0
        perfScrollAnim.restart()
        perfStopScroll.restart()
    } }
    Timer { id: perfStopScroll; interval: 3000; onTriggered: {
        perfFramesAnim.running = false
        console.log("PERF scroll fps=" + (root.perfFrames / 3.0).toFixed(1))
    } }
}
