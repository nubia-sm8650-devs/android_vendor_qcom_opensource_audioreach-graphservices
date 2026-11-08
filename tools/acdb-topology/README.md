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
subgraph or graph identity. The DAI identity is recorded in the endpoint's
`binding`, not used as a replacement graph ID.

Static bindings do not set `runtime_bindings`. Rates, channel maps and
clock ownership still come from the hardware DAI path, and calibration
remains in userspace. MI2S word-select source follows the live DAI format.
Unsupported interfaces, ambiguous bindings and unresolved cross-subgraph
references still block topology output. `--exports` reports each selection
individually; it does not produce a combined runtime topology.
