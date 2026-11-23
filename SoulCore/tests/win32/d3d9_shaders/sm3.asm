; (b) shader model 3.0 with semantics, a loop over constants and discards through vPos / texcoord
### vs30_loop
vs_3_0
dcl_position v0
dcl_normal v1
dcl_texcoord0 v2
dcl_color0 v3
dcl_position o0
dcl_texcoord1 o1          ; output registers are deliberately not in the order of the pixel shader inputs
dcl_color0 o2
dcl_texcoord0 o3
defi i0, 3, 0, 1, 0
def c8, 0, 0, 0, 0
mov o0, v0
mov o1, v1
mov o3, v2
mov r0, c8
loop aL, i0
add r0, r0, c[aL + 10]    ; c10 + c11 + c12
endloop
mul o2, v3, r0

### ps30_discard
ps_3_0
dcl_color0 v0
dcl_texcoord1 v1
dcl_texcoord0 v2
dcl vPos.xy
def c0, 0.125, 0.0625, 0, 0.28
mul r1.x, vPos.x, c0.x
frc r1.x, r1.x
add r0.x, r1.x, -c0.y     ; < 0 for vPos.x = 0 (mod 8): column discarded
add r0.y, v2.y, -c0.w     ; < 0 while the texture v coordinate is below 0.28: rows discarded
mov r0.zw, c0.z
texkill r0
mov oC0, v0
mov oC0.z, v1.z
