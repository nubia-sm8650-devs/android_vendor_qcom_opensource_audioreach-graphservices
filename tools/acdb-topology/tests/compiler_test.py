# SPDX-License-Identifier: BSD-3-Clause
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


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


def fixture(shared=False, extra=False):
    sg_ids = [0xB0000001, 0xB0000002] if shared else [0xB0000001]
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
    if shared:
        modules = (words(2) + words(sg_ids[0], 0xE0000001, 1)
                   + words(0x07001005, 0x4001)
                   + words(sg_ids[1], 0xE0000001, 1)
                   + words(0x07001004, 0x4002))
    else:
        modules = (words(1, sg_ids[0], 0xE0000001, 2)
                   + words(0x07001005, 0x4001, 0x07001004, 0x4002))
    ports = words(2)
    for iid in (0x4001, 0x4002):
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

    def test_truncated_parameter_is_rejected(self):
        source = fixture()
        source["graph_open"] = source["graph_open"][:-2]
        completed = self.run_compiler(source)
        self.assertEqual(completed.returncode, 1, completed.stderr)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
