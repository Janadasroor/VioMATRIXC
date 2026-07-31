#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Synchronized ABI: (double time, const double* inputs) */
typedef double (*viospice_jit_func_t)(double t, const double* inputs);
/* V2 ABI: (const char* id, double time, const double* inputs, double* output, double* jacobian) */
typedef void (*viospice_jit_v2_func_t)(const char* id, double t, const double* inputs, double* output, double* jacobian);

/* Resolve viospice_jit_lookup at runtime so we don't need import libs. */
#ifdef _WIN32
#include <windows.h>
static void* viospice_jit_lookup(const char* block_id) {
    static void* (*real_lookup)(const char*) = NULL;
    if (!real_lookup) {
        HMODULE mod = GetModuleHandleA("libngspice.dll");
        if (mod) real_lookup = (void* (*)(const char*))GetProcAddress(mod, "viospice_jit_lookup");
    }
    return real_lookup ? real_lookup(block_id) : NULL;
}
static void* viospice_jit_lookup_v2(const char* block_id) {
    static void* (*real_lookup)(const char*) = NULL;
    if (!real_lookup) {
        HMODULE mod = GetModuleHandleA("libngspice.dll");
        if (mod) real_lookup = (void* (*)(const char*))GetProcAddress(mod, "viospice_jit_lookup_v2");
    }
    return real_lookup ? real_lookup(block_id) : NULL;
}
#else
#include <dlfcn.h>
static void* viospice_jit_lookup(const char* block_id) {
    static void* (*real_lookup)(const char*) = NULL;
    if (!real_lookup) {
        void* handle = dlopen("libngspice.so", RTLD_LAZY | RTLD_LOCAL);
        if (handle) real_lookup = (void* (*)(const char*))dlsym(handle, "viospice_jit_lookup");
        if (!real_lookup) real_lookup = (void* (*)(const char*))dlsym(RTLD_DEFAULT, "viospice_jit_lookup");
    }
    return real_lookup ? real_lookup(block_id) : NULL;
}
static void* viospice_jit_lookup_v2(const char* block_id) {
    static void* (*real_lookup)(const char*) = NULL;
    if (!real_lookup) {
        void* handle = dlopen("libngspice.so", RTLD_LAZY | RTLD_LOCAL);
        if (handle) real_lookup = (void* (*)(const char*))dlsym(handle, "viospice_jit_lookup_v2");
        if (!real_lookup) real_lookup = (void* (*)(const char*))dlsym(RTLD_DEFAULT, "viospice_jit_lookup_v2");
    }
    return real_lookup ? real_lookup(block_id) : NULL;
}
#endif

void cm_viospice_jit_block(ARGS) {
    int i, size;
    double *inputs;
    const char *block_id;
    viospice_jit_func_t jit_func;
    viospice_jit_v2_func_t jit_v2_func;
    double base_output;
    double *jacobian;

    size = PORT_SIZE(in);
    inputs = (double *) alloca(size * sizeof(double));

    for (i = 0; i < size; i++) {
        inputs[i] = INPUT(in[i]);
    }

    block_id = PARAM_NULL(jit_id) ? "default" : PARAM(jit_id);
    
    /* Try V2 first (analytic Jacobian) */
    jit_v2_func = (viospice_jit_v2_func_t)viospice_jit_lookup_v2(block_id);
    if (jit_v2_func) {
        jacobian = (double *) alloca(size * sizeof(double));
        jit_v2_func(block_id, TIME, (const double*)inputs, &base_output, jacobian);
        
        OUTPUT(out) = base_output;
        if (mif_private->conn[1]->port[0]->partial && mif_private->conn[1]->port[0]->partial[0].port) {
            for (i = 0; i < size; i++) {
                PARTIAL(out, in[i]) = jacobian[i];
            }
        }
        return;
    }

    /* Fallback to V1 (numerical perturbation) */
    jit_func = (viospice_jit_func_t)viospice_jit_lookup(block_id);

    if (jit_func) {
        base_output = jit_func(TIME, (const double*)inputs);
        OUTPUT(out) = base_output;

        if (mif_private->conn[1]->port[0]->partial && mif_private->conn[1]->port[0]->partial[0].port) {
            for (i = 0; i < size; i++) {
                double saved = inputs[i];
                double delta = 1e-6 * fmax(1.0, fabs(saved));
                double perturbed_output;

                inputs[i] = saved + delta;
                perturbed_output = jit_func(TIME, (const double*)inputs);
                PARTIAL(out, in[i]) = (perturbed_output - base_output) / delta;
                inputs[i] = saved;
            }
        }
    } else {
        OUTPUT(out) = 0.0;
        if (mif_private->conn[1]->port[0]->partial && mif_private->conn[1]->port[0]->partial[0].port) {
            for (i = 0; i < size; i++) {
                PARTIAL(out, in[i]) = 0.0;
            }
        }
    }
}
