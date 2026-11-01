#pragma once

// DeepFilterNet libDF C API (capi feature). Declared here so we do not depend
// on a generated cbindgen header. See libDF/src/capi.rs.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct DFState;

struct DFState* df_create(const char* path, float atten_lim, const char* log_level);
size_t          df_get_frame_length(struct DFState* st);
void            df_set_atten_lim(struct DFState* st, float lim_db);
void            df_set_post_filter_beta(struct DFState* st, float beta);
float           df_process_frame(struct DFState* st, float* input, float* output);
void            df_free(struct DFState* model);

#ifdef __cplusplus
}
#endif
