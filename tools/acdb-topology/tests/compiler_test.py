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


def record(parameter, payload):
    return (words(1, parameter, len(payload), 0) + payload
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


def widgets(data, kinds=None):
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
                assert tag == 4 and length == 12 + elements * 8
                groups.append(dict(struct.iter_unpack(
                    "<2I", data[position + 12:position + length])))
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

    def run_exports(self, sources):
        directory = Path(tempfile.mkdtemp(dir=self.root))

        for index, source in enumerate(sources):
            (directory / f"{index}.json").write_text(json.dumps(source))

        return subprocess.run([
            str(COMPILER), "--exports", str(directory), "--report",
            str(self.report)], capture_output=True, text=True)

    def read_topology(self, kinds=None):
        if not shutil.which("alsatplg"):
            self.skipTest("alsatplg is not installed")
        binary = self.root / "topology.bin"
        subprocess.run(["alsatplg", "-c", str(self.output), "-o", str(binary)],
                       check=True, capture_output=True)
        return widgets(binary.read_bytes(), kinds)

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
                sound {
                    speaker {
                        link-name = "Speaker Playback";
                        cpu { sound-dai = <&backend 16>; };
                    };
                    capture {
                        link-name = "WCD Capture";
                        cpu { sound-dai = <&backend 120>; };
                    };
                    disabled {
                        status = "disabled";
                        link-name = "Unused Playback";
                        cpu { sound-dai = <&backend 16>; };
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
            </device_profile></resource_manager_info>''')
        kv = self.root / "usecases.xml"
        kv.write_text('''<graph_key_value_pair_info>
            <devices><device id="OUT"><keys_and_values>
                <graph_kv key="0xA2000000" value="0xA2000001"/>
            </keys_and_values></device></devices>
            <devicepps><devicepp id="IN, FEEDBACK, ABSENT">
                <keys_and_values StreamType="VOICE,SENSOR" DevicePPType="MIC">
                    <graph_kv key="0xA3000000" value="0xA3000007"/>
                    <graph_kv key="0xAD000000" value="0xAD000001"/>
                </keys_and_values>
            </devicepp></devicepps></graph_key_value_pair_info>''')
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
        self.assertEqual([link["dai"] for link in board["links"]], [16, 120])
        self.assertEqual(board["assignments"][1]["conditions"], {
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


if __name__ == "__main__":
    unittest.main()
