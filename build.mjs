import { cp, mkdir, readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { resolve } from "node:path";

const root = fileURLToPath(new URL(".", import.meta.url));
const dist = resolve(root, "dist");
await mkdir(dist, { recursive: true });

const required = ["index.html", "styles.css", "app.js"];
for (const file of required) {
  const source = resolve(root, file);
  const content = await readFile(source, "utf8");
  if (!content.trim()) throw new Error(`${file} is empty`);
  await cp(source, resolve(dist, file));
}

console.log(`Built LAB 03 web console in ${dist}`);
