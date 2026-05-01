# VioMATRIXC

VioMATRIXC is a VioSpice-oriented fork of [ngspice](https://ngspice.sourceforge.io/). It maintains the ngspice simulation core, licensing, and upstream structure, while adding compatibility and integration work required by the VioSpice software stack.

This repository should be treated as:
* **ngspice upstream codebase**
* Plus **VioSpice-specific simulator behavior**
* Plus **fork-maintained build scripts** and packaging defaults

## What This Fork Adds

The main fork-specific changes currently documented in this tree are:
* **LTspice compatibility defaults** through `--enable-vicompat`
* **Automatic transformation** of LTspice fixed-pin A-devices into ngspice/XSPICE equivalents during netlist loading
* **`WAVEFILE` support** for voltage and current sources for audio-driven transient simulations
* **Build scripts** intended for VioSpice integration, including shared-library builds with the compatibility mode enabled

See [VIOCHANGES.md](VIOCHANGES.md) for the fork delta relative to stock ngspice.

## Repository Layout

Top-level directories and files most relevant to this fork:
* `src/`: Simulator source code
* `examples/`: Example circuits and feature demos
* `tests/`: Regression and device tests
* `visualc/`: Visual Studio project files
* `compile_linux_vicompat.sh`: Linux executable build with VioSpice defaults
* `compile_linux_shared_vicompat.sh`: Linux shared-library build for embedding
* `INSTALL`: Build and install notes for this fork
* `COPYING`: Upstream license text that must remain with redistributions
* `AUTHORS` and `ChangeLog`: Upstream project attribution/history

## Build Quick Start

### Console build on Linux
```bash
./autogen.sh
mkdir -p release
cd release
../configure --with-x --enable-cider --enable-vicompat
make -j"$(nproc)"
```

### Shared-library build for VioSpice embedding
```bash
./autogen.sh
mkdir -p releasesh
cd releasesh
../configure --with-ngshared --enable-cider --enable-vicompat
make -j"$(nproc)"
```

The helper scripts `compile_linux_vicompat.sh` and `compile_linux_shared_vicompat.sh` wrap these workflows with default flags and optional install steps.

## Compatibility Default

When configured with `--enable-vicompat`, the generated `spinit` sets:
```spice
set vicompat=lt
```
This enables LTspice-oriented compatibility behavior by default in this fork.

## License and Attribution

VioMATRIXC remains derived from ngspice and keeps the upstream licensing and attribution files intact.

Please retain these files in redistributions:
* `COPYING` (Main License)

* `AUTHORS`
* `ChangeLog`

If you need full ngspice project information beyond this fork summary, consult the upstream project at [https://ngspice.sourceforge.io/](https://ngspice.sourceforge.io/).
