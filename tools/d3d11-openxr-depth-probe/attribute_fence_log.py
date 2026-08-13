#!/usr/bin/env python3

import argparse
import re
import sys


TIMEOUT = re.compile(
    r"Waiting on client fence timed out > 100ms! client=(\d+) .*"
    r"frame=(-?\d+) timeout_count=(\d+)"
)
TERMINAL = re.compile(r"Fence waiting failed! client=(\d+) ")
RECOVERED = re.compile(r"Client fence recovered after (\d+) timeout\(s\): client=(\d+) ")
ATTRIBUTION = re.compile(r"Client fence attribution enabled: client=(\d+) ")


def classify(text: str, application: str, persistent_count: int) -> int:
    described_id = None
    applications = []
    for line in text.splitlines():
        id_match = re.search(r"\bid: (\d+)\s*$", line)
        if id_match:
            described_id = int(id_match.group(1))
            continue
        app_match = re.search(r"application_name: '([^']*)'", line)
        if app_match and described_id is not None:
            applications.append((described_id, app_match.group(1)))
            described_id = None

    target_ids = [client_id for client_id, app in applications if app == application]
    target_id = target_ids[-1] if target_ids else None
    untagged = 0
    timeout_counts = []
    terminal = False
    recovered = []
    attributed_clients = set()
    for line in text.splitlines():
        match = ATTRIBUTION.search(line)
        if match:
            attributed_clients.add(int(match.group(1)))
        if "Waiting on client fence timed out > 100ms!" in line:
            match = TIMEOUT.search(line)
            if not match:
                untagged += 1
            elif target_id is not None and int(match.group(1)) == target_id:
                timeout_counts.append(int(match.group(3)))
        match = TERMINAL.search(line)
        if match and target_id is not None and int(match.group(1)) == target_id:
            terminal = True
        match = RECOVERED.search(line)
        if match and target_id is not None and int(match.group(2)) == target_id:
            recovered.append(int(match.group(1)))

    if terminal:
        print(f"FAIL: client {target_id} ({application}) reported terminal fence failure")
        return 1
    maximum = max(timeout_counts, default=0)
    if maximum >= persistent_count:
        print(
            f"FAIL: client {target_id} ({application}) reached {maximum} "
            "consecutive 100ms fence timeouts"
        )
        return 1
    if target_id is None:
        print(f"WARN: no IPC client description found for {application}; attribution unavailable")
        return 2
    if target_id not in attributed_clients:
        print(
            f"WARN: client {target_id} ({application}) has no fence-attribution "
            "capability marker; attribution unavailable"
        )
        return 2
    if untagged:
        print(
            f"WARN: ignored {untagged} untagged compositor-wide fence timeout(s); "
            "the deployed runtime lacks complete client attribution"
        )
        return 2
    print(
        f"PASS: client {target_id} ({application}) had no terminal or persistent "
        f"fence wait (max consecutive timeouts={maximum}, recovered={recovered})"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    parser.add_argument("--application", default="d3d11-openxr-depth-probe")
    parser.add_argument("--persistent-count", type=int, default=5)
    args = parser.parse_args()
    with open(args.log, encoding="utf-8", errors="replace") as stream:
        return classify(stream.read(), args.application, args.persistent_count)


if __name__ == "__main__":
    sys.exit(main())
