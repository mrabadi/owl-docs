import { cp, mkdir, rm, access } from "node:fs/promises";
import { resolve } from "node:path";

const source = resolve("node_modules/@excalidraw/excalidraw/dist/prod/fonts");
const destination = resolve("public/fonts");
await rm(destination, { recursive: true, force: true });
await mkdir(resolve("public"), { recursive: true });
await cp(source, destination, { recursive: true });

const ubuntuFontDirectory = "/usr/share/fonts/truetype/ubuntu";
const ubuntuFonts = ["Ubuntu-R.ttf", "Ubuntu-B.ttf", "Ubuntu-RI.ttf", "Ubuntu-BI.ttf"];
try {
  await mkdir(resolve("public/fonts/Ubuntu"), { recursive: true });
  for (const font of ubuntuFonts) {
    const sourceFont = `${ubuntuFontDirectory}/${font}`;
    await access(sourceFont);
    await cp(sourceFont, resolve(`public/fonts/Ubuntu/${font}`));
  }
} catch {
  throw new Error(`Ubuntu font family not found at ${ubuntuFontDirectory}. Install the ubuntu-font-family package.`);
}
