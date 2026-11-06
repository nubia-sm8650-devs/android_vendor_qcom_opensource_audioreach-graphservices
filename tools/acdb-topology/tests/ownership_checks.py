# SPDX-License-Identifier: BSD-3-Clause
import struct

from reconstruction import entry


def source_facts(source):
    modules = {}
    links = []

    for buffer in ("graph_open", "graph_connections"):
        data = bytes.fromhex(source[buffer])
        position = 0
        record = 0

        while position < len(data):
            target, parameter, size, error = struct.unpack_from(
                "<4I", data, position)
            end = position + 16 + size
            cursor = position + 16
            assert error == 0 and end <= len(data)

            if target == 1 and parameter == 0x08001002:
                groups, = struct.unpack_from("<I", data, cursor)
                cursor += 4

                for _ in range(groups):
                    sg, container, count = struct.unpack_from("<3I", data,
                                                              cursor)
                    cursor += 12

                    for _ in range(count):
                        mid, iid = struct.unpack_from("<2I", data, cursor)
                        cursor += 8
                        assert iid not in modules
                        modules[iid] = (sg, container, mid)

                assert cursor == end

            if target == 1 and parameter in (0x08001004, 0x08001061):
                control = parameter == 0x08001061
                kind = "control_links" if control else "data_links"
                count, = struct.unpack_from("<I", data, cursor)
                cursor += 4

                for item in range(count):
                    start = cursor
                    peers = struct.unpack_from("<4I", data, cursor)
                    cursor += 16

                    if control:
                        properties, = struct.unpack_from("<I", data, cursor)
                        cursor += 4

                        for _ in range(properties):
                            _, length = struct.unpack_from("<2I", data,
                                                           cursor)
                            cursor += 8 + length

                    links.append((buffer, record, item, kind, peers,
                                  data[start:cursor]))

                assert cursor == end

            position = end + -size % 8
            record += 1

        assert position == len(data)

    return modules, links


def owner_json(owner):
    return dict(zip(("subgraph", "container", "type"), owner))


def check_requirements(factored, sources):
    facts = [source_facts(source) for source in sources]
    corpus = {}

    for modules, _ in facts:
        for iid, owner in modules.items():
            corpus.setdefault(iid, set()).add(owner)

    assert factored["module_ownership"] == {
        str(iid): [owner_json(owner) for owner in sorted(owners)]
        for iid, owners in corpus.items()}
    assert len(factored["selections"]) == len(sources)

    for selection, source, (modules, links) in zip(
            factored["selections"], sources, facts):
        values = selection["connection_requirements"]
        assert len(values) == len(links)
        selected_sgs = {int(s, 0) for s in source["subgraph_ids"]}

        for value, (buffer, record, item, kind, peers, raw) in zip(values,
                                                                  links):
            assert (value["buffer"], value["record"], value["item"],
                    value["kind"]) == (buffer, record, item, kind)
            definition = factored["definitions"][kind][value["definition"]]
            assert entry(kind, definition) == raw
            owners = []

            for name, iid, port in (("source", peers[0], peers[1]),
                                    ("destination", peers[2], peers[3])):
                endpoint = value[name]
                assert endpoint["module"] == iid and endpoint["port"] == port
                assert endpoint["selected"] == (iid in modules)
                owner = modules.get(iid)
                candidates = corpus.get(iid, set())

                if owner is not None:
                    resolution = "selection"
                elif len(candidates) == 1:
                    owner = next(iter(candidates))
                    resolution = "corpus"
                else:
                    resolution = "ambiguous" if candidates else "unknown"

                assert endpoint["resolution"] == resolution
                assert endpoint["owner"] == (owner_json(owner)
                                              if owner is not None else None)
                owners.append(owner)

            unselected = sorted({iid for iid in (peers[0], peers[2])
                                 if iid not in modules})
            complete = all(owner is not None for owner in owners)
            assert value["ownership_complete"] == complete
            assert value["endpoints_selected"] == (not unselected)
            assert value["unselected_modules"] == unselected

            if not complete:
                assert value["required_subgraphs"] is None
                assert value["missing_subgraphs"] is None
                assert value["relation"] == "unresolved"
                continue

            required = sorted({owner[0] for owner in owners})
            assert value["required_subgraphs"] == required
            assert value["missing_subgraphs"] == sorted(
                set(required) - selected_sgs)
            left, right = owners
            relation = ("same-subgraph" if left[0] == right[0]
                        else "shared-container" if left[1] == right[1]
                        else "cross-container")
            assert value["relation"] == relation
