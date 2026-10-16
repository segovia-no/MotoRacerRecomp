#!/bin/bash
# Print the x86 listing of one translated function, from the comments in the generated C.
#   tools/fasm.sh <entry hex va> [gen dir (default build/cmake/gen)]
# e.g. tools/fasm.sh 49b2c0 build/pspprof/gen
set -euo pipefail
va=$(printf "%08x" "0x$1")
gen=${2:-build/cmake/gen}
f=$(/usr/bin/grep -l "^void f_$va(void) {" "$gen"/code_*.c)
awk -v s="void f_$va(void) {" 'index($0, s) == 1 {on=1} on && /^}/ {exit}
    on && /\/\* [0-9a-f]+: / {sub(/^ +\/\* /, ""); sub(/ \*\/$/, ""); print}' "$f"
