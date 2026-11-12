#!/usr/bin/env node
// Runs an Orb! program compiled with `orb build --target wasm` under Node.js (no browser APIs).
// usage: node tools/orb-node.js program.wasm [--call name arg...]
'use strict';
const fs = require('fs');

class OrbExit extends Error { constructor(code) { super('exit ' + code); this.code = code; } }

function createHost(out = (fd, s) => (fd === 2 ? process.stderr : process.stdout).write(s)) {
  const st = { mem: null };
  const dec = new TextDecoder('utf-8');
  const env = {
    write(fd, p, n) { out(fd, dec.decode(new Uint8Array(st.mem.buffer, p, n))); return n; },
    read() { return 0; },
    exit(code) { throw new OrbExit(code); },
  };
  return { st, imports: { orb_rt: env } };
}

async function load(file, extra = {}) {
  const bytes = fs.readFileSync(file);
  const host = createHost(extra.out);
  const imports = Object.assign({}, host.imports, extra.imports || {});
  const { instance } = await WebAssembly.instantiate(bytes, imports);
  host.st.mem = instance.exports.memory;
  return { instance, host };
}

async function main() {
  const file = process.argv[2];
  if (!file) { console.error('usage: orb-node.js program.wasm'); process.exit(2); }
  const { instance } = await load(file);
  try { instance.exports._start(); }
  catch (e) { if (e instanceof OrbExit) process.exit(e.code); throw e; }
}
if (require.main === module) main();
module.exports = { load, createHost, OrbExit };
