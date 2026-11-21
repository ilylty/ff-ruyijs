#!/usr/bin/env bash
# Build the Firefox parts ruyijs links, one directory at a time so a single
# failing directory is named instead of aborting the whole batch.
set -uo pipefail
export MOZCONFIG='J:\workspace\ff-ruyijs\ci\firefox\windows\mozconfig.browser'
export MOZBUILD_STATE_PATH='J:\mozbuild'
export HOME='/c/Users/jin'
export USERPROFILE='C:\Users\jin'
export MACH_NO_TERMINAL_FOOTER=1
PY=/c/mozilla-build/python3/python3.exe
LOG=/j/workspace/ff-ruyijs/bp1.log
cd /j/workspace/ff-ruyijs/firefox
mach() { "$PY" -u ./mach --no-interactive "$@" </dev/null; }

fail=0
while read -r dir target rest; do
  [ -z "${dir:-}" ] && continue
  if [ -z "${target:-}" ]; then
    printf 'DIR  %-45s ' "$dir"
    if mach build --allow-subdirectory-build "$dir" > "$LOG" 2>&1; then
      echo ok
    else
      echo FAIL
      tail -12 "$LOG"
      fail=1
    fi
  fi
done < /j/workspace/ff-ruyijs/obj/browser-steps.txt
echo "PASS1_DONE fail=$fail"
[ "$fail" -eq 0 ] || exit 1

while read -r dir target rest; do
  [ -z "${dir:-}" ] && continue
  if [ -n "${target:-}" ]; then
    printf 'LINK %-45s %s\n' "$dir" "$target"
    mach build --directory "$dir" "$target" || { echo "FAIL $dir $target"; exit 1; }
  fi
done < /j/workspace/ff-ruyijs/obj/browser-steps.txt
echo "STEP_BUILD_DONE"

"$PY" /j/workspace/ff-ruyijs/ci/firefox/collect.py \
  /j/workspace/ff-ruyijs/obj/browser /j/workspace/ff-ruyijs/obj/firefox-parts
echo "COLLECT_DONE"
