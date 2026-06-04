/*.......1.........2.........3.........4.........5.........6.........7.........8
================================================================================

FILE analog/samplehold/cfunc.mod — sample-and-hold codemodel

LTspice-compatible SAMPLEHOLD A-device: edge-triggered sample on
clock rising edge crossing vt. Zero-order hold between samples.
Output clamped to [vlow, vhigh].

Two integration storage slots:
  INT1 — previous clock voltage (for edge detection)
  INT2 — held output voltage

Ports (from ifspec):
  in  — analog input (IN/v)
  clk — clock input (IN/v)
  out — sampled output (OUT/v)

Params:
  vt     — clock threshold voltage (default 2.5 V)
  vhigh  — output high clamp (default 5 V)
  vlow   — output low clamp (default 0 V)

================================================================================*/

/*=== INCLUDE FILES ====================*/

#include <math.h>


/*=== CONSTANTS ========================*/

#define INT1 1
#define INT2 2


/*=== CM_SAMPLEHOLD ROUTINE ===*/

void
cm_samplehold(ARGS)
{
    double vt, vhigh, vlow;
    double clk_in;
    double in_val;
    double *prev_clk;
    double *held_out;

    vt     = PARAM(vt);
    vhigh  = PARAM(vhigh);
    vlow   = PARAM(vlow);

    if (INIT == 1) {
        cm_analog_alloc(INT1, sizeof(double));
        cm_analog_alloc(INT2, sizeof(double));
    }

    if (ANALYSIS == MIF_DC) {

        in_val = INPUT(in);
        clk_in = INPUT(clk);

        prev_clk = (double *) cm_analog_get_ptr(INT1, 0);
        held_out = (double *) cm_analog_get_ptr(INT2, 0);

        if (in_val > vhigh) in_val = vhigh;
        if (in_val < vlow)  in_val = vlow;

        *prev_clk = clk_in;
        *held_out = in_val;

        OUTPUT(out) = in_val;
        PARTIAL(out, in) = 1;
        PARTIAL(out, clk) = 0;

    } else if (ANALYSIS == MIF_TRAN) {

        in_val = INPUT(in);
        clk_in = INPUT(clk);

        prev_clk = (double *) cm_analog_get_ptr(INT1, 0);
        held_out = (double *) cm_analog_get_ptr(INT2, 0);

        if (*prev_clk < vt && clk_in >= vt) {
            *held_out = in_val;
        }

        if (*held_out > vhigh) *held_out = vhigh;
        if (*held_out < vlow)  *held_out = vlow;

        *prev_clk = clk_in;

        OUTPUT(out) = *held_out;
        PARTIAL(out, in) = 0;
        PARTIAL(out, clk) = 0;

        cm_analog_set_perm_bkpt(TIME);

    } else {

        OUTPUT(out) = 0;
    }
}
