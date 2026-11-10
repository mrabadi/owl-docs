# Codex authoring verification for OwlDocs 0.6.5

This record covers the semantic authoring adapter and shared UI planning work.
It does not by itself establish the final release, packaging, preservation-corpus,
or live Codex/Excalidraw acceptance gates. Those results must be recorded after
their commands have completed.

## Contract and implementation

`EditorSemanticOperations` validates each request against its generated operation
schema, plans against a private document copy, and publishes the resulting
operations/affected identities only when the complete request succeeds. The
document session's shared `applyDocumentOperation` supplies the actual semantics.
The bridge remains responsible for document routing, revision checks, isolated
preview creation, user acceptance and aggregate request budgets.

`authoring_tools.cpp` defines 28 operations and generates their capability
descriptions from those same schemas:

- Tables: insertion, stable-cell text/range replacement, character and paragraph
  formatting, nine-position alignment, table styles, geometry, row/column
  insertion/deletion, dimension distribution, border presets and table deletion.
- Pictures: bounded PNG/JPEG insertion in body/cells/default stories, resizing,
  accessible text, supported wrapping and deletion.
- Stories: complete or left/center/right default header/footer text, including
  the existing `{PAGE}`/`{PAGES}` field representation.
- Structure: paragraph splitting/merging and body-block movement.
- Academic metadata: identity-preserving citation/reference edits, caption
  metadata and bibliography roles with generated number fields.

These operations use existing authored document semantics. They do not add
comments, tracked changes, merged/nested tables, rich headers, arbitrary drawing
formats, standalone TeX, file saving/printing, or application preferences.

## Executed RED/GREEN cycles

The initial focused target was built with the repository's strict Release flags.
All failures below are executed behavioral failures, rather than compiler errors.

| Slice | RED assertion | GREEN evidence |
| --- | --- | --- |
| Create a populated table and replace a stable cell range | `FAILED: agent cannot create tables` | `/tmp/owldocs-semantic-table-green.log` |
| Format/resize/restructure a table and preserve shared borders | `FAILED: agent cannot format tables` | `/tmp/owldocs-semantic-format-green.log` |
| Insert/edit/remove body, cell and story pictures; edit story regions | `FAILED: agent cannot insert or edit pictures/stories` | `/tmp/owldocs-semantic-images-green.log` |
| Split/merge/move paragraphs and edit academic metadata | `FAILED: agent cannot edit document structure/academic metadata` | `/tmp/owldocs-semantic-structure-green.log` |
| Address cells created earlier in the same preview | `FAILED: agent cannot address newly created cells in the same preview` | `/tmp/owldocs-semantic-cell-ids-green.log` |

RED logs are `/tmp/owldocs-semantic-table-red.log`,
`/tmp/owldocs-semantic-format-red.log`,
`/tmp/owldocs-semantic-images-red.log`,
`/tmp/owldocs-semantic-structure-red.log` and
`/tmp/owldocs-semantic-cell-ids-red.log`.

The creation target's initial configured command was:

```bash
cmake --build build-release --target editor_semantic_operations_tests -j2
QT_QPA_PLATFORM=offscreen build-release/editor_semantic_operations_tests
```

Later independent adapter checks used a private strict compiler build in
`/tmp/owldocs-semantic-build`, compiling the same test, adapter, schema and raster
decoder sources without modifying shared build outputs. The script recorded
core/Codex/raster archive hashes before and after linking. A structure build was
explicitly rejected when a peer rebuilt the Codex archive during compilation;
the executable was relinked with stable hashes before its tests ran. Logs retain
that rejection. The first private linker setup also found absent Qt pkg-config
metadata; existing CMake-resolved library paths fixed the setup. Neither event
is counted as a behavioral RED or a passing test.

The adapter's configured target subsequently passed independently in the root
integration build (`/tmp/owldocs-065-semantic-integration-green.log`). Later changes
still require the final configured-target rerun.

## Refactoring and additional characterization

The shared paragraph/character-format parsers were extracted from the already
passing cell formatter into `parseEditorParagraphFormat` and
`parseEditorCharacterFormat`. Existing adapter cases remained
green (`/tmp/owldocs-semantic-paragraph-refactor-green.log`). It parses supported
alignment, indents, before/after spacing, multiplier/point line spacing, keep
flags and page-break-before. Body style identity/provenance remains the existing
body planner's responsibility. The character parser also supplies the existing
strict cell schema checks and nullable No Highlight behavior to the body bridge;
its body regression cycle is recorded by the root.

Optional rectangular `insert_table.cellIds` supplies validated, unique stable
identities. A subsequent operation in the same preview can style a new cell;
the test also rejects duplicate requested identities without partial mutation.

Additional passing characterization checks cover:

- A failed populated-table insertion rolls back its preliminary paragraph split,
  generated table and affected-identity list.
- Invalid range/geometry/alignment, unknown properties and malformed UTF-8 leave
  the working document and planned operations unchanged.
- A malformed raster is rejected, and maximum-length base64 decoding to more
  than 4 MiB fails before image decoding.
- Adding a previously absent center story region preserves neighboring text;
  multiline story text remains plain text and existing picture identities stay
  anchored.
- Picture deletion works in body, cell and story containers without deleting
  adjacent text. Floating cell pictures are rejected by existing core limits.

These checks characterize already implemented guards; they are not presented as
separate newly observed RED cycles. All passed in
`/tmp/owldocs-semantic-final-verify.log` with stable private-link archive hashes.

## Shared list planner and selection discovery

The root bridge's numbered-list behavioral RED was executed before adding
`planListOperations` (`/tmp/owldocs-065-lists-red.log`). The planner constructs a
private canvas from the exact supplied document, copies editor/list defaults,
and captures successful operations from the actual UI implementation. It uses
the same toggle, level, per-list layout and numbering logic as direct editing.
It does not load the working copy through import/default-adoption paths.

`list_operation_planning_tests` characterizes planned versus direct UI numbering,
absolute levels, layout, removal and bullet conversion, with separately generated
list IDs compared by relationships rather than fixed random values. It also
checks idempotence, live/input isolation, noncontiguous rejection, and that
number refresh neither changes bullets nor adopts unrelated literal numbered
prose. Its configured strict GREEN passed in the fresh app build
(`/tmp/owldocs-agent-list-planner.log`). The first test compile had an unrelated
ADL collision between a local `apply` helper and `std::apply`; renaming the test
helper to `replayPlan` fixed the scaffolding. That compiler error is not a RED.

Passive selection getters expose stable table-cell identities, active cell
UTF-16 text ranges, and active header/footer region/cursor ranges. Story offsets
are explicitly relative to their left/center/right region. Bridge read-selection
integration is recorded separately by the root; the getters alone do not claim
to fix tool routing or insertion placement.

## Native Excalidraw helper protocol and geometry

The bundled helper retains ordinary v1 create/open behavior and adds v2 native
patch rendering. V2 requires a native scene, 1–128 unique active patch IDs,
Professional mode and render-only operation; it rejects skeletons and echoes
version 2 in its result. Renderer IPC carries the native scene and patch IDs.

`node scripts/figure-protocol.test.mjs` first passed the v1 characterization but
failed the new v2 acceptance with `Invalid OwlDocs figure request`
(`/tmp/owldocs-native-protocol-red.log`). After implementation, all three protocol
tests passed (`/tmp/owldocs-native-protocol-green.log`).

`node scripts/native-scene-patch.test.mjs` exposed stale edited-label dimensions
(`20 !== 170`) and missing dependent movement
(`/tmp/owldocs-native-geometry-red.log`). After implementation, three geometry
orchestration tests passed (`/tmp/owldocs-native-geometry-green.log`). These unit
tests inject a small engine boundary to verify measurement-before-binding order,
parent/connector/arrow-label traversal, input isolation and unchanged styles,
groups, unrelated objects and assets. They do not claim to test Excalidraw's
actual font or connector mathematics.

The production v2 path waits for pinned font loading, restores native elements
with binding repair, and calls the pinned `redrawTextBoundingBox` and
`updateBoundElements` implementations. It does not run native objects through
the Professional skeleton restyler. The offline
`npm_config_offline=true npm run build` completed successfully
(`/tmp/owldocs-native-helper-build.log`). The real C++ renderer acceptance gate
must still validate this bundle's actual geometry and native source merge.

Export options were then extracted with the existing white-background/API-file
behavior passing (`owldocs-native-export-refactor-green.log`). A native-source
background regression failed with `#ffffff` instead of `#19352b`
(`owldocs-native-export-red.log`). The v2 path now uses source app state and
embedded files for serialization and PNG export before UI hydration; ordinary
v1 serialization is unchanged. Both export-option tests passed
(`owldocs-native-export-green.log`), along with the six protocol/geometry tests.
The offline bundle rebuild is recorded in
`/tmp/owldocs-native-helper-final-build.log`; actual native PNG fidelity remains
part of the separate renderer gate.

## Native scene inspection and preservation

`ExcalidrawScene` is a QtCore-only boundary shared by figure reads and native
patch rendering. Its public tests were compiled independently with C++20,
`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`, then executed directly;
this did not modify shared Ninja outputs.

| Slice | Meaningful failing assertion | Passing evidence |
|---|---|---|
| Native skeleton inspection | `owldocs-scene-inspect-red.log`: supported bound labels were unavailable | `owldocs-scene-inspect-green.log`: source text, stable label identity/font size, conservative completeness, empty scenes and 128-element truncation excluding bound labels. |
| Native patch and source merge | `owldocs-scene-patch-red.log`: native edits were unavailable | `owldocs-scene-patch-green.log`: parent/connector/arrow-label closure and derived geometry merge retain original groups, assets, custom data and unrelated objects. |
| Embedded asset boundary | `owldocs-scene-assets-red.log`: external/non-image asset reached native rendering | `owldocs-scene-assets-green.log`: file, HTTP, relative and non-image URLs fail before rendering; embedded base64 image data remains accepted. |
| Native edit bounds | `owldocs-scene-bounds-red.log`: UTF-8 text exceeded its byte bound | `owldocs-scene-bounds-green.log`: 4,000-byte text and ASCII IDs, duplicate/invalid edits, missing renderer elements and binding retargeting fail atomically. |
| Bound-label style preservation | `owldocs-scene-label-styles-red.log`: custom label color/opacity was declared losslessly replaceable | `owldocs-scene-label-styles-green.log`: unsupported label presentation requires native patching. |
| Locked native targets | `owldocs-scene-locked-red.log`: a locked target could be edited | `owldocs-scene-locked-green.log`: locked targets fail explicitly while unrelated locked objects remain unchanged. |
| App-state completeness | `owldocs-scene-app-state-red.log`: nonrepresentable app state was declared replaceable | `owldocs-scene-app-state-green.log`: dark export, unknown metadata and invalid state require native patching; known viewport, selection and grid editing aids remain neutral. |
| Transparent skeleton defaults | `owldocs-scene-transparent-red.log`: transparent shape background was emitted outside the existing color schema | `owldocs-scene-transparent-green.log`: default transparency is omitted and recreated by the pinned helper; explicit colored backgrounds remain present. |

All selectors also passed together (`/tmp/owldocs-scene-all-green.log`). Native
scene input/output is bounded to 8 MiB; patches permit 1–128 edits totaling at
most 256 KiB. Native patch text uses UTF-8 byte bounds. Source JSON remains
authoritative: the renderer may supply geometry, wrapped visible text, binding
geometry and version metadata only for the affected closure. It cannot retarget
bindings or replace unrelated objects, assets, groups, style, custom fields or
the edited `originalText` source. Pure QtCore tests use synthetic render results;
the real pinned renderer remains a separate acceptance gate.

After these tests were green, a 1,000-element dependency-chain characterization
also passed (`owldocs-scene-chain-baseline-green.log`). The repeated scene scans
were refactored to a queue with dependency indexes, keeping the exact affected
set and original output order. The chain and complete suite remained green
(`owldocs-scene-chain-refactor-green.log`, `owldocs-scene-all-green.log`). This is
a behavior-preserving GREEN-to-GREEN refactor, with no timing assertion or new
element limit.

## Same-identity image preview pixels

`image_payload_preview_tests body` compiled successfully and then failed because
a `ReplaceImagePayload` preview painted the original cached PNG despite the
new semantic bytes (`/tmp/owldocs-image-payload-red.log`). The cache formerly
used only image identity, which intentionally remains stable during figure
editing.

The decoded cache now also tracks the immutable encoded payload using shared
storage. Decode and reconciliation invalidate entries whose bytes changed;
document reset, imported presentation, body/cell/story insertion and direct
Excalidraw replacement maintain the same payload keys. Eviction releases its
decoded-byte allowance before a replacement is decoded.

After a fresh strict app build, the complete paint regression passed in body,
table cell, header and footer contexts
(`/tmp/owldocs-image-payload-green-build.log`,
`/tmp/owldocs-image-payload-green.log`). It warms the original cache, checks
custom-color pixels from the actual viewport, verifies preview leaves the live
document and dirty state unchanged, and checks discard, accept, one Undo and
Redo repaint the expected original/replacement image. Source and payload
identity are checked alongside the rendered pixels.

## Structural routes and native numbering

The real Bridge regressions separately exposed missing numbering refresh after
`insert_page_break`, `insert_caption` and `insert_bibliography_entry` inside a
native numbered run (`owldocs-numbering-refresh-red.log`,
`owldocs-numbering-caption-red.log`, `owldocs-numbering-bibliography-red.log`).
Each failed the shared native planner fixed-point check after an otherwise
successful preview and acceptance.

The Bridge now detects emitted `SplitParagraph` operations before its existing
numbering refresh. This also covers internal paragraph splits from TOC
insertion without duplicating route-specific numbering rules. All three
regressions passed (`/tmp/owldocs-numbering-refresh-green.log`), checking the
shared planner's expected following marker, live/preview isolation, searchable
preview branch, discard, one transaction and one Undo/Redo retaining source
identities and formatting.

The same fresh configured build produced native figure patch and conservative
reconstruction GREEN results (`owldocs-native-patch-green.log`,
`owldocs-reconstruction-green.log`) and retained the existing figure suite
(`owldocs-figure-existing-green.log`). The earlier corresponding runtime REDs
were missing native image-only target routing and silent omission of source
element/label identities, rather than compiler failures. Full build output is
`/tmp/owldocs-structural-figure-green-build.log`; detailed figure ownership and
real-renderer acceptance evidence are recorded by the figure/release owners.

## Table-cell figure dimensions and export fidelity

A private, strict-compiler parity regression linked against hash-checked frozen
archives and exposed a concrete stored-size mismatch: native insertion authored
224 pt while the Codex default authored 432 pt in the same two-column cell
(`/tmp/owldocs-cell-size-red.log`). Canvas painting scaled the oversized tool
figure down, but DOCX export uses the model's stored extents. The first private
fixture compile required explicit integer-to-double casts in diagnostic output;
that compiler setup failure was not a RED.

Six public regression selectors then failed meaningfully before the fix
(`owldocs-figure-geometry-<selector>-red.log`): default tool/native parity,
ordinary native insertion into an authored unequal column, same-batch creation
of a table and figure, proposed page margins, column widening after figure
insertion, and page margins after figure insertion.

`fitTableCellImageSize` now plans on an exact-document shadow and reuses actual
table visuals, cell padding, document defaults, imported presentation and final
proposed page geometry. Native default picture insertion shares this helper.
The Bridge fits only surviving table-cell figures touched by the proposal,
after all batch operations; it keeps their original desired sizes until the
final fit, so later column widening does not retain a prematurely shrunken
figure. Unrelated imported figures and explicit raster operations are unchanged.

The configured strict build and all six selectors passed
(`/tmp/owldocs-figure-geometry-green-build.log`,
`/tmp/owldocs-figure-geometry-green.log`). They also check aspect ratio, native
source retention, preview/discard isolation and one Undo/Redo for content and
proposed margins. Native patch, reconstruction and existing figure suites also
passed against this fresh build (`owldocs-native-patch-cellfit-green.log`,
`owldocs-reconstruction-cellfit-green.log`,
`owldocs-figure-existing-cellfit-green.log`). The release owner reruns the final
all-target, corpus and scaling gates after this correction.

## Limits and release verification

The Codex raster tool accepts at most 4 MiB of encoded image bytes per image;
the bridge enforces 4 MiB aggregate new image bytes per preview and an 8 MiB
request frame. Existing desktop image limits remain 16 MiB per atom and 32 MiB
per document. Larger existing desktop images can be edited by identity without
uploading their bytes through the tool.

Table columns/row minima use points in requests and twips in the model. Automatic
column distribution requires an explicit total width when no authored widths
exist; row distribution operates on authored minimum heights. The root preview
layer has page/font geometry needed for rendered fit checks.

Reproduce configured adapter/list checks after the final integration:

```bash
cmake --build build-release --target editor_semantic_operations_tests list_operation_planning_tests -j2
QT_QPA_PLATFORM=offscreen ctest --test-dir build-release -R '^(editor_semantic_operations_tests|list_operation_planning_tests)$' --output-on-failure
```

Full CTest, final strict Release, pinned preservation corpus, rendered UI,
real figure renderer, package inspection and upgrade simulation results are
recorded separately in `RELEASE_VALIDATION_0.6.5.md` and the external package
validation record. Focused GREEN logs do not substitute for those gates.

## Bridge and desktop integration

The bridge uses the same core operation dispatcher as `DocumentSession`.
That extraction was a behavior-preserving refactor verified with the existing
core suite before expanding the bridge.

| Slice | Executed RED | Verified behavior |
|---|---|---|
| Complete document discovery | `owldocs-065-discovery-red.log`: empty tables/cells absent | Document/table/story/image/metadata reads expose stable identities, empty structural blocks and continuation. |
| Page-layout proposals | `owldocs-065-page-layout-red.log`: geometry could not be proposed with content | Live geometry/dirty state stay unchanged during preview; accept, discard, one Undo and Redo include geometry. A separate layout token catches margin changes without a text revision. |
| Native lists | `owldocs-065-lists-red.log`: numbered-list proposal unavailable | Preview planning captures the actual UI list operations on an isolated canvas, including list identity, nesting and layout. |
| Runtime capability dispatch | `owldocs-065-capability-discovery-red.log`: unknown tool | Discovery binds catalog/runtime data to a document and revision; malformed inputs fail explicitly. |
| Formatting reads | `owldocs-065-format-discovery-red.log`: indents/keep flags absent | Reads retain authored paragraph spacing, indents, pagination flags and list metadata. |
| Extended paragraph formatting | `owldocs-065-paragraph-format-red.log`: desktop paragraph properties unavailable | Body and cell formatting share validated native paragraph deltas while the body style planner retains provenance. |
| Clear highlighting | `owldocs-agent-root-paragraph-format.log`: highlight could not be cleared | Body and cell character formatting share the validated parser, including explicit No Highlight. |
| Current selection | `owldocs-agent-root-table-selection.log`: cell caret ignored | Reads identify the active cell caret, rectangular cells and header/footer region with explicit offset units. |
| Structural numbering | `owldocs-agent-planner-structure-fresh.log`: moved lists retain old counters | A pre-edit baseline supplies authored starting counters to the shared numbering planner; final GREEN is recorded with the release gates. |
| Empty outline/selection limits | `owldocs-agent-root-empty-read-bounds-red.log`: empty paragraphs bypass structural limits | `owldocs-agent-root-empty-read-bounds-green.log`: one shared bounded writer enforces `maxBlocks` and continuation for document, outline and selection reads. |

Fresh strict Release runs verified page/layout/list/capability/formatting and
selection selectors after implementation. The compound
`codex_authoring_workflow_tests` executes every newly advertised semantic
operation through the real bridge, then checks preview isolation, discard,
acceptance, one Undo/Redo, failed-batch atomicity, exact recovery and native DOCX
save/reopen. It also checks an open header editor entering preview and rejection
of a story that does not fit.

Compiler errors, temporary header/source mismatches during coordinated builds
and a test-helper ADL naming collision were setup failures, not RED evidence.
Legacy diagnostic assertions were updated to the shared parser's property
paths; spacing boundary checks now use the current catalog's 20,000 pt maximum.

### Preview-state audit regressions

The review added five executed runtime failures before changing the preview
implementation: a page-only proposal newly clipped an unchanged header;
removing the selected table or selected row left a stale cell cursor; inserting
or deleting an earlier row redirected the cursor into a different stable cell.
The logs are `owldocs-065-preview-{layout,table-delete,row-insert,row-delete,row-delete-active}-red.log`.

The isolated preview now records its original cursor state, maps surviving
cells by stable identity, clears deleted selection contexts and retains one
Undo record. Story fitting compares previous and proposed capacity using the
existing pagination engine, including candidate page-number fields. It refuses
newly hidden content while allowing non-worsening retained imported overflow.
All five corresponding `-green.log` selectors and the complete preview-state,
interaction, Bridge, list and compound authoring suites pass.

The numbering refactor retains each run's original authored starting counters
when paragraphs move. Existing run-membership logic also marks visited items,
so refresh plans each numbered run once. This refactor passed the existing
planner suite after the structural move regression became GREEN.
