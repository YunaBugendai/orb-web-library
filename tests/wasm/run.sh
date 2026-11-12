#!/bin/sh
# WebAssembly backend tests that need no browser (Node.js >= 16 runs the modules).
# Every program of tests/ok must print what the native build prints (in both control-flow modes and
# without optimisation), tests/panic must fail the same way, plus ABI and stack-overflow checks.
cd "$(dirname "$0")/../.." || exit 1
ORB=build/orb; TMP=build/wasm-tests; mkdir -p $TMP
command -v node >/dev/null 2>&1 || { echo "wasm tests skipped: node not found"; exit 0; }
pass=0; fail=0
ok() { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $*"; }
for f in tests/ok/*.orb; do
    n=$(basename $f .orb)
    [ $n = wordcount ] && continue          # needs stdin
    want=tests/ok/$n.out; [ -f tests/wasm/$n.out ] && want=tests/wasm/$n.out    # argc / files differ on the web
    for mode in "" "--wasm-dispatch" "--no-opt"; do
        if $ORB build $f --target wasm $mode -o $TMP/$n.wasm 2>$TMP/err.txt; then
            timeout 120 node tools/orb-node.js $TMP/$n.wasm > $TMP/$n.out 2>/dev/null
            if cmp -s $TMP/$n.out $want; then ok; else bad "$n [$mode] output differs"; fi
        else bad "$n [$mode] does not compile: $(head -1 $TMP/err.txt)"; fi
    done
done
for f in tests/panic/*.orb tests/wasm/stackoverflow.orb; do
    n=$(basename $f .orb); want=$(head -1 $f | sed 's#^// expect: ##')
    if $ORB build $f --target wasm -o $TMP/$n.wasm 2>$TMP/err.txt; then
        node tools/orb-node.js $TMP/$n.wasm >/dev/null 2>$TMP/$n.err; code=$?
        if { [ $code -eq 101 ] || [ $code -eq 1 ]; } && grep -qF -- "$want" $TMP/$n.err; then ok; else bad "$n: exit $code, stderr: $(head -c 120 $TMP/$n.err)"; fi
    else bad "$n does not compile"; fi
done
$ORB build tests/wasm/abi.orb --target wasm -o $TMP/abi.wasm && node tests/wasm/abi_test.js $TMP/abi.wasm >/dev/null && ok || bad "abi test"
# the compiler must refuse what the wasm target cannot do
printf 'fn main() { __syscall(60, 0); }\n' > $TMP/sys.orb
$ORB build $TMP/sys.orb --target wasm -o $TMP/sys.wasm 2>$TMP/err.txt && bad "__syscall should be rejected" || ok
printf 'use web;\nfn main() {}\n' > $TMP/web_native.orb
$ORB build $TMP/web_native.orb -o $TMP/web_native 2>$TMP/err.txt && bad "use web on a native target should be rejected" || { grep -q "target wasm" $TMP/err.txt && ok || bad "web error message"; }
echo "wasm tests: passed $pass, failed $fail"
[ $fail -eq 0 ]
