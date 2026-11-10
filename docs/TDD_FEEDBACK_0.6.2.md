# Application workflow evidence — 0.6.2

The application integration checks use actual MainWindow commands and canvas
operations. The focused selectors run with `QT_QPA_PLATFORM=offscreen` and
isolated configuration/data directories. Every production slice follows the
requested RED → GREEN → REFACTOR workflow.

## Direct equation commands

`feedback_workflow_tests equation` executed RED with
`selected equations have no connected direct commands`.

MainWindow now registers Edit, Inline, Display and Delete for the selected
equation. Context synchronization is shared across selection changes, tab
restoration and document creation, including editable-figure recognition and
cell-picture capabilities. Display preserves canonical source and identity;
placement and deletion undo exactly. The same selector passed in the combined
0.64-second feedback workflow run.

## Table insertion

`feedback_workflow_tests table` executed RED with
`Insert Table command does not expose the dimension grid`.

The real action now uses the tested grid picker. Hover is read-only, insertion
is one transaction, and Cancel leaves the document unchanged. The same selector
passed. See `TDD_TABLE_PICKER_0.6.2.md` for the picker and keyboard cycles.

## Automatic and custom colors

`feedback_workflow_tests automatic` executed RED with
`Automatic ignores the selection or hardcodes a color instead of restoring its style`.

Allowing an invalid QColor through the action and clearing the color property
passed the original neutral-style case. Once the requested colored catalog was
active, the same test failed again: the effective color fell back to black.
The implementation now restores the imported/authored inherited baseline and
removes the selected ranges' foreground override masks in the same transaction.
Other character formatting and unselected overrides remain intact. GREEN:
the same selector passes with the colored catalog, exact override masks,
partial selection retaining underline and unselected red text, undo/redo, and a
subsequent heading-style color change.

`feedback_workflow_tests custom` then executed RED with `closing the custom
picker does not retain exactly the committed color`. The completed dialog now
remembers the final valid choice once during destruction; preview changes do
not populate history. The same test passes. The complete application workflow
target passes in 1.37 seconds, including new-tab context clearing and selected
equation/cell-picture restoration, supported cell-picture capabilities and two
header/footer margin save/reopen cycles.

Compiler setup/warnings and stale objects during shared builds are not counted
as behavioral RED evidence. Final broad verification is recorded separately.
