# OwlDocs 0.6.1 feedback verification

The follow-up keeps the original owl artwork and all persistence identities.
Behavioral changes use executed RED → GREEN cycles; this file records the
root-owned style and Codex changes. Workspace, header/footer and compatibility
cycles have their own focused evidence alongside this record.

## Neutral document styles

`core_tests` first failed 22 assertions requiring neutral colors and the new
heading hierarchy. The current catalog now uses a black 28-point Title,
20/16/14-point Heading 1–3, progressively smaller deeper headings, a muted
regular Subtitle and an italic gray Quote. Heading spacing is consistent and
headings stay with their following paragraph. Font families continue to follow
the document defaults.

The same target passed after implementation (0.13 seconds). An existing
provenance fixture was made explicitly representative of an imported colored
Heading 2, preserving all its direct/inherited-formatting assertions while the
new built-ins share black.

The compatibility lookup had a separate executed missing-public-symbol RED.
`findLegacyBuiltInParagraphStyle` exposes the frozen 0.6.0 definitions without
making them the new default. The expanded core target passed (0.12 seconds).
Legacy package eligibility still requires complete package reproduction.

## GPT-6.1 Sol default

The fake-process controller test first failed both the default-model and ordered
reasoning assertions. It now selects `gpt-6.1-sol` ahead of another model marked
as the server default. For Sol it publishes the supported subset of `low`,
`medium`, `high`, `xhigh`, `ultra` in that order. The UI labels `low` as Light.
Other models retain their discovered choices. Reopening an already connected
controller does not start another connection.

`codex_controller_tests` passed after the implementation (0.03 seconds). The
local model cache was inspected without printing authentication data and
advertised all five requested levels for Sol. This follows the official
[Codex App Server model/list contract](https://learn.chatgpt.com/docs/app-server),
which requires using the account's returned capabilities, and the
[Codex model guidance](https://learn.chatgpt.com/docs/models?surface=cli).

Focused commands:

```sh
cmake --build build --target core_tests codex_controller_tests -j2
ctest --test-dir build --output-on-failure -R '^(core_tests|codex_controller_tests)$'
```

Build failures from another slice's temporarily missing API were not counted
as behavioral RED results for the controller tests.

## Display branding and storage continuity

The Qt identity regression first failed because the new public identity helper
did not exist. After adding that small boundary, it failed at runtime on the
launcher name. It now passes with `OwlDocs` as the visible name and unchanged
`QSettings` filename, application-data path and `owl-docs` desktop identity.
The internal organization/application names remain `DOCX Studio`; package,
executable, database and Codex machine identifiers remain unchanged.

Launcher/MIME metadata, CLI text, current product documentation, diagnostic
messages and the bundled Excalidraw editor's visible labels use the same
spelling. `npm run build` rebuilt the complete local helper successfully using
the existing dependencies. No logo artwork or generated icon bytes changed.
Historical 0.6.0 evidence retains its original recorded output.

## Backward style profiles

Before changing the style definitions, the frozen 0.6.0 Release libraries
generated immutable native-engine and academic DOCX fixtures. A third fixture
was then authored through those untouched desktop libraries. SHA-256 provenance
and the distinction between engine and desktop fixtures are documented in
`tests/fixtures/README.md`.

The native profile test executed a missing-public-field RED before adding
`DocumentDefaults::legacy_style_defaults`. It is GREEN for exact old styles-part
recognition and reproduction, modern defaults for new files, rejection of
modified style catalogs, and rejection of opaque extra parts by the full
regeneration proof. A producer label never selects this profile.
The native write/reopen assertions also check all twelve changed derived
styles' size, bold/italic state, color and before/after spacing against the
requested values; that strengthened target passed.

The desktop test first exposed an unrelated engine-to-desktop projection
difference. Running it against the untouched 0.6.0 libraries confirmed that
difference was pre-existing, so it was excluded from the style RED claim and
the proof was left intact. The actual desktop-authored fixture saves normally
with the old libraries. With modern core defaults and the old converter
baseline, the same margin edit failed at runtime with a conversion prompt.
Selecting the frozen legacy baseline from the source profile made the test
GREEN.

The workflow verifies all old paragraph appearances and byte-exact old
`styles.xml`, then applies modern Heading 2 and verifies its font, color,
emphasis and spacing after save/reopen while retaining that old styles part.
Visual comparison resolves absent/default properties rather than equating
optional representation; nondefault properties remain checked. The old
academic fixture also restores exact note, field and TOC identities and saves
a layout edit normally. Source defaults carry the profile through baseline
projection, native academic proof, saving and retained-source recovery.

The recovery boundary checkpoints a layout edit to the old academic document,
deletes its source file, restores from the retained SQLite package and saves
elsewhere without conversion warnings. The saved package keeps byte-exact old
styles, the detected legacy profile and its verified academic registry. The
three configured profile/workflow/identity tests passed together (2.80 seconds).

Focused commands:

```sh
cmake --build build --target legacy_style_profile_tests legacy_style_workflow_tests application_identity_tests -j2
ctest --test-dir build --output-on-failure -R '^(legacy_style_profile_tests|legacy_style_workflow_tests|application_identity_tests)$'
```

All configured builds use the shared `/tmp/owldocs-build.lock`. The desktop
library was rebuilt cleanly after the appended defaults flag and legacy lookup
API were frozen. Temporary projection diagnostics were removed from production.

## Imported display defaults

With insertion preferences deliberately set to DejaVu Serif at 18 points, the
desktop workflow first failed because an imported document's display defaults
used those preferences instead of its stored font and size. Document-tab
creation now chooses retained/imported source defaults for those documents;
new documents still use the configured insertion preferences. The same
rich/academic and missing-source recovery workflow passed after implementation,
including explicit checks of the recovered display defaults. This also keeps
inherited academic text and generated TOC pagination on the source baseline.

## Header/footer image identity

The core regression first failed twelve assertions: deleting the first image
in a story substituted its PNG identity, bytes, accessible name and dimensions
for the surviving JPEG. A second case showed that unhinted deletion between
adjacent identical placeholder characters silently guessed an image identity.
The original implementation reassigned the remaining image slots by ordinal.

The first GREEN retains images only through provable unchanged source
prefix/suffix positions and rejects ambiguous unhinted changes atomically.
An additional missing-public-symbol RED introduced an optional old-source
UTF-16 replacement range on header/footer text operations. With this validated
range, either adjacent picture can be deleted while preserving the other's
identity and payload. Newly typed orphan placeholders and invalid/mismatched
ranges are rejected without mutation. Snapshot schema remains unchanged.

`core_tests` passed after implementation (0.20 seconds), covering both story
kinds, PNG/JPEG payloads, surviving metadata, shifted offsets and exact
undo/redo. Public/core API edits were applied under the shared build lock,
then frozen before the UI adapter rebuild. The on-page adapter's native edit
coordinates are checked separately in the header/footer evidence. An additional
recovery check encodes and decodes the surviving second picture in both story
kinds after deleting the first adjacent picture. It verifies exact IDs, image
bytes and complete document equality through the existing schema; the focused
`academic_recovery_tests` passed. This is supporting verification, not a new
production RED/GREEN claim.

## Human-readable Codex identity

The existing handshake test gained a joint assertion that the visible client
title is `OwlDocs` and the stable client name remains `owl_docs`. It failed at
runtime with `Codex client title is not OwlDocs or its stable integration
identity changed`. After updating human-readable controller strings, the same
target passed (0.07 seconds). The integration identity and account storage
were not renamed.

## Related style regression maintenance

Executing `app_smoke_tests`, `app_import_tests` and
`editor_tool_bridge_tests` exposed their old hard-coded orange/purple heading
sizes and spacing. These historical expectations were updated to the tested
new catalog; imported merge and empty-mark fixtures explicitly use the frozen
legacy catalog so they retain distinct colors and all their provenance checks.
The partial replacement case also checks that explicit black retains a direct
override mask when the new inherited color is black.

The focused targets then passed: import/save/reopen (8.37 seconds), Codex
preview/style application (0.51 seconds), and desktop smoke (0.50 seconds).
These are maintenance and related verification results, not additional
claimed tests-first production cycles.

## Inline story input and native fields

`header_footer_ime_tests` exercises the actual inline editor through Qt input
events and rendered pixels. Its first executed RED exposed a semantic revision
from an uncommitted composition in a noncanonical story; the isolated canonical
fixture then reproduced invisible preedit. Transient native layout signals are
now kept out of semantic/recovery transactions, and the shared story layout
renders composition text and its underline. The target passed (0.13 seconds),
including committed text and one-step Undo. The final test also retains the
noncanonical-source boundary.

A second executed RED showed the input-method candidate rectangle following
the hidden `{PAGE}` source width rather than the painted caret. Candidate and
anchor rectangles now use the same resolved field layout as painting. The
target passed (0.12 seconds).

The next RED reported Home following phantom wrapped lines created by raw
field tokens. Home/End and Up/Down now navigate rendered lines; selection
endpoints snap to complete native fields. Deletion and Undo preserve the field
relationship. This focused cycle passed (0.15 seconds).

The cancellation edge then failed because native Qt removes selected source
when preedit starts and leaves it removed on cancellation. The editor now
retains the pre-composition source and selection, restores cancelled drafts,
and requests an input-method commit when focus or pointer navigation leaves
composition. Cancel, focus change, selection replacement, committed source and
Undo pass together (0.27 seconds). Reusing a QTextCursor across native document
replacement was corrected in the additional test setup; fresh cursors are
created for each new selection. That setup correction was not counted as a
production RED.

```sh
cmake --build build --target header_footer_ime_tests -j2
ctest --test-dir build --output-on-failure -R '^header_footer_ime_tests$'
```

Shared-file builds were serialized. When another compile had read a source
before an edit but wrote its object afterward, the affected source/test was
touched under the build lock and rebuilt before accepting a result. The final
release still requires a completely clean build after all source freezes.

The rendered-line boundary test then executed RED: Home at `One\n|Two`
selected the previous line. Shared caret/navigation line lookup now uses the
next line's start as its boundary, so explicit breaks and soft-wrapped starts
select the correct line. The focused target passed (0.17 seconds).

A native input-method replacement of the last character of `{PAGE}` executed
RED by leaving a partial token. Replacement ranges now expand across complete
page fields before Qt commits the edit. The same target passed (0.15 seconds),
including one-step semantic Undo.

The adjacent soft-wrap End test executed RED because the logical offset at a
wrap boundary painted on the following line. Visual navigation now carries
line affinity separately from the source offset; this lets End remain at the
right end of its line while Home at the start of the next line remains there.
The focused target passed (0.16 seconds), retaining the composition, field,
selection, explicit-newline and soft-wrap regressions.

## Native independent header/footer regions

`story_ooxml_tests` first executed RED because multiline regional stories were
written as sequential line breaks and tabs instead of independent native cells.
The writer now emits one deterministic, fixed-width, borderless three-cell table
for supported multiline regions. Its parser accepts only that bounded table
shape, restores the exact raw region text and page-field tokens, and retains
picture offsets, identities and source bytes. The complete package proof still
compares every native member; a producer label does not authorize restoration.

The fixture `pre-regional-table-single-line-header.xml` was captured before this
writer change. The new test compares single-line output against those exact
bytes. A synthetic older sequential multiline part is deliberately rejected by
regeneration while an eligible body text patch saves its original story bytes.
A table-width mutation is rejected rather than interpreted as an editable story.

A separate executed missing-public-member RED introduced authoring-only
`header_regional_layout` and `footer_regional_layout` decisions. These force the
same native table for natural wrapping without an explicit line break; they are
not snapshot fields or a trusted source profile. The UI projection obtains them
from the shared story layout using source defaults. Exact source reproduction
continues to decide whether an imported package can regenerate.

A native LibreOffice open and field-refresh smoke also rendered the explicit
multiline stories: center/right header text occupied the first left line, both
left continuations stayed in their own regions, the original owl image remained
visible and footer page fields resolved correctly. Native note numbering and
all five REF checks remained correct after field refresh. The final clean
Release reruns this smoke before packaging.

The footer-edge regression then executed RED because every native cell used
`top` vertical alignment. Headers now require top-aligned cells and footers
require bottom-aligned cells; strict source recognition verifies the matching
metadata. `story_ooxml_tests`, `ooxml_tests` and
`legacy_style_profile_tests` passed together (3/3, 0.53 seconds), including the
no-explicit-break authoring hints.

The rerun native smoke covered both explicit breaks and naturally wrapped
regions. In the refreshed PDF, header center/right text shares the first left
line (`yMin=36.25` points); footer page/right text shares the last left line
(`yMin=742.85` points), as the Canvas edge alignment requires. Both two-page
PDFs were rendered and visually inspected, with the owl picture, note markers,
resolved page fields and all five refreshed references retained. This is
bounded native-render evidence, not a general Word layout-equivalence claim.

The 100-page desktop projection test exposed a further native source defect:
spaces between consecutive NUMPAGES fields disappeared on import. The focused
native test reproduced that exact raw-source mismatch before implementation.
Story parsing now retains whitespace-only text nodes, including the spaces
inside `w:t xml:space="preserve"`; no field or snapshot vocabulary changed.
The native story and OOXML targets passed together (2/2, 0.59 seconds). The
desktop workflow then passed with exact field separators, 100 actual pages,
source font defaults, layout-save after reopen and older-part preservation.
All production sources were frozen before the clean 0.6.1 Release build.
