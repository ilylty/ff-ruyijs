#!/usr/bin/env bash
set -u
PY="J:/workspace/ruyi_test/.venv/Scripts/python.exe"
ROOT="J:/workspace/ff-ruyijs-feature"
export RUYIJS_SHELL="$ROOT/dist/js.exe"
export PYTHONPATH="$ROOT/python${PYTHONPATH:+;$PYTHONPATH}"
cd "$ROOT" || exit 2

tests=(
  ci/feature_html_runner.py
  ci/smoke_test.py
  ci/layout_check.py
  ci/atob_check.py
  ci/fetch_abort_check.py
  ci/fetch_abort_transfer_check.py
  ci/load_timer_check.py
  ci/timer_scale_check.py
  ci/selection_check.py
  ci/cascade_collection_check.py
  ci/node_getter_check.py
  ci/timer_scaling_check.py
  ci/worker_blob_message_check.py
  ci/dom_id_index_check.py
  ci/dom_tag_ns_check.py
  ci/dom_id_fastpath_check.py
  ci/socks_proxy_check.py
  ci/layout_fingerprint_check.py
  ci/computed_style_check.py
  ci/hit_test_and_live_range_check.py
  ci/flex_nesting_check.py
  ci/net_log_check.py
  ci/http_timeout_check.py
  ci/network_smoke.py
  ci/proxy_smoke.py
  ci/hello_check.py
  ci/engine_check.py
)

pass=0
fail=0
blocked=0
: > "$ROOT/artifacts/full-test-summary.txt"

for test in "${tests[@]}"; do
  echo "===== $test =====" | tee -a "$ROOT/artifacts/full-test-summary.txt"
  if "$PY" "$test" >>"$ROOT/artifacts/full-test-summary.txt" 2>&1; then
    echo "PASS $test" | tee -a "$ROOT/artifacts/full-test-summary.txt"
    pass=$((pass + 1))
  else
    code=$?
    if [ "$code" -eq 2 ]; then
      echo "BLOCKED $test" | tee -a "$ROOT/artifacts/full-test-summary.txt"
      blocked=$((blocked + 1))
    else
      echo "FAIL $test" | tee -a "$ROOT/artifacts/full-test-summary.txt"
      fail=$((fail + 1))
    fi
  fi
done

echo "FULL TEST SUMMARY pass=$pass fail=$fail blocked=$blocked"
exit $((fail > 0 || blocked > 0))
