; (c) pixel shader 1.4 with a dependent read after "phase", (d) mixed fixed function / programmable, (e) fog, (f) constants
### ps14_phase
ps_1_4
texld r0, t0
phase
texld r1, r0
mul r0, r1, v0

### vs11_wvp
vs_1_1
m4x4 oPos, v0, c0
mov oD0, v5               ; vs_1_1 without dcl: the stock register layout (position v0, diffuse v5, texcoord0 v7)
mov oT0.xy, v7

### ps20_const
ps_2_0
dcl t0.xy
dcl v0
dcl_2d s0
def c1, 0.5, 1, 0.25, 1
texld r0, t0, s0
mul r0, r0, v0
mul r0, r0, c1
mov oC0, r0

### ps20_solid
ps_2_0
def c0, 1, 0, 0, 1
mov oC0, c0

### vs30_pos
vs_3_0
dcl_position v0
dcl_position o0
mov o0, v0

### ps30_solid
ps_3_0
def c0, 1, 0, 0, 1
mov oC0, c0

### vs20_consts
vs_2_0
dcl_position v0
mov r0, c4
loop aL, i0
add r0, r0, c5
endloop
if b0
mul r0, r0, c6
endif
add oPos, v0, c7
mov oD0, r0

### ps11_diffuse
ps_1_1
mov r0, v0
