# ARM64 JIT Goal Summary

## Goal

Integrate the WinUAE ARM64 dynamic recompiler into Previous on Apple Silicon, make it coexist with Previous's 68040 MMU and device scheduler, boot OpenStep 4.2 reliably with JIT enabled, and provide a repeatable benchmark for comparing JIT and interpreter performance.

## Current status

This checkpoint builds successfully as a native arm64 macOS application, and JIT mode starts and runs without the earlier immediate CPU halt, host crash, double fault, or firmware-loop failures. It does **not** yet satisfy the full goal: OpenStep 4.2 still does not complete a graphical boot with JIT enabled.

Both native compiled blocks and a diagnostic fallback-only JIT run reach the OpenStep SCSI initialization stage, then stop making useful progress near guest PC `0x040924xx`, commonly with interrupt mask 6. The display may blank, flicker, or retry, but the desktop does not appear. The fallback-only result is important: it suggests the remaining fault is in the shared JIT dispatch/cycle/device-event path rather than exclusively in ARM64 instruction translation.

Interpreter mode remains the known-good reference and reaches the graphical purple OpenStep screen.

## Critical improvements retained in this checkpoint

### Build, configuration, and generated CPU support

- Added the ARM64 JIT sources and executable-memory support to the CPU build.
- Enabled JIT only for ARM64 builds and added the generated 68040/JIT CPU tables.
- Added a persistent `bJIT` system setting, disabled by default.
- Added the WinUAE compatibility headers, preferences, C/C++ linkage declarations, and VM implementation needed by the JIT sources.
- Added macOS hardened-runtime/JIT memory handling, including write/execute mode transitions and instruction-cache synchronization.

### 68040 MMU and memory integration

- Preserved the 68040 MMU while JIT is enabled instead of allowing the generic WinUAE preference fixer to disable it.
- Added MMU-aware instruction fetch helpers for compiled blocks and direct-PC fallback handlers.
- Added JIT memory banks and data-access wrappers that route guest accesses through Previous's existing MMU callbacks.
- Forced untranslated instructions to use the MMU-aware 68040 fallback table. This fixes cases such as memory bitfield instructions being treated as physical-address accesses.
- Corrected fallback instruction/extension fetch behavior and synchronized scalar PC state with the direct PC used by JIT dispatch.

### CPU state, exceptions, and dispatch

- Added ARM64-native condition-code storage compatible with generated flag operations.
- Checkpointed flags and instruction PC before potentially faulting work.
- Added restart handling around the JIT dispatcher so 68040 MMU exceptions restore CPU state, enter the guest exception handler, and resume dispatch instead of halting or double-faulting.
- Kept direct and scalar PC representations synchronized across normal execution, fallback execution, and exception entry.
- Cleared the one-shot `SPCFLAG_END_COMPILE` request in the non-threaded specialty path so it does not remain latched indefinitely.
- Corrected the trap-opcode byte-order classification used by ARM64 generated code.

### Bootstrap, cache, and scheduling

- Deferred enabling the configured JIT cache until the NeXT ROM bootstrap hands off into main guest RAM. This avoids early firmware paths that are not yet restart-safe in the JIT.
- Avoided shrinking or committing a JIT cache before its backing address space exists.
- Added JIT cycle accounting and short scheduler slices, with hooks that advance Previous's main cycle counter, device interrupt scheduler, DSP, and i860 work.
- Converted generated fallback-handler cycle returns into the cycle units expected by Previous.

### Diagnostics and benchmark support

- Added `--jit` and `--mhz` controls to `benchmark/run.sh` and made each run use an isolated copy-on-write disk image.
- Added PC and SR to `BENCHMARK_BEGIN`, captured a framebuffer screenshot at the measurement boundary, redirected guest print output into the result directory, and made a benchmark exit automatically if the CPU halts.
- Added explicit CPU halt reason and PC logging.
- Removed the broad instruction, interrupt, MMU-walk, and device tracing used during diagnosis, along with two late experiments that did not improve boot behavior. Those experiments were immediate interrupt delivery from status-register changes and treating `currcycle` as guest-cycle units.

## Verification and evidence

- Clean checkpoint build command:

  ```sh
  cmake --build /private/tmp/previous-jit-build -j8
  ```

  Result on 2026-08-31: success; target `Previous` reached 100%. The imported JIT sources still emit compiler warnings, primarily missing C prototypes and unused generated-code variables, but there are no build errors.

- Post-cleanup JIT launch smoke test (`--warmup 2 --duration 1`): success, with a completed benchmark result in `artifacts/benchmark-results/20260831-152045`. This verifies launch, brief execution, and orderly benchmark exit; it is not long enough to validate the OpenStep boot.

- Known-good interpreter graphical boot: `artifacts/benchmark-results/20260831-123401` (graphical OpenStep screen after a 170-second warm-up).
- Known-good interpreter text boot: `artifacts/benchmark-results/20260831-134154` (full text boot after a 110-second warm-up).
- Native JIT failure evidence: `artifacts/benchmark-results/20260831-145147` and `artifacts/benchmark-results/20260831-150219`.
- Fallback-only JIT failure evidence: `artifacts/benchmark-results/20260831-145536`.

The `artifacts/` directory is intentionally not staged because it contains large local disk images, logs, samples, and screenshots. The paths above are local diagnostic evidence rather than source inputs to this checkpoint.

## What is still failing

1. OpenStep does not reach the graphical desktop in JIT mode. Progress stalls shortly after the SCSI 53C90A controller initializes, around `0x040924xx`.
2. JIT event timing is not demonstrably equivalent to the interpreter. The ordering or boundary at which SCSI/DMA/timer events and level-6 interrupts are delivered remains the leading suspect.
3. Deferred initialization can leave the eventual JIT cache smaller than requested because the initial zero-cache bootstrap setup and the later allocation are not yet a single contiguous lifecycle.
4. The ARM64 JIT import has substantial compiler-warning noise. This does not prevent the build, but it should be cleaned before treating the backend as production quality.
5. No valid JIT performance comparison is available until JIT mode reaches the same stable guest state as the interpreter.

## Recommended next investigation

Start from the fallback-only result and compare interpreter versus JIT-dispatch execution at the same SCSI-stage guest PC. Record only targeted scheduler state: `nCyclesMainCounter`, the next pending `CycInt` deadline, interrupt level/mask, and the precise instruction boundary at which the event is delivered. Once fallback-only dispatch matches interpreter event ordering and boots, re-enable native compilation and isolate any remaining translator-specific fault. After correctness, consolidate deferred cache initialization and clean the generated-code warnings.

Useful benchmark commands are:

```sh
# JIT
PREVIOUS_BENCHMARK_BUILD_DIR=/private/tmp/previous-jit-build \
  ./benchmark/run.sh --no-build --jit --warmup 230 --duration 3 --trials 1

# Interpreter reference
PREVIOUS_BENCHMARK_BUILD_DIR=/private/tmp/previous-jit-build \
  ./benchmark/run.sh --no-build --warmup 170 --duration 3 --trials 1
```

In this document, “working build” means the staged source compiles and launches. It does not mean the unresolved JIT boot goal is complete.
