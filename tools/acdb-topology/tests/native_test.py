# SPDX-License-Identifier: BSD-3-Clause
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from reconstruction import reconstruct
from ownership_checks import check_requirements
from endpoint_checks import check_endpoints, check_native_reference


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--acdb", action="append", required=True)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--bindings-reference", type=Path)
    args = parser.parse_args()
    paths = sorted(args.exports.glob("*.json"))
    assert paths, "No reference exports"
    bindings = (json.loads(args.bindings_reference.read_text())
                if args.bindings_reference else None)
    if bindings is not None:
        assert len(bindings) == len(paths)

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
        check_endpoints(factored, [json.loads(p.read_text()) for p in paths])

        for index, path in enumerate(paths):
            expected = json.loads(path.read_text())
            command = [str(args.compiler.resolve()), "--report", str(report)]

            for database in args.acdb:
                command.extend(["--acdb", database])

            for key, value in expected["graph_keys"]:
                command.extend(["--key", f"{key}={value}"])

            completed = subprocess.run(command, capture_output=True, text=True)
            assert completed.returncode in (0, 2), (path, completed.stderr)
            actual = json.loads(report.read_text())
            source = actual["graphs"][0]["source"]
            original = dict(source)
            if "endpoint_queries" not in expected:
                original.pop("endpoint_queries")
            assert original == expected, path
            assert reconstruct(actual["factored"]) == [source], path
            check_requirements(actual["factored"], [source])
            check_endpoints(actual["factored"], [source])
            if bindings is not None:
                check_native_reference(source, bindings[index])

        command = [str(args.compiler.resolve()), "--report", str(report),
                   "--corpus"]
        for database in args.acdb:
            command.extend(["--acdb", database])

        completed = subprocess.run(command, capture_output=True, text=True)
        assert completed.returncode in (0, 2), completed.stderr
        expected = {json.dumps(e["graph_keys"]): e for e in
                    (json.loads(p.read_text()) for p in paths)}
        corpus = [graph["source"] for graph in
                  json.loads(report.read_text())["graphs"]]
        assert len(corpus) == len(expected)

        for source in corpus:
            reference = expected.pop(json.dumps(source["graph_keys"]))
            original = dict(source)
            if "endpoint_queries" not in reference:
                original.pop("endpoint_queries")
            assert original == reference, source["graph_keys"]

    print(f"Native ACDB queries match all {len(paths)} reference exports")
    print("The ACDB corpus query enumerates exactly the reference selections")
    print("Factored declarations reconstruct every original graph buffer")
    print("Connection ownership and selection requirements match source")
    print("Host and physical endpoint candidates match source declarations")
    if bindings is not None:
        print("Hardware tags and endpoint payloads match reference queries")


if __name__ == "__main__":
    main()
