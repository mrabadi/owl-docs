# M0 public DOCX corpus evidence

## Scope

The initial preservation corpus is a deterministic selection of 100 files from
LibreOffice core's `sw/qa/extras/ooxmlexport/data` directory at commit
`2776654bbb109af114a44fe00ec9771e4ad2d99d`. The source is documented as
MPL-2.0 in `tests/corpus/public_sources.json`. Corpus binaries, generated
manifests, exact copies, and reports remain outside this repository.

The selection is the first 100 repository paths in lexicographic order after
selecting regular `.docx` files and excluding `Encrypted_*`. Those excluded
fixtures are password-encrypted Compound File Binary containers, which are an
explicit product boundary; they are not silently treated as successful DOCX
opens. Downloads are accepted only when `git hash-object` matches the blob ID
from the pinned GitHub index. The generated manifest also records SHA-256.

## Reproduction

```bash
bash tools/bootstrap_docx_corpus.sh /tmp/owl-docs-public-corpus 100
cmake --build build-strict --target owl-docs-corpus-runner
build-strict/owl-docs-corpus-runner \
  --manifest /tmp/owl-docs-public-corpus/manifest.json \
  --output-dir /tmp/owl-docs-public-corpus-results \
  --minimum-fixtures 100
```

The runner fails unless every configured fixture passes all requested checks;
the minimum cannot be satisfied by skipped or rejected files.

## Recorded result

On 2026-09-11, the strict build reported:

| Measure | Result |
|---|---:|
| Manifest fixtures | 100 |
| Successfully opened | 100 |
| Byte-identical untouched Save As | 100 |
| Failures | 0 |

The permanent unit test separately exercises localized-edit mode. It patches
one mapped `w:t`, checks that the XML difference stays inside that text node,
and verifies identical uncompressed bytes for every other package member.

This closes the initial 100-document preservation gate. It does not claim
layout fidelity, editable support for every feature in those files, coverage
of every document producer, or Microsoft Word parity.
