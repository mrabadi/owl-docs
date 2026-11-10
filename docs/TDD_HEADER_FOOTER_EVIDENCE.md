# Header/footer editing evidence

Historical evidence for the 0.6.0/0.6.1 implementation follows. The fixed-region
and Narrow-margin rejection contract is superseded in 0.6.2 by measured story
space reserved during body pagination. See [the margin regression evidence](TDD_MARGINS_0.6.2.md).

Executed against the working tree on 2026-11-09. Builds use
`flock /tmp/owldocs-build.lock cmake --build build --target header_footer_ui_tests -j2`;
focused execution uses `QT_QPA_PLATFORM=offscreen ./build/header_footer_ui_tests`.

The requested entry/exit behavior follows Microsoft's [editing existing headers
and footers](https://support.microsoft.com/en-gb/word/edit-your-existing-headers-and-footers-in-word):
double-click or use Insert to enter, and use Close Header and Footer or Escape
to leave.

## Inline appearance and multiline text

- RED: rendering the same header before and during editing reported
  `entering header editing changes rendered text size or page position`.
- GREEN: frameless input widgets consume the same page geometry and rendering
  as the final page; header/footer typography now uses the same logical point
  convention as body text, avoiding an additional display-DPI conversion.
- RED: a two-line header reported
  `header newlines are not rendered as distinct document lines`.
- GREEN: one shared `QTextLayout` path wraps and aligns header/footer lines,
  resolves PAGE/NUMPAGES values, and positions inline image slots.

## Caret and displayed-field hit testing

- RED: inline editing reported
  `focused inline story has no caret at its rendered text position`.
- GREEN: caret and selection overlays use the shared resolved story layout.
- RED: clicking the end of `Page 1 of 1` reported
  `clicking the displayed page-field text puts the caret inside its hidden source token`.
- GREEN: pointer coordinates map through the same source/display offset maps
  used for rendering. The stored `{PAGE}`/`{PAGES}` vocabulary and native DOCX
  fields remain unchanged.

The above focused cycles were built and executed. Missing executables during
another agent's clean build were treated as infrastructure, never as RED evidence.

## Direct editing and guarded body actions

- RED: a single margin click entered story mode. GREEN: double-click enters the
  clicked region, ordinary body click exits, and Escape/Close exit explicitly.
  Header/Footer, region selection, Page Number and Page Count are directly
  available; the body dims while editing.
- RED: Left entered the source of a displayed page-count field. GREEN: field
  boundaries are atomic for horizontal navigation and deletion, and Undo
  restores the exact field source.
- RED: formatting a header changed a hidden body selection; a separate RED
  reproduced the same issue for Insert Equation. GREEN: body operations are
  guarded while plain-text story editing is active, and the controls report the
  document's actual story font rather than a stale body font.
- RED: typing in the footer of page 2 at 150% zoom scrolled to the old body
  caret. GREEN: the selected footer remains visible and focused at the same
  scroll position after the edit.

## Bounded text and pictures

- RED: an oversized typed insertion changed both the source and revision.
  GREEN: a validated proposal uses the shared wrapped layout (including image
  ascent and the widest page-number result) before committing. Rejection leaves
  the semantic document, revision, widget source and last valid caret intact,
  and reports which margin to increase through persistent status.
- RED: oversized imported text painted below the inline region. The fixture
  was corrected to hide the unrelated body caret and the RED was replayed
  against a freshly compiled unclipped renderer. GREEN: canvas, input, print
  and PDF use the same clipping rectangle; existing oversized source survives
  entry unchanged, can be shortened, and has an amber overflow marker plus
  margin guidance. Automatic margin growth remains outside this release.
- RED: a newly pasted portrait stored a height larger than its displayed
  region. GREEN: new picture size uses the actual region and line descent;
  combined text/picture height is validated before its single transaction.

## Clipboard and picture identity

- RED: Select All and menu clipboard commands targeted hidden body text.
  GREEN: Copy, Cut, Select All and Paste as Text Only use the active story and
  alignment region; regression assertions keep body paragraphs unchanged.
- RED: deleting the first of two adjacent pictures in the right region
  rejected or lost its distinct neighbor. GREEN: native document-change ranges
  are converted to old story UTF-16 coordinates only when the old joined
  regional source is canonical; the core verifies untouched prefix/suffix.
  IME composition skips this native-range hint. Surviving identity/payload and
  exact Undo are asserted.
- RED: copying one story picture lost its payload. GREEN: one selected picture
  uses the existing native image MIME plus PNG/JPEG fallback; Cut/paste preserves
  both picture payloads. Mixed or multiple-picture Cut is guarded so the
  clipboard cannot silently discard objects.
- RED: native Ctrl+C bypassed that picture lifecycle. GREEN: story keyboard
  Copy/Cut/Paste/Select All use the same document-aware commands.
- RED: Ctrl+Z in the focused story left a pasted picture in the document.
  GREEN: Undo/Redo use the core history for pictures and text, including
  Ctrl+Shift+Z. Keyboard Cut/paste retains a picture's native payload.

## Narrow views, zoom and native page distances

- RED: entering the right region left its editor outside a 420 px viewport.
  GREEN: entry, resizing and zoom reveal the active region or caret on both
  axes. Typing keeps it visible and focused; the toolbar moves below the active
  region when it would cover the caret. These assertions cover headers and
  footers at 100% and 150%.
- RED: header glyphs appeared above the supported native `w:header="720"`
  distance. GREEN: shared geometry anchors headers 36 pt from the top and
  footers 36 pt from the bottom, matching the native 720-twip defaults. A 6 pt
  body gap leaves a 30 pt region at default 72 pt margins, enough for two 12 pt
  lines. Pixel/click fixtures were maintained for this changed geometry;
  those fixture edits were not counted as RED evidence.
- PDF verification rasterizes at 96 dpi and compares header glyph bounds and
  multiline position with the canvas. Both page-field values (`Page 1 of 2`
  and `Page 2 of 2`) are extracted, with no unresolved raw field tokens and no
  semantic revision or pagination change. The final canvas and PDF rasters
  were also visually inspected.

The final focused build explicitly recompiled Canvas and linked all related
executables. `header_footer_ui_tests`, `header_footer_clipboard_tests`, its
`--keyboard-only` mode, `header_footer_toolbar_tests` and
`header_footer_ime_tests` all passed against the 36 pt geometry.

Because several agents edited independent sections of the same large Canvas
file, a compiler could read it before a later edit yet write its object after
that edit. Relink-only runs were not accepted as verification of new source.
The final focused cycles explicitly rebuilt Canvas after an agreed source
freeze and reran the executable tests.

## Final label raster and Narrow-margin regressions

- Raster RED: `header_footer_ui_tests --labels-only` at 150% reproduced the
  final capture's clipped Header label: child editor/toolbar painting covered
  77 of 134 antialias-aware label pixels. The reference paints the same Canvas
  without child widgets; the assertion compares every label-colored pixel
  with the final widget raster. The mask was corrected for fractional-position
  antialiasing, then RED was executed again against the original coordinates.
- GREEN: only the label coordinates changed. Header/Footer labels occupy the
  left page gutter, with a 4 pt gap from the editor and a Header baseline clear
  of the toolbar at Narrow margins. The raster matrix covers both story kinds,
  72/36 pt margins, 1100/420 px viewports and normal/150% scaling. Fresh captures
  were inspected with both the editors and toolbar visible.
- Narrow-space RED: `header_footer_ui_tests --narrow-fit-only` accepted 8 pt
  text where a 36 pt margin leaves no room between the native 36 pt story
  anchor and body. GREEN: available region height clamps to zero rather than
  inventing 12 pt of space. Empty semantic sections count as zero height for
  fit assessment; caret layout is unchanged. New text is rejected with the
  existing increase-margin guidance, while imported text can be shortened or
  cleared. Native footer anchoring remains 36 pt from the page edge.
- Verification: `flock /tmp/owldocs-build.lock cmake --build build-release
  --target header_footer_ui_tests header_footer_clipboard_tests
  header_footer_toolbar_tests header_footer_ime_tests story_wrap_workflow_tests
  workspace_ui_tests workspace_controls_tests -j2` explicitly rebuilt Canvas.
  Eight direct executable cases (including clipboard `--keyboard-only`) passed
  at normal and 150% scale: 16/16. All runs used offscreen Qt and private XDG
  settings. Evidence and captures are retained in
  `/tmp/owldocs-label-fix-061-3Pggcv/validation.json`; the earlier failed visual
  gate remains separately retained for audit. A second clean Release and full
  suite are owned by the release validation agent after this source freeze.
