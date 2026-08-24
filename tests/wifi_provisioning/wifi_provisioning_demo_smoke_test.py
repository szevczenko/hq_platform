#!/usr/bin/env python3
"""
TASK-130 - POSIX provisioning demo smoke test.

Launches the built `wifi_provisioning_demo` (simulated Wi-Fi) and verifies its
Definition of Done end-to-end:

  1. the provisioning portal opens on http://127.0.0.1:8080/;
  2. DNS (udp://127.0.0.1:10053) and all HTTP workflows operate against the
     simulated Wi-Fi environment (status, scan, scan results, connect with
     credentials, disconnect);
  3. a clean shutdown (SIGTERM, reverse ownership order) is performed;
  4. shutdown releases both listener ports (8080 TCP, 10053 UDP).

The demo binary path is taken from argv[1]. Only the Python standard library
is used so the test runs on any POSIX host with python3.

Usage:
    python3 wifi_provisioning_demo_smoke_test.py /path/to/wifi_provisioning_demo
"""

import http.client
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

HTTP_HOST = "127.0.0.1"
HTTP_PORT = 8080
DNS_HOST = "127.0.0.1"
DNS_PORT = 10053

PORTAL_TITLE = "HQ Wi-Fi Provisioning Portal"
DNS_NAME = "provision.local"
DNS_ANSWER = "10.10.0.1"

STARTUP_TIMEOUT_S = 25.0
POLL_TIMEOUT_S = 15.0
SHUTDOWN_TIMEOUT_S = 15.0


def fail(msg):
    print("FAIL: " + msg, file=sys.stderr)
    sys.exit(1)


def log_tail(log_path, limit=4000):
    try:
        with open(log_path, "rb") as fh:
            fh.seek(0, os.SEEK_END)
            size = fh.tell()
            fh.seek(max(0, size - limit))
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return "<no demo log>"


def http_request(method, path, body=None, timeout=5):
    conn = http.client.HTTPConnection(HTTP_HOST, HTTP_PORT, timeout=timeout)
    try:
        headers = {}
        payload = None
        if body is not None:
            headers["Content-Type"] = "application/json"
            payload = json.dumps(body).encode("utf-8")
        conn.request(method, path, body=payload, headers=headers)
        resp = conn.getresponse()
        status = resp.status
        data = resp.read()
    finally:
        conn.close()
    return status, data


def wait_for(pred, timeout, desc, proc, log_path):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            break
        try:
            if pred():
                return
        except Exception as exc:  # noqa: BLE001 - reported on timeout
            pass
        time.sleep(0.25)
    tail = log_tail(log_path)
    fail("timed out waiting for %s\n--- demo log tail ---\n%s" % (desc, tail))


def wait_status_state(state, proc, log_path):
    def pred():
        status, body = http_request("GET", "/api/v1/wifi/status")
        return status == 200 and json.loads(body.decode("utf-8")).get("state") == state

    wait_for(pred, POLL_TIMEOUT_S, "Wi-Fi state %r" % state, proc, log_path)


def dns_query_a(name, host, port, timeout=3):
    """Send a single-question A query and return the decoded A record."""
    header = struct.pack(">HHHHHH", 0x1234, 0x0100, 1, 0, 0, 0)
    qname = b"".join(bytes([len(part)]) + part.encode("ascii")
                     for part in name.split(".")) + b"\x00"
    query = header + qname + struct.pack(">HH", 1, 1)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.sendto(query, (host, port))
        data, _ = sock.recvfrom(512)
    finally:
        sock.close()
    return parse_dns_a(data)


def parse_dns_a(data):
    if len(data) < 12:
        return None
    flags = struct.unpack(">H", data[2:4])[0]
    ancount = struct.unpack(">H", data[6:8])[0]
    if not (flags & 0x8000) or ancount < 1:
        return None
    rdata = data[-4:]
    if len(rdata) != 4:
        return None
    return socket.inet_ntoa(rdata)


def port_free(port, sock_type):
    """True if `port` can be bound on loopback (i.e. no listener holds it).

    For TCP, SO_REUSEADDR is set so short-lived TIME_WAIT connections left by
    the portal (which closes HTTP/1.1 responses first) do not mask a released
    listening socket.
    """
    s = socket.socket(socket.AF_INET, sock_type)
    try:
        if sock_type == socket.SOCK_STREAM:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HTTP_HOST, port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def check_http_workflows(proc, log_path):
    # Portal root opens in a browser.
    status, body = http_request("GET", "/")
    if status != 200:
        fail("GET / returned %d\n%s" % (status, log_tail(log_path)))
    if PORTAL_TITLE not in body.decode("utf-8", "replace"):
        fail("portal root does not contain %r\n%s" % (PORTAL_TITLE, log_tail(log_path)))
    print("PASS: portal root opens (%d, title present)" % status)

    # Captive-detection probe served by the portal.
    status, _ = http_request("GET", "/hotspot-detect.html")
    if status != 200:
        fail("captive probe /hotspot-detect.html returned %d" % status)
    print("PASS: captive probe /hotspot-detect.html served")

    # Status reports the provisioning app running.
    status, body = http_request("GET", "/api/v1/wifi/status")
    if status != 200:
        fail("GET /api/v1/wifi/status returned %d\n%s" % (status, log_tail(log_path)))
    info = json.loads(body.decode("utf-8"))
    if info.get("provisioning") != "running":
        fail("provisioning state is %r (expected running)\n%s" %
             (info.get("provisioning"), log_tail(log_path)))
    print("PASS: status reports provisioning=running")

    # Scan -> asynchronous 202.
    status, _ = http_request("POST", "/api/v1/wifi/scans")
    if status != 202:
        fail("POST /api/v1/wifi/scans returned %d" % status)
    print("PASS: scan request accepted")

    # Networks -> simulated APs become visible.
    def networks_visible():
        status, body = http_request("GET", "/api/v1/wifi/networks")
        if status != 200:
            return False
        nets = json.loads(body.decode("utf-8")).get("networks", [])
        return any(n.get("ssid") == "properly_ap" for n in nets)

    wait_for(networks_visible, POLL_TIMEOUT_S, "scan results with properly_ap",
             proc, log_path)
    print("PASS: scan results contain simulated network properly_ap")

    # Credentials against the simulated Wi-Fi -> connected.
    status, _ = http_request("POST", "/api/v1/wifi/credentials",
                             {"ssid": "properly_ap", "password": "12345678"})
    if status != 202:
        fail("POST /api/v1/wifi/credentials returned %d" % status)
    print("PASS: credentials accepted")

    wait_status_state("connected", proc, log_path)
    print("PASS: station connected to simulated AP")

    # Disconnect; portal must stay up.
    status, _ = http_request("DELETE", "/api/v1/wifi/connection")
    if status != 202:
        fail("DELETE /api/v1/wifi/connection returned %d" % status)
    print("PASS: disconnect request accepted")

    wait_status_state("disconnected", proc, log_path)
    print("PASS: station disconnected")


def check_dns(proc, log_path):
    answer = dns_query_a(DNS_NAME, DNS_HOST, DNS_PORT)
    if answer != DNS_ANSWER:
        fail("DNS A answer for %s is %r (expected %r)\n%s" %
             (DNS_NAME, answer, DNS_ANSWER, log_tail(log_path)))
    print("PASS: DNS answers %s -> %s" % (DNS_NAME, answer))


def main():
    if len(sys.argv) != 2:
        fail("usage: wifi_provisioning_demo_smoke_test.py <demo-binary>")
    demo = os.path.abspath(sys.argv[1])
    if not os.path.isfile(demo):
        fail("demo binary not found: %s" % demo)

    with tempfile.TemporaryDirectory(prefix="wifi_prov_demo_") as workdir:
        log_path = os.path.join(workdir, "demo.log")
        log = open(log_path, "wb", buffering=0)
        proc = subprocess.Popen([demo], cwd=workdir,
                                stdin=subprocess.DEVNULL, stdout=log,
                                stderr=subprocess.STDOUT)
        try:
            # Wait for the HTTP listener (also proves the DNS listener is up
            # because the provisioning app only reaches RUNNING after both).
            deadline = time.time() + STARTUP_TIMEOUT_S
            ready = False
            while time.time() < deadline:
                if proc.poll() is not None:
                    break
                try:
                    conn = socket.create_connection((HTTP_HOST, HTTP_PORT),
                                                    timeout=1)
                    conn.close()
                    ready = True
                    break
                except OSError:
                    time.sleep(0.25)
            if not ready:
                fail("demo listener did not open within %.0f s\n%s" %
                     (STARTUP_TIMEOUT_S, log_tail(log_path)))
            print("PASS: demo started, HTTP listener open on :%d" % HTTP_PORT)

            check_http_workflows(proc, log_path)
            check_dns(proc, log_path)

            # Clean shutdown via SIGTERM (scripted runner) in reverse order.
            proc.send_signal(signal.SIGTERM)
            try:
                rc = proc.wait(timeout=SHUTDOWN_TIMEOUT_S)
            except subprocess.TimeoutExpired:
                proc.kill()
                fail("demo did not shut down within %.0f s\n%s" %
                     (SHUTDOWN_TIMEOUT_S, log_tail(log_path)))
            if rc != 0:
                fail("demo exited with rc=%d after SIGTERM\n%s" %
                     (rc, log_tail(log_path)))

            demo_log = log_tail(log_path, limit=8000)
            for marker in ("shutting down in reverse ownership order",
                           "shutdown 1/3: provisioning app",
                           "shutdown complete: HTTP :8080 and DNS :10053 were released"):
                if marker not in demo_log:
                    fail("shutdown log missing %r\n%s" % (marker, demo_log))
            print("PASS: clean shutdown in reverse ownership order")

            if not port_free(HTTP_PORT, socket.SOCK_STREAM):
                fail("HTTP port %d still bound after shutdown" % HTTP_PORT)
            print("PASS: HTTP port %d released" % HTTP_PORT)
            if not port_free(DNS_PORT, socket.SOCK_DGRAM):
                fail("DNS port %d still bound after shutdown" % DNS_PORT)
            print("PASS: DNS port %d released" % DNS_PORT)
        finally:
            if proc.poll() is None:
                proc.kill()
            log.close()

    print("PASS: POSIX provisioning demo smoke test complete")


if __name__ == "__main__":
    main()