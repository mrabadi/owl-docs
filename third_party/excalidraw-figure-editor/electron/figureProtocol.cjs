"use strict";

function validateFigureRequest(request) {
  if (!request || ![1, 2].includes(request.version) || typeof request.token !== "string" ||
      !/^[a-f0-9]{64}$/.test(request.token) ||
      (request.scene !== null && typeof request.scene !== "object") ||
      (request.skeleton !== undefined && !Array.isArray(request.skeleton)) ||
      (request.renderOnly !== undefined && typeof request.renderOnly !== "boolean")) {
    throw new Error("Invalid OwlDocs figure request");
  }
  if (request.version === 2) {
    if (request.mode !== "professional" || request.renderOnly !== true ||
        Object.hasOwn(request, "skeleton") || !request.scene || Array.isArray(request.scene) ||
        !Array.isArray(request.scene.elements) || !Array.isArray(request.patchElementIds) ||
        request.patchElementIds.length < 1 || request.patchElementIds.length > 128 ||
        request.patchElementIds.some((id) => typeof id !== "string" || !/^[A-Za-z0-9_-]{1,64}$/.test(id)) ||
        new Set(request.patchElementIds).size !== request.patchElementIds.length) {
      throw new Error("Invalid native scene patch request");
    }
    const elements = new Map(request.scene.elements.map((element) => [element?.id, element]));
    if (request.patchElementIds.some((id) => !elements.has(id) || elements.get(id).isDeleted)) {
      throw new Error("A native scene patch target is unavailable");
    }
  }
  return request;
}

function rendererRequest(request, mode) {
  return request.version === 2
    ? { version: 2, scene: request.scene, mode: "professional", patchElementIds: request.patchElementIds, renderOnly: true }
    : { version: 1, scene: request.scene, mode, skeleton: request.skeleton || null, renderOnly: request.renderOnly === true };
}

function figureResult(request, mode, result) {
  return { ...result, version: request.version, token: request.token, mode: request.version === 2 ? "professional" : mode };
}

module.exports = { validateFigureRequest, rendererRequest, figureResult };
