# SPDX-License-Identifier: BSD-3-Clause
import struct

from ownership_checks import source_facts


HOSTS = {0x07001000: ("playback", "packet"),
         0x07001001: ("capture", "packet"),
         0x07001006: ("playback", "pull"),
         0x07001007: ("capture", "push")}
PHYSICAL = {0x0700100A: ("playback", "MI2S"),
            0x0700100B: ("capture", "MI2S"),
            0x07001023: ("playback", "CODEC_DMA"),
            0x07001024: ("capture", "CODEC_DMA"),
            0x07001029: ("playback", "SLIM"),
            0x0700102A: ("capture", "SLIM"),
            0x0700104F: ("playback", "USB_AUDIO"),
            0x07001050: ("capture", "USB_AUDIO"),
            0x07001069: ("playback", "DISPLAY_PORT")}


def tagged(query, modules):
    data = bytes.fromhex(query["data"])
    offset = 0
    result = []

    for _ in range(query["count"]):
        tag, count = struct.unpack_from("<2I", data, offset)
        offset += 8
        value = {"tag": tag, "modules" if modules else "keys": []}

        for _ in range(count):
            left, right = struct.unpack_from("<2I", data, offset)
            offset += 8
            if modules:
                value["modules"].append({"type": left, "module": right})
            else:
                value["keys"].append([left, right])

        result.append(value)

    assert offset == len(data)
    return result


def parameters(text, module):
    data = bytes.fromhex(text)
    offset = 0
    result = []

    while offset < len(data):
        iid, pid, size, error = struct.unpack_from("<4I", data, offset)
        assert error == 0
        end = offset + 16 + size
        aligned = end + -size % 8
        assert aligned <= len(data)
        if iid == module:
            result.append({"parameter": pid,
                           "payload": data[offset + 16:end].hex(),
                           "padding": data[end:aligned].hex()})
        offset = aligned

    assert offset == len(data)
    return result


def check_endpoints(factored, sources):
    assert len(factored["selections"]) == len(sources)

    for selection, source in zip(factored["selections"], sources):
        modules, _ = source_facts(source)
        endpoints = selection["endpoints"]
        queries = source.get("endpoint_queries")
        assert endpoints["native_queries_available"] == (queries is not None)
        assert endpoints["runtime_bindings"] is False

        for direction in ("playback", "capture"):
            expected = []
            for iid, (sg, container, mid) in sorted(modules.items()):
                if mid in HOSTS and HOSTS[mid][0] == direction:
                    expected.append({"module": iid, "type": mid,
                                     "subgraph": sg, "container": container,
                                     "direction": direction,
                                     "mode": HOSTS[mid][1]})
            host = endpoints["host"][direction]
            assert host["candidates"] == expected
            assert host["resolution"] == ("none" if not expected else
                "unique" if len(expected) == 1 else "ambiguous")

        backends = {iid: owner for iid, owner in modules.items()
                    if owner[2] in PHYSICAL}
        assert {p["module"] for p in endpoints["physical"]} == set(backends)

        if queries:
            assert endpoints["tags"] == tagged(queries["tags"], True)
            assert endpoints["tag_variants"] == tagged(queries["tag_keys"],
                                                        False)

        for endpoint in endpoints["physical"]:
            iid = endpoint["module"]
            sg, container, mid = backends[iid]
            assert (endpoint["subgraph"], endpoint["container"],
                    endpoint["type"]) == (sg, container, mid)
            assert (endpoint["direction"], endpoint["family"]) == PHYSICAL[mid]
            defaults = []
            variants = []

            if queries:
                defaults = parameters(queries["empty_ckv_defaults"]["data"],
                                      iid)
                for data in queries["hardware_tag_data"]:
                    values = parameters(data["data"], iid)
                    if values:
                        variants.append({"variant": data["variant"],
                                         "parameters": values})

            assert endpoint["default_parameters"] == defaults
            assert endpoint["tag_parameters"] == variants
            assert all(c["binding"] is None
                       for c in endpoint["board_candidates"])


def check_native_reference(source, reference):
    assert source["graph_keys"] == reference["keys"]
    queries = source["endpoint_queries"]
    assert queries["tags"]["count"] == reference["tag_count"]
    assert queries["tags"]["data"] == reference["tags"]
    variants = tagged(queries["tag_keys"], False)
    actual = []

    for entry in queries["hardware_tag_data"]:
        if entry["present"]:
            variant = variants[entry["variant"]]
            actual.append(dict(variant, records=entry["data"]))

    assert actual == reference["endpoint_data"]

    for entry in reference["empty_ckv_defaults"]:
        assert entry["status"] == 0
        assert parameters(queries["empty_ckv_defaults"]["data"],
                          entry["module"]) == parameters(entry["records"],
                                                         entry["module"])
