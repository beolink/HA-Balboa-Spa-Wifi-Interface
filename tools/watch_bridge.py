#!/usr/bin/env python3
"""Follow a bridge on the network: bus state, spa status and new bus-log lines.

  python3 tools/watch_bridge.py spa-bridge.local [--user admin --password ...] [--save capture.log]

Standard library only. Read-only: it never changes a setting.
"""

from __future__ import annotations

import argparse
import base64
import json
import time
import urllib.request


def get(base: str, path: str, auth: str | None):
    req = urllib.request.Request(base + path)
    if auth:
        req.add_header("Authorization", "Basic " + auth)
    with urllib.request.urlopen(req, timeout=5) as resp:
        return json.load(resp)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password")
    ap.add_argument("--save", help="append the bus log here")
    ap.add_argument("--every", type=float, default=2.0)
    args = ap.parse_args()
    base = f"http://{args.host}"
    auth = base64.b64encode(f"{args.user}:{args.password}".encode()).decode() if args.password else None
    seen = 0.0
    last_line = ""
    out = open(args.save, "a", encoding="utf-8") if args.save else None
    try:
        while True:
            try:
                s = get(base, "/api/status", auth)
                b, p = s["bus"], s["spa"]
                line = (
                    f"bus {b['mode']}/{b['state']} ch 0x{b['channel']:02x} frames {b['frames']} "
                    f"cts {b['cts_ours']}/{b['cts_seen']} answer {b['answer_us_last']}us (max {b['answer_us_max']}) "
                    f"| spa {p.get('model') or '?'} water {p.get('water')} target {p.get('target')} "
                    f"pumps {p.get('pumps')} light {p.get('light')} | clients {s['tcp']['clients']}"
                )
                if line != last_line:
                    print(time.strftime("%H:%M:%S"), line, flush=True)
                    last_line = line
                log = get(base, "/api/log", auth)
                if log and log[-1]["ms"] / 1000 < seen - 5:
                    seen = 0.0  # the bridge restarted
                for e in log:
                    if e["ms"] / 1000 <= seen:
                        continue
                    seen = e["ms"] / 1000
                    text = f"{seen:10.3f} {'->' if e['tx'] else '  '} {e['hex']}"
                    print("    " + text, flush=True)
                    if out:
                        out.write(text + "\n")
                        out.flush()
            except OSError as err:
                print(time.strftime("%H:%M:%S"), "unreachable:", err, flush=True)
            time.sleep(args.every)
    except KeyboardInterrupt:
        pass
    finally:
        if out:
            out.close()


if __name__ == "__main__":
    main()
