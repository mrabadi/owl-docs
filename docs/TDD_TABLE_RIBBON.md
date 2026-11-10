# Table ribbon TDD evidence

This change keeps table tools in the contextual row beneath the primary writing controls. It preserves direct row/column insertion/deletion and the table style chooser, and adds direct cell alignment and keyboard sizing alternatives to Canvas dragging.

## Direct cell positioning and whole-table selection

The focused test registers real command actions, enters table context, verifies all nine positioning buttons are visible and fully inside the ribbon at widths 1280, 820 and 420, activates the center choice with Enter, activates Select Table and confirms table-only actions are disabled after context exit.

RED: `QT_QPA_PLATFORM=offscreen build-release/workspace_controls_tests table-alignment` failed with `table text positioning requires an indirect command instead of a direct nine-position grid`.

GREEN: the same command passed after adding the direct three-by-three grid with distinct alignment glyphs and a Select Table command button. Build: `flock /tmp/owldocs-build.lock cmake --build build-release --target workspace_controls_tests -j2`.

## Committed geometry and selection state

The next focused test checks selected-column width and row minimum height (including Auto), no requests during state synchronization or unfinished typing, exactly one request per committed changed value, and uniform/mixed selection state in the positioning grid.

RED: the same target build failed at the new public methods/signals: `tableColumnWidthRequested`, `tableRowMinimumHeightRequested`, `setTableDimensions`, and `setTableCellAlignment` were not members of RibbonWidget. This was a deliberate missing-API RED; the test was added before production declarations and no unrelated dependency failed.

GREEN: `QT_QPA_PLATFORM=offscreen build-release/workspace_controls_tests table-dimensions` passed after adding the direct fields and selection synchronization methods. The fields emit on changed committed edits, while synchronization blocks signals and updates the committed value. Row minimum zero displays Auto.

## Embedded header/footer lifetime

Root established the embedded, enabled and inline-editor-focus opening workflow GREEN in `/tmp/owldocs-header-ribbon-green.log`. The behavior-preserving safety refactor adds an explicit RibbonWidget destructor that disconnects the adopted control destruction callback before QWidget base teardown. Adopted controls remain Canvas-owned through QPointer; the Ribbon does not delete or replace them. Root's expanded workflow additionally covers active MainWindow teardown and hosted Canvas deletion. Root's complete embedded workflow passed after the destructor rebuild, including active-window teardown and hosted Canvas deletion.

## Borders and distribution

The focused border test verifies direct preset, line-weight and palette controls, all eight presets, defaults of 0.5 pt black, color/weight preparation without applying borders, Enter activation of a red swatch, distribution command execution and table-context disabling.

RED: `flock /tmp/owldocs-build.lock cmake --build build-release --target workspace_controls_tests -j2` failed at the deliberately new `tableBorderWidthPoints` and `tableBorderColor` getters. Production APIs and controls were added only afterward. GREEN: `QT_QPA_PLATFORM=offscreen build-release/workspace_controls_tests table-borders` passed, as did reruns of table-dimensions and table-alignment. The public getters do not apply changes; the real preset command reads them only when invoked.

## MainWindow command integration

Root executed the real MainWindow acceptance RED (`/tmp/owldocs-table-format-ui-red.log`): `selected cells have no direct nine-position text command`. The approved integration adds actual registry actions for nine positions, eight border presets, Select Table and both distribution routes, then forwards committed numeric changes to Canvas and mirrors selection state back into the Ribbon. Canvas behavior is owned and tested by root; this slice does not implement model or drawing behavior.

## Related controls regression

Adding the table selectors to the default controls suite exposed an existing fixed-height standalone fixture: `workspace_controls_tests` failed `an expanded contextual control is clipped at a supported width`. The fixture still allocates only 180 px total height at 420 px width, even though the expanded table tools now wrap into additional real rows. The geometry diagnosis and measured-height fixture correction are recorded below. No direct command will be moved behind an overflow wrapper to satisfy this fixture.

The clipping diagnostic found `ribbonButton.table.styles` at `(49,142)` with size `164×39` inside an `820×180` standalone bar: the last pixel exceeded the old allocation. The context page genuinely wraps to another row. The fixture now settles the page and allocates the actual `sizeHint()` height, then still checks every visible contextual button's complete global rectangle at 820 and 420 widths. The unchanged containment assertions pass. The actual MainWindow checks use 820×600 and 1280×860 separately.

A further selection boundary came from the Canvas getter contract: negative numeric values indicate mixed selected sizes. Runtime RED: `workspace_controls_tests table-dimensions` failed `mixed table dimensions are shown as a misleading fixed size`. The minimal fix displays Mixed without emitting edits, keeps an unchanged mixed value quiet, and restores the valid Width / Auto row state when the user commits an explicit value. The same focused test passes.

The full `workspace_controls_tests` default runner now includes all three table acceptance functions. It passed both `QT_QPA_PLATFORM=offscreen build-release/workspace_controls_tests` and `QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1.5 build-release/workspace_controls_tests` against the current Ribbon source. Actual application captures and root Canvas integration verification remain separate gates.

## Real application and related verification

`workspace_ui_tests` first stopped at its old expectation that the entire Ribbon is disabled during header/footer editing. The body-action guard checks already passed. The updated assertion requires disabled Home formatting controls, enabled adopted story controls inside the Ribbon, supported Picture insertion, and restoration on exit. This preserves the old body-edit protection while matching the requested embedded contextual row.

The complete `workspace_controls_tests`, `workspace_ui_tests`, `ribbon_widget_tests`, `branding_tests` and `list_properties_ui_tests` runners passed at normal and 150% scaling. There are no remaining source changes planned in this UI slice.

Actual `workspace_ui_tests capture-contexts` runs passed for all six semantic contexts at 820×600 and 1280×860, normal and 150% scaling. PNGs and action logs are preserved in `output/ui-review/0.6.3/workspace`. Both Table widths and scales were visually inspected: all nine position controls, Select Table, both numeric fields, Borders/weight/color, distribution, six row/column operations and Table Styles remain visible without wrappers or clipping. At 820 the contextual controls use two rows; at 1280 they fit one row. The normal primary row retains only genuinely displaced Font in More at 820 and hides More at 1280.

The final resource-complete header/footer embedded workflow passed at normal and 150% scaling after root's final focused build. All four new header/footer captures were visually inspected: original owl present, controls embedded beneath the primary row, inline caret visible, body controls disabled, and no floating popup or fixture-state error. Root owns the corresponding Canvas, drag geometry, model and OOXML release gates. No commit or package was created by this UI slice.

## Final release verification

The final strict Release verification passed all 79 CTest cases and all 12
scaled direct cases. The earlier pending capture/integration stages above
are resolved: eight fresh Table/Header captures were inspected with no
unresolved findings. The shared release record documents the initial 227-step
clean build and the later 49-step strict low-zoom Canvas repair before the
final complete run. No Ribbon production change was needed for that repair.
