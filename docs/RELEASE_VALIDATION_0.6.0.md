# Owl Docs 0.6.0 release validation

Validated on 2026-11-09 on Ubuntu 22.04.5 LTS with GCC 11.4.0, CMake 3.22.1 and Qt 6.2.4. This records a development release's tested scope; it does not establish general Word compatibility.

## Build and complete test run

The final production sources were frozen before a clean strict Release build. All 168 build steps completed successfully with the bundled figure editor required.

```sh
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DDOCXSTUDIO_STRICT_WARNINGS=ON \
  -DOWL_DOCS_REQUIRE_FIGURE_EDITOR_BUNDLE=ON \
  -DOWL_DOCS_CORPUS_MANIFEST=/tmp/owl-docs-public-corpus-060/manifest.json
flock /tmp/owldocs-build.lock cmake --build build-release --clean-first -j2
ctest --test-dir build-release --output-on-failure
```

Final result: **49/49 tests passed, 0 failures, 20.76 seconds**. The suite includes save/recovery, native DOCX and academic round trips, equations, clipboard, the restricted parser, document layout, tables/pictures, workspace behavior, unchanged artwork and the installed Codex protocol schema. The final maintenance corrections updated legacy forest-selection/DPR assertions, the expected tool-catalog version and the reviewed icon-generation runtime; application sources remained unchanged after the clean build.

The external preservation gate opened **100/100** pinned public LibreOffice DOCX fixtures and created **100/100 byte-identical unchanged copies**. Downloads were verified by pinned Git blob identity and SHA-256. The corpus is external and is not included in the package.

## Native and real-helper checks

- LibreOffice 7.3.7.2 opened an authored native DOCX containing cases, a centered ordinary matrix, TOC, caption/REF fields, manual numeric citations/bibliography and both note types, and exported a two-page PDF without a conversion error. Both independent note sequences displayed decimal `1`.
- Five REF caches were deliberately set to `STALE`. A private headless LibreOffice UNO session refreshed fields and exported PDF. Caption/bibliography references recomputed correctly after visible prefixes were deleted; their hidden sequence targets stayed hidden. The full-text caption reference recomputed from the remaining visible prose. Extracted text and the rendered first page were checked.
- The opt-in real Excalidraw acceptance test passed with `OWL_DOCS_TEST_LOCAL_EXCALIDRAW_RENDERER=1`, including actual offline Professional rendering, editable PNG metadata and Codex preview/accept. A scoped run allowed the helper's own bubblewrap sandbox to operate after the outer tool sandbox rejected NETLINK_ROUTE. All 73 installed helper files matched the bundle used for packaging.
- Workspace and legacy layout/table tests passed at normal and 150% display scale. Screenshots at 1280×860 and 820×600 were reviewed; detailed evidence is in `TDD_WORKSPACE_EVIDENCE.md` and `TDD_LAYOUT_EVIDENCE.md`.

## Package verification record

The Debian artifact is `owl-docs_0.6.0_amd64.deb`. Its final digest and extraction checks are recorded outside the package in `dist/SHA256SUMS` and `dist/validation-0.6.0.json`, avoiding a self-referential package hash. Verification covers package metadata, the executable's version, resolved runtime libraries, the stripped restricted parser processing a real native fixture, all eight original icon assets, all 73 bundled helper files and exact installed documentation copies. It does not install or launch a replacement desktop application.

See `RELEASE_NOTES_0.6.0.md`, `COMPATIBILITY.md`, `PRIVACY.md` and the five focused TDD evidence files for supported behavior and remaining boundaries.
