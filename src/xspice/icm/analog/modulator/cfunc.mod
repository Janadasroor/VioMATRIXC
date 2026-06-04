/*.......1.........2.........3.........4.........5.........6.........7.........8
================================================================================

FILE modulator/cfunc.mod — quadrature modulator codemodel

LTspice-compatible MODULATOR A-device: FM and AM inputs,
quadrature sine/cosine outputs.

freq = space + (mark - space) * V(fm_in)
sin_out = V(am_in) * sin(2*pi*phase)
cos_out = V(am_in) * cos(2*pi*phase)

Ports (from ifspec):
  fm_in   — frequency modulation voltage input (IN/v)
  am_in   — amplitude modulation voltage input (IN/v)
  cos_out — cosine output (OUT/v)
  sin_out — sine output (OUT/v)

Params:
  mark  — frequency when V(fm) = 1V (default 1e6 Hz)
  space — frequency when V(fm) = 0V (default 5e5 Hz)

================================================================================*/

/*=== INCLUDE FILES ====================*/

#include <math.h>
#include <stdlib.h>


/*=== CONSTANTS ========================*/

#define INT1 1

static char *allocation_error = "\n**** Error ****\nMODULATOR: Error allocating storage \n";


/*=== CM_MODULATOR ROUTINE ===*/

void
cm_modulator(ARGS)
{
    double fm_voltage;
    double am_voltage;
    double mark_freq;
    double space_freq;
    double freq;
    double *phase;
    double *phase1;
    double radian;
    double sin_out;
    double cos_out;

    Mif_Complex_t ac_gain;

    mark_freq  = PARAM(mark);
    space_freq = PARAM(space);

    if (INIT == 1) {
        cm_analog_alloc(INT1, sizeof(double));
    }

    if (ANALYSIS == MIF_DC) {

        OUTPUT(cos_out) = 0;
        OUTPUT(sin_out) = 0;
        PARTIAL(cos_out, fm_in) = 0;
        PARTIAL(cos_out, am_in) = 0;
        PARTIAL(sin_out, fm_in) = 0;
        PARTIAL(sin_out, am_in) = 0;
        phase = (double *) cm_analog_get_ptr(INT1, 0);
        *phase = 0;

    } else if (ANALYSIS == MIF_TRAN) {

        phase  = (double *) cm_analog_get_ptr(INT1, 0);
        phase1 = (double *) cm_analog_get_ptr(INT1, 1);

        fm_voltage = INPUT(fm_in);
        am_voltage = INPUT(am_in);

        freq = space_freq + (mark_freq - space_freq) * fm_voltage;

        if (freq <= 0)
            freq = 1e-16;

        *phase = *phase1 + freq * (TIME - T(1));
        radian = *phase * 2.0 * M_PI;

        sin_out = am_voltage * sin(radian);
        cos_out = am_voltage * cos(radian);

        OUTPUT(sin_out) = sin_out;
        OUTPUT(cos_out) = cos_out;
        PARTIAL(sin_out, fm_in) = 0;
        PARTIAL(sin_out, am_in) = 0;
        PARTIAL(cos_out, fm_in) = 0;
        PARTIAL(cos_out, am_in) = 0;

    } else {

        ac_gain.real = 0.0;
        ac_gain.imag = 0.0;
        AC_GAIN(sin_out, fm_in) = ac_gain;
        AC_GAIN(sin_out, am_in) = ac_gain;
        AC_GAIN(cos_out, fm_in) = ac_gain;
        AC_GAIN(cos_out, am_in) = ac_gain;
    }
}
