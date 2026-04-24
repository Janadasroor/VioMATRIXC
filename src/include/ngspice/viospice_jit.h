#ifndef VIOSPICE_JIT_H
#define VIOSPICE_JIT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef double (*viospice_jit_func_t)(double t, const double* inputs);

/* Exported for Qt App */
#if defined(_WIN32) || defined(__CYGWIN__)
  #define JIT_EXPORT __declspec(dllexport)
#else
  #define JIT_EXPORT __attribute__((visibility("default")))
#endif

JIT_EXPORT void ngSpice_RegisterJitLogic(const char* block_id, viospice_jit_func_t func_ptr);
viospice_jit_func_t viospice_jit_lookup(const char* block_id);

#ifdef __cplusplus
}
#endif

#endif
