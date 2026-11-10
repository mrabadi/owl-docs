# Codex Excalidraw verification — 0.6.5

The figure changes follow the requested TDD skill. Tests use the public editor-tool dispatcher and inject only the external renderer boundary; actual rendering is a separate offline integration gate. No model inference is needed for these deterministic checks.

## Executed cycles

| Behavior | Observed RED | GREEN evidence |
| --- | --- | --- |
| Labeled arrows | `excalidraw_figure_tests`: “Codex discarded the advertised arrow label before rendering” | `/tmp/owldocs-agent-figure-label-green.log` |
| Plain line labels fail explicitly before rendering | “unsupported line label was silently discarded instead of rejected before rendering” | `/tmp/owldocs-agent-figure-line-green.log` |
| Validate replacement image identity before rendering | “missing figure target was rendered before its identity was validated” | `/tmp/owldocs-agent-figure-target-green.log` |
| Closing the document during nested rendering | Actual SIGSEGV, exit 139, from the renderer deleting its canvas | `/tmp/owldocs-agent-validity-fresh.log`; a `QPointer` now rejects the result before accessing the canvas |
| Disconnecting Codex during rendering | Regression added alongside the actual closed-canvas crash; no separate runtime RED is claimed | `/tmp/owldocs-agent-validity-fresh.log`; session identity is checked before creating a preview |
| Header/footer image quotas include table images | `core_tests` accepted a 513th/514th image after 512 table images and changed the document | `/tmp/owldocs-agent-story-budget-green.log`; the shared document image-resource assessment is used |
| Native table-cell and header/footer figure placement | “Codex cannot create an editable figure inside a table cell” | `/tmp/owldocs-agent-placement-green.log`; full figure tests pass, including discard, explicit image identities, one Undo and invalid story anchors rejected before rendering |
| Bound-label source reads | “figure read loses bound-label identity/source text or hides skeleton completeness” | Inspector integration reached the next read→edit regression, proving the metadata assertions passed |
| Read→edit label and connector identity | “read-to-edit normalization discarded bound-label or connector identity” | Label IDs and arrow shape references are retained; the next run reached the distinct reason-budget regression |
| Unsupported-reason strings share the read budget | “unsupported figure reasons bypass the shared read context budget” | `/tmp/owldocs-excalidraw_figure_tests-native-green.log`; full figure binary passes with the latest Canvas ABI and renderer API |
| Native patch validates the destination before rendering | “native patch did not validate image identity before rendering” | `/tmp/owldocs-native-patch-green.log`; existing native scenes retain unrelated elements, groups, assets and metadata while changed text reflows its container and connector |
| Partial replacement cannot silently lose native identities | “partial skeleton silently replaced original scene/label identities” | `/tmp/owldocs-reconstruction-green.log`; safe replacement retains every original element identity, while explicit reconstruction produces a specific, visible loss report |

The final cell-sizing regression is recorded in [the shared Canvas/agent authoring evidence](TDD_CODEX_AUTHORING_0.6.5.md). It exposed stored widths of 432 pt in a table cell where native insertion produced 224 pt. Figures touched by an agent proposal are now fitted once, after the complete batch, using the final candidate table geometry and page settings. The six GREEN cases include unequal columns, a newly created table, and geometry or margins changed after the figure operation. Unrelated imported pictures are not resized.

The renderer and session-callback seams had intermediate missing-symbol/signature compilation checks (`/tmp/owldocs-agent-figure-interface-red.log` and `/tmp/owldocs-agent-figure-session-interface-red.log`). These are interface setup checks, excluded from behavioral RED evidence. The actual closed-canvas crash drives that guard cycle.

## Commands and isolation

Focused builds use the strict Release configuration and coordinated build lock:

```sh
flock /tmp/owldocs-build.lock cmake --build build-release --target excalidraw_figure_tests -j4
QT_QPA_PLATFORM=offscreen XDG_CONFIG_HOME=/tmp/owldocs-figure-test-config XDG_DATA_HOME=/tmp/owldocs-figure-test-data ./build-release/excalidraw_figure_tests
```

Core quota verification used `./build-release/core_tests`. Test directories and private renderer sessions avoid user documents and preferences. Preview tests assert the committed document remains unchanged until acceptance and that discard and one Undo preserve the original.

## Offline renderer acceptance

Native scene patching, explicit reconstruction and the existing figure suite pass against the final cell-fitting implementation. Native process-boundary teardown, callback deletion, protocol-version and timeout RED/GREEN evidence is recorded in `docs/TDD_CODEX_CONTROLLER_0.6.5.md`; all those focused checks pass.

The actual freshly bundled offline renderer gate passed after the final source freeze, using the current 0.6.5 helper and the freshly linked test binary. Its ASAR SHA-256 is `1e31a0e3cb8c7b97bd0a85a43051b7c1d7fcf647af0c0cc3c8cf5b23ab787e3a`. The test created a labeled Professional figure, read and replaced its canonical scene while retaining identities, and patched a human scene containing native groups, a connected arrow, a freehand stroke, a custom background and an embedded image asset. It checked preview isolation, discard, acceptance, one Undo, Redo, DOCX reopen and exact recovery.

Earlier gate attempts found two test-fixture mistakes: selecting the last image after insertion selected the previous figure, and comparing all core GUIDs across an ordinary DOCX expected identities that Word does not store. The gate now locates the inserted figure by its unique accessible name and checks DOCX PNG/source bytes, native element identities, accessibility text, layout, dimensions and offsets. Direct recovery and in-session Undo/Redo still require exact document equality. These fixture corrections are not claimed as product behavioral REDs. Explicit reconstruction permission and the visible identity-loss report are checked through the public dispatcher with a deterministic renderer seam; the actual helper gate exercises the same Professional rendering path separately.

The exported one-page Letter PDF was rendered with the system Poppler at 120 dpi and visually reviewed. Pixel assertions locate both figures, retain the green background and magenta image asset, verify the native figure's aspect ratio and page margins, and compare its rendered PDF image with the accepted PNG (mean channel errors 2.93, 2.90 and 3.42, each below 5). The unrelated freehand crop remains byte-identical before and after the native patch. The actual gate used private XDG directories and the trusted C++ `executableOverride_` test seam, which launches the helper directly and bypasses the installed application's Linux `bwrap` launcher. Electron/Chromium sandboxing and helper offline restrictions were exercised with outer tool-sandbox escalation. The installed application's default `bwrap` isolation remains unchanged but was not exercised by this override gate. No live model inference or installation was performed.

Persistent evidence and representative PNGs, editable scenes, DOCX, PDF and PDF render are under `output/codex-review/0.6.5/figures/`. `renderer-gate.json` records exact commands, exit codes, source/helper/binary hashes and artifact hashes; `evidence/pdf-pixel-gate.py` and its JSON record the quantitative PDF checks. The release coordinator also consumes `/tmp/owldocs-065-excalidraw-gate.json`. Final full CTest, package and install checks are recorded by release validation separately.
