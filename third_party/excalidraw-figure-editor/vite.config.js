import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import { resolve } from "node:path";

export default defineConfig({
  base: "./",
  resolve: {
    alias: [
      { find: /^@excalidraw\/excalidraw$/, replacement: resolve(__dirname, "vendor/excalidraw-workspace/packages/excalidraw/index.tsx") },
      { find: /^@excalidraw\/excalidraw\/(.*)$/, replacement: resolve(__dirname, "vendor/excalidraw-workspace/packages/excalidraw") + "/$1" },
      { find: /^@excalidraw\/utils$/, replacement: resolve(__dirname, "vendor/excalidraw-workspace/packages/utils/index.ts") },
      { find: /^@excalidraw\/utils\/(.*)$/, replacement: resolve(__dirname, "vendor/excalidraw-workspace/packages/utils") + "/$1" },
      { find: /^@excalidraw\/math$/, replacement: resolve(__dirname, "vendor/excalidraw-workspace/packages/math") },
      { find: /^@excalidraw\/math\/(.*)$/, replacement: resolve(__dirname, "vendor/excalidraw-workspace/packages/math") + "/$1" }
    ]
  },
  plugins: [react()],
  build: { outDir: "dist", emptyOutDir: true }
});
