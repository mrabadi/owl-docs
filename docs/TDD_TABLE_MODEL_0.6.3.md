# Table model, recovery and clipboard: 0.6.3 TDD evidence

This slice adds column widths, minimum row heights, per-cell vertical alignment
and four direct borders to the existing rectangular table model. Exact row
heights, merges, nested tables and floating tables remain outside this authoring
subset. Builds and public-header changes use `/tmp/owldocs-build.lock`.

## Core geometry and cell layout

`table_layout_core_tests` first failed to compile because `TableGeometry`,
`TableCellLayout`, their operations and Document setters did not exist (27316).
The complete header/core/session implementation was applied in one locked
window (15469), then the same target built and executed GREEN (23083).

The tests cover durable sparse metadata, explicit `none` distinct from inherited
borders, batch Undo/Redo, atomic rejection of invalid counts/zero widths/total
width/border values, adjacent row/column dimension inheritance, structural
deletion and exact clearing back to the original inherited table. The expanded
test additionally verifies an over-limit column insertion cannot change cell
identities or existing geometry before rejecting.

Geometry uses twips. Empty vectors retain automatic sizing; explicit vectors
match their column/row count. Column widths are positive; row minimum zero means
automatic. Individual dimensions and total table width are bounded to 31,680
twips. Inserted rows/columns inherit the chosen adjacent source dimension;
deletion removes corresponding entries. Cell IDs, text and picture atoms remain
independent of these properties.

Cell layout has optional Top/Center/Bottom and optional per-edge borders.
Absence inherits from table style. Explicit `none` has width zero; solid borders
use opaque ARGB and width 1–96 eighth-points. Layout operations replace the
requested cell's sparse layout; UI commands can compose them with horizontal
paragraph alignment in one transaction and reconcile opposite shared edges.

## Recovery

The runtime RED reported `new table metadata did not use schema16`. The codec
change is GREEN (88407): schema 16 appears only when geometry or cell layout is
nonempty, restores every field exactly, rejects those fields under schema 15,
and returns byte-identical ordinary schema 14 JSON after layout is cleared.
Cell-picture-only snapshots retain 15; older ordinary snapshots retain 14.

Expanded negative cases pass for integer overflow, mismatched vector lengths,
noncanonical no-border widths, transparent colors and unknown vertical
alignment. Decoding bounds integers before narrowing and uses the same core
validators. The future-version fixture now uses 17 because 16 is supported;
this maintenance is excluded from RED evidence.

## Clipboard

`table_layout_clipboard_tests` executed an actual runtime RED against the fresh
application archive: `copied table rectangle loses its column widths or row
minimum heights` (66668). The minimal change crops geometry vectors with the
copied row/column rectangle and carries them when creating the pasted table.
Cell layout already travels with the copied cell. Existing destination table
geometry is retained when filling its cells; copied-cell layouts and content
are preserved. The forced final GREEN build is 25382. The clipboard regression,
existing pictured-cell clipboard/recovery cases, existing recovery suite, full
core suite and focused table-picture/model/recovery tests all execute GREEN.

Related checks include the existing cell-picture clipboard and recovery suites,
so a geometry addition cannot silently flatten PNG/JPEG atoms or alter older
snapshot formats. Native DOCX/desktop transport has its separate evidence in
`TDD_TABLE_LAYOUT_0.6.3.md`.

## Canvas formatting transactions

The separate `DocumentCanvasTableEditing.cpp` implementation was added only
after the configured public API test executed missing-definition linker RED
(97780). Root's actual MainWindow workflow had separately executed a runtime
RED for the missing nine-position table-alignment command. The new file is
registered as one source-list entry and does not move the shared Canvas
rendering or drag implementation.

Focused direct formatting tests and the actual MainWindow formatting workflow
executed GREEN (21651): atomic two-axis alignment, mixed-value getters, numeric
column widths and raw row minimums, fixed-sum column distribution, measured row
distribution, mirrored shared borders, no-op history preservation and invalid
value/story-mode guards. Border presets retain unrelated properties and apply
the neighbor's opposite side in the same transaction, including neighbors
outside the selected rectangle.

The imported-presentation alignment regression then executed actual runtime RED:
`cell alignment getter ignores inherited imported right/bottom presentation`.
The minimal effective-alignment helper now follows the renderer's stable cell
identity, duplicate-ID and legacy shape guards. Both getter and no-op decisions
use the effective value, so selecting Top Left creates explicit overrides when
the cache shows Bottom Right. Its one-step Undo restores absent core properties.

This case is GREEN (8601/89844), including duplicate-ID rejection, legacy
same-shape fallback, stale legacy-shape rejection and preview/story mutation
guards. The direct formatting target passes normally and at 150% scaling; the
actual MainWindow formatting workflow also passes. Width commands preserve raw
row minima, row commands preserve raw column widths, mixed numeric values use
the -1 sentinel and automatic row minimum uses 0. Shared borders, cell content,
picture IDs and other layout properties remain unchanged outside each explicit
formatting transaction.
