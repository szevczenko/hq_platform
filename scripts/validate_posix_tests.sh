#!/bin/bash
#
# TASK-528 — Validate self-contained POSIX test executables
#
# Builds and runs the unit-test executables that do NOT require an external
# broker:
#   osal_tests, wifi_tests, mqtt_tests, mqtt_safety_tests, tb_tests
#
# For each binary it asserts:
#   * the process exits 0,
#   * Unity reports every registered test exactly once (no duplication),
#   * Unity's summary line reports the expected test count and zero failures,
#   * no custom result-tracking summary (e.g. "Total failed tests",
#     "tests_passed", success-rate) remains in the output.
#
# No source file is modified: this script only reads the build tree and the
# captured test output. It does not weaken any test.
#

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Build directory (defaults to the task-mandated `build` tree).
BUILD_DIR="${1:-$PROJECT_DIR/build}"
TESTS_DIR="$BUILD_DIR/tests"

# Expected Unity summary counts captured from the TASK-506..TASK-524 test
# registrations (RUN_TEST entries) in each aggregated test binary.
declare -A EXPECTED=(
  [osal_tests]=39
  [wifi_tests]=80
  [mqtt_tests]=6
  [mqtt_safety_tests]=4
  [tb_tests]=52
)

# Custom result-tracking markers that must NOT appear in validated output.
CUSTOM_SUMMARY_RE='Total failed tests|tests_passed|Total tests|Success rate|Failed:|Passed:|ALL TESTS PASSED|tests failed|tests passed'

pass=0
fail=0

note()  { printf '\n==> %s\n' "$*"; }
ok()    { printf '    PASS: %s\n' "$*"; pass=$((pass + 1)); }
bad()   { printf '    FAIL: %s\n' "$*" >&2; fail=$((fail + 1)); }

# ---- Configure and build (task-mandated commands) --------------------------
note "Configuring POSIX build in $BUILD_DIR"
cmake -B "$BUILD_DIR" \
  -DHQ_DEFCONFIG="$PROJECT_DIR/defconfig/posix.defconfig" \
  -DHQ_BUILD_TESTS=ON -DHQ_BUILD_EXAMPLES=ON
cmake --build "$BUILD_DIR"

# ---- Validate each self-contained binary -----------------------------------
for bin in osal_tests wifi_tests mqtt_tests mqtt_safety_tests tb_tests; do
  expected="${EXPECTED[$bin]:-}"
  exe="$TESTS_DIR/$bin"
  log="$BUILD_DIR/$bin.log"
  printf '\n==========================================\n'
  printf 'Validating: %s (expected %s tests)\n' "$bin" "$expected"
  printf '==========================================\n'

  # 1. Executable must exist.
  if [ ! -x "$exe" ]; then
    bad "$exe is missing or not executable"
    continue
  fi
  ok "executable present: $exe"

  # 2. Must exit 0.
  "$exe" > "$log" 2>&1
  rc=$?
  if [ "$rc" -eq 0 ]; then
    ok "process exited with code 0"
  else
    bad "process exited with code $rc (expected 0)"
  fi

  # 3. Unity summary must be present and report the expected count.
  summary="$(grep -E '^[0-9]+ Tests [0-9]+ Failures [0-9]+ Ignored[[:space:]]*$' "$log" | tail -n 1)"
  if [ -z "$summary" ]; then
    bad "no Unity summary line found"
    summary_result="no summary"
  else
    summary_result="$summary"
    reported="$(printf '%s\n' "$summary" | awk '{print $1}')"
    if [ "$reported" = "$expected" ]; then
      ok "Unity summary reports $reported tests (expected $expected)"
    else
      bad "Unity summary reports $reported tests (expected $expected): $summary"
    fi
    ok "Unity summary: $summary"
    if printf '%s\n' "$summary" | grep -qE '^[0-9]+ Tests 0 Failures 0 Ignored[[:space:]]*$'; then
      ok "zero failures reported"
    else
      bad "failures present in summary: $summary"
    fi
  fi

  # 4. Unity verdict must be OK.
  verdict="$(grep -E '^(OK|FAIL)$' "$log" | tail -n 1)"
  if [ "$verdict" = "OK" ]; then
    ok "Unity verdict: OK"
  else
    bad "Unity verdict is not OK: '${verdict}'"
  fi

  # 5. No custom result-tracking summary may remain in the output.
  if grep -qiE "$CUSTOM_SUMMARY_RE" "$log"; then
    bad "custom result-tracking summary found in output"
    grep -inE "$CUSTOM_SUMMARY_RE" "$log" | sed 's/^/        /'
  else
    ok "no custom summary output present"
  fi

  # 6. Every registered test must be reported and none duplicated:
  #    total PASS lines == expected and unique test names == total PASS lines.
  if [ "$summary_result" != "no summary" ]; then
    total_pass="$(grep -cE ':[A-Za-z0-9_]+:PASS$' "$log")"
    unique_pass="$(grep -oE '[A-Za-z0-9_]+:PASS$' "$log" | sort -u | wc -l)"
    if [ "$total_pass" -eq "$expected" ] && [ "$unique_pass" -eq "$expected" ]; then
      ok "reported $total_pass unique PASS lines (matches expected $expected)"
    else
      bad "expected $expected unique PASS lines, got total=$total_pass unique=$unique_pass"
    fi
    failed_lines="$(grep -cE ':[A-Za-z0-9_]+:FAIL$' "$log")"
    if [ "$failed_lines" -eq 0 ]; then
      ok "no per-test FAIL lines"
    else
      bad "$failed_lines per-test FAIL lines present"
    fi
  fi
done

printf '\n==========================================\n'
printf 'VALIDATION SUMMARY: %d passed, %d failed\n' "$pass" "$fail"
printf '==========================================\n'

[ "$fail" -eq 0 ] || exit 1
exit 0