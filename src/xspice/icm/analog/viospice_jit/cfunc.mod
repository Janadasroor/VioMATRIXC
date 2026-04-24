#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Synchronized ABI: (double time, const double* inputs) */
typedef double (*viospice_jit_func_t)(double t, const double* inputs);
extern void* viospice_jit_lookup(const char* block_id);

void cm_viospice_jit_block(ARGS) {
    int i, size;
    double *inputs;
    const char *block_id;
    viospice_jit_func_t jit_func;
    double base_output;

    size = PORT_SIZE(in);
    inputs = (double *) alloca(size * sizeof(double));

    for (i = 0; i < size; i++) {
        inputs[i] = INPUT(in[i]);
    }

    block_id = PARAM_NULL(jit_id) ? "default" : PARAM(jit_id);
    jit_func = (viospice_jit_func_t)viospice_jit_lookup(block_id);

    if (jit_func) {
        /*
         * Guardrail:
         * Native SmartSignal blocks can be strongly nonlinear functions of
         * their analog inputs. If we only stamp OUTPUT(out) and never publish
         * PARTIAL(out, in[i]), ngspice may evaluate the JIT function against a
         * stale input estimate during transient Newton iterations. The visible
         * failure mode is a block that follows constants but reads live analog
         * inputs as 0 V except at the initial/final solve points.
         *
         * Use a small numerical derivative so all future SmartSignal A-devices
         * stay solver-coupled without hardcoding per-block math here.
         */
        base_output = jit_func(TIME, (const double*)inputs);
        OUTPUT(out) = base_output;

        for (i = 0; i < size; i++) {
            double saved = inputs[i];
            double delta = 1e-6 * fmax(1.0, fabs(saved));
            double perturbed_output;

            inputs[i] = saved + delta;
            perturbed_output = jit_func(TIME, (const double*)inputs);
            PARTIAL(out, in[i]) = (perturbed_output - base_output) / delta;
            inputs[i] = saved;
        }
    } else {
        OUTPUT(out) = 0.0;
        for (i = 0; i < size; i++) {
            PARTIAL(out, in[i]) = 0.0;
        }
    }
}
