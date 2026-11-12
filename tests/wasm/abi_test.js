// Checks the WebAssembly ABI of abi.orb: exports with natural types, imports, function pointers.
const assert = require('assert');
const { load } = require('../../tools/orb-node.js');
(async () => {
  let mem; const logs = [];
  const { instance } = await load(process.argv[2], { out: (fd, s) => logs.push(s), imports: { env: {
    log: (p, n) => logs.push('JS got:' + Buffer.from(mem.buffer, p, n).toString()),
    scale: (x, k) => x * k,
    pair: () => { new Uint8Array(mem.buffer, 64, 5).set(Buffer.from('hello')); return [64, 5]; },
  } } });
  mem = instance.exports.memory;
  const e = instance.exports;
  e._start();
  // host imports run immediately, the program's buffered stdout arrives when _start finishes
  assert.deepStrictEqual(logs, ['JS got:raw log', 'apply=42\nscale=6.000000\npair=hello\n']);
  assert.strictEqual(e.add(40n, 2n), 42n);                 // i64 <-> BigInt
  assert.strictEqual(e.mul_f(1.5, 4), 6);                  // f64
  assert.strictEqual(e.small(-5, 200), 195);               // i32 sign-extended, u8 zero-extended
  new Uint8Array(mem.buffer, 128, 3).set(Buffer.from('abc'));
  assert.strictEqual(e.len_of(128, 3), 3n);                // str = (ptr, len)
  assert.ok(e.__indirect_function_table.length >= 2);
  assert.ok(e.__orb_sp.value > 0);
  console.log('abi ok');
})().catch((err) => { console.error(err); process.exit(1); });
