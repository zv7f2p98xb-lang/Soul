// SoulCore - Direct3D 9 shader engine (see d3d9_shader.h).
// SKELETON: parsing/execution not implemented yet. d3dsh_create() fails so the Direct3D 9 front
// end can build and run fixed-function code; the linkage helpers are real.
#include "d3d9_shader.h"
#include <stdio.h>
#include <string.h>

D3dShader *d3dsh_create(const uint32_t *tokens, size_t max_tokens, char *err, size_t errlen) {
    (void)tokens; (void)max_tokens;
    if (err && errlen) snprintf(err, errlen, "shader engine not implemented");
    return NULL;
}
void d3dsh_free(D3dShader *sh) { (void)sh; }
int d3dsh_is_pixel(const D3dShader *sh) { (void)sh; return 0; }
int d3dsh_major(const D3dShader *sh) { (void)sh; return 0; }
int d3dsh_minor(const D3dShader *sh) { (void)sh; return 0; }
size_t d3dsh_token_count(const D3dShader *sh) { (void)sh; return 0; }
const uint32_t *d3dsh_tokens(const D3dShader *sh) { (void)sh; return NULL; }

int d3dsh_varying_slot(int usage, int usage_index) {
    switch (usage) {
    case D3DSU_TEXCOORD: return usage_index >= 0 && usage_index < 8 ? usage_index : (usage_index == 8 ? 11 : -1);
    case D3DSU_COLOR: return usage_index == 0 ? 8 : usage_index == 1 ? 9 : -1;
    case D3DSU_FOG: return usage_index == 0 ? 10 : -1;
    case D3DSU_NORMAL: case D3DSU_TANGENT: case D3DSU_BINORMAL: case D3DSU_BLENDWEIGHT: case D3DSU_BLENDINDICES:
    case D3DSU_TESSFACTOR: case D3DSU_DEPTH: case D3DSU_SAMPLE:
        return 11; // shared slot: only one such semantic per shader pair (the engine reports -1 for a second)
    default: return -1;
    }
}

int d3dsh_vs_inputs(const D3dShader *vs, D3dShaderInput *out, int max) { (void)vs; (void)out; (void)max; return 0; }
uint32_t d3dsh_vs_output_mask(const D3dShader *vs) { (void)vs; return 0; }
uint32_t d3dsh_ps_input_mask(const D3dShader *ps) { (void)ps; return 0; }
int d3dsh_ps_nvary(const D3dShader *ps) { (void)ps; return 0; }
int d3dsh_ps_writes_depth(const D3dShader *ps) { (void)ps; return 0; }
uint32_t d3dsh_ps_sampler_mask(const D3dShader *ps) { (void)ps; return 0; }
int d3dsh_ps_sampler_type(const D3dShader *ps, int s) { (void)ps; (void)s; return -1; }

void d3dsh_run_vs(const D3dShader *vs, const D3dShaderConsts *c, const float in[D3DSH_VS_INPUTS][4], D3dVsOut *out) {
    D3dVsCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.c = c;
    d3dsh_run_vs_ex(vs, &ctx, in, out);
}
void d3dsh_run_vs_ex(const D3dShader *vs, const D3dVsCtx *ctx, const float in[D3DSH_VS_INPUTS][4], D3dVsOut *out) {
    (void)vs; (void)ctx; (void)in;
    memset(out, 0, sizeof *out);
    out->pos[3] = 1;
}
void d3dsh_run_vs_batch(const D3dShader *vs, const D3dVsCtx *ctx, const float (*in)[D3DSH_VS_INPUTS][4], D3dVsOut *out, int n) {
    for (int i = 0; i < n; i++) d3dsh_run_vs_ex(vs, ctx, in[i], &out[i]);
}

int d3dsh_ps_pixel(void *ctx, const SgPixelIn *in, float color_out[4], float *depth_out) {
    (void)ctx; (void)in; (void)depth_out;
    color_out[0] = color_out[1] = color_out[2] = 0; color_out[3] = 1;
    return 1;
}
