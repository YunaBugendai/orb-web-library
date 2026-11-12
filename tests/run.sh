#!/bin/sh
# Orb! test suite: runs every program in tests/ok (stdout compared with .out),
# tests/err (compile errors) and tests/panic (runtime panics).
cd "$(dirname "$0")/.." || exit 1
ORB=./build/orb
pass=0; fail=0
tmp=$(mktemp -d)

ok() { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; [ -n "$2" ] && echo "      $2"; }

for f in tests/ok/*.orb; do
    name=${f%.orb}
    expected="$name.out"
    input=/dev/null; [ -f "$name.in" ] && input="$name.in"
    if [ ! -f "$expected" ]; then
        bad "$f (missing expected output fixture: $expected)" ""
        continue
    fi
    for mode in "" "--no-opt" "--unchecked"; do
        $ORB build $f $mode -o $tmp/t.bin 2> $tmp/err.txt || { bad "$f [$mode] (compile)" "$(head -3 $tmp/err.txt)"; continue; }
        $tmp/t.bin < $input > $tmp/out.txt 2> $tmp/stderr.txt
        if cmp -s $tmp/out.txt "$expected"; then ok; else bad "$f [$mode] (output differs)" "$(diff $tmp/out.txt $expected | head -3)"; fi
    done
done

for f in tests/err/*.orb; do
    case $f in */helper_mod.orb) continue;; esac
    want=$(head -1 $f | sed 's#^// expect: ##')
    if $ORB check $f > /dev/null 2> $tmp/err.txt; then bad "$f (expected a compile error)"; continue; fi
    if grep -qF -- "$want" $tmp/err.txt; then ok; else bad "$f (wrong message)" "wanted: $want / got: $(head -1 $tmp/err.txt)"; fi
done

for f in tests/panic/*.orb; do
    want=$(sed -n 's#^// expect: ##p' "$f" | head -1)
    want_code=$(sed -n 's#^// exit: ##p' "$f" | head -1)
    [ -n "$want_code" ] || want_code=101
    if [ -z "$want" ]; then bad "$f (missing // expect fixture)" ""; continue; fi
    $ORB build "$f" -o "$tmp/t.bin" 2> "$tmp/err.txt" || { bad "$f (compile)" "$(head -3 "$tmp/err.txt")"; continue; }
    "$tmp/t.bin" > /dev/null 2> "$tmp/stderr.txt"; code=$?
    if [ "$code" -ne "$want_code" ]; then bad "$f (expected exit code $want_code, got $code)"; continue; fi
    if grep -qF -- "$want" "$tmp/stderr.txt"; then ok; else bad "$f (stderr)" "wanted: $want / got: $(head -1 "$tmp/stderr.txt")"; fi
done

# VS Code extension (needs node)
if command -v node >/dev/null 2>&1; then
    if node --test vscode-orb/test/lib.test.js vscode-orb/test/extension.mock.test.js > $tmp/node.txt 2>&1; then ok; else bad "vs code extension tests" "$(grep -E 'not ok' $tmp/node.txt | head -3)"; fi
else
    echo "SKIP: VS Code extension tests (Node.js not installed)"
fi

# command line interface
$ORB version | grep -q "Orb!" && ok || bad "orb version"
$ORB check examples/hello.orb | grep -q ok && ok || bad "orb check"
$ORB run examples/hello.orb | grep -q "Hello, Orb" && ok || bad "orb run"

rm -rf $tmp
echo "passed: $pass, failed: $fail"
[ $fail -eq 0 ]
