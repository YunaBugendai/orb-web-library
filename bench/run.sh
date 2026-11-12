#!/bin/sh
# Benchmarks Orb! against equivalent C programs (gcc -O0 and gcc -O2).
# Prints the best of 3 runs (seconds) and the output check.
cd "$(dirname "$0")" || exit 1
ORB=../build/orb
mkdir -p out
best() { # best wall time of 3 runs of "$@"
    b=999999
    for i in 1 2 3; do
        s=$(date +%s.%N); "$@" > out/last.txt; e=$(date +%s.%N)
        t=$(echo "$e - $s" | bc -l)
        if [ "$(echo "$t < $b" | bc -l)" = 1 ]; then b=$t; fi
    done
    printf "%.3f" "$b"
}
printf "%-10s %10s %10s %10s %10s   %s\n" benchmark "orb" "orb-uncheck" "gcc -O0" "gcc -O2" "output"
for n in ${@:-fib sieve matmul collatz mandel}; do
    $ORB build $n.orb -o out/$n.orb.bin || exit 1
    $ORB build $n.orb --unchecked -o out/$n.orbu.bin || exit 1
    gcc -O0 -o out/$n.c0 $n.c -lm; gcc -O2 -o out/$n.c2 $n.c -lm
    t1=$(best out/$n.orb.bin); o1=$(cat out/last.txt)
    t2=$(best out/$n.orbu.bin)
    t3=$(best out/$n.c0); t4=$(best out/$n.c2); o4=$(cat out/last.txt)
    ok=OK; [ "$o1" = "$o4" ] || ok="MISMATCH($o1 vs $o4)"
    printf "%-10s %9ss %9ss %9ss %9ss   %s\n" $n $t1 $t2 $t3 $t4 "$ok"
done
