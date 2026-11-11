#ifndef LINNET_FAAD_WORKSPACE_H
#define LINNET_FAAD_WORKSPACE_H
#include "faad/common.h"
#include "faad/structs.h"
/* Every mutable decoder singleton moved here; immutable math tables remain
 * resident. Bind only during a synchronous core call, never across a yield. */
struct ln_faad_workspace {
    NeAACDecStruct decoder;
    real_t fb[2][FRAME_LEN],time[2][FRAME_LEN],transform[2*FRAME_LEN];
    real_t coef1[FRAME_LEN],coef2[FRAME_LEN];
    int16_t spec[FRAME_LEN],spec1[FRAME_LEN],spec2[FRAME_LEN],coupling[FRAME_LEN];
    element sce,cpe;drc_info drc;
    uint32_t random1,random2;
    bool discard_output; /* Synchronous LC decode; zero preserves PCM output. */
    bool skip_synthesis; /* Internal intermediate-AU request; no saved spectra. */
};
struct ln_faad_workspace *ln_faad_current(void);
#endif
