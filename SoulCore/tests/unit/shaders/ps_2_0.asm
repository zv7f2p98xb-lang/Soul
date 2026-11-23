; ps_2_0 / ps_2_x / ps_3_0 test shaders
%for OP in add sub mul min max slt sge dp3 dp4 crs pow
### p20_bin_@OP@
ps_2_0
dcl t0
dcl t1
@OP@ r0, t0, t1
mov oC0, r0
%end
%for OP in mov abs frc rcp rsq exp log nrm expp logp
### p20_un_@OP@
ps_2_0
dcl t0
@OP@ r0, t0
mov oC0, r0
%end
%for OP in mad lrp cmp dp2add
### p20_tri_@OP@
ps_2_0
dcl t0
dcl t1
dcl t2
@OP@ r0, t0, t1, t2
mov oC0, r0
%end
%for M in sat pp
### p20_mod_@M@
ps_2_0
dcl t0
mov_@M@ r0, t0
add_@M@ r0, r0, t0
sub r0, r0, t0
mov oC0, r0
%end

### p20_sincos
ps_2_0
dcl t0
def c20, -1.55009923e-6, -2.17013894e-5, 0.00260416674, 0.00026041667
def c21, -0.020833334, -0.125, 1, 0.5
sincos r0.xy, t0.x, c20, c21
mov r0.zw, c20
mov oC0, r0

### p20_tex
ps_2_0
dcl_2d s0
dcl t0.xy
dcl v0
texld r0, t0, s0
mul r0, r0, v0
mov oC0, r0

### p20_proj
ps_2_0
dcl_2d s0
dcl t0
texldp r0, t0, s0
mov oC0, r0

### p20_bias
ps_2_0
dcl_2d s0
dcl t0
texldb r0, t0, s0
mov oC0, r0

### p20_cube
ps_2_0
dcl_cube s1
dcl t1
texld r0, t1, s1
mov oC0, r0

### p20_volume
ps_2_0
dcl_volume s2
dcl t2
texld r0, t2, s2
mov oC0, r0

### p2x_deriv
ps_2_x
dcl t0.xy
dsx r0, t0
dsy r1, t0
mul r0, r0, c0
mad r0, r1, c1, r0
mov oC0, r0

### p2x_deriv2
ps_2_x
dcl t0.xy
mul r1, t0, c2
add r1, r1, c3
dsx r0, r1
mul oC0, r0, c0

### p20_lod
ps_2_0
dcl_2d s0
dcl t0.xy
mul r1, t0, c0
texld r0, r1, s0
mov oC0, r0

### p20_depth
ps_2_0
dcl t0.xy
mov oDepth, c0.x
mov oC0, c1

### p20_kill
ps_2_0
dcl t0
texkill t0
mov oC0, c0

### p2x_flow
ps_2_x
dcl t0
def c5, 1, 1, 1, 1
defi i0, 4, 0, 1, 0
sub r0, c5, c5
rep i0
add r0, r0, c1
endrep
if b0
mad r0, r0, c5, c5
endif
mov oC0, r0

### p2x_pred
ps_2_x
dcl t0
def c0, 0, 0, 0, 0
setp_gt p0, t0, c0
mov r0, c1
(p0) mov r0, c2
(!p0.y) mov r0.w, c3
mov oC0, r0

### p30_basic
ps_3_0
dcl_color0 v0
dcl_texcoord0 v1
dcl_texcoord3 v2
dcl_2d s0
texld r0, v1, s0
mul r0, r0, v0
add oC0, r0, v2

### p30_vpos
ps_3_0
dcl vPos.xy
dcl vFace
mul r0.xy, vPos, c3
cmp r1, vFace, c1, c2
mov oC0.xy, r0
mov oC0.zw, r1

### p30_loop
ps_3_0
dcl_texcoord1 v0
dcl_texcoord0 v1
dcl_texcoord2 v2
defi i0, 3, 0, 1, 0
sub r0, c1, c1
loop aL, i0
add r0, r0, v[aL]
endloop
mov oC0, r0

### p30_rel1
ps_3_0
dcl_texcoord1 v0
dcl_texcoord0 v1
dcl_texcoord2 v2
defi i1, 1, 1, 1, 0
loop aL, i1
mov oC0, v[aL]
endloop

### p30_texldl
ps_3_0
dcl_texcoord0 v0
dcl_2d s0
texldl r0, v0, s0
mov oC0, r0

### p30_texldd
ps_3_0
dcl_texcoord0 v0
dcl_2d s0
texldd r0, v0, s0, c0, c1
mov oC0, r0

### p30_flow
ps_3_0
dcl_texcoord0 v0
def c5, 1, 1, 1, 1
def c6, 0.1, 0.1, 0.1, 0.1
defi i0, 8, 0, 1, 0
sub r0, c5, c5
loop aL, i0
add r0.x, r0.x, c5.x
setp_ge p0.x, r0.x, v0.x
breakp p0.x
endloop
call l0
mul r0, r0, c6
mov oC0, r0
ret
label l0
add r0.y, r0.y, c5.y
add r0.y, r0.y, c5.y
ret

### p30_depth
ps_3_0
dcl_texcoord0 v0
mov oDepth, v0.x
mov oC0, v0

### p30_kill
ps_3_0
dcl_texcoord0 v0
mov r0, v0
texkill r0
mov oC0, c0

### p30_pred
ps_3_0
dcl_texcoord0 v0
def c0, 0, 0, 0, 0
setp_gt p0, v0, c0
mov r0, c1
(p0) mov r0, c2
(!p0.y) mov r0.w, c3
mov oC0, r0

### p30_if
ps_3_0
dcl_texcoord0 v0
if_gt v0.x, c0.x
  if_lt v0.y, c0.y
    mov oC0, c1
  else
    mov oC0, c2
  endif
else
  mov oC0, c3
endif

### hand_ps20
ps_2_0
dcl_2d s0
dcl t0.xy
texld r0, t0, s0
mov oC0, r0

### hand_vs30
vs_3_0
dcl_position v0
dcl_position o0
mov o0, v0
