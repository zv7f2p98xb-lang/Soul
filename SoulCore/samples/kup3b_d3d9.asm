; Küp 3B (Direct3D 9): vertex and pixel shader of the sample (shader model 2.0)
; Regenerate the header with:  python3 ../tests/unit/shaders/d3dasm.py kup3b_d3d9_shaders.h kup3b_d3d9.asm
### vs20_cube
vs_2_0
dcl_position v0
dcl_normal v1
dcl_texcoord0 v2
m4x4 oPos, v0, c0          ; c0..c3: transposed world*view*projection
m3x3 r0.xyz, v1, c4        ; c4..c6: transposed world matrix (rotation only): normal into world space
dp3 r1.x, r0, c8           ; c8: direction towards the light
max r1.x, r1.x, c9.x       ; c9.x = 0
mad oD0, r1.x, c10, c11    ; diffuse colour * N.L + ambient
mov oT0.xy, v2

### ps20_cube
ps_2_0
dcl t0.xy
dcl v0
dcl_2d s0
texld r0, t0, s0
mul r0.xyz, r0, v0
mov oC0, r0
