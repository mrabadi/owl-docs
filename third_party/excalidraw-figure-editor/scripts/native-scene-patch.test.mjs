import assert from "node:assert/strict";
import test from "node:test";
import { prepareNativeScene } from "../src/nativeScenePatch.mjs";

function fixture() {
  return {
    type: "excalidraw", version: 2, appState: { custom: "retained" }, files: { picture: { dataURL: "data:image/png;base64,AA==" } },
    elements: [
      { id: "shape", type: "rectangle", x: 200, y: 100, width: 90, height: 50, angle: 0.2, roughness: 2, fillStyle: "hachure", groupIds: ["group"], boundElements: [{ id: "label", type: "text" }, { id: "arrow", type: "arrow" }] },
      { id: "label", type: "text", containerId: "shape", x: 15, y: 15, width: 20, height: 10, text: "Long edited label", originalText: "Long edited label", fontFamily: 5 },
      { id: "arrow", type: "arrow", x: 100, y: 100, width: 100, height: 0, points: [[0, 0], [100, 0]], endBinding: { elementId: "shape", gap: 8 }, boundElements: [{ id: "arrow-label", type: "text" }] },
      { id: "arrow-label", type: "text", containerId: "arrow", x: 0, y: 0, width: 10, height: 10, text: "connect", originalText: "connect", fontFamily: 2 },
      { id: "unrelated", type: "image", x: -20, y: -10, width: 50, height: 50, fileId: "picture", customData: { original: true }, groupIds: ["other-group"] }
    ]
  };
}

function fakePinnedEngine() {
  const calls = [];
  return {
    calls,
    restoreElements(elements, local, options) {
      assert.equal(local, null);
      assert.deepEqual(options, { repairBindings: true });
      calls.push("restore");
      return structuredClone(elements);
    },
    redrawTextBoundingBox(text, container, map, inform) {
      assert.equal(map.get(text.id), text);
      assert.equal(inform, false);
      calls.push(`text:${text.id}`);
      text.width = text.originalText.length * 10;
      text.height = 20;
      if (container) {
        text.x = container.x + 10;
        text.y = container.y + 10;
        container.width = Math.max(container.width, text.width + 20);
      }
    },
    updateBoundElements(element, map) {
      calls.push(`binding:${element.id}`);
      for (const other of map.values()) {
        if (other.endBinding?.elementId === element.id) {
          other.points[other.points.length - 1] = [element.x + element.width - other.x, element.y - other.y];
        }
      }
    }
  };
}

test("native edits remeasure bound labels and move their connected arrow geometry without restyling", () => {
  const scene = fixture();
  const original = structuredClone(scene);
  const engine = fakePinnedEngine();
  const result = prepareNativeScene(scene, ["label"], engine);
  const byId = new Map(result.elements.map((element) => [element.id, element]));
  assert.equal(byId.get("label").width, "Long edited label".length * 10);
  assert.equal(byId.get("label").x, 210);
  assert.deepEqual(byId.get("arrow").points.at(-1), [290, 0]);
  assert.equal(byId.get("arrow-label").x, 110);
  assert.equal(byId.get("shape").roughness, 2);
  assert.equal(byId.get("shape").fillStyle, "hachure");
  assert.equal(byId.get("label").fontFamily, 5);
  assert.deepEqual(byId.get("shape").groupIds, ["group"]);
  assert.deepEqual(byId.get("unrelated"), original.elements.at(-1));
  assert.deepEqual(result.files, original.files);
  assert.deepEqual(scene, original);
  assert.ok(engine.calls.indexOf("text:label") < engine.calls.indexOf("binding:shape"));
});

test("moving a shape repositions its bound text even when only the shape is patched", () => {
  const scene = fixture();
  const result = prepareNativeScene(scene, ["shape"], fakePinnedEngine());
  assert.equal(result.elements.find((element) => element.id === "label").x, 210);
});

test("native geometry preparation rejects missing or deleted patch targets", () => {
  assert.throws(() => prepareNativeScene(fixture(), ["missing"], fakePinnedEngine()));
  const scene = fixture(); scene.elements[0].isDeleted = true;
  assert.throws(() => prepareNativeScene(scene, ["shape"], fakePinnedEngine()));
});
