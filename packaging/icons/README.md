# Owl Docs application icon

The project owner supplied and approved this artwork on 2026-09-12. The
canonical, unmodified source is `owl-docs-master.png`:

- SHA-256: `058f151ffce9299c30d8cc068d17f21b4b5c34f3701b6b76641a1fa2f58c6205`
- Dimensions: 1254 × 1254 pixels
- Format: 8-bit RGBA PNG with a transparent background

The eight checked-in hicolor PNGs are deterministic derivatives of that
master. They are generated at 16, 24, 32, 48, 64, 128, 256, and 512 pixels by
`tools/generate_app_icons.py`. The generator uses Pillow 12.3.0 and zlib 1.3.2,
resizes premultiplied RGBA data with the Lanczos filter and a reducing gap of
3.0, then converts back to straight RGBA before deterministic PNG encoding.
Premultiplication preserves clean translucent edges during downsampling.

Verify the checked-in assets:

```sh
python3 tools/generate_app_icons.py --check
```

CI or a maintainer build can register the same byte-for-byte check with CTest
by setting `OWL_DOCS_ICON_GENERATOR_PYTHON` to a Python interpreter containing
the pinned dependencies. The test is optional because Pillow is an artwork
maintenance dependency, not an Owl Docs build or runtime dependency.

Regenerate them after an intentional master-artwork change:

```sh
python3 tools/generate_app_icons.py --write
```

The generator intentionally rejects other Pillow or zlib versions because
encoder changes can alter bytes even when decoded pixels appear identical.
