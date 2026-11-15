# GSL on a native topology sound card

`libar-gsl` built from this directory implements `gsl_intf.h` on the
mainline kernel, which runs AudioReach graphs from a topology file. Build it
by setting the `qtiaudio` `feature_native_topology` Soong variable. AGM, PAL
and the HALs link the same library name.

## How it maps

Each graph key vector that a client opens is one selection of the bindings
file. A selection fixes the PCMs and compress devices of the sound card, the
route controls and the backends. The library opens those devices when the
client prepares the graph, and moves data, parameters and events through
them with the runtime in `native/`.

`gsl_init()` reads the card name from `ro.vendor.audio.native_topology.card`
and the bindings from `/vendor/etc/audio/audioreach-native-bindings.json`.
It waits for the card, since the kernel registers it after the ADSP is up.
The process exits when the card disappears, so that init restarts it.

## What a client must send

The DSP learns formats from module parameters. A sound card needs them
before it opens a device, so `gsl_intf.h` has four additions. The SPF
implementation returns `AR_EUNSUPPORTED` for each.

- `GSL_CMD_CONFIGURE_PCM_MEDIA` gives the PCM format of a graph. Send it
  before the buffer commands and before `GSL_CMD_PREPARE`, for hostless
  graphs too.
- `GSL_CMD_CONFIGURE_COMPRESS_MEDIA` gives the compress parameters of a
  compressed graph.
- `gsl_get_backends()` lists the backends of the card.
- `GSL_CMD_CONFIGURE_BACKEND_MEDIA` gives the format of the backend that a
  key vector routes to. Send it before the key vector is prepared or added.

The client must leave the hardware endpoints, the shared memory endpoints
and the PCM format modules to the kernel. The library forwards every other
module parameter.

For a push-pull graph the read and write parameters carry the size of one
period and the number of periods. The position buffer commands return the
PCM descriptor with size zero, and the client reads the position from the
PCM.

## Differences from the SPF implementation

- A graph whose key vector has no PCM, such as the capture side of a
  loopback, opens but does nothing on the card. When it adds a key vector
  that contains the key vector of another graph, that graph runs it.
- Stop and close with properties do nothing. The route runs until the
  client changes or removes the key vector.
- A playback PCM keeps running when the client supplies no data, as a
  graph on the DSP does. A capture PCM that stopped on an overrun restarts
  on the next read.
- `GSL_CMD_FLUSH` on a PCM graph drops the data of its PCMs and prepares
  them again.
- `GSL_CMD_EOS` works on compressed graphs only. When the drain completes
  the client gets the EOS event and then the early EOS event of the gapless
  module.
- New compress parameters on a started graph are those of the next track.
  The kernel takes them only while the compress device runs, so they fail
  after a flush or a full drain.
- A write with `GSL_BUFF_FLAG_MEDIA_FORMAT` is not supported.
- The tuning and logging functions return `AR_EUNSUPPORTED`.

## Test

`ar-gsl-native-test` runs the library against a fake sound card.
