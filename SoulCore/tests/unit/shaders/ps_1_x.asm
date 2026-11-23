; ps_1_1 .. ps_1_4 test shaders
### p11_arith
ps_1_1
def c3, 0.5, 0.25, 1, 0.75
mov r0, v0
mul r0, r0, c0
add r0.rgb, r0, v1
+ mov r0.a, c3.a

### p11_clamp
ps_1_1
def c4, 5, -5, 0.5, 2
mov r0, c4

### p11_mods
ps_1_1
mov r0, v0_bx2
mov r1, 1-v1
add r0, r0, r1
mul_x2 r0.rgb, r0, v1

### p11_shift
ps_1_1
add_d4 r0, v0, v1
mul_x2_sat r1, v0, v1
add r0.a, r0, r1

### p11_cnd
ps_1_1
mov r0, c0
cnd r0, r0.a, c1, c2

### p11_dp3lrp
ps_1_1
dp3 r0, v0_bx2, v1_bx2
lrp r1, c0, v0, v1
mad r0.rgb, r0, r1, c1

### p11_tex
ps_1_1
tex t0
tex t1
mul r0, t0, t1

### p11_texcoord
ps_1_1
texcoord t0
mov r0, t0

### p11_texkill
ps_1_1
texkill t0
mov r0, v0

### p11_texbem
ps_1_1
tex t0
texbem t1, t0
mov r0, t1

### p11_texbeml
ps_1_1
tex t0
texbeml t1, t0
mov r0, t1

### p11_m3x2
ps_1_1
tex t0
texm3x2pad t1, t0_bx2
texm3x2tex t2, t0_bx2
mov r0, t2

### p11_m3x3
ps_1_1
tex t0
texm3x3pad t1, t0_bx2
texm3x3pad t2, t0_bx2
texm3x3tex t3, t0_bx2
mov r0, t3

### p11_m3x3spec
ps_1_1
tex t0
texm3x3pad t1, t0_bx2
texm3x3pad t2, t0_bx2
texm3x3spec t3, t0_bx2, c0
mov r0, t3

### p11_m3x3vspec
ps_1_1
tex t0
texm3x3pad t1, t0_bx2
texm3x3pad t2, t0_bx2
texm3x3vspec t3, t0_bx2
mov r0, t3

### p11_reg2ar
ps_1_1
tex t0
texreg2ar t1, t0
mov r0, t1

### p11_reg2gb
ps_1_1
tex t0
texreg2gb t1, t0
mov r0, t1

### p13_cmp
ps_1_3
mov r0, c0
cmp r0, r0, c1, c2

### p13_dp4
ps_1_3
dp4 r0, v0, v1

### p13_texdp3tex
ps_1_3
tex t0
texdp3tex t1, t0
mov r0, t1

### p13_texdp3
ps_1_3
tex t0
texdp3 t1, t0
mov r0, t1

### p13_m3x3
ps_1_3
tex t0
texm3x3pad t1, t0_bx2
texm3x3pad t2, t0_bx2
texm3x3 t3, t0_bx2
mov r0, t3

### p13_m3x2depth
ps_1_3
tex t0
texm3x2pad t1, t0
texm3x2depth t2, t0
mov r0, c0

### p14_basic
ps_1_4
texld r0, t0
texld r1, t1
phase
texld r2, r0
mul r0, r2, r1

### p14_proj
ps_1_4
texld r0, t0_dw.xyw

### p14_projz
ps_1_4
texld r0, t0_dz.xyz

### p14_texcrd
ps_1_4
texcrd r1.rgb, t0
mov r0, r1

### p14_cnd
ps_1_4
cnd r0, v0, v1, c1

### p14_texdepth
ps_1_4
def c0, 0.25, 0.5, 0, 0
mov r5, c0
texdepth r5
mov r0, c1

### p14_texkill
ps_1_4
texcrd r0.rgb, t0
texkill r0
mov r0, v0

### p14_bem
ps_1_4
def c0, 0.5, 0.25, 0, 0
def c1, 0.5, 0.5, 0, 0
mov r0, c0
mov r1, c1
bem r2.rg, r0, r1
mov r0, r2

### hand_ps11
ps_1_1
tex t0
mov r0, t0
