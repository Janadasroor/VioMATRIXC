# Agent Notes — VioMATRIXC

## Testing A-devices

- **Use the VioMATRIXC binary at `release/src/ngspice`**, not the system `/usr/bin/ngspice`. The system binary does not contain any changes from `inpcompat.c`.
- The VioMATRIXC build has `VICOMPAT_LTSPICE` compiled in (`config.h`), so LT compatibility is always on — no need to `set vicompat=lt`.
- `.spiceinit` must load the codemodels (`digital.cm`, `analog.cm`, `xtradev.cm`, `spice2poly.cm`) before the circuit is parsed. `xtradev.cm` provides `sidiode` for LTspice diode model transforms. `spice2poly.cm` is required when any POLY(n) E/G/F/H source is present — without it, `ENHtranslate_poly` creates `a$poly$...` A-device instances whose `spice2poly` model type is unresolved, causing MIF-ERROR.
- To verify changes to `inpcompat.c`, rebuild with `make -C release` (or the appropriate build dir) and then run with the built binary.

## A-device port mapping (LTspice 8-pin convention)

| Pin | Function         |
|-----|------------------|
| 0   | Input A / D      |
| 1   | Input B / Clock  |
| 2   | Input C / Set    |
| 3   | Input D / Reset  |
| 4   | Input E / ~      |
| 5   | Input F / ~      |
| 6   | Output Q         |
| 7   | Output /Q        |

Unused set/reset ports should use `"0"` (not `"NULL"`), because `MIFget_token` checks for lowercase `"null"`.

## OTA special case

OTA A-devices previously required a numeric instance name (e.g. `A1`, `A2`). The `isdigit_c(cl[1])` guard in the OTA transform (`inpcompat.c:2105`) was removed — the `is_ota` devtype check is sufficient to distinguish OTA from other devices. Names like `A_ota` now work. Verified safe: no other A-device uses the "OTA" type string, and the generic transform runs first, so digital devices are handled before the OTA loop.

## COUNTER A-device (cycles parameter)

- **COUNTER transform**: maps `cycles=N` to `d_fdiv(div_factor=2*N, high_cycles=N)` in `inpcompat.c:3347-3351`. Verified working with `cycles=3`: output toggles with period=60ns (f_clk=100MHz÷6=16.67MHz), 50% duty cycle (30ns high, 30ns low).
- **Both inline and model-card forms work**: `A1 ... counter cycles=3` (inline) and `.model MYCOUNTER counter(cycles=3)` + `A1 ... MYCOUNTER` (model-card) are both implemented. The model-card form replaces the user's `.model` card in-place with the equivalent `d_fdiv` model.
- **Model-card form has no bridges**: since there are no inline voltage parameters in the model-card form, no ADC/DAC bridges are generated. The d_fdiv output is purely digital (0/1 logic levels, not analog voltage). For analog output, add `vhigh=5 vlow=0` inline params on the instance line.
- **DAC bridge is correct**: the dac_bridge model in `digital.cm` correctly propagates digital transitions to analog output (smooth ramps over t_rise/t_fall). The `cm_analog_set_perm_bkpt(TIME)` in the EVENT case properly forces analog re-evaluation.
- **No bugs in d_fdiv or dac_bridge**: debug prints confirmed both models work correctly. Earlier false-positive "bug" was due to stale builds.
## MODULATOR A-device

- **MODULATOR transform**: maps to `a_modulator` analog codemodel (not digital). FM on pin0, AM on pin1, cos on pin6, sin on pin7. 4-pin device (default 4-pin rule in `ltspice_pin_index()`).
- **`do_bridge` forced false**: MODULATOR is purely analog — no ADC/DAC bridges generated.
- **Inline params**: `mark` (default 1e6 Hz) and `space` (default 5e5 Hz) parsed but do NOT set `present=true` (so bridge generation is not triggered).
- **Formula**: `freq = space + (mark - space) * V(fm)`; quadrature outputs scaled by `V(am)`.
- **Analog codemodel**: in `src/xspice/icm/analog/modulator/` — continuous phase integration via `cm_analog_integrate()`.

## SAMPLEHOLD A-device

- **SAMPLEHOLD transform**: maps to `a_samplehold` analog codemodel (not digital). Input on pin0, clock on pin2, output on pin6. 3-pin device (custom pin mapping in `ltspice_a_device_transform()`).
- **`do_bridge` forced false**: SAMPLEHOLD is purely analog — no ADC/DAC bridges generated.
- **Inline params**: `vt` (threshold, default 2.5V) parsed normally (sets `present=true`, but bridge override suppresses generation); `vhigh` (clamp high, default 5V) and `vlow` (clamp low, default 0V) also parsed.
- **Sampling**: edge-triggered on clock rising edge crossing `vt`; zero-order hold between samples.
- **Output clamping**: `v(out) = clamp(sampled_input, vlow, vhigh)`.
- **Analog codemodel**: in `src/xspice/icm/analog/samplehold/` — uses two INT states (prev_clk, held_out).
- **Codemodel files**: `ifspec.ifs` (3-port analog with `vt`/`vhigh`/`vlow` params) and `cfunc.mod` (EVENT: detect rising edge, sample input; DC/AC: hold output).
- **`vt` does NOT set `present=false`**: unlike `mark`/`space` for MODULATOR, `vt` continues to set `present=true` because it is shared with digital devices (SCHMITT, BUF) that DO need bridges. The `do_bridge=false` override for SAMPLEHOLD is sufficient.

## VARISTOR A-device

- **VARISTOR transform**: maps to `a_varistor` analog codemodel (not digital). 2-terminal voltage-controlled resistor. Pin count: 4 (in+, in-, out+, out-), mapping to 3 XSPICE ports (2x in/v + 1x inout/gd). The gd port uses 2 node names (out+, out-) for the floating conductance.
- **`do_bridge` forced false**: VARISTOR is purely analog — no ADC/DAC bridges generated.
- **Inline params**: `vref` (clamp voltage, default 1.0V), `roff` (off resistance, default 1e12Ω), `rclamp` (clamp resistance, default 1.0Ω). Parsed but do NOT set `present=true` (analog-only params, no bridge needed).
- **Clamp model**: piecewise I = V/roff + max(V-Vref, 0)/rclamp, with cubic Hermite smoothing over a 2% window around Vref. Below Vref: tiny leakage (V/roff). Above Vref: sharp clamping through rclamp.
- **Analog codemodel**: in `src/xspice/icm/analog/varistor/` — EVAL computes current and conductance; no DC/AC storage needed.
- **Codemodel files**: `ifspec.ifs` (3-port with in+ in/v, in- in/v, out inout/gd plus vref/roff/rclamp params) and `cfunc.mod` (EVAL: V = V(in+)-V(in-), I = piecewise clamp, G = dI/dV).
- **Netlist format**: `A<name> in+ in- out+ out- <model|params...>` — same order as LTspice (controlling voltage inputs first, then floating resistor terminals).
- **External Vref source**: connecting a voltage source across in+/in- causes singular matrix errors — use inline `vref=` parameter instead (matches LTspice behavior).
- **Tested with DC sweep and transient**: DC sweep shows clamping at Vref=12V (Vout ≈ 11.93V at 20V Vin). Transient 0→20V pulse shows clean clamping that releases when Vin falls below Vref.

## Build notes

- For codemodel changes (d_fdiv, dac_bridge), rebuild only `digital.cm`. For new analog codemodels, rebuild `analog.cm` then relink ngspie.
- After cmpp regeneration, copy `modpath.lst`, `cminfo.h`, `cmextrn.h`, `objects.inc` to `release/src/xspice/icm/analog/`.
- **IMPORTANT — cmpp headers (`cminfo.h`, `cmextrn.h`, `objects.inc`) must be regenerated or manually updated whenever a codemodel is added/removed from `modpath.lst`.** These files are NOT auto-regenerated during `make` — they're generated once by `cmpp` and then copied. If they're stale, the new codemodel is compiled and linked into `analog.cm` but the model type is invisible to ngspice's `INPtypelook()`. Symptoms: "Unknown model type" even though the model's `.o` files are in the link line. Fix: add the missing entry to all three files in `release/src/xspice/icm/analog/`.
- The `release/` directory is in `.gitignore`. To force a rebuild: `sleep 1 && touch src/frontend/inpcompat.c && make -C release`. The `sleep 1` ensures the timestamp changes enough for the VPATH-based build to detect it.

## Expression-level transforms in `inpcompat.c`

The following LTspice/PSpice expression transforms are applied in `ltspice_compat()`:

| Pattern | Transform | Reason |
|---------|-----------|--------|
| `&` (logical AND) | → `&&` | ngspice YYparse error on bare `&` |
| `|` (logical OR) | → `||` | ngspice YYparse error on bare `|` |
| `R=expr`, `L=expr`, `C=expr` | → `R={expr}` etc. | ngspice requires `{}` around expressions on R/L/C elements |
| `UpLim()`, `DnLim()` | handled by injected `.func` definitions | `.func uplim/dnlim` definitions are prepended to the deck at line 2567 |

## Mass test results (429 top-level, 427 with pins — Sborka.lib)

| Category | Count | % | Notes |
|----------|-------|---|-------|
| PASS | 351 | 82.2% | Clean parse + .op |
| FATAL_OTHER | 54 | 12.6% | Missing models, divide-by-zero params, LTspice implicit params, POLY errors |
| MATRIX | 16 | 3.7% | Singular matrix (expected with all pins grounded); includes H11L1 now |
| MODEL | 2 | 0.5% | ILPI-137A, acpl-c87at — model not in .lib |
| OTHER | 4 | 0.9% | Measurement subcircuits needing specific analyses |

### Key transforms for FATAL_OTHER resolution

- **Non-ASCII dashes → ASCII `-`** (added in `ltspice_compat()`): Replaces EN DASH (U+2013), EM DASH (U+2014), and MINUS SIGN (U+2212) with ASCII hyphen-minus. Fixes `spice2poly` "Bad real value" errors when POLY elements use Unicode dashes instead of regular minus signs. Fixed: TLV226x (`FB` POLY(5) with `–10E6`).
- **`{identifier}` stripped in `.param` lines** (added in `ltspice_compat()` at line 2063): LTspice allows `{param_name}` inside `.param` statements, but ngspice requires bare identifiers. Only strips braces around simple identifiers (not around expressions with operators). Fixed: H11L1 (`Vh0=({ion}-{ioff})/({ion}+{ioff})/2`).
- **`&`→`&&`, `|`→`||`** (added in `ltspice_compat()`): fixes YYparse errors (6 cases)
- **`if((...))` extra-wrapping** (in `replace_if_ternary()`): handles `if((cond, tval, fval))` pattern by detecting commas at depth=2. Replaced the flawed B-source-only `if((` fix (which had false positives on `if(((cond)&&(cond2))>0.5,...)`).
- **`TC=value1,value2` comma-separated syntax**: Added comma handling to TC=→tc1=/tc2= transform. MCP family and PSMN family use `TC=2.34M,-4.57U` format.
- **`inv()` → `(!())`**: B-source expression transform (MC33063).
- **`R=expr`** → **`R={expr}`** brace wrapping: ngspice requires braces around expressions on R/L/C elements.
