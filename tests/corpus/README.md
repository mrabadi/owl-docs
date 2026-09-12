# DOCX fidelity corpus

Corpus documents are deliberately not stored in this repository. The public
M0 gate uses the LibreOffice `ooxmlexport` fixtures described in
`public_sources.json`, pinned to one immutable source revision and covered by
MPL-2.0. The bootstrap downloads a deterministic lexicographic selection,
verifies every Git blob ID, records a SHA-256 digest, and saves the pinned
license beside the external cache.

Files named `Encrypted_*` are excluded before selection. They are password-
encrypted Compound File Binary containers rather than readable OPC ZIP
packages, and password-encrypted packages are an explicit Owl Docs product
boundary. The runner is not weakened to count a deliberate rejection as an
open or preservation success.

Prepare and run the 100-document public preservation gate:

```bash
bash tools/bootstrap_docx_corpus.sh /tmp/owl-docs-public-corpus 100
cmake --build build-strict --target owl-docs-corpus-runner
build-strict/owl-docs-corpus-runner \
  --manifest /tmp/owl-docs-public-corpus/manifest.json \
  --output-dir /tmp/owl-docs-public-corpus-results \
  --minimum-fixtures 100
```

Every listed fixture must open and an untouched Save As must be byte-identical
to its pinned input. A failure is reported per fixture in JSON and produces a
nonzero exit status. Corpus results and copied documents are disposable local
test outputs.

The runner can also exercise a deliberately safe localized text patch. Add
`"localized-edit"` to `checks` and provide:

```json
"localizedEdit": {
  "textSpanOrdinal": 0,
  "replacement": "Corpus replacement"
}
```

The runner then verifies that the replacement is confined to one `w:t` text
node and every other package member has identical uncompressed bytes. Use this
only for a fixture whose selected text span is documented as editable.

Private or locally licensed real-world documents may be exercised through an
external manifest based on `external_manifest.example.json`. Keep that manifest
and its files outside the repository, record a stable local version plus a
SHA-256 digest, and use a non-sensitive `local-authorized:` provenance label
when there is no public source URL. Never turn a private document into a CI
dependency. Such local evidence does not count toward the public M0 corpus
minimum.
