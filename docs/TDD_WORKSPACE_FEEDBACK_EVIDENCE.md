# Workspace feedback verification for 0.6.1

The original owl artwork and persistent application identities are unchanged.
All behavior changes below used the explicitly requested TDD skill. Builds use
`flock /tmp/owldocs-build.lock cmake --build build --target ... -j2`.

## Writing workspace

`workspace_ui_tests brand-row` first rejected the old spaced product label and
separate menu row. The menu bar now contains the original owl, `OwlDocs` and
the application menus on one row. A stronger narrow-window regression also
rejected menus covered by the right action corner at 820 pixels. Recomputing
that corner's width after hiding optional controls made every menu reachable.

`workspace_controls_tests alignment` failed because alignment required opening
overflow. All four alignment commands now remain directly available, including
the 420-pixel writing-bar boundary. `inline-list` failed because list controls
were a secondary dropdown. Bullets now reveal direct indent and list-properties
controls on the same writing row; the integration test clicks the real bullet
button and verifies both expansion and collapse.

`workspace_controls_tests popup` failed with `More menu indicator overlaps its
label`. The popup button now reserves label space and draws a separate chevron.
The regression checks separation, actual chevron pixels and menu opening. A
rendered close-up was inspected. The final refactor separates persistent primary
format actions from optional overflow actions without suffix-based matching.

## Codex controls and connection

`workspace_controls_tests chat-choices` first rejected collapsed model settings.
Model, Reasoning and Speed now stay visible. The Sol display name is friendly,
while signals and messages retain the server model identifier. The advertised
five Sol reasoning levels display exactly Light, Medium, High, XHigh and Ultra;
Light maps to `low`. An absent Sol uses the first server-ordered fallback.
These tests check emitted model/reasoning/tier identifiers as well as labels.

The connection regression first had an executed missing-constructor RED for
the injectable process boundary, then an actual runtime RED because opening
Chat did not connect. The final test uses only an in-memory process: startup
is closed with zero starts; opening starts one connection; hiding, resizing
and reopening while connecting/connected do not duplicate it. Explicit
Disconnect stays disconnected through resizing until the next user open.
The check exercises the actual toggle action, connection button and complete
handshake. It also requires the whole model/reasoning/tier controls, composer
and Send button to fit at 1280×860 and 820×600.

That integration test exposed a native teardown crash after its assertions.
The debugger showed a dock visibility callback reading the already-destroyed
command registry during the QWidget base destructor. MainWindow now disconnects
its dock visibility callbacks before member destruction. The same complete
test subsequently exited successfully. No live Codex process or network is
used in the automated workspace tests or screenshot fixture.

## Active document and story mode

`workspace_ui_tests inactive-error` failed when an invalid equation emitted by
a background canvas replaced active-document feedback. The operation-failure
connection now verifies the active canvas before changing its status.

`workspace_ui_tests story-controls` failed because body formatting was offered
while a plain-text header was active. Story mode now disables the writing bar,
body-only insertion, paragraph styles and reference commands. Native story
picture insertion remains available, and closing story mode restores body
controls. Canvas-level guards are covered separately by the header/footer owner.

## Related verification

After these changes the complete focused UI group passed **8/8** (6.58 seconds):
`workspace_ui_tests`, `workspace_controls_tests`, `ribbon_widget_tests`,
`list_properties_ui_tests`, `navigation_workflow_tests`,
`main_window_preferences_tests`, `branding_tests`, and `app_smoke_tests`.
The last smoke test preserves the parent's independent modern-style checks.
Subsequent narrow story-toolbar and scaled-render verification is recorded below.

The complete workspace and standalone controls suites also passed at
`QT_SCALE_FACTOR=1.5`: **2/2** (3.41 seconds). Visual review caught the last four
pixels of `Standard` under the Speed combo's arrow area. The new label-fit
assertion failed before the fix. Widths now use the active style's actual edit
field geometry and the advertised effort/tier labels; the same `chat-choices`
test passed with every requested reasoning label and Standard fully fitting.

## Narrow header/footer controls

`header_footer_toolbar_tests` first failed with `header/footer controls extend
beyond the narrow viewport`. The toolbar now reflows to two rows with every
direct control and the full Close Header and Footer label. That geometry test
passed at 420, 480 and 1100 pixels with the shared application theme applied.

The next keyboard regression failed with `Tab skips the native Page Number
toolbar button`. Focusable Enter-capable buttons alone were insufficient:
the canvas's global focus-traversal suppression also blocked its child toolbar.
The regression retains the chosen footer region and insertion offset, uses
Tab then Enter to insert a native page field, and uses Enter to leave story
mode.

The toolbar-only traversal fix then passed its PAGE/PAGES/Close test. A separate
regression produced `Tab from the initial story editor cannot reach the next
region`, proving the toolbar still needed mouse entry. Traversal now delegates
for the active story editors and toolbar children only. Both header and footer
tests traverse left→center→left and right→toolbar without inserting source tabs.
Existing body/list Tab behavior remains covered by `editor_interaction_tests`.

The last actual RED was `region selector does not follow the focused story
editor`. The activation callback now updates the region combo with signals
blocked, so the focused region is reflected without resetting its caret.
The complete toolbar suite passed at default and 150 percent scale. The final
default-scale related group passed **4/4** (8.98 seconds): toolbar, body/editor
interaction, workspace integration and standalone workspace controls. A prior
expanded related UI group passed **9/9** (11.71 seconds). The final scaled
toolbar/workspace group passed **3/3** (3.42 seconds). These workspace and
toolbar production sources are frozen.

Workspace captures are preserved under `output/ui-review/0.6.1/`:
1280×860, 820×600 and both at 150 percent scale. The inspected views show
all eight menus, the original owl, direct alignment controls, clean More
chevrons, black document headings, visible page top and persistent unsaved
state. Separate 915/420-pixel captures show expanded list controls on one row.
Story-toolbar captures at 420, 480 and 1100 pixels, including 150 percent scale,
were also inspected: full labels and all direct controls fit without clipping.

## Native regional wrapping

A final bounded export slice added an executed missing-symbol RED for
`DocumentCanvas::measureStoryLines`. The static helper then passed its tests
for naturally wrapped versus single-line regions and resolved PAGES field
width at one versus one hundred pages. It calls the existing shared story
layout with the source font, page geometry, picture atoms and real page count;
it adds no Canvas data members.

The desktop workflow next failed with `naturally wrapped regional stories
were saved as sequential tab paragraphs instead of native cells`. Projection
now sets the native header/footer regional-layout hints when canonical
three-region source wraps according to that shared measurement. Import proof
creates the same baseline Canvas pagination used for generated contents, so
PAGES widths use the actual body page count even without a live canvas or TOC.

The workflow keeps exact source equality, saves/reopens a one-hundred-page
field-driven wrap, forces a layout save to exercise regeneration proof, and
checks that source-default fonts retain older single-line parts byte-for-byte.
It also requires safe body-text patches to retain an older wrapped sequential
header part unchanged. Authored wrap fixtures use enlarged margins; no old
18/9-point story edges are hardcoded.

That workflow exposed a native parser loss of whitespace-only runs between
consecutive PAGES fields. Pagination correctly remained one hundred pages;
all seven separator spaces were missing from the restored source. The native
owner received the exact reproducer for a separate RED/GREEN fix. The desktop
source-equality assertion was retained.

After that native whitespace fix, the complete unchanged desktop workflow
passed: `ctest --test-dir build -R '^story_wrap_workflow_tests$'
--output-on-failure`, **1/1** (1.34 seconds). The one-hundred-page body remains
one hundred pages, all field separator spaces survive, and both native-table
regeneration and older source-preserving saves pass without conversion prompts.
The static API, helper and projection were frozen immediately after this GREEN.
`git diff --check` also passed. Final clean Release verification belongs to the
coordinated release gate.
