#!/usr/bin/env python3
"""External console for Predictive Input Buffer.

Tails the mod's events.jsonl and shows live stats in its own terminal window.
Turn on "Write external event log" in the mod settings first. The log path is
printed in the Geode console when the game starts.

    python console.py "C:/path/to/events.jsonl"
"""
import argparse
import json
import os
import sys
import time
from collections import defaultdict

TICK_MS = 1000.0 / 240.0
BINS = [(0, 2), (2, 4), (4, 8), (8, 16), (16, 32), (32, 64), (64, 10**9)]
RESET, DIM, GREEN, YELLOW, CYAN = "\033[0m", "\033[2m", "\033[32m", "\033[33m", "\033[36m"


def bin_label(lo, hi):
    return f"{lo}-{hi - 1}t" if hi < 10**9 else f"{lo}+t"


def bar(n, total, width=28):
    if total == 0:
        return ""
    return "#" * max(1 if n else 0, round(width * n / total))


def render(events, stage, path):
    per = defaultdict(lambda: {"clicks": 0, "padded": 0, "assisted": 0, "raw": []})
    for e in events:
        s = per[e["stage"]]
        s["clicks"] += 1
        s["raw"].append(e["raw_ticks"])
        if e["padded"]:
            s["padded"] += 1
        if e["assisted"]:
            s["assisted"] += 1

    out = ["\033[H\033[J"]
    out.append(f"{CYAN}Predictive Input Buffer - external console{RESET}   {DIM}{path}{RESET}")
    out.append(f"Current stage: {YELLOW}{stage if stage else '?'}{RESET}   events: {len(events)}\n")

    out.append(f"{'stage':<7}{'clicks':>8}{'padded':>8}{'assisted':>10}{'avg raw hold':>16}")
    for st in sorted(per):
        s = per[st]
        avg = sum(s["raw"]) / len(s["raw"]) if s["raw"] else 0
        pad_pct = 100 * s["padded"] / s["clicks"] if s["clicks"] else 0
        ast_pct = 100 * s["assisted"] / s["padded"] if s["padded"] else 0
        out.append(
            f"{st:<7}{s['clicks']:>8}{s['padded']:>5}({pad_pct:>3.0f}%)"
            f"{s['assisted']:>5}({ast_pct:>3.0f}%)"
            f"{avg:>8.1f}t / {avg * TICK_MS:>5.1f}ms"
        )

    out.append(f"\n{DIM}Raw hold length (what your finger actually did), all stages{RESET}")
    total = len(events)
    for lo, hi in BINS:
        n = sum(1 for e in events if lo <= e["raw_ticks"] < hi)
        out.append(f"{bin_label(lo, hi):>8} {n:>5} {GREEN}{bar(n, total)}{RESET}")

    out.append(f"\n{DIM}Last 8 releases{RESET}")
    for e in events[-8:]:
        tag = f"{YELLOW}PAD{RESET}" if e["padded"] else f"{DIM}raw{RESET}"
        ast = f" {GREEN}assisted{RESET}" if e["assisted"] else ""
        out.append(
            f"  S{e['stage']} {e['mode']:<6} {tag} raw {e['raw_ticks']:>6.1f}t"
            f" -> {e['final_ticks']:>6.1f}t{ast}"
        )
    out.append(f"\n{DIM}Ctrl+C to quit{RESET}")
    sys.stdout.write("\n".join(out) + "\n")
    sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log", help="path to events.jsonl (printed by the mod at startup)")
    args = ap.parse_args()
    os.system("")  # enables ANSI colors in Windows terminals

    events, stage = [], 0
    pos = 0
    last_draw = 0.0
    while True:
        try:
            size = os.path.getsize(args.log)
        except OSError:
            sys.stdout.write("\033[H\033[JWaiting for the log file... (is 'Write external event log' on?)\n")
            sys.stdout.flush()
            time.sleep(1)
            continue

        if size < pos:  # mod restarted and truncated the file
            events, stage, pos = [], 0, 0

        with open(args.log, "r", encoding="utf-8") as f:
            f.seek(pos)
            chunk = f.read()
            pos = f.tell()

        # Only consume complete lines.
        if chunk and not chunk.endswith("\n"):
            keep = chunk.rfind("\n") + 1
            pos -= len(chunk[keep:].encode("utf-8"))
            chunk = chunk[:keep]

        for line in chunk.splitlines():
            try:
                e = json.loads(line)
            except json.JSONDecodeError:
                continue
            if e.get("ev") == "stage":
                stage = e["stage"]
            elif e.get("ev") == "release":
                events.append(e)
                stage = e["stage"]

        if chunk or time.time() - last_draw > 1.0:
            render(events, stage, args.log)
            last_draw = time.time()
        time.sleep(0.2)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
