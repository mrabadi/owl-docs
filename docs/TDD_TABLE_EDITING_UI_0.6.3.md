# Direct table editing — RED/GREEN evidence

The desktop implementation uses the shared table semantics. Previewing a resize
does not mutate the document; release commits a single revision-checked geometry
operation. Cell formatting uses one batch and preserves selection and identities.

## Column and row dragging

The initial `table_editing_ui_tests` translation unit failed against the old
public interface because `tableCellViewportRect` was absent. The focused test
then passed after implementing real boundary hit testing, viewport geometry,
live layout preview, adjacent-column resizing and a single release transaction.
It checks unchanged text, fixed total width at interior boundaries, Undo and Redo.

The next executed behavioral RED was:
`hovering a row boundary does not offer direct height resizing`.
Row hit testing, minimum-height preview and commit made the focused `row` case
pass. Other rows retain their height, and content may grow beyond a minimum.

```sh
QT_QPA_PLATFORM=offscreen build-release/table_editing_ui_tests column
QT_QPA_PLATFORM=offscreen build-release/table_editing_ui_tests row
```

## Formatting and rendering

The actual MainWindow `formatting` case first failed:
`selected cells have no direct nine-position text command`.
It subsequently passed with both alignment axes applied to the selected
rectangle, unselected cells retained, No Borders and Outside Borders applied
to a selected table, and each action restored by one Undo.

Separate real rendering tests failed before the canvas respected authored cell
layout: `authored bottom-right cell layout does not move the rendered text and
insertion caret`, and `No Borders leaves the inherited grid stroke on the page`.
Applying semantic cell layout after imported presentation and suppressing
zero-width borders made both pass. Borders, cell text and caret share the
existing screen/PDF drawing path.

```sh
QT_QPA_PLATFORM=offscreen build-release/table_editing_ui_tests formatting
QT_QPA_PLATFORM=offscreen build-release/table_cell_render_tests
```

## Boundary regressions

Executed RED cases exposed:

- Invalid clamp bounds for adjacent 5-point imported columns, even on a click
  with no movement. Existing small widths now remain valid and unchanged.
- Column handles unreachable at 25% zoom because endpoint exclusions consumed
  the entire short row. Exclusions now scale with available row height.
- A zoom change committing stale preview geometry. Changing the view cancels
  the preview; Escape also cancels without a transaction.

After the fixes, the complete direct-editing target passed, including 25%, 75%,
100% and 150% zoom. Logs are `/tmp/owldocs-table-*-red.log`,
`/tmp/owldocs-table-editing-green.log` and
`/tmp/owldocs-table-cell-render-green.log`.

An additional executed RED verifies that scrolling or resizing the viewport
during a drag also cancels rather than committing stale preview geometry.
The complete direct-editing test passed after adding a shared cancellation
helper for scroll, resize, zoom, Escape, focus loss and document replacement.
The final focused build and three root suites passed in
`/tmp/owldocs-table-view-cancel-green-build.log`,
`/tmp/owldocs-table-final-focused.log`,
`/tmp/owldocs-table-final-render.log` and
`/tmp/owldocs-header-ribbon-final-focused.log`.

The completely clean strict Release build passed 227 steps. A final test-only
addition then reproduced a low-zoom interaction conflict:
`row resize hit targets consume ordinary cell clicks at minimum zoom`.
The row hit tolerance is now capped by one quarter of its height, preserving
both ordinary cell clicks and direct border dragging. The subsequent strict
build and final release results explicitly include this post-clean fix.
Logs: `/tmp/owldocs-lowzoom-cell-red.log` and
`/tmp/owldocs-release-063-post-clean.log`.

The first render-test compile used the wrong existing method name
(`setTableCellParagraphFormat` instead of `applyTableCellParagraphFormat`).
That setup error was corrected before the behavioral RED run; it is not counted
as a regression. The initial standalone compiler attempt could not find Qt
pkg-config files; the configured CMake compiler flags were used instead.

Core/model, recovery, clipboard, native XML and desktop save/reopen evidence is
in `TDD_TABLE_MODEL_0.6.3.md` and `TDD_TABLE_LAYOUT_0.6.3.md`.
