/*.......1.........2.........3.........4.........5.........6.........7.........8
================================================================================

FILE varistor/cfunc.mod — analog varistor (voltage-controlled resistor) codemodel

LTspice-compatible VARISTOR A-device: variable resistance between
OUT+ and OUT- controlled by voltage across IN+ and IN-.

  R = roff when V(in) < Vref
  R = rclamp when V(in) > Vref
  Smooth cubic Hermite transition in a 2% window around Vref.

Ports (from ifspec):
  in_pos — controlling voltage positive (IN/v)
  in_neg — controlling voltage negative (IN/v)
  out    — floating conductance (INOUT/gd)

Params:
  vref   — threshold voltage (default 1.0 V)
  roff   — off resistance (default 1e12 ohm)
  rclamp — clamp resistance (default 1.0 ohm)

Usage:
  A_var in+ in- out+ out- varistor vref=12

================================================================================*/

/*=== INCLUDE FILES ====================*/

#include <math.h>


/*=== CM_VARISTOR ROUTINE ===*/

void
cm_varistor(ARGS)
{
    double vref, roff, rclamp;
    double g_off, g_clamp;
    double v_ctrl, v_out;
    double dv, delta, x;
    double g_smooth;   /* smoothed conductance beyond the knee */
    double dg_smooth;  /* derivative d(g_smooth)/d(v_ctrl) */

    vref   = PARAM(vref);
    roff   = PARAM(roff);
    rclamp = PARAM(rclamp);
    g_off  = 1.0 / roff;
    g_clamp = 1.0 / rclamp;

    if (ANALYSIS == MIF_DC || ANALYSIS == MIF_TRAN) {

        v_ctrl = INPUT(in_pos) - INPUT(in_neg);
        v_out = INPUT(out);
        dv = v_ctrl - vref;
        delta = (vref > 0) ? vref * 0.02 : 0.02;

        if (dv >= delta) {
            /* Fully on: rclamp conductance */
            g_smooth  = g_clamp;
            dg_smooth = 0.0;
        } else if (dv <= -delta) {
            /* Fully off: leakage only */
            g_smooth  = 0.0;
            dg_smooth = 0.0;
        } else {
            /* Cubic Hermite transition knee */
            x = dv / delta;
            g_smooth  = g_clamp * (0.5 + 0.75 * x - 0.25 * x * x * x);
            dg_smooth = g_clamp * (0.75 - 0.75 * x * x) / delta;
        }

        /* Total conductance = leakage + smoothed clamp */
        /* OUTPUT(out) is the current I = G * V(out) */
        OUTPUT(out) = (g_off + g_smooth) * v_out;

        /* PARTIAL(out, out) = dI/dVout = G (conductance seen by output) */
        PARTIAL(out, out) = g_off + g_smooth;

        /* PARTIAL(out, in_pos) = dI/dV(in_pos) = Vout * dG/dVctrl */
        PARTIAL(out, in_pos) = INPUT(out) * dg_smooth;

        /* PARTIAL(out, in_neg) = -dI/dV(in_pos) */
        PARTIAL(out, in_neg) = -INPUT(out) * dg_smooth;

    } else {
        OUTPUT(out) = 0;
    }
}
