#!/usr/bin/env python3
"""Localhost load test: starts server_bin, drives it with loadgen, writes JSON.

  python3 tools/loadtest.py [--out results/loadtest.json] [--quick]

Everything is on loopback, one machine: no packet loss, no real latency
asymmetry, and client/server share one clock, so the offset error measured here
is a lower bound for what a real network would give, not a prediction.
"""
import argparse, datetime, json, os, platform, subprocess, sys, time, urllib.request

PORT = 9301
MPORT = 9302


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout.strip()


def hardware():
    if platform.system() == "Darwin":
        return {"cpu": sh("sysctl -n machdep.cpu.brand_string"), "cores": int(sh("sysctl -n hw.ncpu")),
                "mem_bytes": int(sh("sysctl -n hw.memsize"))}
    return {"cpu": sh("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2").strip(), "cores": int(sh("nproc")),
            "mem_bytes": int(sh("awk '/MemTotal/{print $2*1024}' /proc/meminfo"))}


def run(mode, clients, interval, warmup, duration, backend="auto", scrape=False):
    srv = subprocess.Popen(["./server_bin", "--mode", mode, "--port", str(PORT), "--no-shm", "--backend", backend,
                            "--metrics-port", str(MPORT)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(0.5)
    try:
        r = subprocess.run(["./loadgen", "--mode", mode, "--port", str(PORT), "--clients", str(clients),
                            "--interval", str(interval), "--warmup", str(warmup), "--duration", str(duration)],
                           capture_output=True, text=True, timeout=duration + 120)
        row = json.loads(r.stdout.strip().splitlines()[-1]) if r.stdout.strip() else {"error": r.stderr[-300:]}
        row["backend"] = backend
        if scrape:
            try:
                m = json.load(urllib.request.urlopen(f"http://127.0.0.1:{MPORT}/metrics.json", timeout=5))
                row["server_drift"] = m["drift"]
                row["server_rtt_buckets_us"] = m["rtt"]["buckets_us"]
            except Exception as e:  # report, do not hide
                row["scrape_error"] = str(e)
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


def ci(out_path):
    """CI mode: concurrency ramp per backend plus a steady-load latency run; writes JSON and a markdown table."""
    import resource
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    out = {"date": datetime.date.today().isoformat(), "hardware": hardware(), "os": platform.platform(),
           "runner": os.environ.get("RUNNER_NAME", "local"), "nofile_hard": hard,
           "caveat": "loopback, one machine, load generator and server share the CPUs; RTT includes the load "
                     "generator's own scheduling. Ramp rows: 1 request/s per client.",
           "latency": None, "ramp": []}
    out["latency"] = run("tcp", 200, 100, 3, 15, "auto", scrape=True)
    print(json.dumps(out["latency"]), file=sys.stderr)
    for backend in ("auto", "poll"):
        for n in (1000, 5000, 10000, 20000):
            row = run("tcp", n, 1000, 3, 10, backend)
            print(json.dumps(row), file=sys.stderr)
            out["ramp"].append(row)
            time.sleep(2)
    json.dump(out, open(out_path, "w"), indent=2)
    hw = out["hardware"]
    lat = out["latency"]
    md = [f"Hardware: {hw['cpu']}, {hw['cores']} vCPU, {hw['mem_bytes'] >> 30} GiB; {out['os']}; runner `{out['runner']}`", "",
          f"Steady load, 200 TCP clients at 10 req/s each ({lat['replies_per_s']:.0f} replies/s), server backend "
          f"`{lat['backend']}`: RTT p50 {lat['rtt_us']['p50']} us, p99 {lat['rtt_us']['p99']} us, "
          f"p99.9 {lat['rtt_us']['p999']} us (n={lat['rtt_samples']}).", "",
          "| backend | clients requested | connected | dropped | RTT p50 us | RTT p99 us | replies/s |", "|---|---|---|---|---|---|---|"]
    for r in out["ramp"]:
        if "connected" not in r:
            md.append(f"| {r.get('backend')} | ? | error | | | | |")
            continue
        md.append(f"| {r['backend']} | {r['clients_requested']} | {r['connected']} | {r['dropped_after_connect']} | "
                  f"{r['rtt_us']['p50']} | {r['rtt_us']['p99']} | {r['replies_per_s']:.0f} |")
    text = "\n".join(md)
    print(text)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write("## Load test (GitHub runner)\n" + text + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="results/loadtest.json")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--ci", action="store_true", help="CI mode: ramp per backend + latency run")
    ap.add_argument("--ramp", help="comma list: only run these ramp sizes and merge into --out")
    ap.add_argument("--drain", type=int, default=1, help="seconds to wait between runs (TIME_WAIT)")
    a = ap.parse_args()
    if a.ci:
        ci(a.out)
        return
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
