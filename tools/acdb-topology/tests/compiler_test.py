# SPDX-License-Identifier: BSD-3-Clause
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from reconstruction import reconstruct
from ownership_checks import check_requirements


if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
    COMPILER = Path(sys.argv.pop(1)).resolve()
else:
    COMPILER = Path(sys.argv[0]).resolve().with_name("ar-acdb-topology")


def words(*values):
    return struct.pack(f"<{len(values)}I", *values)


def record(parameter, payload):
    return (words(1, parameter, len(payload), 0) + payload
            + bytes(-len(payload) % 8))


def properties(identity, values):
    result = words(identity, len(values))
    for parameter, payload in values:
        result += words(parameter, len(payload)) + payload
    return result


def fixture(shared=False, extra=False, source_only=False):
    sg_ids = ([0xB0000001, 0xB0000002] if shared and not source_only
              else [0xB0000001])
    sg_data = b""
    for sg in sg_ids:
        props = [(0x0800100E, words(1)), (0x0800100F, words(2)),
                 (0x08001010, words(1))]
        if extra:
            props.append((0x080010CC, words(0xFFFFFFFF)))
        sg_data += properties(sg, props)

    container = properties(0xE0000001, [
        (0x08001011, words(1, 0x0B000001)),
        (0x08001012, words(1)), (0x08001013, words(4096)),
        (0x08001014, words(2))])
    if source_only:
        modules = (words(1, sg_ids[0], 0xE0000001, 1)
                   + words(0x07001005, 0x4001))
    elif shared:
        modules = (words(2) + words(sg_ids[0], 0xE0000001, 1)
                   + words(0x07001005, 0x4001)
                   + words(sg_ids[1], 0xE0000001, 1)
                   + words(0x07001004, 0x4002))
    else:
        modules = (words(1, sg_ids[0], 0xE0000001, 2)
                   + words(0x07001005, 0x4001, 0x07001004, 0x4002))
    instances = (0x4001,) if source_only else (0x4001, 0x4002)
    ports = words(len(instances))
    for iid in instances:
        ports += properties(iid, [(0x08001015, words(1, 1))])
    graph = (record(0x08001001, words(len(sg_ids)) + sg_data)
             + record(0x08001000, words(1) + container)
             + record(0x08001002, modules)
             + record(0x08001003, ports)
             + record(0x08001004, words(1, 0x4001, 1, 0x4002, 2)))
    return {"version": 1, "graph_keys": [],
            "subgraph_ids": [f"0x{s:08x}" for s in sg_ids],
            "driver_subgraph_count": len(sg_ids),
            "subgraph_connections": b"".join(words(sg, 0)
                                             for sg in sg_ids).hex(),
            "driver_properties": "", "graph_open": graph.hex(),
            "graph_connections": ""}


def widgets(data):
    position = 0
    result = {}
    while position < len(data):
        header = struct.unpack_from("<9I", data, position)
        magic, abi, _, kind, size, _, payload, _, count = header
        assert magic == 0x41536F43 and abi == 5
        position += size
        end = position + payload
        if kind != 5:
            position = end
            continue
        for _ in range(count):
            widget_size, = struct.unpack_from("<I", data, position)
            name = data[position + 8:position + 52].split(b"\0")[0]
            private_size, = struct.unpack_from("<I", data,
                                               position + widget_size - 4)
            position += widget_size
            private_end = position + private_size
            groups = []
            while position < private_end:
                length, tag, elements = struct.unpack_from("<3I", data,
                                                          position)
                assert tag == 4 and length == 12 + elements * 8
                groups.append(dict(struct.iter_unpack(
                    "<2I", data[position + 12:position + length])))
                position += length
            assert position == private_end
            result[name.decode()] = groups
        assert position == end
    return result


class CompilerTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / "source.json"
        self.report = self.root / "report.json"
        self.output = self.root / "topology.conf"

    def run_compiler(self, source):
        self.source.write_text(json.dumps(source))
        return subprocess.run([
            str(COMPILER), "--input", str(self.source), "--graph-id", "16",
            "--report", str(self.report), "--output", str(self.output)],
            capture_output=True, text=True)

    def run_exports(self, sources):
        directory = Path(tempfile.mkdtemp(dir=self.root))

        for index, source in enumerate(sources):
            (directory / f"{index}.json").write_text(json.dumps(source))

        return subprocess.run([
            str(COMPILER), "--exports", str(directory), "--report",
            str(self.report)], capture_output=True, text=True)

    def test_conventional_topology_preserves_declarations(self):
        source = fixture()
        completed = self.run_compiler(source)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(self.report.read_text())["graphs"][0]
                         ["source"], source)
        if not shutil.which("alsatplg"):
            self.skipTest("alsatplg is not installed")
        binary = self.root / "topology.bin"
        subprocess.run(["alsatplg", "-c", str(self.output), "-o", str(binary)],
                       check=True, capture_output=True)
        decoded = widgets(binary.read_bytes())
        self.assertEqual(set(decoded), {"m16385", "m16386"})
        for iid in (0x4001, 0x4002):
            sg, container, module = decoded[f"m{iid}"]
            self.assertEqual(sg, {2: 0xB0000001, 1: 16, 3: 1, 4: 2, 5: 1})
            self.assertEqual(container, {100: 0xE0000001, 101: 0x0B000001,
                                         103: 1, 102: 4096, 104: 2})
            self.assertEqual(module[201], iid)
            self.assertEqual((module[202], module[203]), (1, 1))
        self.assertEqual(decoded["m16385"][2], {201: 0x4001,
            200: 0x07001005, 202: 1, 203: 1, 206: 1, 207: 2, 209: 0x4002})
        self.assertEqual(decoded["m16386"][2][200], 0x07001004)

    def test_unsupported_property_keeps_source_and_existing_output(self):
        source = fixture(extra=True)
        self.output.write_text("previous output")
        completed = self.run_compiler(source)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        report = json.loads(self.report.read_text())["graphs"][0]
        self.assertEqual(report["source"], source)
        self.assertIn("property", {i["kind"] for i in report["issues"]})
        self.assertEqual(self.output.read_text(), "previous output")

    def test_shared_container_is_not_split_or_renumbered(self):
        source = fixture(shared=True)
        completed = self.run_compiler(source)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        report = json.loads(self.report.read_text())["graphs"][0]
        self.assertEqual(report["source"], source)
        self.assertIn("shared-container", {i["kind"] for i in report["issues"]})
        self.assertFalse(self.output.exists())

    def test_factoring_preserves_choices_variants_and_opaque_records(self):
        first = fixture(shared=True)
        second = fixture(shared=True, extra=True)
        second["graph_keys"] = [["0xa2000000", "0xa2000001"]]
        control = words(0x4001, 7, 0x4002, 8, 2)
        control += words(0x08001062, 12, 0, 1, 0x0800105D)
        control += words(0x0800136F, 4, 2)
        first["graph_connections"] = record(
            0x08001061, words(1) + control[:-4] + words(1)).hex()
        opaque = words(4, 0x08001162, 3, 0) + b"xyz" + b"abcde"
        second["graph_open"] += opaque.hex()
        second["graph_connections"] = (
            record(0x08001004, words(2, 0x4001, 1, 0x4002, 2,
                                     0x4001, 1, 0x4002, 2))
            + record(0x08001061, words(1) + control)).hex()
        sources = [first, second]
        def run(values):
            completed = self.run_exports(values)
            self.assertEqual(completed.returncode, 2, completed.stderr)
            return json.loads(self.report.read_text())["factored"]

        factored = run(sources)
        self.assertEqual(reconstruct(factored), sources)
        definitions = factored["definitions"]
        self.assertEqual(len(definitions["containers"]), 1)
        self.assertEqual({d["id"] for d in definitions["containers"]},
                         {0xE0000001})
        groups = definitions["module_groups"]
        self.assertEqual({g["container"] for g in groups}, {0xE0000001})
        self.assertEqual({g["subgraph"] for g in groups},
                         {0xB0000001, 0xB0000002})
        self.assertEqual(len(definitions["data_links"]), 1)
        self.assertEqual(len(definitions["control_links"]), 2)
        self.assertEqual(len(definitions["subgraphs"]), 4)
        reversed_factored = run(list(reversed(sources)))
        self.assertEqual(reconstruct(reversed_factored),
                         list(reversed(sources)))
        self.assertEqual(reversed_factored["definitions"], definitions)
        self.assertEqual(reversed_factored["module_ownership"],
                         factored["module_ownership"])

        for actual, expected in zip(reversed_factored["selections"],
                                    reversed(factored["selections"])):
            self.assertEqual(actual["connection_requirements"],
                             expected["connection_requirements"])

    def test_connection_requirements_do_not_import_absent_endpoints(self):
        partial = fixture(source_only=True)
        control = words(1, 0x4001, 7, 0x4002, 8, 1, 0x0800136F, 4, 1)
        partial["graph_connections"] = (
            record(0x08001004, words(2, 0x4001, 1, 0x4002, 2,
                                     0x4001, 1, 0x4002, 2))
            + record(0x08001061, control)).hex()
        completed = self.run_compiler(partial)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        factored = json.loads(self.report.read_text())["factored"]
        check_requirements(factored, [partial])

        for value in factored["selections"][0]["connection_requirements"]:
            self.assertEqual(value["destination"]["resolution"], "unknown")
            self.assertIsNone(value["required_subgraphs"])

        sources = [fixture(shared=True), partial]
        completed = self.run_exports(sources)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        factored = json.loads(self.report.read_text())["factored"]
        self.assertEqual(reconstruct(factored), sources)
        check_requirements(factored, sources)
        choices = factored["selections"][1]["connection_requirements"]
        self.assertEqual(len(choices), 4)

        for value in choices:
            self.assertEqual(value["source"]["resolution"], "selection")
            self.assertEqual(value["destination"]["resolution"], "corpus")
            self.assertFalse(value["endpoints_selected"])
            self.assertEqual(value["unselected_modules"], [0x4002])
            self.assertEqual(value["required_subgraphs"],
                             [0xB0000001, 0xB0000002])
            self.assertEqual(value["missing_subgraphs"], [0xB0000002])
            self.assertEqual(value["relation"], "shared-container")

        self.assertEqual(factored["selections"][1]["metadata"]
                         ["subgraph_ids"], ["0xb0000001"])

    def test_truncated_parameter_is_rejected(self):
        source = fixture()
        source["graph_open"] = source["graph_open"][:-2]
        completed = self.run_compiler(source)
        self.assertEqual(completed.returncode, 1, completed.stderr)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
