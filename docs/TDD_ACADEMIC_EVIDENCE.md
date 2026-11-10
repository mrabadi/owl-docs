# Academic editing TDD evidence

Executed against the working tree on 2026-11-09. All CMake builds use
`flock /tmp/owldocs-build.lock cmake --build build --target <target> -j2`.

## Equation replacement

- RED: `c++ -std=c++20 -I include -fsyntax-only tests/academic_core_tests.cpp`
  failed because the new public `ReplaceEquation` operation did not exist.
- GREEN: `academic_core_tests` built and ran successfully after adding the
  identity-preserving operation. The test checks source/display, stable ID,
  offset, surrounding text, and exact undo.

## Academic atoms and resolution

- RED: focused syntax check failed because `Document::setNote` and `NoteKind`
  did not exist; the resolver acceptance test subsequently failed because its
  new public `docxstudio/core/academic.h` header did not exist.
- GREEN: linked `academic_core_tests` passed after implementing field atoms,
  notes, offsets across equation insertion/split/merge, deletion cascade,
  captions, numeric citations with locator, semantic TOC entries, TOC block
  insertion, and preview isolation.

## Recovery schema 14

- RED: built and executed `./build/academic_recovery_tests`; observed
  `Recovery paragraph has orphan inline-object placeholders` followed by
  `FAILED: decode academics`.
- GREEN: the same executable passed after schema-14 encoding/restoration.
  It verifies exact Document equality with notes, fields, caption and generated
  TOC; malformed field offset rejection; and schema-13 migration with absent
  academic collections.

## Native DOCX transport and source authority

- RED: `academic_ooxml_tests` rejected the initial writer, which did not emit
  native note references/parts and a verified current academic snapshot.
- GREEN: the same executable verifies native footnotes/endnotes, repeatable
  package generation, and snapshot invalidation after changing each of the
  document, footnote, endnote, relationship, or content-type parts.
- RED: a reference with no corresponding note story was accepted; the writer
  now rejects the inconsistent package before replacing a target.
- RED: a note containing XML-invalid C0 text could be written. GREEN verifies
  `unsafe_edit` and byte-for-byte preservation of the existing destination.
- RED: section XML lacked explicit decimal endnote policy. GREEN verifies the
  policy in both document settings and the section, for office compatibility.

## Desktop semantic proof and repeated saving

- RED: a package with forged recovery JSON and a matching self-asserted native
  byte index restored the forged semantic text. GREEN reserializes the decoded
  snapshot and requires the complete source package to match before restoring
  identities; otherwise the native source remains authoritative.
- RED: genuine academic documents failed that proof when note settings caused
  the source default-tab reader to substitute a fallback. GREEN reads the
  supported default tab stop without weakening the strict package proof.
- `academic_document_workflow_tests` now verifies a multipage document with
  notes, heading TOC, caption, citation locator, bibliography entry and reference;
  exact authored semantics survive save/reopen. A later edit and ordinary save
  complete without a compatibility popup. TOC targets cover later pages.

## Core identity and plain-text projections

- RED: full caption references duplicated the separator from the visible
  caption prefix. GREEN resolves the actual UI prefix to `Figure 1: An owl`.
- RED: the equation text projection treated UTF-8 bytes as UTF-16 units. GREEN
  verifies Unicode source such as `α+β` survives copy/Codex projection.
- RED: merging a caption into a neutral paragraph and splitting at zero lost
  the semantic role or reference target. GREEN transfers the role and retargets
  the complete reference graph to the surviving semantic paragraph.
- `academic_core_tests` also verifies atomic rejection of a text-range deletion
  across a generated TOC, explicit TOC operations, ID collision rejection,
  offset maintenance and isolated preview/undo.

## Codex contract

- RED: a new academic preview operation was not accepted/advertised by the
  public tool catalog. GREEN covers schema advertisement and execution through
  the revision-checked isolated preview path.
- `academic_tool_tests` verifies note insertion/readback, stable-ID equation
  replacement, citations, references, independent caption/bibliography
  paragraphs, TOC insertion/configuration/deletion, note editing, malformed
  argument rejection, and exact undo of an accepted compound preview.

## Verification

The initial core regression command passed all three tests:
`ctest --test-dir build --output-on-failure -R '^(academic_core_tests|academic_recovery_tests|core_tests)$'`.

After the final source changes, these focused executables all passed:

- `./build/academic_core_tests`
- `./build/academic_recovery_tests`
- `./build/academic_ooxml_tests`
- `QT_QPA_PLATFORM=offscreen XDG_DATA_HOME=/tmp/owl-docs-academic-workflow-tests ./build/academic_document_workflow_tests`
- `QT_QPA_PLATFORM=offscreen XDG_DATA_HOME=/tmp/owl-docs-academic-tool-tests ./build/academic_tool_tests`

The parent task verifies Canvas/UI, source-preserving save/recovery, math,
external office rendering and the complete application suite separately.

## Mixed academic clipboard regression

- RED: built and ran `academic_clipboard_tests`; observed
  `FAILED: mixed cut/paste lost the independent note story`. The old clipboard
  retained metadata only for an exact single-atom selection, so cutting prose
  plus a reference removed its note body and pasted only a visible number.
- GREEN: the regression passed with a bounded range payload, multiple paragraphs,
  equations and independently cloned notes. Repeated references in the copied
  range share one new story. Paste is one undo step and undoing Cut restores the
  exact original document identities. Unsupported structural cuts preserve both
  document and clipboard instead of flattening semantic relationships.
- RED: an additional single-reference cross-document regression failed because
  its absent target was retained as an unresolved hidden ID. The final parser
  uses copied visible text whenever the destination lacks that supported target;
  malformed typed range payloads are rejected before any live mutation.
- Final GREEN: `academic_clipboard_tests`, `academic_ui_tests`,
  `editor_interaction_tests` and `image_ui_tests` all passed with
  `QT_QPA_PLATFORM=offscreen` after rebuilding their targets under the build lock.
- Guard refinement RED: fresh Release and Debug `academic_clipboard_tests`
  reported `ordinary prose cut within a semantic paragraph was incorrectly
  blocked`. The minimum fix restricts the structural role guard to ranges across
  paragraphs; prose inside a heading, caption or bibliography paragraph retains
  its role and ID, while numbered prefix atoms remain protected from Cut.

## Native targets after visible prefix deletion

- RED: `academic_document_workflow_tests` deletes caption and bibliography
  prefix atoms through the existing core API, verifies exact undo/redo, then
  saves through the desktop. Native package inspection reported
  `deleted prefixes still provide hidden native numbered bookmark targets and
  visible references`: REF targets had disappeared even though semantic roles
  remained valid.
- The transport now supplies a deterministic hidden bookmarked SEQ target only
  when a semantic caption/source lacks its visible prefix. Core text remains
  unchanged, cached references remain visible, and normal source proof requires
  the regenerated complete package to match before restoring the snapshot.
- Final GREEN: rebuilt and executed `academic_clipboard_tests`,
  `academic_document_workflow_tests`, `academic_ooxml_tests`, and
  `academic_ui_tests`; all passed. The workflow restores exact no-prefix
  semantics after full native reserialization proof and performs a later
  ordinary save without a compatibility popup.

## Hidden target bookmark boundaries

- RED: generated a native no-prefix caption/bibliography package with the
  current transport API and ran `./build/academic_ooxml_tests`; it reported
  `hidden sequence targets must end before full-text paragraph bookmarks start`.
  The hidden numbered target was inside the whole-paragraph bookmark used by
  full-text references.
- The minimal serializer change emits the hidden target before opening the
  paragraph bookmark. The numbered target retains its separate bookmark, and
  full-text REF targets contain only the remaining visible paragraph content.
- GREEN: rebuilt `academic_ooxml_tests` and `academic_document_workflow_tests`
  under the build lock, then executed both successfully. The native test checks
  both hidden target boundaries and complete package regeneration; the desktop
  workflow checks exact prefix-deleted semantic restoration and subsequent save.
