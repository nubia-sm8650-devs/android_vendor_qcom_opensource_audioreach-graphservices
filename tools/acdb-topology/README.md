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

Builds that cannot depend on the kernel's DTB can pass `--board-links FILE`
instead of `--board-dtb`. The file holds the `board.links` array of a report
generated with the DTB. Each link's transport is recomputed from its DAI and
must match a stored one.

Use repeated `--device` options to restrict the PAL device profiles.
Supply `--device-pp-type` for DevicePPType-qualified assignments and
`--context NAME=VALUE` for other XML conditions, such as CustomConfig.
A condition the context does not supply stays open. The endpoint then binds
only when every assignment that could have produced the selection's keys is
usable and leads to the same port, and the binding lists `open_conditions`.
A device profile that overrides its backend per usecase or custom
configuration keeps the endpoint unresolved. Reported board candidates and
native parameters retain their separate provenance.

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
selector stays open as described above.

Pass `--unavailable FILE` to name the PAL devices and exact graph-key
selections a board cannot use. Each entry needs a reason.

```json
{
  "version": 1,
  "devices": [
    {"device": "PAL_DEVICE_IN_FM_TUNER", "reason": "No FM tuner link."}
  ],
  "selections": [
    {"graph_keys": [["0xa2000000", "0xa2000004"]],
     "reason": "The graph's endpoint contradicts the board's profile."}
  ]
}
```

Board candidates for unavailable devices are skipped. A selection is
excluded when one of its endpoints has only unavailable candidates or when
it is listed, and `excluded` in the report gives each one with its index in
the input corpus. Links from kept selections to modules that only excluded
selections own are dropped and listed in `corpus.excluded_references`.
Devices must appear in the board inputs and listed selections must match an
input selection.

## Corpus declarations

Use `--exports DIRECTORY --corpus --output topology.conf` to lower a corpus
into one topology. With `--acdb FILE ...` and no `--key`, corpus mode
queries every graph key vector the ACDB defines instead of reading exports.
`--graph-id` supplies the first graph ID. Each invariant
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

## PCM and DAPM bindings

Supply `--hardware-widgets FILE` in corpus mode to connect qualified ports
to their existing backend stream widgets. Use stream widget names, not DT
link names or DAI driver names. For example, the speaker port uses this
entry.

```json
{"version": 1, "widgets": [
  {"dai": 16, "direction": "playback", "name": "Primary MI2S Playback",
   "aif": {"id": 0, "name": "PRI_MI2S_RX"},
   "media_controls": {"rate": "Speaker Playback Rate",
                      "format": "Speaker Playback Format",
                      "channels": "Speaker Playback Channels",
                      "data_format": "PRI_MI2S_RX Data Format"}}
]}
```

Every selected physical endpoint needs a matching entry. Graph IDs must
not overlap any backend DAI ID in the supplied board. Hardware switches
gate routing without taking formatting or state transitions from the
backend callbacks.

The optional `media_controls` object supplies exact control names for
managed userspace media configuration. It retains the matching native DT
backend's provider, port, path and link name in the hardware gate's `backend`
binding. If multiple available links match the port and direction, add
`backend` to the widget entry with the desired DT path from the board report.
Repeated links remain separate; no link is chosen by occurrence order.

Rate/Format/Channels names and Data Format names need not share a prefix.
Use the controls exposed by the target card. For codec-owned channel maps,
replace `channels` with `channel_map`, naming the read-only map control, such
as `BT Capture Channel Map`. The two channel sources are mutually exclusive.

The optional `aif` object declares a physical AGM identity independently of
the widget and DT link names. It accepts only `id` and `name`, requires media
controls and preserves the declaration in compact bindings. Use dense IDs
from zero across the published AIF catalogue for AGM's index-based API.
Grouped and virtual AIF declarations are not flattened into physical devices.

Add `--pcm-bindings FILE` to declare explicit frontend roots. Take native
subgraph IDs from the corpus report and choose PCM numbers for the client.
For example, a PCM root declaration has this shape.

```json
{"version": 1, "pcms": [
  {"id": 0, "subgraph": "0xb0000001", "direction": "playback", "mode": "pcm",
   "capabilities": {"formats": ["S16_LE"], "rates": [48000],
                    "channels_min": 1, "channels_max": 2}}
]}
```

These are advertised frontend capabilities, not hardware defaults. A PCM
root must contain one shared-memory endpoint in its declared direction and
no placeholder encoder or decoder.

Use `"mode": "compress"` for compress offload playback. Its root needs a
packet write endpoint and the placeholder decoder, and the frontend becomes
a compressed SectionPCM. Compressed capture is not supported.

For an explicit hostless application, use `"mode": "hostless"`. The
frontend connects to the root's only module, or else to its one module
without intrinsic input for playback or without intrinsic output for
capture. Cross-subgraph links do not count, so a loopback stream's entry
module qualifies. Name the module with a numeric `module` when no single
module qualifies. A hostless binding with `"direction": "duplex"` emits one
PCM with both streams, connecting playback to the inferred entry module and
capture to the inferred exit module, as a loopback stream needs to start
both of its backends. Device graphs cannot own frontends. Bidirectional
non-tunnel bindings are not silently converted to PCM.

Intrinsic data paths retain their native connectivity. Cross-graph data
paths get separate DAPM switches, while DSP connections keep their
reference controls. A selection's `paths` includes only references whose
two graphs belong to that selection. Its `tuning` list names exactly its
selected subgraphs and SET controls, without adding reference peers.
`ownership_bound` describes frontend reachability; `unowned_graphs` keeps
missing ownership explicit. Neither establishes DSP or device acceptance.

Use `--bindings-output FILE` for a compact userspace file without graph
blobs. It retains complete graph keys, frontends, memberships, reference
and path controls, hardware gates and tuning associations. Unavailable or
conflicting hardware still blocks both topology and binding output.
Reference entries retain source and destination graph IDs and their
optional path controls without native payload bytes, so userspace can
compose fragments without importing absent peers.
