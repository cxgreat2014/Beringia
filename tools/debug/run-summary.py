#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Summarize one run directory (scripts/run/experiment.sh calls it): writes summary.json and summary.txt there.
#
#   tools/debug/run-summary.py RUN_DIR [--grep REGEX ...]
#
# Collects: the run's meta.txt; each screenshot (fb-*.png from tools/debug/fbshot.py) with the share of pixels that are
# not near-black and the number of distinct colours (so "did anything appear on screen" needs no eyes); kernel panics
# and Linux warnings in the saved dmesg files and watchdog incidents; macOS crash reports; launchd's exits by service;
# the unified log's size, top writers and the messages matching --grep. Standard library only; run it with python3 -I.
import argparse
import collections
import glob
import gzip
import json
import os
import re
import struct
import zlib


def png_stats(path):
    """(non-black share, distinct colours) of an 8-bit RGB PNG with filter 0 rows (what fbshot.py writes)."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos, idat = 8, b""
    w = h = 0
    while pos < len(data):
        ln, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + ln]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            if depth != 8 or ctype != 2:
                return None
        elif kind == b"IDAT":
            idat += body
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride = 1 + 3 * w
    lit = total = 0
    colours = set()
    for y in range(0, h, 4):                    # every 4th row and column is plenty for a summary
        row = raw[y * stride:(y + 1) * stride]
        if not row or row[0] != 0:
            return None
        for x in range(0, w, 4):
            r, g, b = row[1 + 3 * x:4 + 3 * x]
            total += 1
            if r + g + b > 48:
                lit += 1
            if len(colours) < 5000:
                colours.add((r, g, b))
    return {"lit": round(lit / total, 4) if total else 0, "colours": len(colours)}


def main():
    p = argparse.ArgumentParser(description="Summarize a Beringia run directory")
    p.add_argument("run_dir")
    p.add_argument("--grep", action="append", default=[])
    a = p.parse_args()
    d = a.run_dir
    s = {"run_dir": d}

    meta = {}
    if os.path.exists(os.path.join(d, "meta.txt")):
        for line in open(os.path.join(d, "meta.txt"), errors="replace"):
            k, _, v = line.rstrip("\n").partition("=")
            meta[k] = v
    s["meta"] = {k: meta[k] for k in ("run_id", "date", "boot_args", "disk", "golden", "fb", "note", "image_patch") if k in meta}

    shots = []
    for f in sorted(glob.glob(os.path.join(d, "fb-*.png")), key=lambda f: int(re.sub(r"\D", "", os.path.basename(f)) or 0)):
        st = png_stats(f)
        shots.append(dict({"file": os.path.basename(f)}, **(st or {"unreadable": True})))
    s["screenshots"] = shots

    kern = collections.Counter()
    panic_lines = []
    for f in glob.glob(os.path.join(d, "dmesg*.txt")):
        for line in open(f, errors="replace"):
            for key, pat in (("xnu_panic", "XNU PANIC"), ("linux_bug", "BUG:"), ("linux_oops", "Oops:"),
                             ("linux_warning", "WARNING:"), ("rcu_stall", "rcu: INFO"), ("soft_lockup", "soft lockup")):
                if pat in line:
                    kern[key] += 1
            if "panic(cpu" in line and len(panic_lines) < 5:
                panic_lines.append(line.strip()[-200:])
    s["kernel"] = dict(kern)
    s["panic"] = panic_lines
    s["watchdog_incidents"] = sorted(os.path.basename(f) for f in glob.glob(os.path.join(d, "incident-*.txt")))

    logs = os.path.join(d, "macos-logs")
    s["crash_reports"] = sorted(os.path.basename(f) for f in glob.glob(os.path.join(logs, "DiagnosticReports", "*.ips")))
    exits = collections.Counter()
    for f in glob.glob(os.path.join(logs, "launchd", "launchd.log*")):
        for line in open(f, errors="replace"):
            m = re.search(r"\((?:system|user/\d+|gui/\d+|loginwindow/\d+)/([^ )\[]+)[^)]*\) <Notice>: exited ", line)
            if m:
                exits[m.group(1)] += 1
    s["launchd_exits"] = exits.most_common(15)

    ul = os.path.join(logs, "unifiedlog.jsonl.gz")
    if os.path.exists(ul):
        writers = collections.Counter()
        pats = [re.compile(g) for g in a.grep]
        per_pat = collections.Counter()         # the first 25 matches of each pattern, so one noisy pattern
        matches = []                            # does not crowd out the others
        n = 0
        last = ""
        with gzip.open(ul, "rt", encoding="utf-8", errors="replace") as f:
            for line in f:
                try:
                    r = json.loads(line)
                except ValueError:
                    continue
                n += 1
                proc = (r.get("process") or "").rsplit("/", 1)[-1]
                writers[proc] += 1
                ts = r.get("timestamp") or ""
                last = max(last, ts)
                msg = r.get("message") or ""
                for i, p in enumerate(pats):
                    if per_pat[i] < 25 and (p.search(msg) or p.search(proc)):
                        per_pat[i] += 1
                        matches.append("%s %s[%s] %s" % (ts[11:23], proc, r.get("pid"),
                                                         msg.replace("\n", " | ")[:300]))
                        break
        s["unified_log"] = {"entries": n, "last": last, "top_writers": writers.most_common(10),
                            "matches_per_grep": {g: per_pat[i] for i, g in enumerate(a.grep)}}
        s["matches"] = sorted(matches)

    with open(os.path.join(d, "summary.json"), "w") as f:
        json.dump(s, f, indent=1)
    out = ["run %s  %s" % (s["meta"].get("run_id", "?"), s["meta"].get("note", ""))]
    out.append("screenshots: " + ", ".join("%s lit=%s colours=%s" % (x["file"], x.get("lit"), x.get("colours"))
                                           for x in shots))
    out.append("kernel: %s%s" % (dict(kern) or "clean", "; PANIC" if panic_lines else ""))
    out += ["  " + l for l in panic_lines]
    if s["watchdog_incidents"]:
        out.append("watchdog incidents: %s" % s["watchdog_incidents"])
    out.append("crash reports: %d %s" % (len(s["crash_reports"]),
                                        sorted(collections.Counter(re.sub(r"-\d{4}-.*", "", c)
                                                                   for c in s["crash_reports"]).items())))
    out.append("launchd exits: %s" % s["launchd_exits"][:8])
    if "unified_log" in s:
        out.append("unified log: %(entries)d entries, last %(last)s" % s["unified_log"])
        out.append("  top writers: %s" % s["unified_log"]["top_writers"][:6])
        out += ["  " + m for m in s.get("matches", [])]
    open(os.path.join(d, "summary.txt"), "w").write("\n".join(out) + "\n")
    print("\n".join(out))


if __name__ == "__main__":
    main()
