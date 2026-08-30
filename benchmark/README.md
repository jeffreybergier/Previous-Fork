# Previous performance benchmark

`run.sh` builds an optimized native ARM64 copy of Previous, boots the supplied
OPENSTEP image, measures guest CPU cycles for a fixed wall-clock interval, and
captures a macOS `sample` profile during the measurement.

The source image is never mounted directly. Each trial uses an APFS clone in a
unique temporary directory, so guest writes are discarded when the trial ends.
Networking, NFS, startup dialogs, and quit confirmation are disabled only in the
temporary configuration. Previous requires NFS share 0 to name an existing
directory even when Ethernet is disabled, so the generated config points it at
the otherwise-empty per-run temporary directory.

Run the default one-minute warm-up and 30-second measurement with:

```sh
./benchmark/run.sh
```

For repeated measurements:

```sh
./benchmark/run.sh --warmup 90 --duration 30 --trials 3
```

Use `--no-build` when only rerunning an already-built revision. Results and CPU
profiles are written below `artifacts/benchmark-results/`.

The inputs default to `artifacts/OPENSTEP4.2.cfg` and
`artifacts/OPENSTEP4.2.sd`. They can be overridden with
`PREVIOUS_BENCHMARK_CONFIG` and `PREVIOUS_BENCHMARK_DISK`.
