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


def parse_descriptions(text: str) -> dict[int, dict[str, int | str]]:
    described_id = None
    descriptions: dict[int, dict[str, int | str]] = {}
    for line in text.splitlines():
        id_match = re.search(r"\bid: (\d+)\s*$", line)
        if id_match:
            described_id = int(id_match.group(1))
            descriptions.setdefault(described_id, {})
            continue
        app_match = re.search(r"application_name: '([^']*)'", line)
        if app_match and described_id is not None:
            descriptions[described_id]["application"] = app_match.group(1)
            continue
        pid_match = re.search(r"\bpid: (\d+)\s*$", line)
        if pid_match and described_id is not None:
            descriptions[described_id]["pid"] = int(pid_match.group(1))
            described_id = None
    return descriptions


def find_target_client_id(text: str, application: str, pid: int | None = None):
    descriptions = parse_descriptions(text)
    target_ids = [
        client_id
        for client_id, description in descriptions.items()
        if description.get("application") == application
        and (pid is None or description.get("pid") == pid)
    ]
    return target_ids[-1] if target_ids else None


def classify(
    text: str,
    application: str,
    persistent_count: int,
    runtime_capable: bool = False,
    pid: int | None = None,
) -> int:
    target_id = find_target_client_id(text, application, pid)

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
        qualifier = f" with PID {pid}" if pid is not None else ""
        print(
            f"WARN: no IPC client description found for {application}{qualifier}; "
            "attribution unavailable"
        )
        return 2
    if target_id not in attributed_clients and not runtime_capable:
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
    parser.add_argument("--runtime-capable", action="store_true")
    parser.add_argument("--pid", type=int)
    parser.add_argument("--print-client-id", action="store_true")
    args = parser.parse_args()
    with open(args.log, encoding="utf-8", errors="replace") as stream:
        text = stream.read()
        if args.print_client_id:
            target_id = find_target_client_id(text, args.application, args.pid)
            if target_id is None:
                return 2
            print(target_id)
            return 0
        return classify(
            text,
            args.application,
            args.persistent_count,
            args.runtime_capable,
            args.pid,
        )


if __name__ == "__main__":
    sys.exit(main())
