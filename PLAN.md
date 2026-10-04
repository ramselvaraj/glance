# Glance Build Plan

## Product Goal

Glance is an Omarchy-native PDF and image viewer with the directness of macOS
Preview: opening a file creates one independent window and process, documents
remain responsive under touchpad input, and common reading tasks do not require
a document-management interface.

Glance deliberately has no gallery, library, recent-files database, shared
instance, or background directory scanning.

## Current Baseline

Implemented:

- PDF and common image loading through MuPDF.
- Continuous pages with asynchronous rendering and render-ahead.
- Touchpad scrolling, momentum, pinch zoom, Ctrl+wheel zoom, and drag panning.
- Cursor-anchored zoom, fit-width, fit-page, rotation, and fullscreen.
- Thumbnails and PDF outline navigation.
- Text search with result highlighting.
- Whole-page text copy.
- Omarchy theme loading and hot reload.
- Desktop entry, icon, and local MIME associations.
- Backend self-test for outlines and text search.

Known limitations:

- Pinch zoom relayouts every page delegate. Cost grows with document length.
- Text cannot yet be selected directly on a page.
- Scanned/image-only pages have no searchable or selectable text.
- Packaging and installation are not reproducible yet.
- Temporary performance and self-test paths remain in production code.

## Design Direction

### Document Text Module

Put text extraction, hit testing, range construction, selection geometry, and
clipboard serialization behind one small interface in the C++ document layer.
QML should provide pointer coordinates and render returned rectangles; it
should not reconstruct MuPDF's text model itself.

Proposed interface:

```cpp
QVariantMap hitTestText(int page, QPointF point);
QVariantMap selectText(QVariantMap anchor, QVariantMap focus);
QString selectedText(QVariantMap range);
QVariantList selectionRects(QVariantMap range);
```

The exact types can change during implementation, but callers should only need
an anchor, a focus, selected text, and display rectangles. Extraction caches,
reading-order repair, Unicode normalization, and MuPDF objects remain hidden in
the implementation.

### OCR Module

OCR is a fallback text source behind the same selection/search interface, not a
second UI code path. Embedded PDF text remains authoritative when useful text
exists. OCR runs only when requested or when a page is detected as image-only.

The OCR implementation must:

- Run off the UI thread.
- Support cancellation when a document closes.
- Cache results by file identity, page, raster parameters, OCR language, and
  engine version.
- Expose progress and failure without blocking reading.
- Store word boxes and normalized text so search and selection use the same
  representation.
- Avoid modifying the original file unless an explicit export feature is added.

MuPDF vendors Tesseract sources, but that does not guarantee the current build
links OCR support. The implementation phase must first choose between MuPDF OCR
integration and a system Tesseract adapter based on build size, maintenance,
language-data discovery, and cancellation support.

## Phases

### Phase 1: Stabilize The Viewer

- Fix large-document pinch lag by applying transient scene-graph scaling during
  an active gesture and committing document zoom once at gesture end.
- Preserve exact centroid anchoring when the committed zoom is applied.
- Tune and verify touchpad momentum, drag cursor states, image resize fitting,
  and behavior at document bounds.
- Remove temporary logging after measurements are captured.

Acceptance criteria:

- A 500-page PDF tracks a touchpad pinch without page-count-dependent input lag.
- The point under the pinch centroid remains visually fixed.
- New scroll or pinch input immediately cancels momentum.
- Single images refit when their window is resized until the user chooses a
  manual zoom level.

### Phase 2: Native PDF Text Selection

- Extract structured text spans, lines, characters, and bounds through MuPDF.
- Cache extracted page text independently of rendered page textures.
- Implement point-to-character hit testing.
- Implement forward and backward selection within one page.
- Extend selection across page boundaries in document reading order.
- Draw selection rectangles above page images.
- Copy selected text with sensible spaces, line breaks, and paragraph breaks.
- Add double-click word selection and triple-click line/paragraph selection if
  Qt pointer event handling supports them reliably.
- Primary drag selects when a page has a text layer; Space+drag pans with the
  hand tool; image-only pages retain primary-button panning until OCR supplies
  a text layer.

Acceptance criteria:

- Selection begins at the glyph nearest the pointer.
- Dragging in either direction produces the same logical range.
- Multi-line and multi-page selections render and copy correctly.
- Rotation and zoom do not change the logical selected text.
- Selection does not trigger new page rasterization.

### Phase 3: Unify Search, Copy, And Selection

- Reuse the document text module for search result locations.
- Replace whole-page copy as the primary copy behavior: copy the active
  selection when present, otherwise retain whole-page copy as an explicit
  command.
- Add next/previous result navigation and result counts.
- Preserve selection/search state during render-bucket changes.
- Define behavior when embedded text has broken encoding or invalid geometry.

Acceptance criteria:

- Search highlights and selection rectangles use the same coordinate system.
- Copying a search result yields the displayed logical text.
- Existing outline and search backend tests remain green.

### Phase 4: OCR For Scanned Pages And Images

Status: current-page OCR is implemented as cancellable, low-priority
`pdftoppm` and Tesseract subprocesses. It supplies word boxes to selection,
copy, and search and uses an in-memory file/page cache. Whole-document OCR,
language selection/discovery, the shared C++ text representation, and a bounded
disk cache remain.

- Detect pages with no useful embedded text.
- Add an explicit `Recognize Text` command for the current page and document.
- Implement the chosen OCR adapter and language-data discovery.
- Convert OCR words and boxes into the document text module's representation.
- Cache OCR output on disk with bounded storage and invalidation.
- Surface progress, cancellation, missing language data, and recognition errors.
- Feed OCR text into selection, copy, and search without changing those QML
  interfaces.

Acceptance criteria:

- An image-only PDF becomes searchable and selectable after OCR.
- A standalone image can be recognized and its text selected/copied.
- OCR never blocks scrolling, zooming, or closing the window.
- Reopening an unchanged file reuses cached OCR results.
- Embedded-text PDFs do not run OCR automatically.

### Phase 5: Annotation And Export Decision

This phase requires an explicit product decision before implementation.

Possible scope:

- Highlight selected text.
- Underline or strike out selected text.
- Add simple notes.
- Save to a new PDF by default, preserving the original.
- Export an OCR-text-layer PDF.

Out of scope unless separately approved:

- Full PDF editing.
- Page rearrangement or document merging.
- Form authoring.
- Signature workflows.
- Cloud synchronization.

### Phase 6: Packaging And Release

- Add an Arch/Omarchy `PKGBUILD` or equivalent reproducible package recipe.
- Install the executable, desktop entry, icon, and MIME definitions from the
  package.
- Document runtime/build dependencies and OCR language packages.
- Remove development symlinks from the expected installation flow.
- Update `README.md` with all commands, gestures, shortcuts, and limitations.
- Add version information and release notes.

Acceptance criteria:

- A clean Omarchy installation can build and install Glance from the package.
- PDF and supported image MIME types open Glance from the file manager.
- Uninstall removes packaged files and leaves user documents/caches alone.

## Test Plan

### Automated

- Unit tests for text normalization, range ordering, and rectangle coalescing.
- Document tests for hit testing and selection against known PDF fixtures.
- Search tests shared by embedded text and OCR text sources.
- OCR cache-key and invalidation tests using a fake OCR adapter.
- Backend smoke tests for PDF, PNG, JPEG, WebP, rotation, and invalid files.
- Performance fixture for a large PDF that measures pinch-update cost independently
  of page count.

### Hardware Acceptance

- Fractional-scale internal display and integer-scale external display.
- Touchpad scroll, momentum, pinch, Ctrl+wheel, and mouse drag.
- Small, large, mixed-page-size, rotated, encrypted, and malformed PDFs.
- Text PDFs, scanned PDFs, mixed text/scanned PDFs, and standalone images.
- Multiple files open as independent processes and windows.

## Delivery Order

1. Stabilize large-document zoom and interaction regressions.
2. Build native PDF text selection behind the document text module.
3. Move search and copy onto that module.
4. Add OCR as a fallback adapter to the same module.
5. Decide whether annotation/export belongs in the first release.
6. Package, document, and run final acceptance testing.

Text selection comes before OCR intentionally: OCR should supply text to an
already-working selection/search model rather than create a parallel feature
stack.
