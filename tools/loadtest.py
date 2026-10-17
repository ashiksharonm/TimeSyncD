#!/usr/bin/env python3
"""Localhost load test: starts server_bin, drives it with loadgen, writes JSON.

  python3 tools/loadtest.py [--out results/loadtest.json] [--quick]

Everything is on loopback, one machine: no packet loss, no real latency
asymmetry, and client/server share one clock, so the offset error measured here
is a lower bound for what a real network would give, not a prediction.
"""
import argparse, datetime, json, platform, subprocess, sys, time

PORT = 9301


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout.strip()


def hardware():
    if platform.system() == "Darwin":
        return {"cpu": sh("sysctl -n machdep.cpu.brand_string"), "cores": int(sh("sysctl -n hw.ncpu")),
                "mem_bytes": int(sh("sysctl -n hw.memsize"))}
    return {"cpu": sh("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2").strip(), "cores": int(sh("nproc")),
            "mem_bytes": int(sh("awk '/MemTotal/{print $2*1024}' /proc/meminfo"))}


def run(mode, clients, interval, warmup, duration):
    srv = subprocess.Popen(["./server_bin", "--mode", mode, "--port", str(PORT), "--no-shm"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(0.5)
    try:
        r = subprocess.run(["./loadgen", "--mode", mode, "--port", str(PORT), "--clients", str(clients),
                            "--interval", str(interval), "--warmup", str(warmup), "--duration", str(duration)],
                           capture_output=True, text=True, timeout=duration + 120)
        row = json.loads(r.stdout.strip().splitlines()[-1]) if r.stdout.strip() else {"error": r.stderr[-300:]}
    finally:
        srv.terminate()
        try:
            srv_out = srv.communicate(timeout=10)[0]
        except subprocess.TimeoutExpired:
            srv.kill()
            srv_out = ""
    row["server_log_tail"] = srv_out.strip().splitlines()[-1] if srv_out.strip() else ""
    time.sleep(1)  # let TIME_WAIT sockets drain a little
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="results/loadtest.json")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--ramp", help="comma list: only run these ramp sizes and merge into --out")
    ap.add_argument("--drain", type=int, default=1, help="seconds to wait between runs (TIME_WAIT)")
    a = ap.parse_args()
    if a.ramp:
        out = json.load(open(a.out))
        sizes = [int(x) for x in a.ramp.split(",")]
        out["ramp"] = [r for r in out["ramp"] if r["clients_requested"] not in sizes]
        time.sleep(a.drain)
        for n in sizes:
            row = run("tcp", n, 1000, 10, 25)
            row["drain_before_s"] = a.drain
            print(json.dumps(row), file=sys.stderr)
            out["ramp"].append(row)
            time.sleep(a.drain)
        out["ramp"].sort(key=lambda r: r["clients_requested"])
        json.dump(out, open(a.out, "w"), indent=2)
        return
    out = {"date": datetime.date.today().isoformat(), "hardware": hardware(), "os": platform.platform(),
           "caveat": "loopback on one machine: no loss, no path asymmetry, shared clock. Error figures are "
                     "localhost lower bounds, not a real-network accuracy claim.",
           "accuracy": [], "ramp": []}
    acc = [(1, 100), (100, 200), (1000, 200)] if not a.quick else [(10, 200)]
    for mode in ("tcp", "udp"):
        for n, iv in acc:
            row = run(mode, n, iv, 10, 40 if not a.quick else 15)
            print(json.dumps(row), file=sys.stderr)
            out["accuracy"].append(row)
    if not a.quick:
        for n in (1000, 2000, 4000, 8000, 12000, 16000, 20000, 30000):
            row = run("tcp", n, 1000, 10, 25)
            print(json.dumps(row), file=sys.stderr)
            out["ramp"].append(row)
            if row.get("connected", 0) < n:
                break
    with open(a.out, "w") as f:
        json.dump(out, f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    main()
