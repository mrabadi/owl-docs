# Header/footer contextual ribbon — RED/GREEN evidence

The requested desktop behavior is an embedded contextual row below the primary
formatting bar, with editing remaining on the page. The canvas retains ownership
of its controls while the workspace hosts the active document's row.

## Opening the ribbon

```sh
flock /tmp/owldocs-build.lock cmake --build build-release --target header_footer_toolbar_tests -j6
QT_QPA_PLATFORM=offscreen build-release/header_footer_toolbar_tests ribbon
```

The new actual-MainWindow test failed before production changes:
`Header and Footer controls float over the document instead of appearing in the
contextual ribbon`. The subsequent integration check exposed the generic deferred
body-focus handler after the Header & Footer command. Guarding that handler keeps
typing in the inline story editor. The same opening test passed after the fix.
Logs: `/tmp/owldocs-header-ribbon-red.log` and
`/tmp/owldocs-header-ribbon-green.log`.

## Editing, switching, keyboard and lifetime

```sh
QT_QPA_PLATFORM=offscreen build-release/header_footer_toolbar_tests ribbon-workflow
QT_QPA_PLATFORM=offscreen build-release/header_footer_toolbar_tests
```

Both commands passed. The workflow covers field insertion at the selected caret,
one-step Undo, control containment at 820×600 and 1280×860, Tab from page to row,
Escape while a row control has focus, Close, independent document-tab state,
deleting a canvas with hosted controls, and closing a window in header mode.
The original standalone-canvas keyboard/traversal tests remain covered.

The workflow fixture uses the established three-region story representation
(`abc\t\t`); an initial assertion omitted its region delimiters and was corrected
before claiming the workflow result. This was a test-fixture correction, not an
application regression.

Refactoring replaced descendant searches with a guarded controls pointer after
reparenting, kept the Escape action associated with both editing surfaces, and
disconnects the hosted-controls destruction callback before Ribbon teardown.
The body formatting row remains disabled in story mode while the contextual
controls remain enabled. The original owl resource is included in the capture
test, and settings/data use a private temporary profile.

Final release/scaling results and visual evidence are recorded in
`RELEASE_VALIDATION_0.6.3.md` and `output/ui-review/0.6.3/workspace/`.
