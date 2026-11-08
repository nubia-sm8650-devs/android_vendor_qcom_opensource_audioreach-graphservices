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
from endpoint_checks import check_endpoints


if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
    COMPILER = Path(sys.argv.pop(1)).resolve()
else:
    COMPILER = Path(sys.argv[0]).resolve().with_name("ar-acdb-topology")


def words(*values):
    return struct.pack(f"<{len(values)}I", *values)


def record(parameter, payload, target=1):
    return (words(target, parameter, len(payload), 0) + payload
            + bytes(-len(payload) % 8))


def properties(identity, values):
    result = words(identity, len(values))
    for parameter, payload in values:
        result += words(parameter, len(payload)) + payload
    return result


def fixture(shared=False, extra=False, source_only=False,
            container_properties=(), vsid=None):
    sg_ids = ([0xB0000001, 0xB0000002] if shared and not source_only
              else [0xB0000001])
    sg_data = b""
    for sg in sg_ids:
        props = [(0x0800100E, words(1)), (0x0800100F, words(2)),
                 (0x08001010, words(1))]
        if extra:
            props.append((0x08009999, words(0xFFFFFFFF)))
        if vsid is not None:
            props.append((0x080010CC, words(vsid)))
        sg_data += properties(sg, props)

    container = properties(0xE0000001, [
        (0x08001011, words(1, 0x0B000001)),
        (0x08001012, words(1)), (0x08001013, words(4096)),
        (0x08001014, words(2)), *container_properties])
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


def generic_fixture():
    source = fixture()
    graph = bytes.fromhex(source["graph_open"])
    graph = graph.replace(words(0x07001005), words(0x07001011))
    graph = graph.replace(words(0x07001004), words(0x07001002))
    graph = graph.replace(properties(0x4001, [(0x08001015, words(1, 1))]),
                          properties(0x4001, [(0x08001015, words(1, 18))]))
    graph = graph.replace(words(0x4001, 1, 0x4002, 2),
                          words(0x4001, 23, 0x4002, 2))
    source["graph_open"] = graph.hex()
    return source


def mixed_container_fixture():
    source = fixture(shared=True)
    graph = bytes.fromhex(source["graph_open"])
    original = words(0xB0000002, 0xE0000001, 1, 0x07001004, 0x4002)
    graph = graph.replace(original, words(0xB0000002, 0xE0000002, 1,
                                          0x07001004, 0x4002))
    sg_props = [(0x0800100E, words(1)), (0x0800100F, words(2)),
                (0x08001010, words(1))]
    old = record(0x08001001, words(2) + properties(0xB0000001, sg_props)
                 + properties(0xB0000002, sg_props))
    new = record(0x08001001, words(2) + properties(0xB0000001, [
        *sg_props, (0x080010CC, words(0))])
        + properties(0xB0000002, sg_props))
    graph = graph.replace(old, new)
    cont_props = [(0x08001011, words(1, 0x0B000001)),
                  (0x08001012, words(1)), (0x08001013, words(4096)),
                  (0x08001014, words(2))]
    old = record(0x08001000, words(1) + properties(0xE0000001, cont_props))
    new = record(0x08001000, words(2) + properties(0xE0000001, [
        *cont_props, (0x080010CB, words(0)), (0x08001174, words(1)),
        (0x0800124D, words(0xFFFFFFFF))])
        + properties(0xE0000002, cont_props))
    source["graph_open"] = graph.replace(old, new).hex()
    return source


def fanout_fixture():
    source = fixture()
    graph = bytes.fromhex(source["graph_open"])
    group = words(1, 0xB0000001, 0xE0000001, 10)
    group += words(0x07001011, 0x4001)
    ports = words(10) + properties(0x4001, [(0x08001015, words(1, 9))])
    for iid in range(0x4002, 0x400B):
        group += words(0x07001002, iid)
        ports += properties(iid, [(0x08001015, words(1, 1))])
    old_group = words(1, 0xB0000001, 0xE0000001, 2)
    old_group += words(0x07001005, 0x4001, 0x07001004, 0x4002)
    old_ports = words(2) + b"".join(properties(iid, [
        (0x08001015, words(1, 1))]) for iid in (0x4001, 0x4002))
    graph = graph.replace(record(0x08001002, old_group),
                          record(0x08001002, group))
    graph = graph.replace(record(0x08001003, old_ports),
                          record(0x08001003, ports))
    links = words(9) + b"".join(words(0x4001, 1 + i * 2, 0x4002 + i, 2)
                              for i in range(9))
    graph = graph.replace(record(0x08001004, words(1, 0x4001, 1, 0x4002, 2)),
                          record(0x08001004, links))
    source["graph_open"] = graph.hex()
    return source


def host_fixture(module):
    source = fixture()
    previous = 0x07001005 if module in (0x07001000, 0x07001006) else 0x07001004
    source["graph_open"] = bytes.fromhex(source["graph_open"]).replace(
        words(previous), words(module)).hex()
    return source


def control_fixture():
    source = fixture()
    links = words(3)
    for source_iid, peer_iid, intent, heap in ((0x4001, 0x4002, 0x0800105D, 0),
                                             (0x4002, 0x4001, 0x0800105E, 1)):
        links += words(source_iid, 7, peer_iid, 8, 2)
        links += words(0x08001062, 8, 1, intent, 0x0800136F, 4, heap)
        if source_iid == 0x4001:
            links += words(source_iid, 9, peer_iid, 10, 1,
                           0x08001062, 8, 1, intent)
    source["graph_open"] += record(0x08001061, links).hex()
    return source


def voice_fixture(empty=False, multi=False):
    source = fixture(shared=multi)
    owners = [(0xB0000001, [(0x0800117B, 0x4001),
                          (0x08001177, 0x4002)])]
    if empty:
        owners = [(0xB0000001, [])]
    elif multi:
        owners = [(0xB0000001, [(0x0800117B, 0x4001)]),
                  (0xB0000002, [(0x08001177, 0x4002)])]
    for sg, pairs in owners:
        tags = words(len(pairs)) + b"".join(words(*p) for p in pairs)
        source["graph_open"] += record(0x08001162, words(1) + properties(
            sg, [(0x080011B2, tags)]), target=4).hex()
    return source


def logger_fixture():
    source = fixture()
    source["graph_open"] = bytes.fromhex(source["graph_open"]).replace(
        words(0x07001005), words(0x0700101A)).hex()
    return source


def hardware_fixture(family):
    source = fixture()
    usb = family.startswith("USB")
    playback = family in ("MI2S", "CODEC_DMA_PLAYBACK", "USB_PLAYBACK", "DP")
    iid = 0x4002 if playback else 0x4001
    mid = (0x07001069 if family == "DP" else
           (0x0700104F if playback else 0x07001050) if usb else
           0x07001023 if family == "CODEC_DMA_PLAYBACK" else
           0x0700100A if playback else
           0x0700102A if family == "SLIM" else 0x07001024)
    old = 0x07001004 if playback else 0x07001005
    graph = bytes.fromhex(source["graph_open"])
    graph = graph.replace(words(old), words(mid))
    if not playback:
        graph = graph.replace(properties(0xB0000001, [
            (0x0800100E, words(1)), (0x0800100F, words(2)),
            (0x08001010, words(1))]), properties(0xB0000001, [
            (0x0800100E, words(1)), (0x0800100F, words(1)),
            (0x08001010, words(2))]))
    graph = graph.replace(properties(iid, [(0x08001015, words(1, 1))]),
                          properties(iid, [(0x08001015,
                                           words(1, 0) if playback
                                           else words(0, 1))]))
    source["graph_open"] = graph.hex()
    source["graph_keys"] = ([["0xa2000000", "0xa2000001"]] if playback else
                            [["0xa3000000", "0xa3000007"],
                             ["0xad000000", "0xad000001"]])
    if family == "SLIM":
        source["graph_keys"] = [["0xa3000000", "0xa3000009"]]
    if usb:
        source["graph_keys"] = ([["0xa2000000", "0xa2000009"]] if playback
                                else [["0xa3000000", "0xa300000a"]])
    if family == "DP":
        source["graph_keys"] = [["0xa2000000", "0xa200000a"]]
    tag = 0xC0000004 if playback else 0xC0000005
    interface = (record(0x08001019, words(0, 0, 0x00010002), target=iid)
                 if family == "MI2S" else
                 record(0x08001063, words(3, 1, 1), target=iid))
    defaults = record(0x08001017, words(48000, 0x00020010, 1), target=iid)
    if family == "SLIM":
        interface = record(0x08001068, words(0, 0x2221, 0), target=iid)
        defaults = record(0x08001018, words(1), target=iid)
    if usb or family == "DP":
        defaults = record(0x08001018, words(1), target=iid)
    source["endpoint_queries"] = {
        "tags": {"present": True, "count": 1,
                 "data": words(tag, 1, mid, iid).hex()},
        "tag_keys": {"present": True, "count": 1,
                     "data": words(tag, 1, 0xA9000000, 1).hex()},
        "hardware_tag_data": [{"variant": 0, "queried": True,
                               "present": True, "data": interface.hex()}],
        "empty_ckv_defaults": {"present": True, "modules": [iid],
            "data": defaults.hex()},
    }
    if usb or family == "DP":
        source["endpoint_queries"]["tag_keys"] = {
            "present": True, "count": 0, "data": ""}
        source["endpoint_queries"]["hardware_tag_data"] = []
    return source


def corpus_fixtures():
    together = fixture(shared=True)
    left = fixture(source_only=True)
    right = fixture(source_only=True)
    link = record(0x08001004, words(1, 0x4001, 1, 0x4002, 2))
    graph = bytes.fromhex(right["graph_open"]).replace(link, b"")
    graph = graph.replace(words(0x4001), words(0x4002))
    graph = graph.replace(words(0x07001005), words(0x07001004))
    graph = graph.replace(words(0xB0000001), words(0xB0000002))
    right["graph_open"] = (graph + link).hex()
    right["subgraph_ids"] = ["0xb0000002"]
    right["subgraph_connections"] = words(0xB0000002, 0).hex()
    control = record(0x08001061, words(1, 0x4002, 7, 0x4001, 8, 2,
        0x08001062, 8, 1, 0x0800105D, 0x0800136F, 4, 0))
    for source in (together, left, right):
        source["graph_open"] += control.hex()
    left["graph_connections"] = link.hex()
    return [together, left, right]


def widgets(data, kinds=None, blocks=None):
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
            widget_size, widget_type = struct.unpack_from("<2I", data,
                                                          position)
            name = data[position + 8:position + 52].split(b"\0")[0]
            private_size, = struct.unpack_from("<I", data,
                                               position + widget_size - 4)
            position += widget_size
            private_end = position + private_size
            groups = []
            while position < private_end:
                length, tag, elements = struct.unpack_from("<3I", data,
                                                          position)
                if tag == 4:
                    assert length == 12 + elements * 8
                    groups.append(dict(struct.iter_unpack(
                        "<2I", data[position + 12:position + length])))
                else:
                    assert tag in (0x08001061, 0x080011B2)
                    if blocks is not None:
                        blocks.setdefault(name.decode(), []).append(
                            data[position:position + length])
                position += length
            assert position == private_end
            result[name.decode()] = groups
            if kinds is not None:
                kinds[name.decode()] = widget_type
        assert position == end
    return result


def append_modules(source, modules):
    group = words(1, 0xB0000001, 0xE0000001, len(modules))
    ports = words(len(modules))

    for mid, iid in modules:
        group += words(mid, iid)
        ports += properties(iid, [(0x08001015, words(1, 1))])

    source["graph_open"] += (record(0x08001002, group)
                             + record(0x08001003, ports)).hex()


class CompilerTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / "source.json"
        self.report = self.root / "report.json"
        self.output = self.root / "topology.conf"

    def run_compiler(self, source, *options):
        self.source.write_text(json.dumps(source))
        return subprocess.run([
            str(COMPILER), "--input", str(self.source), "--graph-id", "16",
            "--report", str(self.report), "--output", str(self.output),
            *options],
            capture_output=True, text=True)

    def run_exports(self, sources, *options):
        directory = Path(tempfile.mkdtemp(dir=self.root))

        for index, source in enumerate(sources):
            (directory / f"{index}.json").write_text(json.dumps(source))

        return subprocess.run([
            str(COMPILER), "--exports", str(directory), "--report",
            str(self.report), *options], capture_output=True, text=True)

    def test_corpus_keeps_graph_selection_separate_from_references(self):
        sources = corpus_fixtures()
        completed = self.run_exports(sources, "--corpus", "--graph-id", "16",
                                      "--output", str(self.output))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        report = json.loads(self.report.read_text())
        self.assertEqual(reconstruct(report["factored"]), sources)
        corpus = report["corpus"]
        self.assertTrue(corpus["can_emit"])
        self.assertFalse(corpus["runtime_bindings"])
        self.assertEqual([s["graphs"] for s in corpus["selections"]],
                         [[16, 17], [16], [17]])
        self.assertEqual([len(s["references"]) for s in corpus["selections"]],
                         [2, 3, 2])
        self.assertEqual(len(corpus["references"]), 3)
        decoded = self.read_topology()
        self.assertEqual(set(decoded), {"m16385", "m16386"})
        self.assertEqual([decoded[m][0][2] for m in sorted(decoded)],
                         [0xB0000001, 0xB0000002])
        self.assertEqual([decoded[m][0][1] for m in sorted(decoded)], [16, 17])
        self.assertTrue(all(209 not in v[2] for v in decoded.values()))
        self.assertEqual(self.output.read_text().count("SectionControlMixer"),
                         3)

        changed = corpus_fixtures()
        changed[1]["graph_open"] = changed[1]["graph_open"].replace(
            words(0x07001005).hex(), words(0x07001002).hex())
        self.output.write_text("retain")
        completed = self.run_exports(changed, "--corpus", "--output",
                                      str(self.output))
        self.assertEqual(completed.returncode, 2, completed.stderr)
        self.assertEqual(self.output.read_text(), "retain")
        problems = json.loads(self.report.read_text())["corpus"]["issues"]
        self.assertIn("compiler-corpus-module-owner",
                      {p["kind"] for p in problems})

        changed = fixture()
        changed["graph_open"] = changed["graph_open"].replace(
            words(0x4001, 1, 0x4002, 2).hex(),
            words(0x4001, 3, 0x4002, 2).hex())
        completed = self.run_exports([fixture(), changed], "--corpus",
                                      "--output", str(self.output))
        self.assertEqual(completed.returncode, 2, completed.stderr)
        problems = json.loads(self.report.read_text())["corpus"]["issues"]
        self.assertIn("compiler-corpus-subgraph-variant",
                      {p["kind"] for p in problems})
        subgraph = json.loads(self.report.read_text())["corpus"]["subgraphs"][0]
        self.assertFalse(subgraph["can_emit"])
        self.assertEqual(self.output.read_text(), "retain")

    def read_topology(self, kinds=None, blocks=None):
        if not shutil.which("alsatplg"):
            self.skipTest("alsatplg is not installed")
        binary = self.root / "topology.bin"
        subprocess.run(["alsatplg", "-c", str(self.output), "-o", str(binary)],
                       check=True, capture_output=True)
        return widgets(binary.read_bytes(), kinds, blocks)

    def test_conventional_topology_preserves_declarations(self):
        source = fixture()
        completed = self.run_compiler(source)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(self.report.read_text())["graphs"][0]
                         ["source"], source)
        decoded = self.read_topology()
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

    def test_optional_open_properties_preserve_presence_and_values(self):
        optional = [(0x080010CB, words(0)),
                    (0x08001174, words(0xFFFFFFFF)),
                    (0x0800124D, words(1))]
        for values, vsid in ((optional, 0), (optional[1:], 0xFFFFFFFF)):
            with self.subTest(vsid=vsid):
                source = fixture(container_properties=values, vsid=vsid)
                completed = self.run_compiler(source)
                self.assertEqual(completed.returncode, 0, completed.stderr)
                decoded = self.read_topology()
                sg, container, _ = decoded["m16385"]
                self.assertEqual(sg[6], vsid)
                expected = {106: 0xFFFFFFFF, 107: 1}
                if len(values) == 3:
                    expected[105] = 0
                self.assertEqual({k: v for k, v in container.items()
                                  if k in (105, 106, 107)}, expected)

        source["graph_open"] = source["graph_open"].replace(
            words(0x08001014).hex(), words(0x08009998).hex())
        completed = self.run_compiler(source)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        issues = json.loads(self.report.read_text())["graphs"][0]["issues"]
        self.assertIn("missing-properties", {i["kind"] for i in issues})

    def test_generic_widgets_preserve_wide_port_capacities(self):
        source = generic_fixture()
        completed = self.run_compiler(source)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        report = json.loads(self.report.read_text())["graphs"][0]
        self.assertEqual(report["source"], source)
        self.assertFalse(report["runtime_bindings"])
        kinds = {}
        decoded = self.read_topology(kinds)
        self.assertEqual(kinds, {"m16385": 18, "m16386": 18})
        self.assertEqual(decoded["m16385"][2][203], 18)
        self.assertEqual(decoded["m16385"][2][206], 23)
        self.assertEqual(decoded["m16386"][2][200], 0x07001002)

    def test_shared_memory_widgets_preserve_endpoint_direction(self):
        for module in (0x07001000, 0x07001001, 0x07001006, 0x07001007):
            with self.subTest(module=hex(module)):
                source = host_fixture(module)
                completed = self.run_compiler(source)
                self.assertEqual(completed.returncode, 0, completed.stderr)
                kinds = {}
                decoded = self.read_topology(kinds)
                if module in (0x07001000, 0x07001006):
                    self.assertEqual(kinds, {"m16385": 11, "m16386": 22})
                    self.assertEqual(decoded["m16385"][2][200], module)
                else:
                    self.assertEqual(kinds, {"m16385": 23, "m16386": 12})
                    self.assertEqual(decoded["m16386"][2][200], module)

    def test_fanout_does_not_consume_additional_output_ports(self):
        source = fixture()
        append_modules(source, [(0x07001002, 0x5001)])
        source["graph_connections"] = record(
            0x08001004, words(1, 0x4001, 1, 0x5001, 2)).hex()
        completed = self.run_compiler(source)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        decoded = self.read_topology()
        module = decoded["m16385"][2]
        self.assertEqual(module[203], 1)
        self.assertEqual((module[206], module[210]), (1, 1))
        self.assertEqual((module[209], module[212]), (0x4002, 0x5001))

        source["graph_connections"] = record(
            0x08001004, words(1, 0x4001, 3, 0x5001, 2)).hex()
        completed = self.run_compiler(source)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        report = json.loads(self.report.read_text())["graphs"][0]
        self.assertIn("port-capacity", {i["kind"] for i in report["issues"]})

    def test_connections_extend_beyond_legacy_tuple_slots(self):
        source = fanout_fixture()
        completed = self.run_compiler(source)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        decoded = self.read_topology()
        groups = decoded["m16385"]
        self.assertEqual(groups[2][203], 9)
        self.assertEqual(groups[2][230], 0x4009)
        self.assertEqual(groups[3], {206: 17, 207: 2, 209: 0x400A})
        self.assertEqual(len(decoded), 10)

    def test_control_links_keep_peers_intents_and_heap_properties(self):
        source = control_fixture()
        completed = self.run_compiler(source)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        report = json.loads(self.report.read_text())["graphs"][0]
        self.assertEqual(report["source"], source)
        self.assertEqual(report["control_links"], 3)
        blocks = {}
        self.read_topology(blocks=blocks)
        for iid, peer, intent, heap in ((0x4001, 0x4002, 0x0800105D, 0),
                                        (0x4002, 0x4001, 0x0800105E, 1)):
            block, = blocks[f"m{iid}"]
            payload = words(iid, 7, peer, 8, 2, 0x08001062, 8, 1, intent,
                            0x0800136F, 4, heap)
            count = 1
            if iid == 0x4001:
                payload += words(iid, 9, peer, 10, 1,
                                 0x08001062, 8, 1, intent)
                count += 1
            self.assertEqual(block, words(12 + len(payload), 0x08001061,
                                           count) + payload)

    def test_voice_tags_retain_subgraph_ownership_and_empty_lists(self):
        for empty, multi in ((False, False), (True, False), (False, True)):
            with self.subTest(empty=empty, multi=multi):
                source = voice_fixture(empty=empty, multi=multi)
                completed = self.run_compiler(source)
                self.assertEqual(completed.returncode, 0, completed.stderr)
                blocks = {}
                self.read_topology(blocks=blocks)
                block, = blocks["m16385"]
                payload = words(0) if empty else words(1 if multi else 2,
                    0x0800117B, 0x4001)
                if not empty and not multi:
                    payload += words(0x08001177, 0x4002)
                self.assertEqual(block, words(8 + len(payload), 0x080011B2)
                                 + payload)
                if multi:
                    self.assertEqual(blocks["m16386"], [words(20,
                        0x080011B2, 1, 0x08001177, 0x4002)])

        source = voice_fixture(multi=True)
        source["graph_open"] = source["graph_open"].replace(
            words(0x08001177, 0x4002).hex(), words(0x08001177, 0x4001).hex())
        completed = self.run_compiler(source)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        report = json.loads(self.report.read_text())["graphs"][0]
        self.assertIn("voice-module-reference",
                      {i["kind"] for i in report["issues"]})

    def test_logger_declaration_does_not_invent_calibration(self):
        source = logger_fixture()
        completed = self.run_compiler(source)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        kinds = {}
        decoded = self.read_topology(kinds)
        self.assertEqual(kinds["m16385"], 16)
        self.assertEqual(decoded["m16385"][2][200], 0x0700101A)
        self.assertTrue({259, 260, 261}.isdisjoint(decoded["m16385"][2]))
        report = json.loads(self.report.read_text())["graphs"][0]
        self.assertEqual(report["source"], source)
        self.assertFalse(report["runtime_bindings"])

    def test_shared_container_is_not_split_or_renumbered(self):
        source = fixture(shared=True)
        completed = self.run_compiler(source)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        report = json.loads(self.report.read_text())["graphs"][0]
        self.assertEqual(report["source"], source)
        self.assertFalse(report["issues"])
        decoded = self.read_topology()
        for iid, sg in ((0x4001, 0xB0000001), (0x4002, 0xB0000002)):
            groups = decoded[f"m{iid}"]
            self.assertEqual(groups[0][2], sg)
            self.assertEqual(groups[1][100], 0xE0000001)

    def test_factoring_preserves_choices_variants_and_opaque_records(self):
        first = fixture(shared=True)
        second = fixture(shared=True, extra=True)
        second["graph_keys"] = [["0xa2000000", "0xa2000001"]]
        control = words(0x4001, 7, 0x4002, 8, 2)
        control += words(0x08001062, 12, 0, 1, 0x0800105D)
        control += words(0x0800136F, 4, 2)
        first["graph_connections"] = record(
            0x08001061, words(1) + control[:-4] + words(1)).hex()
        opaque = words(4, 0x0800FFFF, 3, 0) + b"xyz" + b"abcde"
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

    def test_endpoint_roles_preserve_modes_tags_and_parameters(self):
        hostless = fixture()
        source = fixture()
        append_modules(source, [(0x07001000, 0x5001), (0x07001001, 0x5002),
                                (0x0700100A, 0x5003), (0x07001024, 0x5004),
                                (0x07001010, 0x5005), (0x07001041, 0x5006)])
        tags = words(0xC0000004, 3, 0x0700100A, 0x5003,
                     0x07001010, 0x5005, 0x07001041, 0x5006)
        variants = words(0xC0000004, 1, 0xA9000000, 2)
        defaults = words(0x5003, 0x08001019, 12, 0) + words(0, 0, 2) + b"pad!"
        payload = words(0x5003, 0x08001019, 12, 0) + words(0, 1, 2) + bytes(4)
        source["endpoint_queries"] = {
            "tags": {"present": True, "count": 1, "data": tags.hex()},
            "tag_keys": {"present": True, "count": 1,
                         "data": variants.hex()},
            "hardware_tag_data": [{"variant": 0, "present": True,
                                   "data": payload.hex()}],
            "empty_ckv_defaults": {"modules": [0x5003, 0x5005, 0x5006],
                                   "present": True, "data": defaults.hex()}}
        ambiguous = json.loads(json.dumps(source))
        append_modules(ambiguous, [(0x07001006, 0x5007)])
        sources = [hostless, source, ambiguous]
        completed = self.run_exports(sources)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        result = json.loads(self.report.read_text())
        factored = result["factored"]
        self.assertEqual(reconstruct(factored), sources)
        issues = result["graphs"][1]["issues"]
        self.assertEqual({issue["object"] for issue in issues},
                         {0x5003, 0x5004})
        self.assertTrue(all(issue["kind"] == "compiler-module-config" and
                            issue["scope"] == "compiler-coverage"
                            for issue in issues))
        check_endpoints(factored, sources)
        self.assertEqual(factored["selections"][0]["endpoints"]["host"]
                         ["playback"]["resolution"], "none")
        self.assertEqual(factored["selections"][2]["endpoints"]["host"]
                         ["playback"]["resolution"], "ambiguous")
        self.assertEqual([e["module"] for e in factored["selections"][1]
                          ["endpoints"]["physical"]], [0x5003, 0x5004])

        source["endpoint_queries"]["tags"]["data"] = tags[:-1].hex()
        completed = self.run_compiler(source)
        self.assertEqual(completed.returncode, 1, completed.stderr)
        self.assertFalse(self.output.exists())

    def test_haptics_backend_follows_resource_manager_setting(self):
        dtc = shutil.which("dtc") or str(COMPILER.with_name("dtc"))
        if not Path(dtc).is_file():
            self.skipTest("dtc is not installed")

        dts = self.root / "board.dts"
        dtb = self.root / "board.dtb"
        dts.write_text('''/dts-v1/;
            / {
                backend: dais {
                    compatible = "qcom,q6apm-lpass-dais";
                    #sound-dai-cells = <1>;
                };
                codec-bus {
                    codec0: codec { #sound-dai-cells = <0>; };
                };
                platform0: platform { #sound-dai-cells = <0>; };
                sound {
                    haptics {
                        link-name = "Haptics Playback";
                        cpu { sound-dai = <&backend 125>; };
                        codec { sound-dai = <&codec0>; };
                        platform { sound-dai = <&platform0>; };
                    };
                };
            };
        ''')
        subprocess.run([dtc, "-I", "dts", "-O", "dtb", "-o", str(dtb),
                        str(dts)], check=True, capture_output=True)
        rm = self.root / "resources.xml"
        xml = '''<resource_manager_info><config_params>
            <param key="haptics_through_wsa" value="false"/>
            </config_params><device_profile><out-device>
                <id>PAL_DEVICE_OUT_HAPTICS_DEVICE</id>
                <back_end_name>CODEC_DMA-LPAIF_RXTX-RX-6</back_end_name>
                <usecase><name>PAL_STREAM_HAPTICS</name>
                    <custom-config key="haptics-over-wsa">
                        <backend_name>CODEC_DMA-LPAIF_WSA2-RX-0</backend_name>
                    </custom-config>
                </usecase>
            </out-device></device_profile></resource_manager_info>'''
        rm.write_text(xml)
        kv = self.root / "usecases.xml"
        kv.write_text('''<graph_key_value_pair_info><devices>
            <device id="PAL_DEVICE_OUT_HAPTICS_DEVICE"><keys_and_values>
                <graph_kv key="0xA2000000" value="0xA2000001"/>
            </keys_and_values></device>
            </devices><devicepps/></graph_key_value_pair_info>''')
        source = hardware_fixture("CODEC_DMA_PLAYBACK")
        options = ["--resource-manager", str(rm), "--usecase-kv", str(kv),
                   "--board-dtb", str(dtb), "--device",
                   "PAL_DEVICE_OUT_HAPTICS_DEVICE"]
        completed = self.run_compiler(source, *options)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.read_topology()
        report = json.loads(self.report.read_text())
        self.assertEqual(report["factored"]["selections"][0]["endpoints"]
                         ["physical"][0]["binding"]["dai"], 125)

        original_dts = dts.read_text()
        for target in ("codec { sound-dai = <&codec0>; };",
                       "platform { sound-dai = <&platform0>; };",
                       "codec-bus {"):
            with self.subTest(target=target):
                replacement = (target + ' status = "disabled";'
                               if target == "codec-bus {" else "")
                dts.write_text(original_dts.replace(target, replacement))
                subprocess.run([dtc, "-I", "dts", "-O", "dtb", "-o", str(dtb),
                                str(dts)], check=True, capture_output=True)
                self.output.write_text("retain")
                completed = self.run_compiler(source, *options)
                self.assertEqual(completed.returncode, 2, completed.stderr)
                self.assertEqual(self.output.read_text(), "retain")

        dts.write_text(original_dts)
        subprocess.run([dtc, "-I", "dts", "-O", "dtb", "-o", str(dtb),
                        str(dts)], check=True, capture_output=True)
        rm.write_text(xml.replace('value="false"', 'value="true"'))
        self.output.write_text("retain")
        completed = self.run_compiler(source, *options)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        self.assertEqual(self.output.read_text(), "retain")
        report = json.loads(self.report.read_text())
        profile = report["board"]["profiles"][0]
        self.assertEqual(profile["backend"], "CODEC_DMA-LPAIF_WSA2-RX-0")
        self.assertEqual(profile["declared_backend"],
                         "CODEC_DMA-LPAIF_RXTX-RX-6")
        self.assertIn('<custom-config key="haptics-over-wsa">', profile["xml"])
        self.assertEqual(reconstruct(report["factored"]), [source])

    def test_board_candidates_preserve_qualifiers_and_mismatches(self):
        dtc = shutil.which("dtc") or str(COMPILER.with_name("dtc"))
        if not Path(dtc).is_file():
            self.skipTest("dtc is not installed")
        dts = self.root / "board.dts"
        dtb = self.root / "board.dtb"
        dts.write_text('''/dts-v1/;
            / {
                backend: dais {
                    compatible = "qcom,q6apm-lpass-dais";
                    #sound-dai-cells = <1>;
                };
                codec0: codec-a { #sound-dai-cells = <1>; };
                codec1: codec-b { #sound-dai-cells = <0>; };
                platform0: platform { #sound-dai-cells = <0>; };
                sound {
                    speaker {
                        link-name = "Speaker Playback";
                        cpu { sound-dai = <&backend 16>; };
                        codec { sound-dai = <&codec0 0>; };
                        platform { sound-dai = <&platform0>; };
                    };
                    capture {
                        link-name = "WCD Capture";
                        cpu { sound-dai = <&backend 120>; };
                        codec { sound-dai = <&codec0 1>, <&codec1>; };
                        platform { sound-dai = <&platform0>; };
                    };
                    bt {
                        link-name = "BT Capture";
                        cpu { sound-dai = <&backend 15>; };
                        codec { sound-dai = <&codec0 0>; };
                        platform { sound-dai = <&platform0>; };
                    };
                    usb_rx {
                        link-name = "USB Playback";
                        cpu { sound-dai = <&backend 136>; };
                        codec { sound-dai = <&codec0 0>; };
                        platform { sound-dai = <&platform0>; };
                    };
                    usb_tx {
                        link-name = "USB Capture";
                        cpu { sound-dai = <&backend 153>; };
                        codec { sound-dai = <&codec0 0>; };
                        platform { sound-dai = <&platform0>; };
                    };
                    dp {
                        link-name = "DP Playback";
                        cpu { sound-dai = <&backend 131>; };
                        codec { sound-dai = <&codec0 0>; };
                        platform { sound-dai = <&platform0>; };
                    };
                    disabled {
                        status = "disabled";
                        link-name = "Unused Playback";
                        cpu { sound-dai = <&backend 16>; };
                        codec { sound-dai = <&codec0 0>; };
                        platform { sound-dai = <&platform0>; };
                    };
                };
            };
        ''')
        subprocess.run([dtc, "-I", "dts", "-O", "dtb", "-o", str(dtb),
                        str(dts)], check=True, capture_output=True)
        rm = self.root / "resources.xml"
        rm.write_text('''<resource_manager_info><device_profile>
            <out-device><id>OUT</id>
                <back_end_name>MI2S-LPAIF-RX-PRIMARY</back_end_name>
                <samplerate>48000</samplerate>
                <usecase><name>VOICE</name>
                    <samplerate>16000</samplerate></usecase>
            </out-device>
            <in-device><id>IN</id>
                <back_end_name>CODEC_DMA-LPAIF_RXTX-TX-3</back_end_name>
            </in-device>
            <in-device><id>FEEDBACK</id>
                <back_end_name>MI2S-LPAIF-TX-PRIMARY</back_end_name>
            </in-device>
            <in-device><id>BTIN</id>
                <back_end_name>SLIM-DEV2-TX-7</back_end_name>
            </in-device>
            <in-device><id>USBIN</id>
                <back_end_name>USB_AUDIO-TX</back_end_name>
            </in-device>
            <out-device><id>USBOUT</id>
                <back_end_name>USB_AUDIO-RX</back_end_name>
            </out-device>
            <out-device><id>DPOUT</id>
                <back_end_name>DISPLAY_PORT-RX</back_end_name>
            </out-device>
            </device_profile></resource_manager_info>''')
        kv = self.root / "usecases.xml"
        kv.write_text('''<graph_key_value_pair_info>
            <devices><device id="OUT"><keys_and_values>
                <graph_kv key="0xA2000000" value="0xA2000001"/>
            </keys_and_values></device>
            <device id="USBOUT"><keys_and_values>
                <graph_kv key="0xA2000000" value="0xA2000009"/>
            </keys_and_values></device>
            <device id="DPOUT"><keys_and_values>
                <graph_kv key="0xA2000000" value="0xA200000A"/>
            </keys_and_values></device></devices>
            <devicepps><devicepp id="IN, FEEDBACK, ABSENT">
                <keys_and_values StreamType="VOICE,SENSOR" DevicePPType="MIC">
                    <graph_kv key="0xA3000000" value="0xA3000007"/>
                    <graph_kv key="0xAD000000" value="0xAD000001"/>
                </keys_and_values>
            </devicepp><devicepp id="BTIN"><keys_and_values>
                <graph_kv key="0xA3000000" value="0xA3000009"/>
            </keys_and_values></devicepp><devicepp id="USBIN"><keys_and_values>
                <graph_kv key="0xA3000000" value="0xA300000A"/>
            </keys_and_values></devicepp></devicepps>
            </graph_key_value_pair_info>''')
        source = fixture()
        source["graph_keys"] = [["0xa2000000", "0xa2000001"],
                                ["0xa3000000", "0xa3000007"],
                                ["0xad000000", "0xad000001"]]
        append_modules(source, [(0x0700100A, 0x5001), (0x07001024, 0x5002)])
        options = ["--resource-manager", str(rm), "--usecase-kv", str(kv),
                   "--board-dtb", str(dtb)]
        completed = self.run_compiler(source, *options)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        result = json.loads(self.report.read_text())
        self.assertEqual(reconstruct(result["factored"]), [source])
        check_endpoints(result["factored"], [source])
        board = result["board"]
        self.assertEqual([link["dai"] for link in board["links"]],
                         [16, 120, 15, 136, 153, 131])
        self.assertEqual(board["assignments"][3]["conditions"], {
            "StreamType": "VOICE,SENSOR", "DevicePPType": "MIC"})
        self.assertIn("<samplerate>16000</samplerate>",
                      board["profiles"][0]["xml"])
        endpoints = result["factored"]["selections"][0]["endpoints"]["physical"]
        self.assertEqual(endpoints[0]["board_candidates"], [{
            "assignment": 0, "profile": 0, "links": [0],
            "binding": None, "status": "candidate"}])
        candidates = endpoints[1]["board_candidates"]
        self.assertEqual([c["status"] for c in candidates],
                         ["candidate", "interface-mismatch",
                          "missing-device-profile"])

        source["graph_keys"].pop()
        completed = self.run_compiler(source, *options)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        endpoints = json.loads(self.report.read_text())["factored"]
        endpoints = endpoints["selections"][0]["endpoints"]["physical"]
        self.assertEqual(endpoints[1]["board_candidates"], [])

        source = hardware_fixture("SLIM")
        completed = self.run_compiler(source, *options, "--device", "BTIN")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        decoded = self.read_topology()
        self.assertEqual(decoded["m16385"][2][250], 1)
        self.assertNotIn(253, decoded["m16385"][2])
        report = json.loads(self.report.read_text())
        endpoint = report["factored"]["selections"][0]["endpoints"]
        endpoint = endpoint["physical"][0]
        self.assertEqual(endpoint["binding"]["dai"], 15)
        self.assertEqual(endpoint["binding"]["status"], "dai-configured")
        self.assertTrue(endpoint["binding"]["required_runtime_format"])
        self.assertEqual(endpoint["tag_parameters"][0]["parameters"][0]
                         ["payload"], words(0, 0x2221, 0).hex())
        original_dts = dts.read_text()
        dts.write_text(original_dts.replace("disabled {", '''extra_bt {
            link-name = "Other BT Capture";
            cpu { sound-dai = <&backend 13>; };
            codec { sound-dai = <&codec0 0>; };
            platform { sound-dai = <&platform0>; };
        };
        disabled {'''))
        subprocess.run([dtc, "-I", "dts", "-O", "dtb", "-o", str(dtb),
                        str(dts)], check=True, capture_output=True)
        completed = self.run_compiler(source, *options, "--device", "BTIN")
        self.assertEqual(completed.returncode, 2, completed.stderr)
        endpoint = json.loads(self.report.read_text())["factored"]
        endpoint = endpoint["selections"][0]["endpoints"]["physical"][0]
        self.assertEqual(endpoint["binding"]["status"],
                         "ambiguous-board-binding")
        dts.write_text(original_dts)
        subprocess.run([dtc, "-I", "dts", "-O", "dtb", "-o", str(dtb),
                        str(dts)], check=True, capture_output=True)

        for family, device, iid, dai in (("USB", "USBIN", "m16385", 153),
                                         ("USB_PLAYBACK", "USBOUT",
                                          "m16386", 136)):
            source = hardware_fixture(family)
            completed = self.run_compiler(source, *options, "--device", device)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            decoded = self.read_topology()
            self.assertTrue({250, 251, 253}.isdisjoint(decoded[iid][2]))
            endpoint = json.loads(self.report.read_text())["factored"]
            endpoint = endpoint["selections"][0]["endpoints"]["physical"][0]
            self.assertEqual(endpoint["binding"]["status"], "dai-configured")
            self.assertEqual(endpoint["binding"]["dai"], dai)
            self.assertTrue(endpoint["binding"]["required_runtime_format"])
            self.assertTrue(endpoint["binding"]["required_runtime_usb_token"])

        completed = self.run_compiler(hardware_fixture("USB"), *options,
                                      "--device", "USBOUT")
        self.assertEqual(completed.returncode, 2, completed.stderr)

        source = hardware_fixture("DP")
        completed = self.run_compiler(source, *options, "--device", "DPOUT")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        decoded = self.read_topology()
        self.assertTrue({250, 251, 253}.isdisjoint(decoded["m16386"][2]))
        endpoint = json.loads(self.report.read_text())["factored"]
        endpoint = endpoint["selections"][0]["endpoints"]["physical"][0]
        self.assertEqual(endpoint["binding"]["dai"], 131)
        self.assertEqual(endpoint["binding"]["status"], "dai-configured")

        source = hardware_fixture("MI2S")
        completed = self.run_compiler(source, *options, "--device", "OUT")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        decoded = self.read_topology()
        self.assertEqual({k: decoded["m16386"][2][k]
                          for k in (250, 251, 253, 256, 257)},
                         {250: 0, 251: 0, 253: 1, 256: 2, 257: 1})

        interface = source["endpoint_queries"]["hardware_tag_data"][0]
        interface["data"] += record(0x08001019, words(0, 0, 0x00010001),
                                     target=0x4002).hex()
        completed = self.run_compiler(source, *options, "--device", "OUT")
        self.assertEqual(completed.returncode, 2, completed.stderr)
        endpoint = json.loads(self.report.read_text())["factored"]
        endpoint = endpoint["selections"][0]["endpoints"]["physical"][0]
        self.assertEqual(endpoint["binding"]["status"],
                         "ambiguous-native-interface")

        source = hardware_fixture("CODEC_DMA")
        completed = self.run_compiler(source, *options, "--device", "IN")
        self.assertEqual(completed.returncode, 2, completed.stderr)
        qualified = ["--device", "IN", "--stream-type", "VOICE",
                     "--device-pp-type", "MIC"]
        completed = self.run_compiler(source, *options, *qualified)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        decoded = self.read_topology()
        self.assertEqual({k: decoded["m16385"][2][k] for k in (250, 251, 253)},
                         {250: 4, 251: 1, 253: 1})
        report = json.loads(self.report.read_text())
        endpoint = report["factored"]["selections"][0]["endpoints"]
        endpoint = endpoint["physical"][0]
        self.assertEqual(endpoint["binding"]["dai"], 120)
        self.assertEqual(endpoint["tag_parameters"][0]["parameters"][0]
                         ["payload"], words(3, 1, 1).hex())
        self.assertFalse(report["graphs"][0]["runtime_bindings"])
        kv.write_text(kv.read_text().replace(
            'DevicePPType="MIC"',
            'DevicePPType="MIC" CustomConfig="voice-mic"'))
        completed = self.run_compiler(source, *options, *qualified)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        completed = self.run_compiler(source, *options, *qualified,
                                      "--context", "CustomConfig=voice-mic")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        completed = self.run_compiler(source, *options, "--device", "FEEDBACK",
                                      "--stream-type", "VOICE",
                                      "--device-pp-type", "MIC",
                                      "--context", "CustomConfig=voice-mic")
        self.assertEqual(completed.returncode, 2, completed.stderr)

        sources = [hardware_fixture("CODEC_DMA"), hardware_fixture("SLIM")]
        renamed = json.dumps(sources[1])
        for old, new in ((0x4001, 0x4101), (0x4002, 0x4102),
                         (0xB0000001, 0xB0000002)):
            renamed = renamed.replace(words(old).hex(), words(new).hex())
        sources[1] = json.loads(renamed)
        sources[1]["subgraph_ids"] = ["0xb0000002"]
        contexts = {"version": 1, "selections": [
            {"graph_keys": list(reversed(sources[0]["graph_keys"])),
             "devices": ["IN"], "context": {
                 "StreamType": "VOICE", "DevicePPType": "MIC",
                 "CustomConfig": "voice-mic"}},
            {"graph_keys": list(sources[1]["graph_keys"]),
             "devices": ["BTIN"], "context": {}}]}
        context_file = self.root / "contexts.json"
        context_file.write_text(json.dumps(contexts))
        selected = [*options, "--selection-contexts", str(context_file)]
        completed = self.run_exports(sources, *selected)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        report = json.loads(self.report.read_text())
        self.assertEqual(reconstruct(report["factored"]), sources)
        endpoints = [s["endpoints"]["physical"][0]
                     for s in report["factored"]["selections"]]
        self.assertEqual([e["binding"]["dai"] for e in endpoints], [120, 15])
        self.assertTrue(all(not g["runtime_bindings"]
                            for g in report["graphs"]))
        completed = self.run_exports(sources, *selected, "--corpus",
                                      "--output", str(self.output))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        corpus = json.loads(self.report.read_text())["corpus"]
        self.assertEqual([s["graphs"] for s in corpus["selections"]],
                         [[0], [1]])
        self.assertEqual(len(self.read_topology()), 4)

        contexts["selections"][0]["context"]["CustomConfig"] = None
        context_file.write_text(json.dumps(contexts))
        completed = self.run_exports(sources, *selected)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        endpoint = json.loads(self.report.read_text())["factored"]
        endpoint = endpoint["selections"][0]["endpoints"]["physical"][0]
        self.assertEqual(endpoint["binding"]["status"],
                         "missing-board-binding")
        kv.write_text(kv.read_text().replace(' CustomConfig="voice-mic"', '')
            .replace('</devicepps>', '''<devicepp id="IN">
                <keys_and_values StreamType="VOICE" DevicePPType="MIC"
                                 CustomConfig="voice-mic">
                    <graph_kv key="0xA3000000" value="0xA3000007"/>
                    <graph_kv key="0xAD000000" value="0xAD000001"/>
                </keys_and_values>
            </devicepp></devicepps>'''))
        completed = self.run_exports(sources, *selected)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        endpoint = json.loads(self.report.read_text())["factored"]
        endpoint = endpoint["selections"][0]["endpoints"]["physical"][0]
        self.assertEqual(endpoint["binding"]["status"], "static-bound")

        contexts["selections"].pop(0)
        context_file.write_text(json.dumps(contexts))
        completed = self.run_exports(sources, *selected)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        report = json.loads(self.report.read_text())
        self.assertEqual(report["factored"]["selections"][0]["endpoints"]
                         ["physical"][0]["binding"]["status"],
                         "unresolved-board-candidate")
        completed = self.run_exports(sources, *selected, "--device", "IN")
        self.assertEqual(completed.returncode, 1, completed.stderr)
        contexts["selections"][0]["graph_keys"].pop()
        context_file.write_text(json.dumps(contexts))
        self.output.write_text("retain")
        completed = self.run_exports(sources, *selected, "--corpus",
                                      "--output", str(self.output))
        self.assertEqual(completed.returncode, 1, completed.stderr)
        self.assertIn("matches no source", completed.stderr)
        self.assertEqual(self.output.read_text(), "retain")


if __name__ == "__main__":
    unittest.main()
