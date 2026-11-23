; (a) world-view-projection from constants, texture x diffuse
### vs20_wvp
vs_2_0
dcl_position v0
dcl_color0 v1
dcl_texcoord0 v2
m4x4 oPos, v0, c0          ; c0..c3 = transposed world*view*projection
mov oD0, v1
mov oT0.xy, v2

### ps20_texmod
ps_2_0
dcl t0.xy
dcl v0
dcl_2d s0
texld r0, t0, s0
mul r0, r0, v0
mov oC0, r0
