#!/bin/bash
#
# TASK-529 — Validate broker-dependent integration test executables
#
# Builds the POSIX test tree and runs the three broker integration tests with
# their documented harnesses:
#
#   * mqtt_functional_tests         local TLS broker on 127.0.0.1:8883
#                                   (certificates from the repo `cert/` dir)
#   * tb_reconnect_integration_tests  local Mosquitto on 127.0.0.1:1884 plus
#                                   broker stop/start commands via env vars
#   * tb_tls_integration_tests      trusted, unknown-CA and hostname-mismatch
#                                   TLS endpoints on localhost:8885 / 8886
#                                   (certificates in cert/tls_it/, which is
#                                    generated and gitignored)
#
# For each binary whose environment is available it asserts:
#   * the process exits 0,
#   * Unity reports the expected number of tests and zero failures,
#   * Unity's verdict is OK,
#   * Unity identifies the reported test cases by name (PASS/FAIL lines),
#   * no manual/custom result-tracking summary remains in the output.
#
# It also asserts that setup failures surface through the Unity FAIL API rather
# than a hidden "ALL TESTS PASSED" counter: a deliberately incomplete run must
# produce Unity FAIL lines naming each aborted case and a non-zero exit code.
#
# Infrastructure that cannot be acquired (for example no Docker daemon, or a
# broker image that is not available) is reported as NOT RUN and is *never*
# counted as a passing test.
#
# No source file is modified: this script only reads the build tree, drives the
# gitignored certificate directory under cert/tls_it and the broker container
# lifecycle, and captures the test output.
#

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Build directory (defaults to the task-mandated `build` tree). Resolved to an
# absolute path so host paths handed to Docker bind mounts are always absolute
# (Docker rejects relative host paths for -v volumes).
BUILD_DIR="${1:-$PROJECT_DIR/build}"
case "$BUILD_DIR" in
    /*) : ;;
    *) BUILD_DIR="$PWD/$BUILD_DIR" ;;
esac
TESTS_DIR="$BUILD_DIR/tests"

# Docker image used for the local Mosquitto brokers.
BROKER_IMAGE="${TASK529_BROKER_IMAGE:-eclipse-mosquitto:2}"

pass=0
fail=0
skipped=0

note()  { printf '\n==> %s\n' "$*"; }
ok()    { printf '    PASS: %s\n' "$*"; pass=$((pass + 1)); }
bad()   { printf '    FAIL: %s\n' "$*" >&2; fail=$((fail + 1)); }
skip()  { printf '    NOT RUN: %s\n' "$*"; skipped=$((skipped + 1)); }

CUSTOM_SUMMARY_RE='Total failed tests|tests_passed|Total tests|Success rate|Failed:|Passed:|ALL TESTS PASSED|tests failed|tests passed'

# -----------------------------------------------------------------------------
# Validate a captured run against the expected Unity test count.
# -----------------------------------------------------------------------------
validate_unity_result() {
    local name="$1"; shift
    local exe="$1"; shift
    local rc="$1"; shift
    local log="$1"; shift
    local expected="$1"
    local summary reported verdict total_pass unique_pass failed_pass error

    # 1. The process must exit 0.
    if [ "$rc" -eq 0 ]; then
        ok "process exited with code 0"
    else
        bad "process exited with code $rc (expected 0)"
        error=yes
    fi

    # 2. Unity summary present, expected count, zero failures.
    summary="$(grep -E '^[0-9]+ Tests [0-9]+ Failures [0-9]+ Ignored[[:space:]]*$' "$log" | tail -n 1)"
    if [ -z "$summary" ]; then
        bad "no Unity summary line found"
    else
        reported="$(printf '%s\n' "$summary" | awk '{print $1}')"
        if [ "$reported" = "$expected" ]; then
            ok "Unity summary reports $reported tests (expected $expected)"
        else
            bad "Unity summary reports $reported tests (expected $expected)"
            error=yes
        fi
        if printf '%s\n' "$summary" | grep -qE '^[0-9]+ Tests 0 Failures 0 Ignored[[:space:]]*$'; then
            ok "zero failures reported"
        else
            bad "failures present in summary: $summary"
            error=yes
        fi
    fi

    # 3. Unity verdict must be OK.
    verdict="$(grep -E '^(OK|FAIL)$' "$log" | tail -n 1)"
    if [ "$verdict" = "OK" ]; then
        ok "Unity verdict: OK"
    else
        bad "Unity verdict is not OK: '${verdict}'"
        error=yes
    fi

    # 4. No manual/custom result-tracking summary may remain.
    if grep -qiE "$CUSTOM_SUMMARY_RE" "$log"; then
        bad "manual/custom result-tracking summary found"
        error=yes
    else
        ok "no manual/custom result-tracking summary present"
    fi

    # 5. All expected test cases reported by name and none duplicated/failed.
    failed_pass="$(grep -cE ':[A-Za-z0-9_]+:FAIL$' "$log")"
    total_pass="$(grep -cE ':[A-Za-z0-9_]+:PASS$' "$log")"
    unique_pass="$(grep -oE '[A-Za-z0-9_]+:PASS$' "$log" | sort -u | wc -l)"
    if [ "$total_pass" -eq "$expected" ] && [ "$unique_pass" -eq "$expected" ]; then
        ok "reported $total_pass unique PASS lines (expected $expected)"
    else
        bad "expected $expected unique PASS lines, got total=$total_pass unique=$unique_pass"
        error=yes
    fi
    if [ "$failed_pass" -eq 0 ]; then
        ok "no per-test FAIL lines"
    else
        bad "$failed_pass per-test FAIL lines present"
        error=yes
    fi
}

# -----------------------------------------------------------------------------
# Pre-flight: configure and build the task-mandated tree.
# -----------------------------------------------------------------------------
note "Configuring POSIX test build in $BUILD_DIR"
cmake -B "$BUILD_DIR" \
  -DHQ_DEFCONFIG="$PROJECT_DIR/defconfig/posix.defconfig" \
  -DHQ_BUILD_TESTS=ON -DHQ_BUILD_EXAMPLES=ON \
  > "$BUILD_DIR/cfg_529.log" 2>&1
cmake --build "$BUILD_DIR" > "$BUILD_DIR/build_529.log" 2>&1

FUNC_BIN="$TESTS_DIR/mqtt_functional_tests"
RECON_BIN="$TESTS_DIR/tb_reconnect_integration_tests"
TLS_BIN="$TESTS_DIR/tb_tls_integration_tests"

if [ ! -x "$FUNC_BIN" ] || [ ! -x "$RECON_BIN" ] || [ ! -x "$TLS_BIN" ]; then
    bad "build did not produce all three integration executables"
    printf '\n== VALIDATION SUMMARY: %d passed, %d failed, %d skipped ==\n' \
        "$pass" "$fail" "$skipped"
    exit 1
fi
ok "build produced all three integration executables"

# -----------------------------------------------------------------------------
# Infrastructure availability flags.
# -----------------------------------------------------------------------------
docker_ok=1
if ! command -v docker >/dev/null 2>&1 || ! docker ps >/dev/null 2>&1; then
    docker_ok=0
fi
openssl_ok=1
if ! command -v openssl >/dev/null 2>&1; then
    openssl_ok=0
fi

# -----------------------------------------------------------------------------
# 1) mqtt_functional_tests — TLS broker on 127.0.0.1:8883.
# -----------------------------------------------------------------------------
note "mqtt_functional_tests — TLS broker on 127.0.0.1:8883"
FUNC_BROKER="hq_t529_mqtt_func"
if [ ! -x "$FUNC_BIN" ]; then
    skip "executable missing: $FUNC_BIN"
elif [ "$docker_ok" -ne 1 ]; then
    skip "Docker unavailable; TLS broker on 8883 not started"
elif [ ! -f "$PROJECT_DIR/cert/server.crt" ] || [ ! -f "$PROJECT_DIR/cert/server.key" ]; then
    skip "repo certificates under cert/ missing (required to serve TLS on 8883)"
else
    WORKDIR="$BUILD_DIR/t529_func"
    mkdir -p "$WORKDIR"
    cat > "$WORKDIR/mosquitto.conf" <<EOF
listener 8883 0.0.0.0
allow_anonymous true
cafile /mosq/certs/ca.crt
certfile /mosq/certs/server.crt
keyfile /mosq/certs/server.key
require_certificate false
EOF
    docker rm -f "$FUNC_BROKER" >/dev/null 2>&1
    if ! docker run -d --name "$FUNC_BROKER" \
        -p 8883:8883 \
        -v "$PROJECT_DIR/cert:/mosq/certs:ro" \
        -v "$WORKDIR/mosquitto.conf:/mosquitto/config/mosquitto.conf:ro" \
        "$BROKER_IMAGE" >/dev/null 2>&1; then
        skip "TLS broker container failed to start; mqtt_functional_tests not run"
    else
        sleep 2
        if ! docker ps --format '{{.Names}}' | grep -qx "$FUNC_BROKER"; then
            skip "TLS broker did not stay up; mqtt_functional_tests not run"
        else
            log="$BUILD_DIR/mqtt.functional.log"
            (cd "$PROJECT_DIR" && timeout 90 "$FUNC_BIN") > "$log" 2>&1
            rc=$?
            validate_unity_result "mqtt_functional_tests" "$FUNC_BIN" "$rc" "$log" 1
        fi
        docker rm -f "$FUNC_BROKER" >/dev/null 2>&1
    fi
fi

# -----------------------------------------------------------------------------
# 2) tb_reconnect_integration_tests — documented harness (broker + stop/start).
# -----------------------------------------------------------------------------
note "tb_reconnect_integration_tests — Mosquitto on 127.0.0.1:1884"
if [ ! -x "$RECON_BIN" ]; then
    skip "executable missing: $RECON_BIN"
elif [ "$docker_ok" -ne 1 ]; then
    skip "Docker unavailable; broker on 1884 not started"
else
    log="$BUILD_DIR/tb.reconnect.log"
    TB_IT_BUILD_DIR="$BUILD_DIR" bash \
        "$SCRIPT_DIR/thingboard_reconnect_integration_test.sh" > "$log" 2>&1
    rc=$?
    validate_unity_result "tb_reconnect_integration_tests" "$RECON_BIN" "$rc" "$log" 2
fi

# -----------------------------------------------------------------------------
# 3) tb_tls_integration_tests — documented harness (TLS endpoints 8885 / 8886).
# -----------------------------------------------------------------------------
note "tb_tls_integration_tests — TLS endpoints on localhost:8885 / 8886"
if [ ! -x "$TLS_BIN" ]; then
    skip "executable missing: $TLS_BIN"
elif [ "$docker_ok" -ne 1 ]; then
    skip "Docker unavailable; TLS endpoints not started"
elif [ "$openssl_ok" -ne 1 ]; then
    skip "openssl unavailable; TLS certificates cannot be generated"
else
    log="$BUILD_DIR/tb.tls.log"
    TB_TLS_IT_BUILD_DIR="$BUILD_DIR" bash \
        "$SCRIPT_DIR/thingboard_tls_integration_test.sh" > "$log" 2>&1
    rc=$?
    validate_unity_result "tb_tls_integration_tests" "$TLS_BIN" "$rc" "$log" 3
fi

# -----------------------------------------------------------------------------
# 4) Setup failures must be reported through Unity FAIL, not hidden counters.
# -----------------------------------------------------------------------------
note "setup-failure reporting (deliberate missing prerequisite must be a Unity FAIL)"
if [ ! -x "$TLS_BIN" ]; then
    skip "executable missing; setup-failure reporting not verified"
else
    nlog="$BUILD_DIR/setup.neg.log"
    (cd "$PROJECT_DIR" && timeout 20 "$TLS_BIN") > "$nlog" 2>&1
    nrc=$?
    if [ "$nrc" -ne 0 ]; then
        ok "broken-prerequisite run exits non-zero"
    else
        bad "broken-prerequisite run exited 0 (setup failure was not surfaced)"
    fi
    fail_lines="$(grep -cE ':[A-Za-z0-9_]+:FAIL:' "$nlog")"
    if [ "$fail_lines" -gt 0 ]; then
        ok "setup failure reported by Unity ($fail_lines FAIL lines)"
    else
        bad "no Unity FAIL line in broken-prerequisite output"
    fi
    if grep -qiE '^OK[[:space:]]*$' "$nlog"; then
        bad "negative run reported Unity OK despite broken prerequisites"
    else
        ok "negative run did not report Unity OK"
    fi
    if grep -qiE 'ALL TESTS PASSED' "$nlog"; then
        bad "negative run contains hidden 'ALL TESTS PASSED' counter"
    else
        ok "negative run contains no hidden pass counter"
    fi
fi

printf '\n==========================================\n'
printf 'VALIDATION SUMMARY: %d passed, %d failed, %d not-run (unavailable)\n' \
    "$pass" "$fail" "$skipped"
printf '==========================================\n'

[ "$fail" -eq 0 ] || exit 1
exit 0