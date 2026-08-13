#!/usr/bin/env python3

import argparse
import pathlib
import re
import subprocess


def output(*args):
    return subprocess.run(args, check=True, text=True, capture_output=True).stdout


def main():
    parser = argparse.ArgumentParser(description="Verify a matched WineOpenXR PE/Unix private-ABI pair")
    parser.add_argument("pe", type=pathlib.Path)
    parser.add_argument("unix", type=pathlib.Path)
    parser.add_argument("generated_header", type=pathlib.Path)
    args = parser.parse_args()

    header = args.generated_header.read_text()
    enum_body = header.split("enum unix_call", 1)[1].split("};", 1)[0]
    entries = [
        entry
        for entry in re.findall(r"^\s+(unix_[A-Za-z0-9_]+),", enum_body, re.MULTILINE)
        if entry != "unix_count"
    ]
    expected_bytes = len(entries) * 8

    symbols = output("readelf", "-Ws", str(args.unix))
    match = re.search(r"^\s*\d+:\s+[0-9a-fA-F]+\s+(\d+)\s+OBJECT\s+GLOBAL\s+DEFAULT\s+\d+\s+__wine_unix_call_funcs$", symbols, re.MULTILINE)
    if not match:
        raise SystemExit("FAIL: Unix library has no public __wine_unix_call_funcs symbol")
    actual_bytes = int(match.group(1))
    if actual_bytes != expected_bytes:
        raise SystemExit(f"FAIL: generated table is {expected_bytes} bytes but Unix library exports {actual_bytes}")

    pe_strings = output("strings", str(args.pe))
    unix_strings = output("strings", str(args.unix))
    if "PE/Unix ABI handshake failed" not in pe_strings:
        raise SystemExit("FAIL: PE does not contain the private-ABI handshake diagnostic")
    if "PE/Unix ABI magic mismatch" not in unix_strings:
        raise SystemExit("FAIL: Unix library does not contain the private-ABI handshake diagnostic")

    print(f"PASS: matched WineOpenXR pair carries a {len(entries)}-call ({actual_bytes}-byte) private ABI")


if __name__ == "__main__":
    main()
