# Workspace modernization TDD evidence

The implementation preserves the approved Owl Docs artwork and translates the
installed ApeTek 1.3.3 application's warm neutral and forest-green UI tokens to
native Qt Widgets. Each behavior below is developed with an executed failing
acceptance test before production edits.

## Baseline

`ctest --test-dir build -R '^(ribbon_widget_tests|navigation_dock_tests)$' --output-on-failure`

Observed: 2/2 passed before workspace production edits.

## Command availability

Command: `flock /tmp/owldocs-build.lock cmake --build build --target command_registry_tests -j2 && ctest --test-dir build -R '^command_registry_tests$' --output-on-failure`.

- RED: `command search exposes hidden or unavailable actions`.
- GREEN: filter hidden/disabled actions; 1/1 passed.
- Next RED: compilation reports missing new `describe`/`category` APIs.
- GREEN: category/keyword metadata added without changing existing add calls;
  1/1 passed.

## Compact controls, theme and outline

Command: `flock /tmp/owldocs-build.lock cmake --build build --target workspace_controls_tests -j2 && ctest --test-dir build -R '^workspace_controls_tests$' --output-on-failure`.

- RED: `writing bar still requires switching ribbon tabs`.
- GREEN: compact writing bar, labelled Insert/Page/Review/context menus and
  More formatting; 1/1 passed.
- RED: `workspace controls do not use the ApeTek neutral and forest palette`.
- GREEN: shared Fusion palette and scoped styles; 1/1 passed.
- RED: missing public outline mode, heading list and activation API.
- GREEN: accessible heading list and Enter activation; 1/1 passed.
- RED: `Codex model controls are not labelled for keyboard and assistive users`.
- GREEN: labelled collapsible model settings and composer; 1/1 passed.

## Document header

Command: build/run `workspace_ui_tests` with the shared build lock.

- RED: `workspace lacks a persistent document header and Save button`.
- GREEN: logo/name and persistent Open/Undo/Redo/Save/Find/Codex actions;
  1/1 passed.
- Native Qt validation caught a crash when assigning separate styles to nested
  docks. A debugger backtrace identified `QDockWidget::event` during
  `QWidget::setStyle`. QApplication now owns one Fusion style; header and
  controls pass together after that correction.

## Honest command surface

- RED (`workspace_ui_tests`): `workspace offers an action that cannot perform its advertised task`.
- GREEN: removed the eight unimplemented comment/text-box/columns/tracked-review/ruler
  commands rather than advertising empty handlers; 1/1 passed.

## Command search, panes and task menus

- Palette RED: Enter in the search field did not run the highlighted command.
  GREEN: Enter/double-click select a command, close the dialog, then invoke it;
  the test verifies no modal widget remains when the action runs.
- Narrow-pane RED: both Navigation and Codex crowded the 820px workspace.
  GREEN: one auxiliary pane at 1100px and below; latest pane wins, header
  secondary actions collapse, visibility and toggle state agree. The test
  verifies an unchanged 820px window and at least 420px document viewport.
- Task-menu RED: academic actions had no discoverable References menu.
  GREEN: References and Format menus expose their real registered workflows.
- Overflow availability RED: named paragraph styles remained actionable in an
  unsupported table context. GREEN: the More submenu follows the same
  availability state as the primary style selector.
- Minimum-width RED: toolbar content forced a wider window or clipped the More
  control at a 420px viewport. GREEN: internal page layouts respect the compact
  container and preserve the overflow route; controls acceptance passed 1/1.
- Fit command RED: Page Width used a fixed 125 percent. GREEN: it invokes native
  page-fit geometry, verified against the visible page/viewport dimensions.

## Fit scrollbars (shared canvas integration)

`QT_QPA_PLATFORM=offscreen build/workspace_ui_tests fit-scrollbars`

RED: a fresh 900x700 canvas at 25 percent acquired a vertical scrollbar after fit,
then retained horizontal overflow. This is an executed regression, not a timeout.
Canvas fixes and the final GREEN result are verified with the workspace suite.

## Document isolation and outline integration

- Outline RED: opening Navigation did not show headings from the current
  document. The integration now derives supported Heading 1–9 entries in body
  order, preserves Find/Replace state and activates stable paragraph identities.
- Inactive-tab RED: a background canvas's format/page signals changed active
  toolbar and word/page status. Signal adapters now check the active canvas for
  character, highlight, list and page-state updates.

## Final workspace refinements

- A focused callback test proved routine edit failures opened a blocking message
  box. The adapter now presents the full message in the status bar; GREEN
  verifies it can return without a modal surface.
- Manual-save-state RED: `dirty document is presented as saved or ready`.
  GREEN: persistent `Unsaved changes`, `New document`, or `Saved` follows the
  active document. The old timed `Ready` reset is removed with save-owner
  coordination; transaction and conflict behavior is unchanged.
- Visual QA identified a redundant Insert/Page/Review row and excess toolbar
  height. Executed RED assertions required a <=60px normal writing bar and a
  real Layout menu. Ordinary writing now uses one row; List/Table/Picture
  menus appear only in their context. Layout reuses the working margin,
  paper-size and paragraph/page actions.
- Legacy checks caught two regressions: Navigation reopened the wrong mode and
  mouse Bullets could take typing focus. The pane now remembers Find/Replace
  while fresh Navigation opens Outline; mouse formatting keeps document focus,
  and keyboard users retain shortcuts, labelled menus and command search.

## Final verification

After the final production edits, built the ten related targets under
`flock /tmp/owldocs-build.lock` and ran:

```sh
ctest --test-dir build -R '^(workspace_ui_tests|workspace_controls_tests|command_registry_tests|ribbon_widget_tests|navigation_dock_tests|navigation_workflow_tests|main_window_preferences_tests|branding_tests|app_smoke_tests|list_properties_ui_tests)$' --output-on-failure
```

Observed **10/10 passed**, 5.65 seconds. This includes unchanged artwork hashing,
existing focus and navigation workflows, preferences, list tools and app smoke
coverage as well as new workspace behavior.

`QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1.5 build/workspace_ui_tests`
also completed with exit 0 and all twelve behavior groups passing.

Generated captures with `build/workspace_ui_tests capture` at default scale and
150 percent scale. Inspected the 1280x860 and 820x600 logical layouts, including
Layout/References access, unclipped primary controls and overflow, mutually
exclusive compact panes, visible paper top and accurate unsaved state.
Captures are preserved under `output/ui-review/` (ignored build evidence).

The outline integration test also caught a native navigation wrapper that used
an invalid zero-length search hit and opened an edit-error dialog. The canvas
owner replaced that wrapper with direct cursor navigation; the final workspace
suite verifies real heading activation. Test state now uses a fresh temporary
AppData directory for each process as well as isolated settings.

Final review added a background-tab close/save regression with two dirty
documents. The test clicks the real Save button and proves that the background
file changed while the active canvas remains dirty. RED: `workspace_ui_tests
inactive-save` exited 1 with `saving a background tab presents the active dirty
document as saved`. Removed the final unconditional `Saved` label assignment;
the existing active-document synchronization supplies the persistent state,
and status-bar durability/error messages remain intact. A concurrent header
change exposed stale `NewParagraph` ABI objects during the first rebuild;
debugger evidence showed an invalid vector size at the fixture write boundary.
Ran `flock /tmp/owldocs-build.lock cmake --build build --target
workspace_ui_tests --clean-first -j2` against the frozen headers, then the
isolated regression passed at both default and 150 percent scale. The complete
workspace suite passed (all thirteen behavior groups, 1.84 seconds).

The clean Release gate exposed two older blue-only render predicates. Both
failures were reproduced before changing the assertions. Captures showed the
correct selected RTL grapheme (267 pixels of the composited forest highlight)
and a selected table with continuous forest borders (3,041 accent pixels) and
visible cell tint (35,925 pixels). Updated the predicates to `accentColor()`
and its exact alpha composition over paper. The table still requires a
horizontal rule with more than 180 matching pixels and a rectangular selection
with more than 1,000 additional tint pixels; the RTL check still requires more
than 12 highlight pixels and now restricts them to the selected caret segment.
The captures are preserved in `output/ui-review/owl-docs-rtl-selection.png`
and `output/ui-review/owl-docs-selected-table.png`.

Running the same complete suites at 150 percent scale exposed test coordinates
that compared physical grab pixels with logical IME/mouse coordinates. The
image-bound helper now retains fractional logical bounds, and the table drag
uses a logical mouse position. All existing geometry and drag assertions remain.
The complete `layout_conformance_tests` and `table_ui_tests` pass **2/2** at
default scale (1.08 seconds) and **2/2** at 150 percent scale (2.05 seconds).
These final corrections change test predicates and coordinates only.

Deliberate limits: this change does not implement comments, tracked-change
review, multi-column/text-box authoring, arbitrary custom-style outline
inference, dark-theme preferences or a full AT-SPI document tree. Such inactive
commands are not advertised as available authoring tasks.
