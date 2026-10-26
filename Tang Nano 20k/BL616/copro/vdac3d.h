/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VDAC3D_H
#define VDAC3D_H
#include <stdint.h>
/* VDAC4 extension to the FT command FIFO. Not commands of a physical FT812. */
#define CMD_3D_TARGET    0xffffffa0u
#define CMD_3D_PROJECT   0xffffffa1u
#define CMD_3D_IDENTITY  0xffffffa2u
#define CMD_3D_TRANSLATE 0xffffffa3u
#define CMD_3D_ROTATE    0xffffffa4u
#define CMD_3D_SCALE     0xffffffa5u
#define CMD_3D_PUSH      0xffffffa6u
#define CMD_3D_POP       0xffffffa7u
#define CMD_3D_LIGHT     0xffffffa8u
#define CMD_3D_CLEAR     0xffffffa9u
#define CMD_3D_MESH      0xffffffaau
#define CMD_3D_RENDER    0xffffffabu
#define CMD_3D_INFO      0xffffffacu
#define CMD_3D_TEXTURE  0xffffffadu
#define CMD_3D_TEXMESH  0xffffffaeu
#define CMD_3D_MATERIAL 0xffffffafu
enum vdac3d_texture_format {
    VDAC3D_INDEX8_ARGB8888=0, VDAC3D_RGB565=1,
    VDAC3D_ARGB1555=2, VDAC3D_INDEX8_MATERIAL=3
};
enum vdac3d_palette_flags { VDAC3D_UNLIT=1, VDAC3D_NO_FOG=2 };
enum vdac3d_material_flags { VDAC3D_FOG=1, VDAC3D_SMOOTH_FOG=2 };
enum vdac3d_mesh_flags { VDAC3D_CULL=1, VDAC3D_FLAT_LIGHT=2, VDAC3D_SMOOTH_LIGHT=4, VDAC3D_STATIC=8 };
enum vdac3d_target_format {
    VDAC3D_TARGET_ARGB1555=0, VDAC3D_TARGET_L8=3, VDAC3D_TARGET_RGB332=4,
    VDAC3D_TARGET_ARGB2=5, VDAC3D_TARGET_RGB565=7, VDAC3D_TARGET_SSAA=256
};
#define VDAC3D_ALPHA_REF(n) (((uint32_t)(n)&255u)<<8)
typedef struct { int16_t x,y,z,reserved; } vdac3d_flat_vertex;
typedef struct { uint16_t a,b,c,rgb565; } vdac3d_flat_face;
typedef struct { int16_t x,y,z; uint16_t u,v,r,g,b; } vdac3d_vertex;
typedef struct { uint16_t a,b,c,reserved; } vdac3d_face;
typedef struct { uint32_t argb,flags; } vdac3d_palette_entry;
/* XYZ signed Q8.8; UV unsigned Q12.4 texels; RGB unsigned Q4.12.
   Array addresses and bus writes must be 4-byte aligned, little endian. */
#endif
