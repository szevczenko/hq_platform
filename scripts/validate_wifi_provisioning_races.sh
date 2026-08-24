#!/bin/bash
#
# TASK-143 — Wi-Fi HTTP provisioning race / stress verification matrix
#
# Automatically reproduces the pre-fix ordering bugs (TASK-140/141) and
# demonstrates deterministic behaviour after the lifecycle fixes.  It builds
# and runs:
#
#   1. The two formerly-flaky executables
#        wifi_http_provisioning_disconnect_tests
#        wifi_provisioning_fallback_flow_tests
#      under `ctest --repeat until-fail:200` (200 consecutive runs each) so a
#      pre-fix ordering race cannot hide behind machine speed.
#
#   2. The complete POSIX unit label suite, both sequentially and in parallel.
#      Every unit executable installs its own isolated littlefs fixture
#      (TASK-142), so parallel runs never contaminate each other's state.
#
#   3. An optional focused sanitizer pass over wifi_provisioning_race_tests:
#        - AddressSanitizer + UndefinedBehaviorSanitizer (default toolchain),
#        - ThreadSanitizer (clang -fsanitize=thread) where the installed
#          toolchain supports it.
#      A focused sanitizer run must report no use-after-free, data race,
#      invalid setjmp/longjmp, leaked thy thread / timer / listener / OSAL
#      object.
#
#   4. The documented broker integration validation
#      (scripts/validate_broker_integration_tests.sh) after the unit stress
#      pass, so Mongoose lifecycle regressions cannot hide behind the
#      unit-only build.
#
# The script never modifies repository sources.  It only configures/builds
# (including the gitignored build_asan / build_tsan trees), drives the test
# executables and captures output under build/logs/.  When the active
# toolchain cannot link a sanitizer build because the sanitizer runtimes were
# split out of the base compiler (on Fedora they live in the separate libasan /
# libubsan / libtsan package), the script fetches a user-local copy of the
# matching runtimes under build/sanitize_runtime/ (never system state) and
# points the focused builds at it.
#

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Build directory (defaults to the task-mandated `build` tree).
BUILD_DIR="${1:-$PROJECT_DIR/build}"
case "$BUILD_DIR" in
    /*) : ;;
    *) BUILD_DIR="$PWD/$BUILD_DIR" ;;
esac

TESTS_DIR="$BUILD_DIR/tests"
LOG_DIR="$BUILD_DIR/logs"
ASAN_DIR="$PROJECT_DIR/build_asan"
TSAN_DIR="$PROJECT_DIR/build_tsan"

mkdir -p "$LOG_DIR"

pass=0
fail=0
skipped=0

note()  { printf '\n==> %s\n' "$*"; }
ok()    { printf '    PASS: %s\n' "$*"; pass=$((pass + 1)); }
bad()   { printf '    FAIL: %s\n' "$*" >&2; fail=$((fail + 1)); }
skip()  { printf '    NOT RUN: %s\n' "$*"; skipped=$((skipped + 1)); }

# Regexes for sanitizer / lifecycle-leak symptoms the focused runs must not
# show.
SANITIZER_ISSUE_RE='AddressSanitizer|runtime error:|ThreadSanitizer|data race|LeakSanitizer|InvalidJump|invalid setjmp|leaked'

# Locate a Python interpreter that can run genconfig (kconfiglib) for fresh
# build trees.
HQ_CONFIG_PY=""
for cand in "$PROJECT_DIR/.kconfig-venv/bin/python" \
            "$PROJECT_DIR/.venv/bin/python"; do
    if [ -x "$cand" ] && "$cand" -c 'import kconfiglib' >/dev/null 2>&1; then
        HQ_CONFIG_PY="$cand"
        break
    fi
done
if [ -z "$HQ_CONFIG_PY" ]; then
    HQ_CONFIG_PY="$(command -v python3 || true)"
fi

# Common cmake arguments for every tree this script configures.
CMAKE_COMMON_ARGS=(-DHQ_DEFCONFIG="$PROJECT_DIR/defconfig/posix.defconfig")
CMAKE_COMMON_ARGS+=(-DHQ_BUILD_TESTS=ON -DHQ_BUILD_EXAMPLES=ON)
if [ -n "$HQ_CONFIG_PY" ]; then
    CMAKE_COMMON_ARGS+=(-DPYTHON="$HQ_CONFIG_PY")
fi

# ---------------------------------------------------------------------------
# Pre-flight: configure + build the task-mandated POSIX tree.
# ---------------------------------------------------------------------------
note "Configuring POSIX test build in $BUILD_DIR"
if ! cmake -B "$BUILD_DIR" "${CMAKE_COMMON_ARGS[@]}" \
        > "$LOG_DIR/cfg_143.log" 2>&1; then
        bad "POSIX test build configuration failed"
        tail -n 120 "$LOG_DIR/cfg_143.log" | sed 's/^/        /'
        exit 1
fi
if ! cmake --build "$BUILD_DIR" > "$LOG_DIR/build_143.log" 2>&1; then
        bad "POSIX test build failed"
        tail -n 120 "$LOG_DIR/build_143.log" | sed 's/^/        /'
        exit 1
fi

RACE_BIN="$TESTS_DIR/wifi_provisioning_race_tests"
if [ ! -x "$RACE_BIN" ]; then
    bad "build did not produce wifi_provisioning_race_tests"
    printf '\n== VALIDATION SUMMARY: %d passed, %d failed, %d skipped ==\n' \
        "$pass" "$fail" "$skipped"
    exit 1
fi
ok "build produced wifi_provisioning_race_tests"

# ---------------------------------------------------------------------------
# 1) Barrier-driven 200x stress of the two formerly-flaky executables.
# ---------------------------------------------------------------------------
note "Stress: disconnect + fallback flow tests, --repeat until-fail:200"
STRESS_RE='wifi_http_provisioning_disconnect_tests|wifi_provisioning_fallback_flow_tests'
stress_log="$LOG_DIR/stress_until_fail_200.log"
ctest --test-dir "$BUILD_DIR" -R "$STRESS_RE" --repeat until-fail:200 \
    --output-on-failure > "$stress_log" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then
    ok "both formerly-flaky executables completed 200 consecutive runs"
else
    bad "200x stress run failed (rc=$rc)"
    tail -n 120 "$stress_log" | sed 's/^/        /'
fi

# ---------------------------------------------------------------------------
# 2) Full unit label suite, sequential then parallel (isolated fixtures).
# ---------------------------------------------------------------------------
note "Unit suite: sequential"
seq_log="$LOG_DIR/unit_sequential.log"
ctest --test-dir "$BUILD_DIR" -L unit --output-on-failure > "$seq_log" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then
    ok "unit suite passed sequentially"
else
    bad "unit suite failed sequentially (rc=$rc)"
    tail -n 120 "$seq_log" | sed 's/^/        /'
fi

note "Unit suite: parallel (isolated fixtures)"
par_log="$LOG_DIR/unit_parallel.log"
ctest --test-dir "$BUILD_DIR" -j 4 -L unit --output-on-failure > "$par_log" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then
    ok "unit suite passed in parallel"
else
    bad "unit suite failed in parallel (rc=$rc)"
    tail -n 120 "$par_log" | sed 's/^/        /'
fi

# ---------------------------------------------------------------------------
# 3) Optional focused sanitizer builds for the race regression executable.
# ---------------------------------------------------------------------------
# probe_sanitize: determine whether the active compiler toolchain can link and
# (when a runtime dir is given) can load a tiny -fsanitize probe.  A split-out
# or absent sanitizer runtime fails the link, so this is the capability probe
# used to decide whether the sanitizer passes can run on this host.
probe_sanitize() {
    local kind="$1"
    local dir="$2"
    local flags="" src bin

    if [ "$kind" = "thread" ]; then
        flags="-fsanitize=thread"
    else
        flags="-fsanitize=address,undefined -fno-omit-frame-pointer"
    fi
    if [ -n "$dir" ]; then
        flags="$flags -L$dir"
    fi
    src="$(mktemp "$BUILD_DIR/sanitize_probe.XXXXXX.c")" || return 1
    bin="$src.bin"
    echo 'int main(void) { return 0; }' > "$src"
    # shellcheck disable=SC2086 -- the flag string is fixed and space-separated.
    if gcc $flags "$src" -o "$bin" > /dev/null 2>&1; then
        if [ -n "$dir" ] && ! LD_LIBRARY_PATH="$dir" "$bin" > /dev/null 2>&1; then
            rm -f "$src" "$bin"
            return 1
        fi
    else
        rm -f "$src"
        return 1
    fi
    rm -f "$src" "$bin"
    return 0
}

# obtain_sanitizer_runtime: fetch the distro's sanitizer runtime packages into
# a user-local dir under the build tree (Fedora ships these as the separate
# libasan / libubsan / libtsan packages) and add the unversioned aliases the
# compiler's linker looks up for `-lasan` / `-lubsan` / `-ltsan`.  Never
# touches system state.  Leaves SAN_RUNTIME_DIR set on success.
obtain_sanitizer_runtime() {
    local dl="$BUILD_DIR/sanitize_dl"
    local rt="$BUILD_DIR/sanitize_runtime"
    local rpm base f

    if ! command -v dnf >/dev/null 2>&1 || ! command -v rpm2cpio >/dev/null 2>&1 \
       || ! command -v cpio >/dev/null 2>&1; then
        return 1
    fi
    rm -rf "$dl" "$rt"
    mkdir -p "$dl"
    if dnf download --destdir "$dl" \
            libasan.x86_64 libubsan.x86_64 libtsan.x86_64 \
            > "$dl/download.log" 2>&1; then
        for rpm in "$dl"/libasan-*.rpm "$dl"/libubsan-*.rpm \
                   "$dl"/libtsan-*.rpm; do
            if [ -f "$rpm" ]; then
                ( cd "$dl" && rpm2cpio "$(basename "$rpm")" | cpio -idm --quiet ) > /dev/null 2>&1
            fi
        done
        mkdir -p "$rt"
        for d in "$dl/usr/lib64" "$dl/usr/lib"; do
            if [ -d "$d" ]; then
                cp -a "$d"/. "$rt"/
            fi
        done
        for base in libasan libubsan libtsan; do
            f="$(find "$rt" -maxdepth 1 -name "$base.so.*" \
                    \( -type f -o -type l \) 2>/dev/null | head -n 1)"
            if [ -n "$f" ]; then
                ln -sf "$(basename "$f")" "$rt/$base.so"
            fi
        done
        if [ -f "$rt/libasan.so" ] && [ -f "$rt/libubsan.so" ] \
           && [ -f "$rt/libtsan.so" ]; then
            SAN_RUNTIME_DIR="$rt"
            return 0
        fi
    fi
    return 2
}

# run_race_binary: execute a (possibly instrumented) race binary and assert it
# exits 0, reports no sanitizer/leak signature, and finishes with a Unity OK
# verdict.
run_race_binary() {
    local bin="$1"
    local log="$2"
    local rc

    if [ -n "$SAN_RUNTIME_DIR" ]; then
        LD_LIBRARY_PATH="$SAN_RUNTIME_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
            "$bin" > "$log" 2>&1
    else
        "$bin" > "$log" 2>&1
    fi
    rc=$?
    if [ "$rc" -ne 0 ]; then
        bad "$bin exited non-zero (rc=$rc)"
        tail -n 80 "$log" | sed 's/^/        /'
        return 1
    fi
    if grep -qE "$SANITIZER_ISSUE_RE" "$log"; then
        bad "sanitizer reported an issue for $bin"
        grep -nE "$SANITIZER_ISSUE_RE" "$log" | sed 's/^/        /'
        return 1
    fi
    if ! grep -qE '^OK[[:space:]]*$' "$log"; then
        bad "$bin did not report a Unity OK verdict"
        tail -n 40 "$log" | sed 's/^/        /'
        return 1
    fi
    ok "$(basename "$bin") reported clean under sanitizer"
    return 0
}

# The active toolchain's default linker must not be expected to link a
# sanitizer probe out of the box.  On Fedora the runtimes live in the separate
# libasan/libubsan/libtsan packages; when those are absent a native -fsanitize
# link fails.  Probe once, and if needed obtain a user-local runtime that the
# sanitizer builds are pointed at (the instrumented binaries then run with
# LD_LIBRARY_PATH set, see run_race_binary).
SAN_RUNTIME_DIR=""
SAN_CMAKE_ARGS=()
if probe_sanitize address ""; then
    ok "active toolchain links AddressSanitizer natively; no separate runtime needed"
elif obtain_sanitizer_runtime \
     && probe_sanitize address "$SAN_RUNTIME_DIR" \
     && probe_sanitize thread "$SAN_RUNTIME_DIR"; then
    ok "user-local sanitizer runtime prepared at $SAN_RUNTIME_DIR"
    SAN_CMAKE_ARGS+=(-DHQ_SANITIZE_RUNTIME_DIR="$SAN_RUNTIME_DIR")
else
    bad "no usable sanitizer runtime available (native probe and user-local runtime unavailable)"
fi

note "Focused AddressSanitizer + UndefinedBehaviorSanitizer build"
if cmake -B "$ASAN_DIR" \
    "${CMAKE_COMMON_ARGS[@]}" -DHQ_SANITIZE=address "${SAN_CMAKE_ARGS[@]}" \
    > "$LOG_DIR/cfg_asan.log" 2>&1 \
   && cmake --build "$ASAN_DIR" --target wifi_provisioning_race_tests \
    > "$LOG_DIR/build_asan.log" 2>&1; then
    ASAN_BIN="$ASAN_DIR/tests/wifi_provisioning_race_tests"
    if [ -x "$ASAN_BIN" ]; then
        ok "ASan+UBSan race binary built"
        run_race_binary "$ASAN_BIN" "$LOG_DIR/race_asan.log"
    else
        fail "ASan/UBSan build did not produce the race executable"
    fi
else
    skip "AddressSanitizer/UndefinedBehaviorSanitizer build unavailable"
fi

note "Focused ThreadSanitizer build (where supported)"
# clang-only -fsanitize=thread.  If the toolchain rejects it the build fails
# and the capability is reported as not supported rather than a corrupted run.
if cmake -B "$TSAN_DIR" \
    "${CMAKE_COMMON_ARGS[@]}" -DHQ_SANITIZE=thread "${SAN_CMAKE_ARGS[@]}" \
    > "$LOG_DIR/cfg_tsan.log" 2>&1 \
   && cmake --build "$TSAN_DIR" --target wifi_provisioning_race_tests \
    > "$LOG_DIR/build_tsan.log" 2>&1; then
    TSAN_BIN="$TSAN_DIR/tests/wifi_provisioning_race_tests"
    if [ -x "$TSAN_BIN" ]; then
        ok "ThreadSanitizer race binary built"
        run_race_binary "$TSAN_BIN" "$LOG_DIR/race_tsan.log"
    else
        skip "ThreadSanitizer build succeeded but produced no executable"
    fi
else
    skip "ThreadSanitizer (-fsanitize=thread) not supported by this toolchain"
fi

# ---------------------------------------------------------------------------
# 4) Broker integration validation after the unit stress pass.
# ---------------------------------------------------------------------------
note "Broker integration validation (Mongoose lifecycle regression detection)"
brok_log="$LOG_DIR/broker_integration.log"
bash "$SCRIPT_DIR/validate_broker_integration_tests.sh" "$BUILD_DIR" \
    > "$brok_log" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then
    ok "broker integration validation passed after the stress matrix"
else
    bad "broker integration validation failed (rc=$rc)"
    tail -n 120 "$brok_log" | sed 's/^/        /'
fi

# ---------------------------------------------------------------------------
# 5) Documented pre-fix signatures / post-fix commands.
# ---------------------------------------------------------------------------
printf '\n========================================================\n'
cat <<'EOF'
=== Pre-fix failure signatures (TASK-140/141, reproduced by the barrier tests) ===
  1. delayed HAL init        GOT_IP injected before the WiFi worker installs the
                              HAL callback => readiness never entered; the
                              station could not connect (flaky GOT_IP-before-
                              ready race), or an early event was mishandled.
  2. one-shot Mongoose     In an older invocation scheme a timed-out caller
     invocation token       could consume/steal another caller's completion
                              token, so a concurrent run lost a completion or
                              leaked a process object.
  3. grace expiry racing     A grace timer expiry raced a station disconnect and
     disconnect+deinit        controller deinit, allowing a stale expiry from a
                              cancelled session to retire the next session (or a
                              use-after-free on the torn-down controller).
  4. teardown while          Stopping provisioning and deinitializing Mongoose
      listener pending        underneath a parked request caused a use-after-free
                              / leaked listener because the operation was not
                              confirmed closed before the shared process was gone.

=== Post-fix verification commands (TASK-143) ===
  ctest --test-dir build -R 'wifi_http_provisioning_disconnect_tests|wifi_provisioning_fallback_flow_tests' --repeat until-fail:200
  ctest --test-dir build -L unit --output-on-failure
  ctest --test-dir build -j 4 -L unit --output-on-failure
  cmake -B build_asan -DHQ_DEFCONFIG=defconfig/posix.defconfig -DHQ_BUILD_TESTS=ON -DHQ_SANITIZE=address
  build_asan/tests/wifi_provisioning_race_tests
  cmake -B build_tsan -DHQ_DEFCONFIG=defconfig/posix.defconfig -DHQ_BUILD_TESTS=ON -DHQ_SANITIZE=thread   # clang only
  bash scripts/validate_broker_integration_tests.sh build
EOF
printf '========================================================\n'

printf '\n==========================================\n'
printf 'VALIDATION SUMMARY: %d passed, %d failed, %d not-run/skipped\n' \
    "$pass" "$fail" "$skipped"
printf '==========================================\n'

[ "$fail" -eq 0 ] || exit 1
exit 0