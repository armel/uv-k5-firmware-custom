#!/bin/bash
# Round trips a spread of POCSAG transmissions through multimon-ng and checks
# that the RIC, function and message all survive.
#
#   MULTIMON=/path/to/multimon-ng ./roundtrip.sh

set -u
MULTIMON=${MULTIMON:-multimon-ng}
BIN=./pocsag_wav
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0

# decode <ric> <func> <baud> <type> <msg> <expected-text>
check() {
    local ric=$1 func=$2 baud=$3 type=$4 msg=$5 expect=$6 extra=${7:-}
    local wav="$TMP/t.wav" out

    $BIN -r "$ric" -f "$func" -b "$baud" -t "$type" $extra -o "$wav" "$msg" >/dev/null || {
        echo "  FAIL encode  ric=$ric baud=$baud type=$type"; fail=$((fail+1)); return
    }

    out=$($MULTIMON -t wav -a "POCSAG$baud" -q "$wav" 2>/dev/null | grep POCSAG)

    # multimon right-aligns the address field, so compare the parsed number
    local got_ric got_func
    got_ric=$(sed -n 's/.*Address: *\([0-9]*\).*/\1/p' <<<"$out")
    got_func=$(sed -n 's/.*Function: *\([0-9]*\).*/\1/p' <<<"$out")

    if [ "$got_ric" = "$ric" ] && [ "$got_func" = "$func" ] && grep -qF "$expect" <<<"$out"; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        echo "  FAIL ric=$ric func=$func baud=$baud type=$type msg='$msg' $extra"
        echo "       got: ${out:-<no decode>}"
    fi
}

echo "== BCH self test =="
$BIN -T || fail=$((fail+1))

echo "== every frame position x every baud rate =="
for i in 0 1 2 3 4 5 6 7; do
    for b in 512 1200 2400; do
        check $((1000000 + i)) 3 $b alpha "FRAME$i" "FRAME$i"
    done
done

echo "== message lengths 1..60 chars (spans batch boundaries) =="
for n in 1 2 3 5 6 8 12 19 20 21 34 35 36 45 57 60; do
    msg=$(printf 'A%.0s' $(seq 1 $n))
    for i in 0 6 7; do
        check $((2000 + i)) 3 1200 alpha "$msg" "$msg"
    done
done

echo "== RIC edge values =="
for ric in 0 1 7 8 9 1000 65535 2097143 2097144 2097150 2097151; do
    check "$ric" 3 1200 alpha "EDGE" "EDGE"
done

echo "== all four function codes =="
# The function bits tell the pager how to read the payload, so they are checked
# with tone-only pages. Alpha text only makes sense under function 3.
for f in 0 1 2 3; do
    check 1234567 "$f" 1200 tone "" ""
done

echo "== numeric =="
for m in 0 12345 0123456789 "1234567890123456789" "555-1234"; do
    check 1234560 0 1200 numeric "$m" "$m"
done

echo "== tone only =="
for i in 0 3 7; do
    check $((5000 + i)) 1 1200 tone "" ""
done

echo "== shaped waveform and inverted polarity =="
check 1234567 3 1200 alpha "SHAPED" "SHAPED" "-p"
check 1234567 3 1200 alpha "INVERTED" "INVERTED" "-i"

echo
echo "pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
