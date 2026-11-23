; vs_2_0 / vs_2_x / vs_3_0 test shaders
%for OP in add sub mul min max slt sge dp3 dp4 dst crs pow
### v20_bin_@OP@
vs_2_0
dcl_position v0
dcl_texcoord0 v1
dcl_texcoord1 v2
mov oPos, v0
@OP@ oT0, v1, v2
%end
%for OP in mov abs frc rcp rsq exp log nrm lit expp logp
### v20_un_@OP@
vs_2_0
dcl_position v0
dcl_texcoord0 v1
mov oPos, v0
@OP@ oT0, v1
%end
%for OP in mad lrp cmp
### v20_tri_@OP@
vs_2_0
dcl_position v0
dcl_texcoord0 v1
dcl_texcoord1 v2
dcl_texcoord2 v3
mov oPos, v0
@OP@ oT0, v1, v2, v3
%end
%for OP in m4x4 m4x3 m3x4 m3x3 m3x2
### v20_mat_@OP@
vs_2_0
dcl_position v0
mov oPos, v0
@OP@ oT0, v0, c4
%end

### v20_loops
vs_2_0
dcl_position v0
defi i0, 4, 0, 1, 0
defi i1, 3, 0, 0, 0
defi i2, 2, 0, 1, 0
defi i3, 10, 0, 1, 0
defi i4, 3, 14, 3, 0
defb b1, true
def c10, 1, 0, 0, 0
def c11, 0, 2, 0, 0
def c12, 0, 0, 3, 0
def c13, 0, 0, 0, 4
def c8, 0.5, 0.25, 0.125, 1
def c9, 1, 1, 1, 1
def c7, 5, 5, 5, 5
mov oPos, v0
sub r0, c9, c9
loop aL, i0
add r0, r0, c[aL + 10]
endloop
mov oT0, r0
sub r1, c9, c9
rep i1
add r1, r1, c8
endrep
mov oT1, r1
sub r2, c9, c9
loop aL, i0
loop aL, i2
add r2.x, r2.x, c9.x
endloop
endloop
mov oT2, r2
sub r3, c9, c9
loop aL, i3
add r3.x, r3.x, c9.x
break_ge r3.x, c7.x
endloop
mov oT3, r3
if_lt c7.x, c9.x
mov oT4, c10
else
mov oT4, c11
endif
if b0
mov oT5, c9
else
mov oT5, c8
endif
if b1
mov oT6, c9
endif
sub r4, c9, c9
loop aL, i4
add r4.x, r4.x, c[aL].x
endloop
mov oT7, r4

### v20_call
vs_2_0
dcl_position v0
def c9, 1, 1, 1, 1
mov oPos, v0
mov r0, v0
call l0
call l1
mov oT0, r0
callnz l0, b0
mov oT1, r0
ret
label l0
add r0, r0, c9
ret
label l1
add r0, r0, c9
add r0, r0, c9
ret

### v2x_pred
vs_2_x
dcl_position v0
dcl_texcoord0 v1
def c0, 0, 0, 0, 0
defi i0, 10, 0, 1, 0
mov oPos, v0
setp_gt p0, v1, c0
mov r0, c1
(p0) mov r0, c2
(!p0.x) mov r0.w, c3
mov oT0, r0
if p0.x
mov oT1, c4
else
mov oT1, c5
endif
mov r1, c0
loop aL, i0
add r1.x, r1.x, c6.x
setp_ge p0.x, r1.x, c7.x
breakp p0.x
endloop
mov oT2, r1
(p0.y) mov oT3, c2

### v20_ops
vs_2_0
dcl_position v0
dcl_texcoord0 v1
dcl_texcoord1 v2
def c20, -1.55009923e-6, -2.17013894e-5, 0.00260416674, 0.00026041667
def c21, -0.020833334, -0.125, 1, 0.5
mov oPos, v0
sgn oT4, v1, r1, r2
sincos oT5.xy, v1.x, c20, c21
mova a0.x, v1.y
mov oT6, c[a0.x + 2]

### v30_link
vs_3_0
dcl_position v0
dcl_normal v1
dcl_texcoord0 v2
dcl_color0 v3
dcl_position o0
dcl_texcoord3 o4
dcl_color0 o7
dcl_normal o2
dcl_psize o1
dcl_fog o9
mov o0, v0
mov o4, v2
mul o7, v3, c0
mov o2, v1
mov o1.x, c1.x
mov o9.x, c1.y

### v30_vtf
vs_3_0
dcl_position v0
dcl_texcoord0 v1
dcl_2d s0
dcl_position o0
dcl_texcoord0 o1
mov o0, v0
texldl r0, v1, s0
mov o1, r0

### v30_loop
vs_3_0
dcl_position v0
dcl_texcoord0 v1
dcl_texcoord1 v2
dcl_texcoord2 v3
dcl_position o0
dcl_texcoord0 o1
defi i0, 3, 1, 1, 0
mov o0, v0
mov r0, v0
loop aL, i0
add r0, r0, v[aL]
endloop
mov o1, r0

### v30_sincos
vs_3_0
dcl_position v0
dcl_texcoord0 v1
dcl_position o0
dcl_texcoord0 o1
mov o0, v0
sincos o1.xy, v1.x
