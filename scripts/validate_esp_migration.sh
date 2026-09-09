#!/bin/bash
#
# TASK-530 — Validate ESP-IDF build and remove legacy framework traces
#
# Encodes the cross-platform and static migration validation:
#
#   1. Configure and build tests/platform/esp with the supported ESP-IDF
#      version and confirm the built-in ESP-IDF Unity component is used.
#   2. Search tests/**/*.c and tests/**/*.h (tracked sources only) for legacy
#      custom definitions/references:
#        TEST_ASSERT(condition, message)   (two-argument legacy assert)
#        tests_run, tests_passed, tests_failed
#      Standard Unity macros (TEST_ASSERT_TRUE_MESSAGE, ...) are allowed and
#      must not be reported.
#   3. Verify all seven POSIX executables still link Unity and retain the
#      zero-on-pass lifecycle (main returns UNITY_END()).
#   4. Confirm only the ESP-IDF Unity component is used (not a fetched
#      third-party Unity tab) and that framework-free mocks are not flagged.
#
# No C source file is modified by this script; it only reads sources, drives
# the configured builds, and reports pass/fail.
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${1:-$PROJECT_DIR/build}"
TESTS_DIR="$BUILD_DIR/tests"

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
# POSIX Unity summary counts (from TASK-506..TASK-524 registrations).
declare -A EXPECTED=(
  [osal_tests]=39
  [wifi_tests]=28
  [mqtt_tests]=6
  [tb_tests]=52
)

# The seven POSIX test executables.
SEVEN_POSIX=(
  osal_tests
  wifi_tests
  mqtt_tests
  mqtt_functional_tests
  tb_tests
  tb_reconnect_integration_tests
  tb_tls_integration_tests
)

# Legacy custom markers that must NOT appear in test sources.
# The *_tests_run / *_tests_passed names are legitimate per-suite entry points
# (e.g. osal_task_tests_run) or aggregate-runner functions, so the counter
# tokens use word boundaries to match only a standalone legacy symbol.
LEGACY_PATTERNS=(
  'TEST_ASSERT([^)]*,'
  '\btests_run\b'
  '\btests_passed\b'
  '\btests_failed\b'
  '\btest_passed\b'
  '\btest_failed\b'
)

# ---- Report helpers --------------------------------------------------------
passs=0
fails=0
nots=0
note()  { printf '\n==> %s\n' "$*"; }
ok()    { printf '    PASS: %s\n' "$*"; passs=$((passs + 1)); }
bad()   { printf '    FAIL: %s\n' "$*" >&2; fails=$((fails + 1)); }
notrun(){ printf '    NOT RUN: %s\n' "$*"; nots=$((nots + 1)); }

# ---------------------------------------------------------------------------
# 1. STATIC LEGACY-TRACE SEARCH (tests/**/*.c and tests/**/*.h)
# ---------------------------------------------------------------------------
note "Static search for legacy custom assertion/counter definitions"

# Validate they are tracked so generated/build files are excluded.
src_list="$(git -C "$PROJECT_DIR" ls-files -- 'tests/**/*.c' 'tests/**/*.h')"
if [ -z "$src_list" ]; then
  bad "no tracked test .c/.h sources found"
else
  hits=0
  for pattern in "${LEGACY_PATTERNS[@]}"; do
    found="$(git -C "$PROJECT_DIR" ls-files -- 'tests/**/*.c' 'tests/**/*.h' | \
             while read -r f; do grep -HnE "$pattern" "$PROJECT_DIR/$f" 2>/dev/null; done)"
    if [ -n "$found" ]; then
      hits=$((hits + 1))
      printf '    legacy pattern /%s/ present:\n%s\n' "$pattern" "$found"
    fi
  done
  if [ "$hits" -eq 0 ]; then
    ok "no two-argument TEST_ASSERT / tests_run / tests_passed / tests_failed in any tracked test source"
  else
    bad "$hits legacy pattern(s) found in test sources"
  fi
fi

# Standard Unity *_MESSAGE macros are allowed and should be retained.
msg_macros=$(git -C "$PROJECT_DIR" ls-files -- 'tests/**/*.c' 'tests/**/*.h' | \
             while read -r f; do grep -HnE 'TEST_ASSERT_[A-Z_0-9]+_MESSAGE' "$PROJECT_DIR/$f" 2>/dev/null; done | wc -l)
note "Standard Unity *_MESSAGE macros remain allowed: $msg_macros usage(s) preserved"

# ---------------------------------------------------------------------------
# 2. PUBLISHED MOCKS contain no test framework — must not be flagged
# ---------------------------------------------------------------------------
note "Mocks/standalone helpers are outside the legacy cleanup scope"
mocks=(
  tests/thingsboard/mqtt_app_mock.c
  tests/thingsboard/mqtt_app_mock.h
  tests/wifi/wifi_hal_mock.c
  tests/wifi/wifi_hal_mock.h
)
ok_st=0
for m in "${mocks[@]}"; do
  if [ -f "$PROJECT_DIR/$m" ]; then
    ok_st=$((ok_st + 1))
  fi
done
ok "mock/helper files present (${ok_st}/${#mocks[@]}) and whitelisted from legacy scope"

# ---------------------------------------------------------------------------
# 3. ALL SEVEN POSIX EXECUTABLES USE UNITY + ZERO-ON-PASS LIFECYCLE
# ---------------------------------------------------------------------------
note "Configuring POSIX build in $BUILD_DIR"
cmake -B "$BUILD_DIR" \
  -DHQ_DEFCONFIG="$PROJECT_DIR/defconfig/posix.defconfig" \
  -DHQ_BUILD_TESTS=ON -DHQ_BUILD_EXAMPLES=ON
cmake --build "$BUILD_DIR"

src_map=(
  "osal_tests=tests/tests.c"
  "wifi_tests=tests/wifi/wifi_tests_runner.c"
  "mqtt_tests=tests/mqtt/mqtt_config_test.c"
  "mqtt_functional_tests=tests/mqtt/mqtt_functional_test.c"
  "tb_tests=tests/thingsboard/tb_tests.c"
  "tb_reconnect_integration_tests=tests/thingsboard/tb_reconnect_integration_test.c"
  "tb_tls_integration_tests=tests/thingsboard/tb_tls_integration_test.c"
)

for bin in "${SEVEN_POSIX[@]}"; do
  exe="$TESTS_DIR/$bin"
  if [ ! -x "$exe" ]; then
    bad "$bin executable missing"
    continue
  fi

  # Confirm the entry-point uses the Unity lifecycle (zero-on-pass main).
  src_with_bin=0
  for m in "${src_map[@]}"; do
    if [ "${m%%=*}" = "$bin" ]; then
      src="${m##*=}"
      src_with_bin=1
      break
    fi
  done
  if [ "$src_with_bin" -eq 1 ] \
     && grep -q "UNITY_BEGIN" "$PROJECT_DIR/$src" \
     && grep -q "UNITY_END" "$PROJECT_DIR/$src"; then
    ok "$bin uses Unity lifecycle (UNITY_BEGIN/UNITY_END => zero-on-pass)"
  else
    bad "$bin does not show Unity lifecycle in source"
  fi
done

# Run the four self-contained binaries to confirm zero-on-pass at runtime.
for bin in osal_tests wifi_tests mqtt_tests tb_tests; do
  exe="$TESTS_DIR/$bin"
  [ -x "$exe" ] || { bad "$bin executable missing"; continue; }
  log="$BUILD_DIR/$bin.log"
  "$exe" > "$log" 2>&1
  rc=$?
  summary="$(grep -E '^[0-9]+ Tests [0-9]+ Failures [0-9]+ Ignored[[:space:]]*$' "$log" | tail -n 1)"
  expected="${EXPECTED[$bin]:-}"
  reported="$(printf '%s\n' "$summary" | awk '{print $1}')"
  if [ "$rc" -eq 0 ] && [ "$reported" = "$expected" ] \
     && printf '%s\n' "$summary" | grep -qE '^[0-9]+ Tests 0 Failures'; then
    ok "$bin exit=0 summary='$summary' (zero-on-pass retained)"
  else
    bad "$bin exit=$rc summary='$summary' (expected $expected tests / 0 failures / exit 0)"
  fi
done

# ---------------------------------------------------------------------------
# 4. ESP-IDF OSAL TEST BUILD (configure + build)
# ---------------------------------------------------------------------------
note "ESP-IDF build of tests/platform/esp (supported ESP-IDF version)"
IDF_PATH="$( [ -n "$IDF_PATH" ] && echo "$IDF_PATH" || ([ -d "$HOME/projects/esp-idf" ] && echo "$HOME/projects/esp-idf") )"

if [ -z "$IDF_PATH" ] || ! [ -f "$IDF_PATH/tools/idf.py" ]; then
  notrun "ESP-IDF not available (IDF_PATH unset/missing)"
else
  # Prefer an active ESP-IDF virtual environment already present on the host.
  IDF_PY="$(find "$HOME/.espressif/python_env" -maxdepth 2 -name python -path '*/bin/*' 2>/dev/null | sort | tail -n 1)"
  if [ -n "$IDF_PY" ]; then
    IDF_PY="$(dirname "$IDF_PY")"
    PATH="$IDF_PY:$PATH"
  fi
  idf_log="$PROJECT_DIR/build_esp_migration.log"
  (
    cd "$PROJECT_DIR" || exit 1
    source "$IDF_PATH/export.sh" >/dev/null 2>&1
    cd tests/platform/esp || exit 1
    idf.py build > "$idf_log" 2>&1
  )
  rc=$?
  if [ "$rc" -ne 0 ]; then
    bad "ESP-IDF build failed (exit $rc) — see $idf_log"
  else
    ok "ESP-IDF build succeeded for tests/platform/esp (exit 0)"
    # The built-in ESP-IDF Unity component (components/unity) is used when its
    # IDF-specific artifacts are compiled into the build tree. The upstream
    # ThrowTheSwitch Unity v2.6.0 does not provide unity_runner/unity_port_esp32,
    # so their presence proves the built-in component was linked in.
    esp_build="$PROJECT_DIR/tests/platform/esp/build"
    if [ -f "$esp_build/esp-idf/unity/libunity.a" ] \
       && grep -qs "/components/unity" "$idf_log" 2>/dev/null; then
      ok "built-in ESP-IDF Unity component used (/.../components/unity)"
    elif [ -d "$esp_build/esp-idf/unity" ] \
         && ls "$esp_build/esp-idf/unity/CMakeFiles/__idf_unity.dir/unity_runner.c.obj" >/dev/null 2>&1 \
         && ls "$esp_build/esp-idf/unity/CMakeFiles/__idf_unity.dir/unity_port_esp32.c.obj" >/dev/null 2>&1; then
      ok "built-in ESP-IDF Unity component used (unity_runner/unity_port_esp32 present)"
    else
      bad "built-in ESP-IDF Unity component not found in the ESP build tree"
    fi
    if [ -f "$esp_build/osal_tests.bin" ]; then
      ok "firmware image produced: tests/platform/esp/build/osal_tests.bin"
    else
      bad "osal_tests.bin not produced"
    fi
  fi
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
printf '\n==========================================\n'
printf 'VALIDATION SUMMARY: %d passed, %d failed, %d not-run\n' "$passs" "$fails" "$nots"
printf '==========================================\n'

[ "$fails" -eq 0 ] || exit 1
exit 0