# ACDB topology compiler

Build the host `ar-acdb-topology` target with its Android.bp definition.
Use `--help` for input, native database query and output options. Reports
retain the original graph buffers and their factored declarations.

## Hardware declarations

Supply the resource-manager XML, usecase-key XML and a merged board DTB
to resolve physical endpoints. A base DTB without the board's audio links
does not supply a hardware binding.

Hardware links must describe DPCM backends with codec and platform
providers. Disabled providers, including those below disabled ancestors,
do not qualify a port. A CPU-only frontend is not a hardware binding.

```sh
ar-acdb-topology --input graph.json --report report.json \
    --resource-manager resourcemanager_pineapple_mtp.xml \
    --usecase-kv usecaseKvManager.xml --board-dtb cerro-merged.dtb \
    --device PAL_DEVICE_OUT_SPEAKER --stream-type PAL_STREAM_DEEP_BUFFER
```

Use repeated `--device` options to restrict the PAL device profiles.
Supply `--device-pp-type` for DevicePPType-qualified assignments and
`--context NAME=VALUE` for other XML conditions, such as CustomConfig.
Conditions are not inferred from missing context. Reported board
candidates and native parameters retain their separate provenance.

Haptics backend selection follows the resource-manager setting
`haptics_through_wsa`. When enabled, nested haptics backend overrides apply
in XML order, not as generic CustomConfig-qualified profiles. The report
retains the original backend in `declared_backend` and its override XML in
`backend_override`. An unsupported override does not fall back to the
original backend.

MI2S and CODEC_DMA endpoints with one qualified board port and consistent
native parameters emit existing hardware tokens. Board-selected bank and
interface values can override the ACDB values without changing the module,
subgraph or graph identity. SLIMbus retains the profile's DSP device ID
separately from its backend port and the board's DAI port. USB and DisplayPort
also require a unique qualified board DAI. The DAI identity is recorded in
the endpoint's `binding`, not used as a replacement graph ID.

Static bindings do not set `runtime_bindings`. Rates, channel maps and
clock ownership still come from the hardware DAI path, and calibration
remains in userspace. MI2S word-select source follows the live DAI format.
SLIMbus, USB and DisplayPort can emit with `dai-configured` bindings when
native format data is absent. They emit no placeholder format token.
Set the hardware DAI's `Data Format` control before preparing those paths.
USB tokens are acquired from the live offload device, never from topology.
Unsupported interfaces, ambiguous bindings and unresolved references still
block topology output.

For a corpus with different device contexts, pass `--selection-contexts FILE`
instead of global device and selector options. Each entry matches one complete
graph-key vector, independent of key order. For example, a speaker selection
can use the following context file.

```json
{
  "version": 1,
  "selections": [
    {
      "graph_keys": [
        ["0xa1000000", "0xa1000001"],
        ["0xab000000", "0x1"],
        ["0xa2000000", "0xa2000001"],
        ["0xac000000", "0xac000002"]
      ],
      "devices": ["PAL_DEVICE_OUT_SPEAKER"],
      "context": {"StreamType": "PAL_STREAM_DEEP_BUFFER"}
    }
  ]
}
```

Entries must match an input selection. Unlisted selections retain normal
qualification checks without borrowing another entry's context. Applied
contexts appear in `factored.selections[].hardware_context`; original source
metadata and native buffers remain unchanged. Supplying a context does not
relax interface, ambiguity or corpus-conflict checks.

Use a JSON `null` for a selector known to be absent, such as
`"CustomConfig": null` when no device custom configuration is set. An omitted
selector remains unknown and blocks qualification if a candidate requires it.

## Corpus declarations

Use `--exports DIRECTORY --corpus --output topology.conf` to lower a corpus
into one topology. `--graph-id` supplies the first graph ID. Each invariant
native subgraph gets its own graph ID; native subgraph, container and module
identities are retained. Conflicting subgraph or hardware definitions block
output rather than selecting a default variant.

Intrinsic links stay on their modules. Cross-subgraph links become standard
boolean reference controls. Control-link properties and repeated connection
occurrences are preserved. The report's `corpus.selections` identifies each
source's selected graphs and reference names. A reference can be staged while
its peer graph is closed; resolving that peer does not add it to the source's
selected graphs.

Each emitted graph reports `parameter_controls.set` and
`parameter_controls.get`, bound to that graph's existing AudioReach TLV
transport. Keep calibration in userspace and send it after graph OPEN,
before hardware preparation. A GET writes its query and then reads the
response through the same control. These controls do not open graphs or
apply defaults automatically.

Corpus output covers declarations, reference controls and parameter controls.
It does not bind PCM ownership, DAPM activation or runtime hardware setup.
`runtime_bindings` remains false. Without `--corpus`, `--exports` retains its
individual-selection reporting behavior.
