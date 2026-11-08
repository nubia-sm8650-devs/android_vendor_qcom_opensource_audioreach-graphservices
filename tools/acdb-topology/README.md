# ACDB topology compiler

Build the host `ar-acdb-topology` target with its Android.bp definition.
Use `--help` for input, native database query and output options. Reports
retain the original graph buffers and their factored declarations.

## Hardware declarations

Supply the resource-manager XML, usecase-key XML and a merged board DTB
to resolve physical endpoints. A base DTB without the board's audio links
does not supply a hardware binding.

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

Corpus output covers declarations and reference controls. It does not bind
PCM ownership, DAPM activation, runtime hardware configuration or calibration.
`runtime_bindings` remains false. Without `--corpus`, `--exports` retains its
individual-selection reporting behavior.
