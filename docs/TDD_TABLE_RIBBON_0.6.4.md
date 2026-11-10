# Single-row table ribbon TDD — OwlDocs 0.6.4

Behavior changes are limited to `RibbonWidget.cpp` and its private declarations.
The semantic table model, operations, recovery format and DOCX transport are
unchanged. Tests exercise real menus, keyboard entry and command dispatch.

Each command below followed a successful focused test build with
`cmake --build build-release --target workspace_controls_tests -j 4`.

## Executed RED → GREEN cycles

1. `QT_QPA_PLATFORM=offscreen build-release/workspace_controls_tests table-alignment`
   failed with “table alignment grid occupies ribbon space before its chooser
   is opened” (`/tmp/owldocs-064-alignment-red.log`). Hosting the grid inside an
   Alignment dropdown passed the same test, including all nine choices at
   1280/820/420 logical pixels, Enter activation, dismissal and Escape without
   dispatch (`/tmp/owldocs-064-alignment-green.log`).
2. `QT_QPA_PLATFORM=offscreen build-release/workspace_controls_tests table-row`
   failed with “table ribbon grows into multiple rows instead of one compact
   row” (`/tmp/owldocs-064-row-red.log`). A horizontal layout with task-specific
   dropdowns and actual-width overflow passed at 1280/820/420/820/1280 pixels
   (`/tmp/owldocs-064-row-green.log`). Detailed fields remain hidden until
   their chooser opens; no visible control is clipped or wraps to a second row.
3. `QT_QPA_PLATFORM=offscreen build-release/workspace_controls_tests table-popups`
   failed with “table dropdown remains open after leaving its document or table
   context” (`/tmp/owldocs-064-popups-red.log`). Closing table menus when their
   page becomes inactive passed the same table-exit and picture-context tests
   (`/tmp/owldocs-064-popups-green.log`).
4. `QT_QPA_PLATFORM=offscreen build-release/workspace_controls_tests table-size-keyboard`
   failed with “keyboard-opened Size dropdown does not focus the column width
   field” (`/tmp/owldocs-064-size-keyboard-red.log`). Focusing and selecting the
   current width after the menu opens passed keyboard entry, one commit, and
   Escape dismissal (`/tmp/owldocs-064-size-keyboard-green.log`).

## Refactor and related checks

The row change shares chooser/command construction rather than duplicating
setup. Updated earlier direct-control tests retain stable action identity,
selection availability, icon/accessibility, Mixed/Auto, incomplete typing,
single commit, line-weight/color independence and keyboard command coverage
through the requested dropdown workflow. The complete focused controls binary
passes (`/tmp/owldocs-064-controls-green.log`).

Final integration, scaled visual, full-suite and Debian package results are
recorded in `RELEASE_VALIDATION_0.6.4.md` and `dist/validation-0.6.4.json`.
The real MainWindow dropdown test passed, as did all 79 CTest cases and six
fresh runs at 150% scaling. All 24 table workspace/popup captures were reviewed
without unresolved findings. The semantic model and transport remain unchanged.
