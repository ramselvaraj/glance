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

    color: Theme.background
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
        if (selectionText !== "" && Doc.copyTextToClipboard(selectionText))
            return
        if (pageCount > 0 && Doc.copyPageToClipboard(currentPage - 1))
            console.log("glance: page copied to clipboard")
    }

    function clearSelection() {
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
        const hasText = Doc.pageHasText(page)
        const capabilities = Object.assign({}, pageTextCapabilities)
        capabilities[page] = hasText
        pageTextCapabilities = capabilities
        if (hasText && explicit) {
            ocrStatus = "page already has selectable text"
            return false
        }
        if (hasText) {
            const size = Doc.pageSizePt(page)
            const nativeSelection = Doc.selectText(page, Qt.point(0, 0),
                                                    Qt.point(size.width, size.height))
            const boxes = Object.assign({}, nativePageBoxes)
            boxes[page] = nativeSelection.boxes
            nativePageBoxes = boxes
            if (!Doc.pageHasImages(page) || nativeSelection.text.length >= 200)
                return false
        }
        ocrGeneration += 1
        ocrRequestedPage = page
        ocrExplicit = explicit
        if (explicit)
            ocrStatus = "recognizing page " + (page + 1)
        Ocr.recognize(Doc.filePath, page, Doc.pageSizePt(page), ocrGeneration)
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
        const result = Doc.selectTextAt(page, point, "word")
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
        if (!selection)
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
        Ocr.recognizeRegion(Doc.filePath, selection.page, size, region, ocrGeneration)
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

    function openFromUrl(url) {
        if (!url)
            return
        const path = url.toString().startsWith("file://")
                    ? decodeURIComponent(url.toString().substring(7))
                    : url.toString()
        saveViewState()
        if (Doc.open(path)) {
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

            HeaderButton { label: "s"; tip: "search"; activeFlag: root.searchActive;
                           onActivated: {
                               root.searchActive = !root.searchActive
                               if (root.searchActive) searchField.forceActiveFocus()
                               else root.closeSearch()
                           } }
            HeaderButton { label: "T"; tip: "outline"; activeFlag: root.showOutline;
                           onActivated: root.toggleOutline() }
            HeaderButton { label: "c"; tip: "copy selection or page"; onActivated: root.copyPage() }
            HeaderButton {
                label: "ocr"
                tip: Ocr.running ? "cancel recognition" : "recognize current page"
                onActivated: {
                    root.recognizeCurrentPage()
                }
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
            HeaderButton { label: "w"; tip: "fit width"; onActivated: root.fitWidth() }
            HeaderButton { label: "p"; tip: "fit page"; onActivated: root.fitPage() }
            HeaderButton { label: "1"; tip: "100%"; onActivated: root.zoom = 1.0 }
            HeaderButton { label: "2"; tip: "two-page view"; activeFlag: root.pagesPerRow === 2
                           onActivated: root.cycleLayout() }
            HeaderButton { label: "r"; tip: "rotate"; onActivated: root.rotateCW() }
            HeaderButton { label: "t"; tip: "thumbnails"; activeFlag: root.showThumbs;
                           onActivated: root.showThumbs = !root.showThumbs }
            HeaderButton { label: "f"; tip: "fullscreen"; onActivated: root.toggleFullscreen() }
            HeaderButton { label: "o"; tip: "open file"; onActivated: picker.open() }
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
                            onLoaded: {
                                const capabilities = Object.assign({}, root.pageTextCapabilities)
                                capabilities[index] = Doc.pageHasText(index)
                                root.pageTextCapabilities = capabilities
                            }
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

    // ---------- shortcuts ----------
    // Single-key shortcuts are disabled while the search field has focus so
    // typing doesn't trigger navigation (or quit!).
    readonly property bool typing: searchField.activeFocus || cmdField.activeFocus

    Shortcut { sequence: "Ctrl+q"; onActivated: Qt.quit() }
    Shortcut { sequence: "Ctrl+c"; enabled: !root.typing; onActivated: root.copyPage() }
    Shortcut { sequence: "Ctrl+Shift+o"; enabled: !root.typing;
               onActivated: root.recognizeCurrentPage() }
    Shortcut { sequence: "Escape"; onActivated: {
        root.vimCount = 0
        root.gPending = false
        if (root.cmdActive)
            root.closeCommand()
        else if (root.searchActive)
            root.closeSearch()
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
    Shortcut { sequence: "J"; enabled: !root.typing
               onActivated: root.jumpTo(root.currentPage + root.takeCount() * root.pagesPerRow, false) }
    Shortcut { sequence: "K"; enabled: !root.typing
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
    Shortcut { sequence: "G"; enabled: !root.typing; onActivated: {
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

    Shortcut { sequence: "T"; enabled: !root.typing; onActivated: root.toggleOutline() }
    Shortcut { sequence: "c"; enabled: !root.typing; onActivated: root.copyPage() }
    Shortcut { sequence: "f"; enabled: !root.typing; onActivated: root.toggleFullscreen() }
    Shortcut { sequence: "F11"; onActivated: root.toggleFullscreen() }
    Shortcut { sequence: "="; enabled: !root.typing; onActivated: root.zoomAt(view.width / 2, view.height / 2, 1.25) }
    Shortcut { sequence: "-"; enabled: !root.typing; onActivated: root.zoomAt(view.width / 2, view.height / 2, 0.8) }
    Shortcut { sequence: "w"; enabled: !root.typing; onActivated: root.fitWidth() }
    Shortcut { sequence: "p"; enabled: !root.typing; onActivated: root.fitPage() }
    Shortcut { sequence: "Ctrl+0"; onActivated: root.zoom = 1.0 }
    Shortcut { sequence: "r"; enabled: !root.typing; onActivated: root.rotateCW() }
    Shortcut { sequence: "D"; enabled: !root.typing; onActivated: root.cycleLayout() }
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
