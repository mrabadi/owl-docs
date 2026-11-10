import assert from "node:assert/strict";
import { createRequire } from "node:module";
import test from "node:test";

const require = createRequire(import.meta.url);
const { validateFigureRequest, rendererRequest, figureResult } = require("../electron/figureProtocol.cjs");
const token = "a".repeat(64);
const native = () => ({
  version: 2, token, title: "Native edited figure", mode: "professional", renderOnly: true,
  scene: { type: "excalidraw", version: 2, elements: [{ id: "shape", type: "rectangle" }, { id: "label", type: "text", containerId: "shape" }], files: { retained: { dataURL: "data:image/png;base64,AA==" } } },
  patchElementIds: ["shape", "label"]
});

test("ordinary version 1 create/open contract stays available", () => {
  const request = { version: 1, token, scene: null, skeleton: [], mode: "professional", renderOnly: true };
  assert.equal(validateFigureRequest(request), request);
  assert.equal(rendererRequest(request, "professional").renderOnly, true);
  assert.deepEqual(figureResult(request, "sketch", { saved: false }), { version: 1, token, mode: "sketch", saved: false });
});

test("native patch protocol carries the original scene and stable patch IDs and echoes version 2", () => {
  const request = native();
  assert.equal(validateFigureRequest(request), request);
  const renderer = rendererRequest(request, "professional");
  assert.equal(renderer.version, 2);
  assert.equal(renderer.scene, request.scene);
  assert.deepEqual(renderer.patchElementIds, ["shape", "label"]);
  assert.equal(Object.hasOwn(renderer, "skeleton"), false);
  assert.deepEqual(figureResult(request, "professional", { saved: true }), { version: 2, token, mode: "professional", saved: true });
});

test("native patch requests reject incomplete, ambiguous or non-render-only input", () => {
  for (const change of [
    { patchElementIds: [] }, { patchElementIds: ["missing"] }, { patchElementIds: ["shape", "shape"] },
    { patchElementIds: Array.from({ length: 129 }, (_, index) => `id${index}`) },
    { renderOnly: false }, { mode: "sketch" }, { skeleton: [] }, { scene: null }
  ]) assert.throws(() => validateFigureRequest({ ...native(), ...change }));
});
