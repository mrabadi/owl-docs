# Third-party notices

The Owl Docs figure editor is derived from the local Excalidraw fork at commit
`6609200dd9a756410a4199fe6c06ab3ce9fbcab0`.

The fork's reviewed Excalidraw 0.18 source snapshot is vendored under
`vendor/excalidraw-workspace` so local font families, bold/italic text styles,
and the Professional palette are compiled from source rather than injected
into a minified dependency bundle.

The locked build currently resolves these primary components:

- `@excalidraw/excalidraw` 0.18.1 — MIT
- React and React DOM 18.3.1 — MIT
- Vite 5.4.21 and its React plugin — MIT
- Electron 32.3.3 — MIT; the packaged runtime also carries Electron's license
  and Chromium's generated third-party license inventory.

`package-lock.json` is the authoritative dependency lock. Release publication
still requires the roadmap's complete SPDX SBOM and license audit; this notice
does not replace those gates.
