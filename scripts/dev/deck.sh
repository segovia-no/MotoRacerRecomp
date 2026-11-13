#!/bin/bash
# Scripted runs on the owner's Steam Deck over SSH (docs/notes/13-e2e-testing.md). Opens a game window
# on the Deck (Desktop Mode) for the duration; muted.
#
#   scripts/dev/deck.sh push <local program> [name]      copy a Linux program next to the Deck's motoracer
#   scripts/dev/deck.sh run <name> <tag> "<keys>" [seconds] [ENV=value ...]
#   scripts/dev/deck.sh log <tag>                         copy the run's log to build/e2e/deck-<tag>.log
#
# DECK_HOST (default deck@steamdeck.local) and DECK_DIR (default mr-test/MotoRacer, a release-layout
# folder: the tarball unpacked and started once with MOTORACER_SETUP). <keys> is MOTORACER_AUTOKEYS
# syntax. run prints the exit code (124: timed out, the game was still running), the log's FATAL /
# warp / renderer warnings and the end of stdout; the full log stays in DECK_DIR/logs/run-<tag>.log.
set -euo pipefail
cd "$(dirname "$0")/../.."
host=${DECK_HOST:-deck@steamdeck.local} dir=${DECK_DIR:-mr-test/MotoRacer}
ssh_() { ssh -o BatchMode=yes -o ConnectTimeout=8 "$host" "$@"; }
case ${1:-} in
push)
    name=${3:-$(basename "$2")}
    scp -q "$2" "$host:$dir/$name" && ssh_ "chmod +x ~/$dir/$name && ls -la ~/$dir/$name"
    ;;
run)
    name=$2 tag=$3 keys=$4 secs=${5:-120}
    shift $(( $# < 5 ? $# : 5 ))
    env_args=$(printf '%q ' MOTORACER_MUTE=1 "MOTORACER_AUTOKEYS=$keys" "$@")
    ssh_ "bash -s" <<EOF
export DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000
export XAUTHORITY=\$(ls /run/user/1000/xauth_* 2>/dev/null | head -1)
cd ~/$dir
env $env_args timeout $secs ./$name > logs/run-$tag.out 2>&1
code=\$?
cp logs/motoracer.log logs/run-$tag.log
echo "[$tag] exit code \$code"
grep -a "FATAL\|warp:\|not usable\|released with draws" logs/run-$tag.log | cut -c1-150 | sort | uniq -c | head -20
tail -3 logs/run-$tag.out
EOF
    ;;
log)
    mkdir -p build/e2e && scp -q "$host:$dir/logs/run-$2.log" "build/e2e/deck-$2.log" && echo "build/e2e/deck-$2.log"
    ;;
*)
    sed -n '2,13p' "$0"; exit 1
    ;;
esac
