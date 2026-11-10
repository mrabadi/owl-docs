# Table insertion picker — 0.6.2

## Quick dimension grid

- RED: `table_insert_picker_tests` failed because the deliberately new
  `TableInsertDialog` public interface did not exist.
- GREEN: the same test passes with an 8-row by 10-column grid. Hovering at
  row 3, column 4 highlights 12 cells without accepting. One click accepts
  those dimensions; Escape rejects the dialog.
- Manual bounded row/column inputs and the existing header-row option remain
  directly below the grid for tables larger than the quick selection.

## Keyboard and application integration

- Keyboard RED: arrow keys could grow the selected column count beyond the
  visible grid. GREEN: arrow selection is bounded to the 10×8 grid, while the
  manual controls still accept 14 columns by 40 rows. Enter accepts; the same
  focused test passes.
- Integration RED: invoking the real MainWindow Insert Table action failed
  `Insert Table command does not expose the dimension grid`.
- GREEN: the action now uses the picker. Hover leaves the document revision
  unchanged, one click inserts the selected 3×4 native table, Undo restores the
  exact original document, and cancellation creates no edit. The combined
  `feedback_workflow_tests` run passed with the equation/color integration.
