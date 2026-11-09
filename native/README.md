# Native topology preparation

`libar-topology` applies userspace default calibration through the emitted
graph SET controls before preparing a PCM. Initialize ACDB with the shipped
database files before using this library.

Configure reference controls, PCM-side paths and the required initial
hardware gates before opening the PCM. `pcm_open()` completes hardware
parameters without PREPARE, leaving the tuning window open. Obtain native
subgraph IDs and SET control names from the compiler binding file's
`tuning` entries.

Call `ar_topology::prepare()` under the session's ownership lock with only
newly acquired, untuned graphs. Do not reapply defaults to graphs already
tuned and shared by another client. The function requires ALSA SETUP state,
queries subgraph defaults with empty calibration-key vectors, preflights
all controls and calibration data, sends native SET records, then calls
`pcm_prepare()`. No calibration data is stored in the kernel or topology.

On error, close the failed PCM and release that session's acquired routes
and graph ownership. Preparation does not run after a tuning failure.
Earlier acknowledged SETs are not rolled back. Shared owners must remain
held during cleanup.

Persistent or global-persistent defaults return `-EOPNOTSUPP` before any
SET or PREPARE. They require registration and acknowledged memory lifetime,
not conversion to ordinary SETs. The library does not replace PAL/AGM
ownership policy, implement live switching or provide restart replay.
