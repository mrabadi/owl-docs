# OwlDocs 0.6.2 workspace feedback evidence

This pass follows the explicitly requested TDD skill. Production changes are preceded by executed focused failing checks; the actual focused checks and final captures are recorded below. The original owl artwork and storage identities remain unchanged.

## Context controls below primary formatting

`workspace_controls_tests contexts` failed before implementation with `selection controls require an object menu or replace the primary formatting row`.

The actual existing List, Table and Picture controls now live in a contextual row below Home. The row wraps controls at narrow widths; selecting a picture in a table prioritizes Picture controls. The hidden legacy tab identities remain for integrations. The same focused command passed after implementation.

## Readable, centered top-level menus

`workspace_ui_tests menu-type` failed before the stylesheet change with `top-level menus use text smaller than fourteen logical pixels`.

Top-level menus now use 14 logical pixel text. Padding centers every action against the preserved owl icon and fits all eight menus beside Save, Find and Codex at 820 and 1280 pixels. The same test passed. A compact-width overlap caught during the GREEN phase was corrected by reducing horizontal item padding; the no-overlap assertion was retained.

## Direct picture wrapping and equation/cell controls

`workspace_controls_tests picture-direct` failed with `picture wrapping still requires opening a Wrap Text dropdown` before replacing that wrapper with direct Inline, Square and Top and Bottom controls.

The new `equation-cell` acceptance test failed to compile for the deliberately missing `setEquationContext` API and fourth `setPictureContext` capability argument. The new API exposes selected equation editing/placement/deletion directly and disables unsupported floating controls for table-cell pictures.

The `contexts`, `picture-direct` and `equation-cell` commands all passed after the implementation. The cell case preserves Size/Alt Text/Delete and Inline while disabling Square, Top and Bottom and Layout Options.

## Keyboard access

`workspace_controls_tests keyboard` failed with `Tab skips the direct primary formatting buttons`. A local command-button helper now permits Tab traversal while keeping mouse clicks from transferring document typing focus. Return/Enter activates the focused command or opens its single-purpose chooser. The same command passed, including primary Bold, Table row insertion and the Table Styles chooser.

## Actual spatial overflow

`workspace_controls_tests overflow` failed with `More duplicates a direct command or omits an overflowed command`. The same check passed against the unchanged contract: at 1280 all primary commands fit directly and More disappears; at 820/420 hidden commands have one overflow route and visible commands have none. Color choosers use their existing menus and appear in More only when the actual button is hidden.

## Commands

All C++ builds use the shared lock:

```sh
flock /tmp/owldocs-build.lock cmake --build build-release --target workspace_controls_tests workspace_ui_tests -j2
QT_QPA_PLATFORM=offscreen build-release/workspace_controls_tests contexts
QT_QPA_PLATFORM=offscreen build-release/workspace_ui_tests menu-type
QT_QPA_PLATFORM=offscreen build-release/workspace_controls_tests picture-direct
QT_QPA_PLATFORM=offscreen build-release/workspace_controls_tests equation-cell
```

The complete `workspace_controls_tests` executable passed at normal and 150% scaling after the context, overflow and keyboard changes.

## Context-switch and direct-control followups

- `context-switch` failed with `switching from list to another object hides indentation from both writing row and overflow`. Primary indentation is now replaced only while the List row is actually visible, and all object-context transitions recompute overflow.
- `figure` failed with `editable local figures do not identify their context or expose direct Edit Figure`. The selected editable source now shows a Figure label and its direct Edit Figure button. The next run also caught the ordinary-picture transition retaining that button; visibility is now explicitly synchronized.
- The added visual distinction assertion in `picture-direct` failed with `direct wrapping choices use indistinguishable icons`. Inline, Square and Top and Bottom now have distinct glyphs while retaining their action names and tooltips.

The full controls suite passed after these fixes. The existing branding focus expectation was updated from `NoFocus` to the precise intended contract: no mouse ClickFocus, with TabFocus. Its following actual document-focus/typing assertions remain intact.

## Layout menu

The related full `workspace_ui_tests` run failed with `page layout has no discoverable Layout menu`. Inspection found an empty visible Layout menu. Restoring its existing Orientation command made `workspace_ui_tests menus` pass. The next `layout-choices` run failed with `Layout has no reachable Margins and Paper Size choices`. The completed Layout menu registers actions that forward into the authoritative existing margin/page-size handlers. The same focused test passed for all five presets, accepted Custom Margins and Letter/A4/Legal dimensions. These routes are also searchable as Layout commands.

## Verified click audit

Counts begin with the relevant document selection already active and count the action entry route; choosing an actual value/style/color inside a legitimate chooser is listed separately. Menu hover does not count as a click. Keyboard users can traverse command buttons with Tab and activate with Enter/Space.

| Context or command | Previous toolbar route | Current route |
| --- | --- | --- |
| Primary B/I/U, alignment, bullets, numbering | 1 click | 1 direct click |
| Strike/super/sub at wide width | More then command: 2 | 1 direct click |
| Font family/size/style | chooser, then value | chooser, then value when it fits; a Font/style route in More only if displaced |
| Text/highlight color | color chooser, then color | color chooser, then swatch; no duplicate route in More while direct |
| List indent and properties | 1 direct click on primary row | 1 direct click on expanded List row below primary |
| Table row/column insertion/deletion | Table wrapper, then command: 2 | 1 direct click below primary |
| Table style | Table wrapper, style submenu, choice | Table Styles chooser, then style; no object wrapper |
| Picture size, alt text, layout options, delete | Picture wrapper, then command: 2 | 1 direct click below primary |
| Picture wrapping | Picture wrapper, Wrap Text submenu, choice | 1 direct Inline/Square/Top and Bottom click |
| Editable local figure | Picture wrapper, Edit Figure: 2 | 1 direct Edit Figure click in Figure context |
| Equation edit/inline/display/delete | document/context editor route | 1 direct click below primary; edit opens its actual editor |

Settled full MainWindow captures and action enumeration passed for Normal, List, Table, Picture, Figure and Equation at 1280×860 and 820×600. At 1280 every primary control fits and More is absent. In the final 820-wide captures at both scales, More contains only Font, the displaced family chooser. List indentation remains direct in its expanded row. All other registered primary commands, including Strike, remain direct. The action listings are saved in `/tmp/owl-docs-062-click-audit.log` and `/tmp/owl-docs-062-click-audit-150.log`. All four alignment choices and font/highlight color remain direct at both widths. The 420-wide standalone boundary additionally proves every hidden command has an overflow route, no visible command is duplicated, and More/color buttons are fully inside the bar.

Actual captures use `/tmp/owl-docs-062-{normal,list,table,picture,figure,equation}-{820,1280}.png`. They show real semantic document selections and settled MainWindow layout; these are not toolbar mockups. Table, Figure and Equation captures were visually inspected for correct row ordering, complete controls, readable menu text and document placement. The real Insert Table action/dialog and font-color menu grid were also captured at both widths and scales as `/tmp/owl-docs-062-{insert-table,font-color}-{820,1280}[-150].png`; neither exceeds its supported logical window, every visible swatch fits the menu, and the dialog retains Rows, Columns, Header row, Insert and Cancel. The increased-scaling Table and color-grid images were visually inspected and remain clear. All six full-window contexts have corresponding `-150.png` captures.

## Related verification

The full `workspace_controls_tests`, `workspace_ui_tests`, `ribbon_widget_tests`, `list_properties_ui_tests` and `branding_tests` passed at normal and 150% scaling against the completed palette integration. The scaling branding run exposed a raw icon-asset assertion comparing a DPR-aware pixmap's physical pixels to its requested logical size; its three asset-only requests now explicitly use DPR1 while retaining the exact physical-size/transparency/contrast checks. The next scaling run exposed the same unit error in the full-window grab assertion; that check now requires the correct physical dimensions for the pixmap's DPR. Both full branding runs passed afterward. No image asset changed.

The standalone `workspace_controls_tests` target now includes the ColorPaletteWidget source/header through root's shared CMake update. All final focused reruns, actual context captures, and real picker captures passed after that correction and the independent live-custom-color fix. Final incremental build: `flock /tmp/owldocs-build.lock cmake --build build-release --target workspace_ui_tests ribbon_widget_tests list_properties_ui_tests branding_tests -j2`; the controls target was linked separately after the CMake correction. The final global clean Release/package results are recorded in RELEASE_VALIDATION_0.6.2.md.

Additional reproduction commands:

```sh
QT_QPA_PLATFORM=offscreen build-release/workspace_controls_tests
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1.5 build-release/workspace_controls_tests
QT_QPA_PLATFORM=offscreen build-release/workspace_ui_tests
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1.5 build-release/workspace_ui_tests
QT_QPA_PLATFORM=offscreen build-release/workspace_ui_tests layout-choices
QT_QPA_PLATFORM=offscreen build-release/workspace_ui_tests capture-contexts
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1.5 OWL_DOCS_CAPTURE_SUFFIX=-150 build-release/workspace_ui_tests capture-contexts
QT_QPA_PLATFORM=offscreen build-release/workspace_ui_tests capture-pickers
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1.5 OWL_DOCS_CAPTURE_SUFFIX=-150 build-release/workspace_ui_tests capture-pickers
```

`git diff --check` passed. No commit, version change or package was created by this workspace slice.
