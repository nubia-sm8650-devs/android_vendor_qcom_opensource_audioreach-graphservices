# SPDX-License-Identifier: BSD-3-Clause
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--acdb", action="append", required=True)
    parser.add_argument("--exports", type=Path, required=True)
    args = parser.parse_args()
    paths = sorted(args.exports.glob("*.json"))
    assert paths, "No reference exports"

    with tempfile.TemporaryDirectory() as directory:
        report = Path(directory) / "report.json"

        for path in paths:
            expected = json.loads(path.read_text())
            command = [str(args.compiler.resolve()), "--report", str(report)]

            for database in args.acdb:
                command.extend(["--acdb", database])

            for key, value in expected["graph_keys"]:
                command.extend(["--key", f"{key}={value}"])

            completed = subprocess.run(command, capture_output=True, text=True)
            assert completed.returncode in (0, 2), (path, completed.stderr)
            actual = json.loads(report.read_text())
            assert actual["graphs"][0]["source"] == expected, path

    print(f"Native ACDB queries match all {len(paths)} reference exports")


if __name__ == "__main__":
    main()
