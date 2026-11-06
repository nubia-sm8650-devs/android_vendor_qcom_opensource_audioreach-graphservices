# SPDX-License-Identifier: BSD-3-Clause
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from reconstruction import reconstruct
from ownership_checks import check_requirements


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
        completed = subprocess.run([
            str(args.compiler.resolve()), "--exports", str(args.exports),
            "--report", str(report)], capture_output=True, text=True)
        assert completed.returncode in (0, 2), completed.stderr
        factored = json.loads(report.read_text())["factored"]
        recovered = reconstruct(factored)
        assert len(recovered) == len(paths)

        for actual, path in zip(recovered, paths):
            assert actual == json.loads(path.read_text()), path

        check_requirements(factored, [json.loads(p.read_text()) for p in paths])

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
            assert reconstruct(actual["factored"]) == [expected], path
            check_requirements(actual["factored"], [expected])

    print(f"Native ACDB queries match all {len(paths)} reference exports")
    print("Factored declarations reconstruct every original graph buffer")
    print("Connection ownership and selection requirements match source")


if __name__ == "__main__":
    main()
