import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { start, OrbWeb, OrbExit } from "../../web/orb.js";

const manifest = JSON.parse(await readFile(new URL("../../package.json", import.meta.url), "utf8"));
const runtimeManifest = JSON.parse(await readFile(new URL("../../web/package.json", import.meta.url), "utf8"));

assert.equal(manifest.name, "orb-web-runtime");
assert.equal(manifest.version, "0.2.0");
assert.equal(runtimeManifest.type, "module");
assert.equal(manifest.exports["."].import, "./web/orb.js");
assert.equal(manifest.exports["."].types, "./web/orb.d.ts");
assert.equal(typeof start, "function");
assert.equal(typeof OrbWeb, "function");
assert.equal(typeof OrbExit, "function");

await readFile(new URL("../../web/orb.d.ts", import.meta.url), "utf8");
await readFile(new URL("../../std/web.orb", import.meta.url), "utf8");
await readFile(new URL("../../std/os_wasm.orb", import.meta.url), "utf8");

console.log("Orb! Web package smoke test: passed");
