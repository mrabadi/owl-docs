# Table-cell pictures: 0.6.2 TDD evidence

The supported content is bounded inline PNG/JPEG data in the existing single
paragraph, rectangular table model. Every picture occupies one UTF-16 U+FFFC
position with a stable image identity, immutable encoded payload, alt text and
EMU geometry. Floating cell drawings are outside this slice.

All project builds use the shared `/tmp/owldocs-build.lock` and Release targets.
Public model changes are saved under that same lock, with other owners notified.

## Insertion and core editing

- Runtime RED: `table_image_ui_tests` rejected a valid generated PNG at an
  active cell caret: `Insert Picture rejects a real PNG at the active table-cell
  caret`. This reproduced the explicit old UI guard.
- API RED: `table_image_core_tests` failed to compile because `TableCell::images`,
  `Document::insertTableCellImage` and `InsertTableCellImage` did not exist.
- GREEN: cells now own normal `ImageAtom` values. The insertion operation
  validates PNG/JPEG containers, global identities, encoded byte/count limits,
  UTF-16 boundaries and geometry. Explicit cell range edits remove covered
  pictures and shift surviving offsets. Generic picture resize, payload and
  alt-text operations locate cell pictures; cell wrapping is inline only.
- Core tests pass for exact identity/payload/order, duplicate rejection without
  mutation, prefix replacement, picture deletion, inline-only layout, and
  exact document Undo/Redo. UI insertion, atom text and Undo/Redo also pass.
- One hand-copied PNG fixture initially had an extra byte. Its malformed input
  rejection was fixture repair, not behavioral RED evidence; the fixture now
  exactly matches the pre-existing validated core PNG bytes.

## Retained image history

- Runtime RED: deleting a table picture with a zero retained-history byte
  budget left Undo available: `deleted cell-picture payload bypasses the
  retained history byte budget`.
- GREEN: shared image traversal includes cell pictures when distinguishing
  active from history-only encoded payloads. The focused core tests pass.

## Rendering and recovery

- Runtime rendering RED: insertion succeeded but the canvas contained no red
  picture pixels: `inline table picture has no rendered raster pixels`.
- Runtime recovery RED: the text placeholder survived but its image metadata
  did not, so decoding failed with `Table cell contains an orphan picture
  placeholder` and the exact snapshot comparison failed.
- GREEN: the shared table line renderer now reserves image width/ascent and
  paints the raster in its row. Screen and exported PDF rasters contain the
  same picture geometry (within three pixels at 96 DPI).
- GREEN: schema 15 is emitted only for snapshots containing cell pictures;
  the original bytes, IDs, formats, offsets, alt text and EMU geometry restore
  exactly. Ordinary snapshots retain schema 14, and schema 14 rejects new cell
  picture metadata. The existing recovery regression suite remains green.

## Selection, clipboard and document editing

- Runtime RED: table picture selection was unavailable to Picture controls.
  After adding cell selection, the same test exposed that Redo restored a
  collapsed caret because the completed image selection was absent from the
  cursor-history entry. The final insertion state is now captured in that
  entry. GREEN includes click selection, sizing, alt text, deletion and Undo.
- Runtime RED: copying a pictured cell produced a bare placeholder and paste
  rejected it. A bounded native table-cell clipboard representation now carries
  text, run/cell formatting and original PNG/JPEG payloads. `SetTableCellContent`
  was first exercised as a missing-API RED, then implemented with destination
  cell identity, global media budgets and image-ID validation. Initial cell
  copy/paste and atomic Undo are GREEN.
- Runtime RED: a valid picture saved through the desktop could not reopen as
  editable cell content. Import/export conversion now maps actual inline
  drawings and emits the independently verified semantic registry for pictured
  tables. The initial desktop save/reopen/edit/save exact-document test is
  GREEN, including table, cell and image identity.
- A stronger mixed-prose fixture then produced an actual runtime RED:
  `typing prose beside a cell picture was rejected or changed its ordering`.
  Canvas still submitted the whole cell to a plain-text operation, which
  rejected the surviving U+FFFC. The minimal explicit-range adapter fix is saved;
  ordinary typing/plain paste/Cut use that range contract. A second actual RED
  (`Backspace beside cell pictures changed surviving atoms`) exposed a separate
  keyboard replacement lambda, now corrected the same way. Typing, Backspace,
  Delete, plain paste and whole-cell replacement are GREEN with exact surviving
  atoms and undo. The earlier fixture did not assert
  the attempted prefix and is not evidence of successful prose editing.

## Codex reads

- Runtime RED: an explicit table-cell read exposed the U+FFFC without picture
  metadata. GREEN: cell reads now use the same bounded image metadata helper as
  body pictures, returning stable ID, UTF-16 offset, MIME, alt text, dimensions,
  layout and encoded byte count. Raw file-image insertion and table mutation
  operations are not added to the Codex capability surface.

## Final focused regressions and verification

- Actual runtime RED: `table_ui_tests` reported imported automatic row height
  134.5 px instead of the expected unrounded 130.87 px. Image ascent reservation
  now applies only to lines containing pictures; image-free table text retains
  its existing OpenType line advances. The complete existing table UI suite is
  GREEN.
- Actual runtime RED: a selected cell picture's context menu offered the
  unsupported floating-layout dialog. That menu action now matches the Ribbon's
  disabled state for cell pictures. Size, alt text and deletion remain usable.
- Actual runtime RED: pasting a pictured fragment before existing `TAIL` left
  the caret after the suffix. The paste transaction now positions it immediately
  after the inserted fragment; following typing is correctly before `TAIL`.
- Final forced production build: shared-lock Release build session 89463,
  followed by test-only rebuild 15705. All six UI selectors/default cases pass,
  and the existing `table_ui_tests`, `editor_tool_bridge_tests`, `recovery_tests`
  and focused `table_image_core_tests` pass. The final clipboard case additionally
  covers a 2×2 PNG/JPEG cell rectangle copied into an offset 3×3 destination,
  preserving destination cell IDs, cloning picture IDs, and exact Paste/Cut
  Undo/Redo. Row/column insertion and surrounding deletion retain original image
  payload/identity; deleting the containing column removes its pictures.
- Current screen/PDF capture gate: `/tmp/qt_temp-rkdENz` contains the canvas PNG,
  exported PDF and 96-DPI PDF raster; picture dimensions agree within three
  pixels. The actual desktop mixed PNG/JPEG DOCX after two exact save/reopen
  cycles is `/tmp/table_image_ui_tests-Hwqtbf/table-picture.docx`.
- LibreOffice 7.3.7.2 independently rendered the earlier semantically identical
  desktop fixture with `Prefix Before [PNG] between [JPEG]` on one Letter page.
  Both drawings retain 381000×228600 EMU (30×18 pt). The reviewed raster has no
  filename fallback, overlap or clipping. External native validation record:
  `/tmp/owldocs-native-cell-062/validation.json`.

The authored subset remains the existing rectangular, single-paragraph-per-cell
table model with bounded inline PNG/JPEG atoms. Cell floating anchors, crop,
rotation, nested tables and arbitrary rich foreign table structures are not
enabled. Their original unsupported package data stays on conservative source
preservation paths. Cell pictures use the same 512-atom, 16 MiB per-picture and
32 MiB document encoded-media budgets as body pictures; retained-history limits
also include their payloads. The private native cell clipboard is bounded to
64 MiB and rejects invalid content before mutation.

## Native OOXML cell picture transport

The configured `table_image_ooxml_tests` executed a runtime RED: supported real
inline PNG/JPEG cell pictures remained view-only after writing/reopening.
The writer's existing image catalog already retained native media bytes and
relationships. The parser's unconditional cell-image rejection was replaced
with bounded eligibility after package media resolution. Valid inline rasters
retain exact picture/text order, geometry, alt text and encoded bytes, and the
complete prepared-package reproduction assessment passes (0.03 s).

A second executed runtime RED showed an unsupported floating cell picture could
overwrite an existing target. The minimal authoring guard now rejects that
placement before writing. The same test is GREEN and also confirms malformed
raster data and external image relationships keep the original table view-only;
a safe text patch outside that table retains its media and relationships
byte-for-byte. Test fixture constructor repair (image belongs in NewRun's fourth
parameter) was setup, excluded from RED evidence. Two existing current-style
assertions were updated from black to the requested green Heading2 while keeping
all native style metadata and direct-spacing precedence checks.
