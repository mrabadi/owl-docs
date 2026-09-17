import { cp, mkdir, rm, access } from "node:fs/promises";
import { resolve } from "node:path";

const source = resolve("node_modules/@excalidraw/excalidraw/dist/prod/fonts");
const destination = resolve("public/fonts");
await rm(destination, { recursive: true, force: true });
await mkdir(resolve("public"), { recursive: true });
await cp(source, destination, { recursive: true });

const ubuntuFont = "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf";
try {
  await access(ubuntuFont);
  await mkdir(resolve("public/fonts/Ubuntu"), { recursive: true });
  await cp(ubuntuFont, resolve("public/fonts/Ubuntu/Ubuntu-R.ttf"));
} catch {
  throw new Error(`Ubuntu font not found at ${ubuntuFont}. Install the ubuntu-font-family package.`);
}
