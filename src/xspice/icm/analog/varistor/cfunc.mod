/*.......1.........2.........3.........4.........5.........6.........7.........8
================================================================================

FILE varistor/cfunc.mod — analog varistor (voltage-controlled resistor) codemodel

LTspice-compatible VARISTOR A-device: variable resistance between
OUT+ and OUT- controlled by voltage across IN+ and IN-.

I = V(out) / roff + max(V(out) - Vref, 0) / rclamp

Clamp knee smoothed over a 2% window around Vref using cubic Hermite spline.

Ports (from ifspec):
  in_pos — controlling voltage positive (IN/v)
  in_neg — controlling voltage negative (IN/v)
  out    — floating conductance (INOUT/gd)

Params:
  vref   — clamp voltage (default 1.0 V)
  roff   — off resistance (default 1e12 ohm)
  rclamp — clamp resistance (default 1.0 ohm)

================================================================================*/

/*=== INCLUDE FILES ====================*/

#include <math.h>


/*=== CM_VARISTOR ROUTINE ===*/

void
cm_varistor(ARGS)
{
    double vref;
    double roff, rclamp;
    double g_off;
    double v_out;
    double dv;
    double delta;
    double clamped_dv;
    double i_out;
    double dclamp_dv;
    double x;

    vref   = PARAM(vref);
    roff   = PARAM(roff);
    rclamp = PARAM(rclamp);
    g_off  = 1.0 / roff;

    if (ANALYSIS == MIF_DC || ANALYSIS == MIF_TRAN) {

        v_out = INPUT(out);
        dv = v_out - vref;
        delta = vref * 0.02;

        if (dv >= delta) {
            clamped_dv = dv;
            dclamp_dv = 1.0;
        } else if (dv <= -delta) {
            clamped_dv = 0.0;
            dclamp_dv = 0.0;
        } else {
            x = dv / delta;
            clamped_dv = delta * (0.5 + 0.75 * x - 0.25 * x * x * x);
            dclamp_dv = 0.75 - 0.75 * x * x;
        }

        i_out = g_off * v_out + clamped_dv / rclamp;

        OUTPUT(out) = i_out;
        PARTIAL(out, out) = g_off + dclamp_dv / rclamp;
        PARTIAL(out, in_pos) = 0;
        PARTIAL(out, in_neg) = 0;

    } else {

        OUTPUT(out) = 0;
    }
}
