; vs_1_1 test shaders
### v11_xform
vs_1_1
m4x4 oPos, v0, c0
mov oD0, v1
mov oT0.xy, v2
mov oFog, c4.x
mov oPts, c4.y

### v11_arith1
vs_1_1
mov oPos, v0
add oT0, v0, v1
sub oT1, v0, v1
mul oT2, v0, v1
mad oT3, v0, v1, v2
dp3 oT4, v0, v1
dp4 oT5, v0, v1
min oT6, v0, v1
max oT7, v0, v1

### v11_arith2
vs_1_1
mov oPos, v0
slt oT0, v0, v1
sge oT1, v0, v1
rcp oT2, v0.x
rsq oT3, v1.y
exp oT4, v0.w
log oT5, v1.w
lit oT6, v2
dst oT7, v0, v1

### v11_arith3
vs_1_1
mov oPos, v0
frc oT0, v0
m3x3 oT1.xyz, v0, c0
m4x3 oT2.xyz, v0, c0
m3x4 oT3, v0, c0
m3x2 oT4.xy, v0, c0
expp oT5, v0.x
logp oT6, v1.x
mov oT7, -v1.wzyx

### v11_rel
vs_1_1
mov a0.x, v3.x
mov oPos, c[a0.x + 4]
mov oT0, c[a0.x]
mov oT1, c[a0.x + 200]

### v11_defs
vs_1_1
def c20, 1, 2, 3, 4
mov oPos, c20
mov oT0, c21
add oT1, c20, c21

### v11_swz
vs_1_1
mov oPos, v0.wzyx
mov oT0, -v0.xxyy
mov oT1.yz, v0.xyzw
add oT2.xw, v0.zzzz, -v1

### v11_zero
vs_1_1
mov oPos, v0
rcp oT0, c0.x
rsq oT1, c0.x
log oT2, c0.x

### hand_vs11
vs_1_1
mov oPos, v0
mov oD0, v1
mov r0, c[a0.x + 3]
