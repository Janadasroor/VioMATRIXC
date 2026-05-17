# VioMATRIXC Fork Changes

This file documents the VioMATRIXC changes that matter to VioSpice users and
integrators. It is not a replacement for ngspice upstream history; upstream
attribution remains in `AUTHORS`, `ChangeLog`.

## Summary

VioMATRIXC is a fork of ngspice customized for the VioSpice software. The fork
currently centers on four functional areas:

1. LTspice-oriented compatibility defaults and netlist preprocessing
2. WAV-file-backed independent sources
3. PWL-file-backed independent sources (native file loading)
4. Build flows that default to the compatibility mode expected by VioSpice

## 1. LTspice Compatibility Mode

The configure option:

```sh
--enable-vicompat
```

enables LTspice compatibility by default and propagates the default into the
generated `spinit` file through `VICOMPAT_DEFAULT`.

Relevant files:

* `configure.ac`
* `src/spinit.in`

Behavior:

* sets `vicompat=lt` in the generated startup configuration
* enables LTspice-oriented frontend compatibility handling without requiring
  the user to set it manually after installation

## 2. LTspice A-Device Translation

The frontend contains a transformation pass that rewrites LTspice fixed 8-pin
A-device syntax into ngspice/XSPICE-compatible forms during netlist loading.

Relevant file:

* `src/frontend/inpcompat.c`

Supported mapped families include:

* buffers and inverters
* Schmitt-trigger style buffers
* two-input logic gates such as `AND`, `OR`, `NAND`, `NOR`, `XOR`, `XNOR`
* sequential digital elements such as `DFF`, `JKFF`, `SRFF`, `DLATCH`
* counter/frequency-divider mapping

Operational notes:

* generated model cards are created when needed
* vector syntax is emitted for gate inputs where ngspice requires it
* missing optional LTspice pins are expanded into valid ngspice port lists

## 3. `WAVEFILE` Source Support

This fork adds `WAVEFILE` handling for independent voltage and current sources.
The source value is read from a WAV file and evaluated during transient
simulation.

Relevant files:

* `src/spicelib/devices/audio.c`
* `src/spicelib/devices/vsrc/`
* `src/spicelib/devices/isrc/`

Capabilities documented from the implementation:

* WAV-backed voltage and current sources
* multi-channel audio selection
* interpolation between samples
* sample normalization
* shared in-memory reuse of parsed WAV data

## 4. `pwlfile` Source Support

This fork adds native `pwlfile` handling for independent voltage (`V`) and current (`I`) sources. This allows the simulator to load Piece-Wise Linear (PWL) data directly from an external text file instead of inlining large lists of points into the SPICE netlist.

Relevant files:

* `src/spicelib/devices/vsrc/vsrcpar.c`
* `src/spicelib/devices/isrc/isrcpar.c`
* `src/spicelib/devices/vsrc/vsrc.c`
* `src/spicelib/devices/isrc/isrc.c`

Usage:

```spice
V1 Net1 0 pwlfile="/path/to/waveform.pwl"
I1 Net1 0 pwlfile="/path/to/current.pwl"
```

Capabilities and behavior:

* **Native File Loading**: Points are read directly from the filesystem during the setup phase.
* **Efficient Storage**: Large waveforms do not bloat the SPICE netlist or exceed line length limits.
* **Automatic Mode Switch**: Setting `pwlfile` automatically configures the source to use the `PWL` transient function.
* **Validation**: Performs time-monotonicity checks on loaded points, issuing warnings for non-increasing time values.
* **Format**: Supports standard text files with `time value` pairs (space or tab separated).

## 5. VioSpice JIT Logic

This fork includes a mechanism for registering and looking up Just-In-Time (JIT) compiled logic functions, intended for use with VioSpice's dynamic code generation.

Relevant files:

* `src/viospice_bridge.c`: Registry implementation
* `src/include/ngspice/viospice_jit.h`: Public API
* `src/xspice/icm/analog/viospice_jit/`: XSPICE code model that utilizes the JIT registry

The registry allows external software (like VioSpice) to register a function pointer associated with a `block_id`. The `viospice_jit` XSPICE model can then invoke these functions during simulation.

## 6. Regression Tests

New tests for fork-specific features are located in:

* `tests/viospice/test_adevice_compat.cir`: LTspice A-device translation
* `tests/viospice/test_pwlfile.cir`: Native PWL file loading

## 7. Build Scripts Used By The Fork

The following scripts are the intended shortcuts for common Linux builds:

* `compile_linux_vicompat.sh`
* `compile_linux_shared_vicompat.sh`

They both configure with `--enable-vicompat` and are the closest match to the
expected VioSpice integration builds.

## 8. Licensing Compliance (GPL Removal)

To ensure that VioSpice (Apache 2.0) can link against VioMATRIXC without being "infected" by GPL requirements, the following changes have been made to this fork:

* **Removal of XSpice Table Models**: The directory `src/xspice/icm/table/` and its contents (GPLv2+ licensed) have been physically removed from the repository.
* **Build System Update**: `src/xspice/icm/GNUmakefile.in` has been modified to exclude the `table` module from the build process.
* **Removal of GPL COPYING File**: The upstream `COPYING` (GPLv3 license text) has been removed to avoid confusion. All remaining third-party code is under permissive licenses (BSD, LGPL, Apache 2.0).
* **Bridge Compatibility**: `ngSpice_IsPaused()` has been added to `src/viospice_bridge.c` to maintain API compatibility for VioSpice after engine stabilization.

This transition ensures that the resulting `libngspice.so` shared library consists only of BSD, LGPL, and Public Domain components, making it legally compatible with permissive-licensed applications.

## Scope Boundary


Unless stated here, the rest of the codebase should be assumed to follow normal
ngspice behavior and upstream layout.
