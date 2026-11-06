#!/bin/sh
# Late input sampling has to beat reading input before the wait for the display (#141).
# Usage: compare_input_sampling.sh <stackide binary>
set -eu

median() {
    "$1" --frames 300 --inject-input 150 --timeout-ms 20000 --input-sampling "$2" 2>&1 |
        sed -n 's/.*input to paint: .*p50=\([0-9.]*\) ms.*/\1/p'
}

naive=$(median "$1" naive)
late=$(median "$1" late)
echo "input to paint p50: naive ${naive} ms, late ${late} ms"

[ -n "$naive" ] && [ -n "$late" ] || {
    echo "a run reported no input to paint"
    exit 1
}
awk -v late="$late" -v naive="$naive" 'BEGIN { exit !(late < naive) }'
