# VioMATRIXC Fork Changes

This file documents the VioMATRIXC changes that matter to VioSpice users and
integrators. It is not a replacement for ngspice upstream history; upstream
attribution remains in `AUTHORS`, `ChangeLog`, and `COPYING`.

## Summary

VioMATRIXC is a fork of ngspice customized for the VioSpice software. The fork
currently centers on three functional areas:

1. LTspice-oriented compatibility defaults and netlist preprocessing
2. WAV-file-backed independent sources
3. Build flows that default to the compatibility mode expected by VioSpice

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

## 4. Build Scripts Used By The Fork

The following scripts are the intended shortcuts for common Linux builds:

* `compile_linux_vicompat.sh`
* `compile_linux_shared_vicompat.sh`

They both configure with `--enable-vicompat` and are the closest match to the
expected VioSpice integration builds.

## Scope Boundary

Unless stated here, the rest of the codebase should be assumed to follow normal
ngspice behavior and upstream layout.
