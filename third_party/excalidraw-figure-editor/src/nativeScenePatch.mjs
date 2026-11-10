export function prepareNativeScene(scene, patchElementIds, engine) {
  if (!scene || !Array.isArray(scene.elements) || !Array.isArray(patchElementIds) ||
      patchElementIds.length < 1 || patchElementIds.length > 128 ||
      new Set(patchElementIds).size !== patchElementIds.length) {
    throw new Error("Invalid native scene geometry request");
  }
  const copied = structuredClone(scene);
  const elements = engine.restoreElements(copied.elements, null, { repairBindings: true });
  const map = new Map(elements.map((element) => [element.id, element]));
  const geometryChanged = new Set(patchElementIds);
  for (const id of patchElementIds) {
    const element = map.get(id);
    if (!element || element.isDeleted) throw new Error("A native patch target cannot be restored");
    if (element.type === "text" && element.containerId) geometryChanged.add(element.containerId);
  }
  const texts = new Set();
  for (const element of elements) {
    if (!element.isDeleted && element.type === "text" &&
        (geometryChanged.has(element.id) || geometryChanged.has(element.containerId))) texts.add(element.id);
  }
  // Reuse Excalidraw's measurement and binding algorithms. A longer edited
  // label may grow its container, so all measurement precedes connector work.
  for (const id of texts) {
    const text = map.get(id);
    const container = text.containerId ? map.get(text.containerId) || null : null;
    engine.redrawTextBoundingBox(text, container, map, false);
    if (container) geometryChanged.add(container.id);
  }
  for (const id of geometryChanged) {
    const element = map.get(id);
    if (element && !element.isDeleted) engine.updateBoundElements(element, map);
  }
  // Connector geometry can move an arrow's midpoint and its bound label.
  // Update those labels after the endpoints have settled, without traversing
  // through the arrow into unrelated shapes on its other end.
  for (const element of elements) {
    if (element.isDeleted || (element.type !== "arrow" && element.type !== "line")) continue;
    if (!geometryChanged.has(element.id) && !geometryChanged.has(element.startBinding?.elementId) &&
        !geometryChanged.has(element.endBinding?.elementId)) continue;
    for (const text of elements) {
      if (!text.isDeleted && text.type === "text" && text.containerId === element.id)
        engine.redrawTextBoundingBox(text, element, map, false);
    }
  }
  return { ...copied, elements };
}
