# SPDX-License-Identifier: BSD-3-Clause
import struct


def words(*values):
    return struct.pack(f"<{len(values)}I", *values)


def properties(values):
    result = words(len(values))

    for value in values:
        payload = bytes.fromhex(value["payload"])
        result += words(value["id"], len(payload)) + payload

    return result


def entry(kind, value):
    if kind in ("subgraphs", "containers", "module_properties",
                "driver_properties"):
        return words(value["id"]) + properties(value["properties"])

    if kind == "module_groups":
        result = words(value["subgraph"], value["container"],
                       len(value["modules"]))

        for module in value["modules"]:
            result += words(module["type"], module["id"])

        return result

    if kind == "data_links":
        return words(*value["peers"])

    if kind == "control_links":
        return words(*value["peers"]) + properties(value["properties"])

    if kind == "adjacency":
        return (words(value["id"], len(value["peers"]))
                + words(*value["peers"]))

    raise AssertionError(f"Unknown definition kind {kind}")


def reconstruct(factored):
    definitions = factored["definitions"]

    def resolve(kind, references):
        return b"".join(entry(kind, definitions[kind][index])
                        for index in references)

    def parameters(records):
        result = b""

        for record in records:
            if "kind" in record:
                payload = (words(len(record["items"]))
                           + resolve(record["kind"], record["items"]))
            else:
                payload = bytes.fromhex(record["payload"])

            padding = bytes.fromhex(record["padding"])
            assert len(padding) == -len(payload) % 8
            result += (words(record["target"], record["parameter"],
                             len(payload), record["error"])
                       + payload + padding)

        return result

    results = []

    for selection in factored["selections"]:
        result = dict(selection["metadata"])
        result["graph_open"] = parameters(selection["graph_open"]).hex()
        result["graph_connections"] = parameters(
            selection["graph_connections"]).hex()
        result["driver_properties"] = resolve(
            "driver_properties", selection["driver_properties"]).hex()
        result["subgraph_connections"] = resolve(
            "adjacency", selection["subgraph_connections"]).hex()
        results.append(result)

    return results
