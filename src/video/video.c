/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Main video-rendering module.
 *
 *          Video timing settings -
 *
 *            8-bit - 1mb/sec
 *              B = 8 ISA clocks
 *              W = 16 ISA clocks
 *              L = 32 ISA clocks
 *
 *            Slow 16-bit - 2mb/sec
 *              B = 6 ISA clocks
 *              W = 8 ISA clocks
 *              L = 16 ISA clocks
 *
 *            Fast 16-bit - 4mb/sec
 *              B = 3 ISA clocks
 *              W = 3 ISA clocks
 *              L = 6 ISA clocks
 *
 *            Slow VLB/PCI - 8mb/sec (ish)
 *              B = 4 bus clocks
 *              W = 8 bus clocks
 *              L = 16 bus clocks
 *
 *            Mid VLB/PCI -
 *              B = 4 bus clocks
 *              W = 5 bus clocks
 *              L = 10 bus clocks
 *
 *            Fast VLB/PCI -
 *              B = 3 bus clocks
 *              W = 3 bus clocks
 *              L = 4 bus clocks
 *
 *
 *
 * Authors: Sarah Walker, <https://pcem-emulator.co.uk/>
 *          Miran Grca, <mgrca8@gmail.com>
 *
 *          Copyright 2008-2019 Sarah Walker.
 *          Copyright 2016-2019 Miran Grca.
 */
#include <stdatomic.h>
#define PNG_DEBUG 0
#include <png.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include <86box/device.h>
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/rom.h>
#include <86box/config.h>
#include <86box/timer.h>
#include <86box/path.h>
#include <86box/plat.h>
#include <86box/ui.h>
#include <86box/thread.h>
#include <86box/video.h>
#include <86box/vid_svga.h>

#include <minitrace/minitrace.h>

volatile int screenshots = 0;
uint8_t      edatlookup[4][4];
uint8_t      egaremap2bpp[256];
uint8_t      fontdat[2048][8];            /* IBM CGA font */
uint8_t      fontdatm[2048][16];          /* IBM MDA font */
uint8_t      fontdat2[2048][8];           /* IBM CGA 2nd instance font */
uint8_t      fontdatm2[2048][16];         /* IBM MDA 2nd instance font */
uint8_t      fontdatw[512][32];           /* Wyse700 font */
uint8_t      fontdat8x12[256][16];        /* MDSI Genius font */
uint8_t      fontdat12x18[256][36];       /* IM1024 font */
dbcs_font_t *fontdatksc5601       = NULL; /* Korean KSC-5601 font */
dbcs_font_t *fontdatksc5601_user  = NULL; /* Korean KSC-5601 user defined font */
int          herc_blend           = 0;
int          frames               = 0;
int          fullchange           = 0;
int          video_grayscale      = 0;
int          video_graytype       = 0;
int          monitor_index_global = 0;
uint32_t    *video_6to8           = NULL;
uint32_t    *video_8togs          = NULL;
uint32_t    *video_8to32          = NULL;
uint32_t    *video_15to32         = NULL;
uint32_t    *video_16to32         = NULL;
monitor_t          monitors[MONITORS_NUM];
monitor_settings_t monitor_settings[MONITORS_NUM];
atomic_bool        doresize_monitors[MONITORS_NUM];

#ifdef _WIN32
void * (*__cdecl video_copy)(void *_Dst, const void *_Src, size_t _Size) = memcpy;
#else
void *(*video_copy)(void *__restrict, const void *__restrict, size_t);
#endif

PALETTE cgapal = {
    {0,0,0},    {0,42,0},   {42,0,0},   {42,21,0},
    {0,0,0},    {0,42,42},  {42,0,42},  {42,42,42},
    {0,0,0},    {21,63,21}, {63,21,21},  {63,63,21},
    {0,0,0},    {21,63,63}, {63,21,63}, {63,63,63},

    {0,0,0},    {0,0,42},   {0,42,0},   {0,42,42},
    {42,0,0},   {42,0,42},  {42,21,00}, {42,42,42},
    {21,21,21}, {21,21,63}, {21,63,21}, {21,63,63},
    {63,21,21}, {63,21,63}, {63,63,21}, {63,63,63},

    {0,0,0},    {0,21,0},   {0,0,42},   {0,42,42},
    {42,0,21},  {21,10,21}, {42,0,42},  {42,0,63},
    {21,21,21}, {21,63,21}, {42,21,42}, {21,63,63},
    {63,0,0},   {42,42,0},  {63,21,42}, {41,41,41},

    {0,0,0},   {0,42,42},   {42,0,0},   {42,42,42},
    {0,0,0},   {0,42,42},   {42,0,0},   {42,42,42},
    {0,0,0},   {0,63,63},   {63,0,0},   {63,63,63},
    {0,0,0},   {0,63,63},   {63,0,0},   {63,63,63},
};
PALETTE cgapal_mono[6] = {
    { /* 0 - green, 4-color-optimized contrast. */
        {0x00,0x00,0x00},{0x00,0x0d,0x03},{0x01,0x17,0x05},
        {0x01,0x1a,0x06},{0x02,0x28,0x09},{0x02,0x2c,0x0a},
        {0x03,0x39,0x0d},{0x03,0x3c,0x0e},{0x00,0x07,0x01},
        {0x01,0x13,0x04},{0x01,0x1f,0x07},{0x01,0x23,0x08},
        {0x02,0x31,0x0b},{0x02,0x35,0x0c},{0x05,0x3f,0x11},{0x0d,0x3f,0x17},
    },
    { /* 1 - green, 16-color-optimized contrast. */
        {0x00,0x00,0x00},{0x00,0x0d,0x03},{0x01,0x15,0x05},
        {0x01,0x17,0x05},{0x01,0x21,0x08},{0x01,0x24,0x08},
        {0x02,0x2e,0x0b},{0x02,0x31,0x0b},{0x01,0x22,0x08},
        {0x02,0x28,0x09},{0x02,0x30,0x0b},{0x02,0x32,0x0c},
        {0x03,0x39,0x0d},{0x03,0x3b,0x0e},{0x09,0x3f,0x14},{0x0d,0x3f,0x17},
    },
    { /* 2 - amber, 4-color-optimized contrast. */
        {0x00,0x00,0x00},{0x15,0x05,0x00},{0x20,0x0b,0x00},
        {0x24,0x0d,0x00},{0x33,0x18,0x00},{0x37,0x1b,0x00},
        {0x3f,0x26,0x01},{0x3f,0x2b,0x06},{0x0b,0x02,0x00},
        {0x1b,0x08,0x00},{0x29,0x11,0x00},{0x2e,0x14,0x00},
        {0x3b,0x1e,0x00},{0x3e,0x21,0x00},{0x3f,0x32,0x0a},{0x3f,0x38,0x0d},
    },
    { /* 3 - amber, 16-color-optimized contrast. */
        {0x00,0x00,0x00},{0x15,0x05,0x00},{0x1e,0x09,0x00},
        {0x21,0x0b,0x00},{0x2b,0x12,0x00},{0x2f,0x15,0x00},
        {0x38,0x1c,0x00},{0x3b,0x1e,0x00},{0x2c,0x13,0x00},
        {0x32,0x17,0x00},{0x3a,0x1e,0x00},{0x3c,0x1f,0x00},
        {0x3f,0x27,0x01},{0x3f,0x2a,0x04},{0x3f,0x36,0x0c},{0x3f,0x38,0x0d},
    },
    { /* 4 - grey, 4-color-optimized contrast. */
        {0x00,0x00,0x00},{0x0e,0x0f,0x10},{0x15,0x17,0x18},
        {0x18,0x1a,0x1b},{0x24,0x25,0x25},{0x27,0x28,0x28},
        {0x33,0x34,0x32},{0x37,0x38,0x35},{0x09,0x0a,0x0b},
        {0x11,0x12,0x13},{0x1c,0x1e,0x1e},{0x20,0x22,0x22},
        {0x2c,0x2d,0x2c},{0x2f,0x30,0x2f},{0x3c,0x3c,0x38},{0x3f,0x3f,0x3b},
    },
    { /* 5 - grey, 16-color-optimized contrast. */
        {0x00,0x00,0x00},{0x0e,0x0f,0x10},{0x13,0x14,0x15},
        {0x15,0x17,0x18},{0x1e,0x20,0x20},{0x20,0x22,0x22},
        {0x29,0x2a,0x2a},{0x2c,0x2d,0x2c},{0x1f,0x21,0x21},
        {0x23,0x25,0x25},{0x2b,0x2c,0x2b},{0x2d,0x2e,0x2d},
        {0x34,0x35,0x33},{0x37,0x37,0x34},{0x3e,0x3e,0x3a},{0x3f,0x3f,0x3b},
    }
};

const uint32_t shade[5][256] = {
    {0}, // RGB Color (unused)
    {0}, // RGB Grayscale (unused)
    {    // Amber monitor
        0x000000, 0x060000, 0x090000, 0x0d0000, 0x100000, 0x120100, 0x150100, 0x170100, 0x1a0100, 0x1c0100, 0x1e0200, 0x210200, 0x230200, 0x250300, 0x270300, 0x290300,
        0x2b0400, 0x2d0400, 0x2f0400, 0x300500, 0x320500, 0x340500, 0x360600, 0x380600, 0x390700, 0x3b0700, 0x3d0700, 0x3f0800, 0x400800, 0x420900, 0x440900, 0x450a00,
        0x470a00, 0x480b00, 0x4a0b00, 0x4c0c00, 0x4d0c00, 0x4f0d00, 0x500d00, 0x520e00, 0x530e00, 0x550f00, 0x560f00, 0x581000, 0x591000, 0x5b1100, 0x5c1200, 0x5e1200,
        0x5f1300, 0x601300, 0x621400, 0x631500, 0x651500, 0x661600, 0x671600, 0x691700, 0x6a1800, 0x6c1800, 0x6d1900, 0x6e1a00, 0x701a00, 0x711b00, 0x721c00, 0x741c00,
        0x751d00, 0x761e00, 0x781e00, 0x791f00, 0x7a2000, 0x7c2000, 0x7d2100, 0x7e2200, 0x7f2300, 0x812300, 0x822400, 0x832500, 0x842600, 0x862600, 0x872700, 0x882800,
        0x8a2900, 0x8b2900, 0x8c2a00, 0x8d2b00, 0x8e2c00, 0x902c00, 0x912d00, 0x922e00, 0x932f00, 0x953000, 0x963000, 0x973100, 0x983200, 0x993300, 0x9b3400, 0x9c3400,
        0x9d3500, 0x9e3600, 0x9f3700, 0xa03800, 0xa23900, 0xa33a00, 0xa43a00, 0xa53b00, 0xa63c00, 0xa73d00, 0xa93e00, 0xaa3f00, 0xab4000, 0xac4000, 0xad4100, 0xae4200,
        0xaf4300, 0xb14400, 0xb24500, 0xb34600, 0xb44700, 0xb54800, 0xb64900, 0xb74a00, 0xb94a00, 0xba4b00, 0xbb4c00, 0xbc4d00, 0xbd4e00, 0xbe4f00, 0xbf5000, 0xc05100,
        0xc15200, 0xc25300, 0xc45400, 0xc55500, 0xc65600, 0xc75700, 0xc85800, 0xc95900, 0xca5a00, 0xcb5b00, 0xcc5c00, 0xcd5d00, 0xce5e00, 0xcf5f00, 0xd06000, 0xd26101,
        0xd36201, 0xd46301, 0xd56401, 0xd66501, 0xd76601, 0xd86701, 0xd96801, 0xda6901, 0xdb6a01, 0xdc6b01, 0xdd6c01, 0xde6d01, 0xdf6e01, 0xe06f01, 0xe17001, 0xe27201,
        0xe37301, 0xe47401, 0xe57501, 0xe67602, 0xe77702, 0xe87802, 0xe97902, 0xeb7a02, 0xec7b02, 0xed7c02, 0xee7e02, 0xef7f02, 0xf08002, 0xf18103, 0xf28203, 0xf38303,
        0xf48403, 0xf58503, 0xf68703, 0xf78803, 0xf88903, 0xf98a04, 0xfa8b04, 0xfb8c04, 0xfc8d04, 0xfd8f04, 0xfe9005, 0xff9105, 0xff9205, 0xff9305, 0xff9405, 0xff9606,
        0xff9706, 0xff9806, 0xff9906, 0xff9a07, 0xff9b07, 0xff9d07, 0xff9e08, 0xff9f08, 0xffa008, 0xffa109, 0xffa309, 0xffa409, 0xffa50a, 0xffa60a, 0xffa80a, 0xffa90b,
        0xffaa0b, 0xffab0c, 0xffac0c, 0xffae0d, 0xffaf0d, 0xffb00e, 0xffb10e, 0xffb30f, 0xffb40f, 0xffb510, 0xffb610, 0xffb811, 0xffb912, 0xffba12, 0xffbb13, 0xffbd14,
        0xffbe14, 0xffbf15, 0xffc016, 0xffc217, 0xffc317, 0xffc418, 0xffc619, 0xffc71a, 0xffc81b, 0xffca1c, 0xffcb1d, 0xffcc1e, 0xffcd1f, 0xffcf20, 0xffd021, 0xffd122,
        0xffd323, 0xffd424, 0xffd526, 0xffd727, 0xffd828, 0xffd92a, 0xffdb2b, 0xffdc2c, 0xffdd2e, 0xffdf2f, 0xffe031, 0xffe133, 0xffe334, 0xffe436, 0xffe538, 0xffe739
    },
    { // Green monitor
        0x000000, 0x000400, 0x000700, 0x000900, 0x000b00, 0x000d00, 0x000f00, 0x001100, 0x001300, 0x001500, 0x001600, 0x001800, 0x001a00, 0x001b00, 0x001d00, 0x001e00,
        0x002000, 0x002100, 0x002300, 0x002400, 0x002601, 0x002701, 0x002901, 0x002a01, 0x002b01, 0x002d01, 0x002e01, 0x002f01, 0x003101, 0x003201, 0x003301, 0x003401,
        0x003601, 0x003702, 0x003802, 0x003902, 0x003b02, 0x003c02, 0x003d02, 0x003e02, 0x004002, 0x004102, 0x004203, 0x004303, 0x004403, 0x004503, 0x004703, 0x004803,
        0x004903, 0x004a03, 0x004b04, 0x004c04, 0x004d04, 0x004e04, 0x005004, 0x005104, 0x005205, 0x005305, 0x005405, 0x005505, 0x005605, 0x005705, 0x005806, 0x005906,
        0x005a06, 0x005b06, 0x005d06, 0x005e07, 0x005f07, 0x006007, 0x006107, 0x006207, 0x006308, 0x006408, 0x006508, 0x006608, 0x006708, 0x006809, 0x006909, 0x006a09,
        0x006b09, 0x016c0a, 0x016d0a, 0x016e0a, 0x016f0a, 0x01700b, 0x01710b, 0x01720b, 0x01730b, 0x01740c, 0x01750c, 0x01760c, 0x01770c, 0x01780d, 0x01790d, 0x017a0d,
        0x017b0d, 0x017b0e, 0x017c0e, 0x017d0e, 0x017e0f, 0x017f0f, 0x01800f, 0x018110, 0x028210, 0x028310, 0x028410, 0x028511, 0x028611, 0x028711, 0x028812, 0x028912,
        0x028a12, 0x028a13, 0x028b13, 0x028c13, 0x028d14, 0x028e14, 0x038f14, 0x039015, 0x039115, 0x039215, 0x039316, 0x039416, 0x039417, 0x039517, 0x039617, 0x039718,
        0x049818, 0x049918, 0x049a19, 0x049b19, 0x049c19, 0x049c1a, 0x049d1a, 0x049e1b, 0x059f1b, 0x05a01b, 0x05a11c, 0x05a21c, 0x05a31c, 0x05a31d, 0x05a41d, 0x06a51e,
        0x06a61e, 0x06a71f, 0x06a81f, 0x06a920, 0x06aa20, 0x07aa21, 0x07ab21, 0x07ac21, 0x07ad22, 0x07ae22, 0x08af23, 0x08b023, 0x08b024, 0x08b124, 0x08b225, 0x09b325,
        0x09b426, 0x09b526, 0x09b527, 0x0ab627, 0x0ab728, 0x0ab828, 0x0ab929, 0x0bba29, 0x0bba2a, 0x0bbb2a, 0x0bbc2b, 0x0cbd2b, 0x0cbe2c, 0x0cbf2c, 0x0dbf2d, 0x0dc02d,
        0x0dc12e, 0x0ec22e, 0x0ec32f, 0x0ec42f, 0x0fc430, 0x0fc530, 0x0fc631, 0x10c731, 0x10c832, 0x10c932, 0x11c933, 0x11ca33, 0x11cb34, 0x12cc35, 0x12cd35, 0x12cd36,
        0x13ce36, 0x13cf37, 0x13d037, 0x14d138, 0x14d139, 0x14d239, 0x15d33a, 0x15d43a, 0x16d43b, 0x16d53b, 0x17d63c, 0x17d73d, 0x17d83d, 0x18d83e, 0x18d93e, 0x19da3f,
        0x19db40, 0x1adc40, 0x1adc41, 0x1bdd41, 0x1bde42, 0x1cdf43, 0x1ce043, 0x1de044, 0x1ee145, 0x1ee245, 0x1fe346, 0x1fe446, 0x20e447, 0x20e548, 0x21e648, 0x22e749,
        0x22e74a, 0x23e84a, 0x23e94b, 0x24ea4c, 0x25ea4c, 0x25eb4d, 0x26ec4e, 0x27ed4e, 0x27ee4f, 0x28ee50, 0x29ef50, 0x29f051, 0x2af152, 0x2bf153, 0x2cf253, 0x2cf354,
        0x2df455, 0x2ef455, 0x2ff556, 0x2ff657, 0x30f758, 0x31f758, 0x32f859, 0x32f95a, 0x33fa5a, 0x34fa5b, 0x35fb5c, 0x36fc5d, 0x37fd5d, 0x38fd5e, 0x38fe5f, 0x39ff60
    },
    { // White monitor
        0x000000, 0x010102, 0x020203, 0x020304, 0x030406, 0x040507, 0x050608, 0x060709, 0x07080a, 0x08090c, 0x080a0d, 0x090b0e, 0x0a0c0f, 0x0b0d10, 0x0c0e11, 0x0d0f12,
        0x0e1013, 0x0f1115, 0x101216, 0x111317, 0x121418, 0x121519, 0x13161a, 0x14171b, 0x15181c, 0x16191d, 0x171a1e, 0x181b1f, 0x191c20, 0x1a1d21, 0x1b1e22, 0x1c1f23,
        0x1d2024, 0x1e2125, 0x1f2226, 0x202327, 0x212428, 0x222529, 0x22262b, 0x23272c, 0x24282d, 0x25292e, 0x262a2f, 0x272b30, 0x282c30, 0x292d31, 0x2a2e32, 0x2b2f33,
        0x2c3034, 0x2d3035, 0x2e3136, 0x2f3237, 0x303338, 0x313439, 0x32353a, 0x33363b, 0x34373c, 0x35383d, 0x36393e, 0x373a3f, 0x383b40, 0x393c41, 0x3a3d42, 0x3b3e43,
        0x3c3f44, 0x3d4045, 0x3e4146, 0x3f4247, 0x404348, 0x414449, 0x42454a, 0x43464b, 0x44474c, 0x45484d, 0x46494d, 0x474a4e, 0x484b4f, 0x484c50, 0x494d51, 0x4a4e52,
        0x4b4f53, 0x4c5054, 0x4d5155, 0x4e5256, 0x4f5357, 0x505458, 0x515559, 0x52565a, 0x53575b, 0x54585b, 0x55595c, 0x565a5d, 0x575b5e, 0x585c5f, 0x595d60, 0x5a5e61,
        0x5b5f62, 0x5c6063, 0x5d6164, 0x5e6265, 0x5f6366, 0x606466, 0x616567, 0x626668, 0x636769, 0x64686a, 0x65696b, 0x666a6c, 0x676b6d, 0x686c6e, 0x696d6f, 0x6a6e70,
        0x6b6f70, 0x6c7071, 0x6d7172, 0x6f7273, 0x707374, 0x707475, 0x717576, 0x727677, 0x747778, 0x757879, 0x767979, 0x777a7a, 0x787b7b, 0x797c7c, 0x7a7d7d, 0x7b7e7e,
        0x7c7f7f, 0x7d8080, 0x7e8181, 0x7f8281, 0x808382, 0x818483, 0x828584, 0x838685, 0x848786, 0x858887, 0x868988, 0x878a89, 0x888b89, 0x898c8a, 0x8a8d8b, 0x8b8e8c,
        0x8c8f8d, 0x8d8f8e, 0x8e908f, 0x8f9190, 0x909290, 0x919391, 0x929492, 0x939593, 0x949694, 0x959795, 0x969896, 0x979997, 0x989a98, 0x999b98, 0x9a9c99, 0x9b9d9a,
        0x9c9e9b, 0x9d9f9c, 0x9ea09d, 0x9fa19e, 0xa0a29f, 0xa1a39f, 0xa2a4a0, 0xa3a5a1, 0xa4a6a2, 0xa6a7a3, 0xa7a8a4, 0xa8a9a5, 0xa9aaa5, 0xaaaba6, 0xabaca7, 0xacada8,
        0xadaea9, 0xaeafaa, 0xafb0ab, 0xb0b1ac, 0xb1b2ac, 0xb2b3ad, 0xb3b4ae, 0xb4b5af, 0xb5b6b0, 0xb6b7b1, 0xb7b8b2, 0xb8b9b2, 0xb9bab3, 0xbabbb4, 0xbbbcb5, 0xbcbdb6,
        0xbdbeb7, 0xbebfb8, 0xbfc0b8, 0xc0c1b9, 0xc1c2ba, 0xc2c3bb, 0xc3c4bc, 0xc5c5bd, 0xc6c6be, 0xc7c7be, 0xc8c8bf, 0xc9c9c0, 0xcacac1, 0xcbcbc2, 0xccccc3, 0xcdcdc3,
        0xcecec4, 0xcfcfc5, 0xd0d0c6, 0xd1d1c7, 0xd2d2c8, 0xd3d3c9, 0xd4d4c9, 0xd5d5ca, 0xd6d6cb, 0xd7d7cc, 0xd8d8cd, 0xd9d9ce, 0xdadacf, 0xdbdbcf, 0xdcdcd0, 0xdeddd1,
        0xdfded2, 0xe0dfd3, 0xe1e0d4, 0xe2e1d4, 0xe3e2d5, 0xe4e3d6, 0xe5e4d7, 0xe6e5d8, 0xe7e6d9, 0xe8e7d9, 0xe9e8da, 0xeae9db, 0xebeadc, 0xecebdd, 0xedecde, 0xeeeddf,
        0xefeedf, 0xf0efe0, 0xf1f0e1, 0xf2f1e2, 0xf3f2e3, 0xf4f3e3, 0xf6f3e4, 0xf7f4e5, 0xf8f5e6, 0xf9f6e7, 0xfaf7e8, 0xfbf8e9, 0xfcf9e9, 0xfdfaea, 0xfefbeb, 0xfffcec
    }
};

typedef struct blit_data_struct {
    int x, y, w, h;
    int busy;
    int buffer_in_use;
    int thread_run;
    int monitor_index;

    thread_t *blit_thread;
    event_t  *wake_blit_thread;
    event_t  *blit_complete;
    event_t  *buffer_not_in_use;
} blit_data_t;

static uint32_t cga_2_table[16];

static void (*blit_func)(int x, int y, int w, int h, int monitor_index);

#ifdef ENABLE_VIDEO_LOG
int video_do_log = ENABLE_VIDEO_LOG;

static void
video_log(const char *fmt, ...)
{
    va_list ap;

    if (video_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define video_log(fmt, ...)
#endif

void
video_setblit(void (*blit)(int, int, int, int, int))
{
    blit_func = blit;
}

void
video_blit_complete_monitor(int monitor_index)
{
    blit_data_t *blit_data_ptr   = monitors[monitor_index].mon_blit_data_ptr;
    blit_data_ptr->buffer_in_use = 0;

    thread_set_event(blit_data_ptr->buffer_not_in_use);
}

void
video_wait_for_blit_monitor(int monitor_index)
{
    blit_data_t *blit_data_ptr = monitors[monitor_index].mon_blit_data_ptr;

    while (blit_data_ptr->busy)
        thread_wait_event(blit_data_ptr->blit_complete, -1);
    thread_reset_event(blit_data_ptr->blit_complete);
}

void
video_wait_for_buffer_monitor(int monitor_index)
{
    blit_data_t *blit_data_ptr = monitors[monitor_index].mon_blit_data_ptr;

    while (blit_data_ptr->buffer_in_use)
        thread_wait_event(blit_data_ptr->buffer_not_in_use, -1);
    thread_reset_event(blit_data_ptr->buffer_not_in_use);
}

static png_structp png_ptr[MONITORS_NUM];
static png_infop   info_ptr[MONITORS_NUM];

static void
video_take_screenshot_monitor(const char *fn, uint32_t *buf, int start_x, int start_y, int row_len, int monitor_index)
{
    png_bytep         *b_rgb         = NULL;
    FILE              *fp            = NULL;
    uint32_t           temp          = 0x00000000;
    const blit_data_t *blit_data_ptr = monitors[monitor_index].mon_blit_data_ptr;

    /* create file */
    fp = plat_fopen(fn, (const char *) "wb");
    if (!fp) {
        video_log("[video_take_screenshot] File %s could not be opened for writing", fn);
        return;
    }

    /* initialize stuff */
    png_ptr[monitor_index] = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

    if (!png_ptr[monitor_index]) {
        video_log("[video_take_screenshot] png_create_write_struct failed");
        fclose(fp);
        return;
    }

    info_ptr[monitor_index] = png_create_info_struct(png_ptr[monitor_index]);
    if (!info_ptr[monitor_index]) {
        video_log("[video_take_screenshot] png_create_info_struct failed");
        fclose(fp);
        return;
    }

    png_init_io(png_ptr[monitor_index], fp);

    png_set_IHDR(png_ptr[monitor_index], info_ptr[monitor_index], blit_data_ptr->w, blit_data_ptr->h,
                 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

    b_rgb = (png_bytep *) malloc(sizeof(png_bytep) * blit_data_ptr->h);
    if (b_rgb == NULL) {
        video_log("[video_take_screenshot] Unable to Allocate RGB Bitmap Memory");
        fclose(fp);
        return;
    }

    for (int y = 0; y < blit_data_ptr->h; ++y) {
        b_rgb[y] = (png_byte *) malloc(png_get_rowbytes(png_ptr[monitor_index], info_ptr[monitor_index]));
        for (int x = 0; x < blit_data_ptr->w; ++x) {
            if (buf == NULL)
                memset(&(b_rgb[y][x * 3]), 0x00, 3);
            else {
                temp                  = buf[((start_y + y) * row_len) + start_x + x];
                b_rgb[y][x * 3]       = (temp >> 16) & 0xff;
                b_rgb[y][(x * 3) + 1] = (temp >> 8) & 0xff;
                b_rgb[y][(x * 3) + 2] = temp & 0xff;
            }
        }
    }

    png_write_info(png_ptr[monitor_index], info_ptr[monitor_index]);

    png_write_image(png_ptr[monitor_index], b_rgb);

    png_write_end(png_ptr[monitor_index], NULL);

    /* cleanup heap allocation */
    for (int i = 0; i < blit_data_ptr->h; i++)
        if (b_rgb[i])
            free(b_rgb[i]);

    if (b_rgb)
        free(b_rgb);

    if (fp)
        fclose(fp);
}

void
video_screenshot_monitor(uint32_t *buf, int start_x, int start_y, int row_len, int monitor_index)
{
    char path[1024];
    char fn[256];

    memset(fn, 0, sizeof(fn));
    memset(path, 0, sizeof(path));

    path_append_filename(path, usr_path, SCREENSHOT_PATH);

    if (!plat_dir_check(path))
        plat_dir_create(path);

    path_slash(path);
    strcat(path, "Monitor_");
    snprintf(&path[strlen(path)], 42, "%d_", monitor_index + 1);

    plat_tempfile(fn, NULL, ".png");
    strcat(path, fn);

    video_log("taking screenshot to: %s\n", path);

    video_take_screenshot_monitor((const char *) path, buf, start_x, start_y, row_len, monitor_index);
    png_destroy_write_struct(&png_ptr[monitor_index], &info_ptr[monitor_index]);

    atomic_fetch_sub(&monitors[monitor_index].mon_screenshots, 1);
}

void
video_screenshot(uint32_t *buf, int start_x, int start_y, int row_len)
{
    video_screenshot_monitor(buf, start_x, start_y, row_len, 0);
}

#ifdef _WIN32
void *__cdecl video_transform_copy(void *_Dst, const void *_Src, size_t _Size)
#else
void *
video_transform_copy(void *__restrict _Dst, const void *__restrict _Src, size_t _Size)
#endif
{
    uint32_t       *dest_ex = (uint32_t *) _Dst;
    const uint32_t *src_ex  = (const uint32_t *) _Src;

    _Size /= sizeof(uint32_t);

    if ((dest_ex != NULL) && (src_ex != NULL)) {
        for (size_t i = 0; i < _Size; i++) {
            *dest_ex = video_color_transform(*src_ex);
            dest_ex++;
            src_ex++;
        }
    }

    return _Dst;
}

static void
blit_thread(void *param)
{
    blit_data_t *data = param;
    while (data->thread_run) {
        thread_wait_event(data->wake_blit_thread, -1);
        thread_reset_event(data->wake_blit_thread);
        MTR_BEGIN("video", "blit_thread");

        if (blit_func)
            blit_func(data->x, data->y, data->w, data->h, data->monitor_index);

        data->busy = 0;

        MTR_END("video", "blit_thread");
        thread_set_event(data->blit_complete);
    }
}

void
video_blit_memtoscreen_monitor(int x, int y, int w, int h, int monitor_index)
{
    MTR_BEGIN("video", "video_blit_memtoscreen");

    if ((w <= 0) || (h <= 0))
        return;

    video_wait_for_blit_monitor(monitor_index);

    monitors[monitor_index].mon_blit_data_ptr->busy          = 1;
    monitors[monitor_index].mon_blit_data_ptr->buffer_in_use = 1;
    monitors[monitor_index].mon_blit_data_ptr->x             = x;
    monitors[monitor_index].mon_blit_data_ptr->y             = y;
    monitors[monitor_index].mon_blit_data_ptr->w             = w;
    monitors[monitor_index].mon_blit_data_ptr->h             = h;

    thread_set_event(monitors[monitor_index].mon_blit_data_ptr->wake_blit_thread);
    MTR_END("video", "video_blit_memtoscreen");
}

uint8_t
pixels8(uint32_t *pixels)
{
    uint8_t temp = 0;

    for (uint8_t i = 0; i < 8; i++)
        temp |= (!!*(pixels + i) << (i ^ 7));

    return temp;
}

uint32_t
pixel_to_color(uint8_t *pixels32, uint8_t pos)
{
    uint32_t temp;
    temp = *(pixels32 + pos) & 0x03;
    switch (temp) {
        default:
        case 0:
            return 0x00;
        case 1:
            return 0x07;
        case 2:
            return 0x0f;
    }
}

void
video_blend_monitor(int x, int y, int monitor_index)
{
    uint32_t            pixels32_1;
    uint32_t            pixels32_2;
    unsigned int        val1;
    unsigned int        val2;
    static unsigned int carry = 0;

    if (!herc_blend)
        return;

    if (!x)
        carry = 0;

    val1       = pixels8(&(monitors[monitor_index].target_buffer->line[y][x]));
    val2       = (val1 >> 1) + carry;
    carry      = (val1 & 1) << 7;
    pixels32_1 = cga_2_table[val1 >> 4] + cga_2_table[val2 >> 4];
    pixels32_2 = cga_2_table[val1 & 0xf] + cga_2_table[val2 & 0xf];
    for (uint8_t xx = 0; xx < 4; xx++) {
        monitors[monitor_index].target_buffer->line[y][x + xx]       = pixel_to_color((uint8_t *) &pixels32_1, xx);
        monitors[monitor_index].target_buffer->line[y][x + (xx | 4)] = pixel_to_color((uint8_t *) &pixels32_2, xx);
    }
}

void
video_process_8_monitor(int x, int y, int monitor_index)
{
    for (int xx = 0; xx < x; xx++) {
        if (monitors[monitor_index].target_buffer->line[y][xx] <= 0xff)
            monitors[monitor_index].target_buffer->line[y][xx] = monitors[monitor_index].mon_pal_lookup[monitors[monitor_index].target_buffer->line[y][xx]];
        else
            monitors[monitor_index].target_buffer->line[y][xx] = 0x00000000;
    }
}

void
cgapal_rebuild_monitor(int monitor_index)
{
    int       c;
    uint32_t *palette_lookup      = monitors[monitor_index].mon_pal_lookup;
    int       cga_palette_monitor = 0;

    /* We cannot do this (yet) if we have not been enabled yet. */
    if (video_6to8 == NULL)
        return;

    if (monitors[monitor_index].target_buffer == NULL || monitors[monitor_index].mon_cga_palette == NULL)
        return;

    cga_palette_monitor = *monitors[monitor_index].mon_cga_palette;

    for (c = 0; c < 256; c++) {
        palette_lookup[c] = makecol(video_6to8[cgapal[c].r],
                                    video_6to8[cgapal[c].g],
                                    video_6to8[cgapal[c].b]);
    }

    if ((cga_palette_monitor > 1) && (cga_palette_monitor < 7)) {
        if (vid_cga_contrast != 0) {
            for (c = 0; c < 16; c++) {
                palette_lookup[c]      = makecol(video_6to8[cgapal_mono[cga_palette_monitor - 2][c].r],
                                                 video_6to8[cgapal_mono[cga_palette_monitor - 2][c].g],
                                                 video_6to8[cgapal_mono[cga_palette_monitor - 2][c].b]);
                palette_lookup[c + 16] = makecol(video_6to8[cgapal_mono[cga_palette_monitor - 2][c].r],
                                                 video_6to8[cgapal_mono[cga_palette_monitor - 2][c].g],
                                                 video_6to8[cgapal_mono[cga_palette_monitor - 2][c].b]);
                palette_lookup[c + 32] = makecol(video_6to8[cgapal_mono[cga_palette_monitor - 2][c].r],
                                                 video_6to8[cgapal_mono[cga_palette_monitor - 2][c].g],
                                                 video_6to8[cgapal_mono[cga_palette_monitor - 2][c].b]);
                palette_lookup[c + 48] = makecol(video_6to8[cgapal_mono[cga_palette_monitor - 2][c].r],
                                                 video_6to8[cgapal_mono[cga_palette_monitor - 2][c].g],
                                                 video_6to8[cgapal_mono[cga_palette_monitor - 2][c].b]);
            }
        } else {
            for (c = 0; c < 16; c++) {
                palette_lookup[c]      = makecol(video_6to8[cgapal_mono[cga_palette_monitor - 1][c].r],
                                                 video_6to8[cgapal_mono[cga_palette_monitor - 1][c].g],
                                                 video_6to8[cgapal_mono[cga_palette_monitor - 1][c].b]);
                palette_lookup[c + 16] = makecol(video_6to8[cgapal_mono[cga_palette_monitor - 1][c].r],
                                                 video_6to8[cgapal_mono[cga_palette_monitor - 1][c].g],
                                                 video_6to8[cgapal_mono[cga_palette_monitor - 1][c].b]);
                palette_lookup[c + 32] = makecol(video_6to8[cgapal_mono[cga_palette_monitor - 1][c].r],
                                                 video_6to8[cgapal_mono[cga_palette_monitor - 1][c].g],
                                                 video_6to8[cgapal_mono[cga_palette_monitor - 1][c].b]);
                palette_lookup[c + 48] = makecol(video_6to8[cgapal_mono[cga_palette_monitor - 1][c].r],
                                                 video_6to8[cgapal_mono[cga_palette_monitor - 1][c].g],
                                                 video_6to8[cgapal_mono[cga_palette_monitor - 1][c].b]);
            }
        }
    }

    if (cga_palette_monitor == 8)
        palette_lookup[0x16] = makecol(video_6to8[42], video_6to8[42], video_6to8[0]);
    else if (cga_palette_monitor == 10) {
        /* IBM 5153 CRT, colors by VileR */
        palette_lookup[0x10] = 0x00000000;
        palette_lookup[0x11] = 0x000000c4;
        palette_lookup[0x12] = 0x0000c400;
        palette_lookup[0x13] = 0x0000c4c4;
        palette_lookup[0x14] = 0x00c40000;
        palette_lookup[0x15] = 0x00c400c4;
        palette_lookup[0x16] = 0x00c47e00;
        palette_lookup[0x17] = 0x00c4c4c4;
        palette_lookup[0x18] = 0x004e4e4e;
        palette_lookup[0x19] = 0x004e4edc;
        palette_lookup[0x1a] = 0x004edc4e;
        palette_lookup[0x1b] = 0x004ef3f3;
        palette_lookup[0x1c] = 0x00dc4e4e;
        palette_lookup[0x1d] = 0x00f34ef3;
        palette_lookup[0x1e] = 0x00f3f34e;
        palette_lookup[0x1f] = 0x00ffffff;
    }
}

void
video_inform_monitor(int type, const video_timings_t *ptr, int monitor_index)
{
    monitor_t *monitor       = &monitors[monitor_index];
    monitor->mon_vid_type    = type;
    monitor->mon_vid_timings = ptr;
}

int
video_get_type_monitor(int monitor_index)
{
    return monitors[monitor_index].mon_vid_type;
}

void
video_update_timing(void)
{
    const video_timings_t *monitor_vid_timings = NULL;
    int                   *vid_timing_read_b   = NULL;
    int                   *vid_timing_read_l   = NULL;
    int                   *vid_timing_read_w   = NULL;
    int                   *vid_timing_write_b  = NULL;
    int                   *vid_timing_write_l  = NULL;
    int                   *vid_timing_write_w  = NULL;

    for (uint8_t i = 0; i < MONITORS_NUM; i++) {
        monitor_vid_timings = monitors[i].mon_vid_timings;
        if (!monitor_vid_timings)
            continue;
        vid_timing_read_b  = &monitors[i].mon_video_timing_read_b;
        vid_timing_read_l  = &monitors[i].mon_video_timing_read_l;
        vid_timing_read_w  = &monitors[i].mon_video_timing_read_w;
        vid_timing_write_b = &monitors[i].mon_video_timing_write_b;
        vid_timing_write_l = &monitors[i].mon_video_timing_write_l;
        vid_timing_write_w = &monitors[i].mon_video_timing_write_w;

        if (monitor_vid_timings->type == VIDEO_ISA) {
            *vid_timing_read_b  = ISA_CYCLES(monitor_vid_timings->read_b);
            *vid_timing_read_w  = ISA_CYCLES(monitor_vid_timings->read_w);
            *vid_timing_read_l  = ISA_CYCLES(monitor_vid_timings->read_l);
            *vid_timing_write_b = ISA_CYCLES(monitor_vid_timings->write_b);
            *vid_timing_write_w = ISA_CYCLES(monitor_vid_timings->write_w);
            *vid_timing_write_l = ISA_CYCLES(monitor_vid_timings->write_l);
        } else if (monitor_vid_timings->type == VIDEO_PCI) {
            *vid_timing_read_b  = (int) (pci_timing * monitor_vid_timings->read_b);
            *vid_timing_read_w  = (int) (pci_timing * monitor_vid_timings->read_w);
            *vid_timing_read_l  = (int) (pci_timing * monitor_vid_timings->read_l);
            *vid_timing_write_b = (int) (pci_timing * monitor_vid_timings->write_b);
            *vid_timing_write_w = (int) (pci_timing * monitor_vid_timings->write_w);
            *vid_timing_write_l = (int) (pci_timing * monitor_vid_timings->write_l);
        } else if (monitor_vid_timings->type == VIDEO_AGP) {
            *vid_timing_read_b  = (int) (agp_timing * monitor_vid_timings->read_b);
            *vid_timing_read_w  = (int) (agp_timing * monitor_vid_timings->read_w);
            *vid_timing_read_l  = (int) (agp_timing * monitor_vid_timings->read_l);
            *vid_timing_write_b = (int) (agp_timing * monitor_vid_timings->write_b);
            *vid_timing_write_w = (int) (agp_timing * monitor_vid_timings->write_w);
            *vid_timing_write_l = (int) (agp_timing * monitor_vid_timings->write_l);
        } else {
            *vid_timing_read_b  = (int) (bus_timing * monitor_vid_timings->read_b);
            *vid_timing_read_w  = (int) (bus_timing * monitor_vid_timings->read_w);
            *vid_timing_read_l  = (int) (bus_timing * monitor_vid_timings->read_l);
            *vid_timing_write_b = (int) (bus_timing * monitor_vid_timings->write_b);
            *vid_timing_write_w = (int) (bus_timing * monitor_vid_timings->write_w);
            *vid_timing_write_l = (int) (bus_timing * monitor_vid_timings->write_l);
        }

        if (cpu_16bitbus) {
            *vid_timing_read_l  = *vid_timing_read_w * 2;
            *vid_timing_write_l = *vid_timing_write_w * 2;
        }
    }
}

int
calc_6to8(int c)
{
    int    ic;
    int    i8;
    double d8;

    ic = c;
    if (ic == 64)
        ic = 63;
    else
        ic &= 0x3f;
    d8 = (ic / 63.0) * 255.0;
    i8 = (int) d8;

    return (i8 & 0xff);
}

int
calc_8to32(int c)
{
    int    b;
    int    g;
    int    r;
    double db;
    double dg;
    double dr;

    b  = (c & 3);
    g  = ((c >> 2) & 7);
    r  = ((c >> 5) & 7);
    db = (((double) b) / 3.0) * 255.0;
    dg = (((double) g) / 7.0) * 255.0;
    dr = (((double) r) / 7.0) * 255.0;
    b  = (int) db;
    g  = ((int) dg) << 8;
    r  = ((int) dr) << 16;

    return (b | g | r);
}

int
calc_15to32(int c)
{
    int    b;
    int    g;
    int    r;
    double db;
    double dg;
    double dr;

    b  = (c & 31);
    g  = ((c >> 5) & 31);
    r  = ((c >> 10) & 31);
    db = (((double) b) / 31.0) * 255.0;
    dg = (((double) g) / 31.0) * 255.0;
    dr = (((double) r) / 31.0) * 255.0;
    b  = (int) db;
    g  = ((int) dg) << 8;
    r  = ((int) dr) << 16;

    return (b | g | r);
}

int
calc_16to32(int c)
{
    int    b;
    int    g;
    int    r;
    double db;
    double dg;
    double dr;

    b  = (c & 31);
    g  = ((c >> 5) & 63);
    r  = ((c >> 11) & 31);
    db = (((double) b) / 31.0) * 255.0;
    dg = (((double) g) / 63.0) * 255.0;
    dr = (((double) r) / 31.0) * 255.0;
    b  = (int) db;
    g  = ((int) dg) << 8;
    r  = ((int) dr) << 16;

    return (b | g | r);
}

void
hline(bitmap_t *b, int x1, int y, int x2, uint32_t col)
{
    if (y < 0 || y >= b->h)
        return;

    for (int x = x1; x < x2; x++)
        b->line[y][x] = col;
}

void
blit(UNUSED(bitmap_t *src), UNUSED(bitmap_t *dst), UNUSED(int x1), UNUSED(int y1), UNUSED(int x2), UNUSED(int y2), UNUSED(int xs), UNUSED(int ys))
{
    //
}

void
stretch_blit(UNUSED(bitmap_t *src), UNUSED(bitmap_t *dst), UNUSED(int x1), UNUSED(int y1), UNUSED(int xs1), UNUSED(int ys1), UNUSED(int x2), UNUSED(int y2), UNUSED(int xs2), UNUSED(int ys2))
{
    //
}

void
rectfill(UNUSED(bitmap_t *b), UNUSED(int x1), UNUSED(int y1), UNUSED(int x2), UNUSED(int y2), UNUSED(uint32_t col))
{
    //
}

void
set_palette(UNUSED(PALETTE p))
{
    //
}

void
destroy_bitmap(bitmap_t *b)
{
    if ((b != NULL) && (b->dat != NULL))
        free(b->dat);

    if (b != NULL)
        free(b);
}

bitmap_t *
create_bitmap(int x, int y)
{
    bitmap_t *b = calloc(sizeof(bitmap_t), (y * sizeof(uint32_t *)));

    b->dat = calloc((size_t) x * y, 4);
    for (int c = 0; c < y; c++)
        b->line[c] = &(b->dat[c * x]);
    b->w = x;
    b->h = y;

    return b;
}

void
video_monitor_init(int index)
{
    memset(&monitors[index], 0, sizeof(monitor_t));
    monitors[index].mon_xsize                            = 640;
    monitors[index].mon_ysize                            = 480;
    monitors[index].mon_res_x                            = 640;
    monitors[index].mon_res_y                            = 480;
    monitors[index].mon_scrnsz_x                         = 640;
    monitors[index].mon_scrnsz_y                         = 480;
    monitors[index].mon_efscrnsz_y                       = 480;
    monitors[index].mon_unscaled_size_x                  = 480;
    monitors[index].mon_unscaled_size_y                  = 480;
    monitors[index].mon_bpp                              = 8;
    monitors[index].mon_changeframecount                 = 2;
    monitors[index].target_buffer                        = create_bitmap(2048, 2048);
    monitors[index].mon_blit_data_ptr                    = calloc(1, sizeof(blit_data_t));
    monitors[index].mon_blit_data_ptr->wake_blit_thread  = thread_create_event();
    monitors[index].mon_blit_data_ptr->blit_complete     = thread_create_event();
    monitors[index].mon_blit_data_ptr->buffer_not_in_use = thread_create_event();
    monitors[index].mon_blit_data_ptr->thread_run        = 1;
    monitors[index].mon_blit_data_ptr->monitor_index     = index;
    monitors[index].mon_pal_lookup                       = calloc(sizeof(uint32_t), 256);
    monitors[index].mon_cga_palette                      = calloc(1, sizeof(int));
    monitors[index].mon_force_resize                     = 1;
    monitors[index].mon_vid_type                         = VIDEO_FLAG_TYPE_NONE;
    atomic_init(&doresize_monitors[index], 0);
    atomic_init(&monitors[index].mon_screenshots, 0);
    if (index >= 1)
        ui_init_monitor(index);
    monitors[index].mon_blit_data_ptr->blit_thread = thread_create(blit_thread, monitors[index].mon_blit_data_ptr);
}

void
video_monitor_close(int monitor_index)
{
    if (monitors[monitor_index].target_buffer == NULL) {
        return;
    }
    monitors[monitor_index].mon_blit_data_ptr->thread_run = 0;
    thread_set_event(monitors[monitor_index].mon_blit_data_ptr->wake_blit_thread);
    thread_wait(monitors[monitor_index].mon_blit_data_ptr->blit_thread);
    if (monitor_index >= 1)
        ui_deinit_monitor(monitor_index);
    thread_destroy_event(monitors[monitor_index].mon_blit_data_ptr->buffer_not_in_use);
    thread_destroy_event(monitors[monitor_index].mon_blit_data_ptr->blit_complete);
    thread_destroy_event(monitors[monitor_index].mon_blit_data_ptr->wake_blit_thread);
    free(monitors[monitor_index].mon_blit_data_ptr);
    if (!monitors[monitor_index].mon_pal_lookup_static)
        free(monitors[monitor_index].mon_pal_lookup);
    if (!monitors[monitor_index].mon_cga_palette_static)
        free(monitors[monitor_index].mon_cga_palette);
    destroy_bitmap(monitors[monitor_index].target_buffer);
    monitors[monitor_index].target_buffer = NULL;
    memset(&monitors[monitor_index], 0, sizeof(monitor_t));
}

void
video_init(void)
{
    uint8_t total[2] = { 0, 1 };

    for (uint8_t c = 0; c < 16; c++) {
        cga_2_table[c] = (total[(c >> 3) & 1] << 0) | (total[(c >> 2) & 1] << 8) | (total[(c >> 1) & 1] << 16) | (total[(c >> 0) & 1] << 24);
    }

    for (uint8_t c = 0; c < 64; c++) {
        cgapal[c + 64].r = (((c & 4) ? 2 : 0) | ((c & 0x10) ? 1 : 0)) * 21;
        cgapal[c + 64].g = (((c & 2) ? 2 : 0) | ((c & 0x10) ? 1 : 0)) * 21;
        cgapal[c + 64].b = (((c & 1) ? 2 : 0) | ((c & 0x10) ? 1 : 0)) * 21;
        if ((c & 0x17) == 6)
            cgapal[c + 64].g >>= 1;
    }
    for (uint8_t c = 0; c < 64; c++) {
        cgapal[c + 128].r = (((c & 4) ? 2 : 0) | ((c & 0x20) ? 1 : 0)) * 21;
        cgapal[c + 128].g = (((c & 2) ? 2 : 0) | ((c & 0x10) ? 1 : 0)) * 21;
        cgapal[c + 128].b = (((c & 1) ? 2 : 0) | ((c & 0x08) ? 1 : 0)) * 21;
    }

    for (uint8_t c = 0; c < 4; c++) {
        for (uint8_t d = 0; d < 4; d++) {
            edatlookup[c][d] = 0;
            if (c & 1)
                edatlookup[c][d] |= 1;
            if (d & 1)
                edatlookup[c][d] |= 2;
            if (c & 2)
                edatlookup[c][d] |= 0x10;
            if (d & 2)
                edatlookup[c][d] |= 0x20;
        }
    }

    for (uint16_t c = 0; c < 256; c++) {
        egaremap2bpp[c] = 0;
        if (c & 0x01)
            egaremap2bpp[c] |= 0x01;
        if (c & 0x04)
            egaremap2bpp[c] |= 0x02;
        if (c & 0x10)
            egaremap2bpp[c] |= 0x04;
        if (c & 0x40)
            egaremap2bpp[c] |= 0x08;
    }

    video_6to8 = malloc(4 * 256);
    for (uint16_t c = 0; c < 256; c++)
        video_6to8[c] = calc_6to8(c);

    video_8togs = malloc(4 * 256);
    for (uint16_t c = 0; c < 256; c++)
        video_8togs[c] = c | (c << 16) | (c << 24);

    video_8to32 = malloc(4 * 256);
    for (uint16_t c = 0; c < 256; c++)
        video_8to32[c] = calc_8to32(c);

    video_15to32 = malloc(4 * 65536);
    for (uint32_t c = 0; c < 65536; c++)
        video_15to32[c] = calc_15to32(c & 0x7fff);

    video_16to32 = malloc(4 * 65536);
    for (uint32_t c = 0; c < 65536; c++)
        video_16to32[c] = calc_16to32(c);

    memset(monitors, 0, sizeof(monitors));
    video_monitor_init(0);
}

void
video_close(void)
{
    video_monitor_close(0);

    free(video_16to32);
    free(video_15to32);
    free(video_8to32);
    free(video_8togs);
    free(video_6to8);

    if (fontdatksc5601) {
        free(fontdatksc5601);
        fontdatksc5601 = NULL;
    }

    if (fontdatksc5601_user) {
        free(fontdatksc5601_user);
        fontdatksc5601_user = NULL;
    }
}

uint8_t
video_force_resize_get_monitor(int monitor_index)
{
    return monitors[monitor_index].mon_force_resize;
}

void
video_force_resize_set_monitor(uint8_t res, int monitor_index)
{
    monitors[monitor_index].mon_force_resize = res;
}

void
loadfont_common(FILE *fp, int format)
{
    switch (format) {
        case 0: /* MDA */
            for (uint16_t c = 0; c < 256; c++) /* 8x14 MDA in 8x8 cell (lines 0-7) */
                for (uint8_t d = 0; d < 8; d++)
                    fontdatm[c][d] = fgetc(fp) & 0xff;
            for (uint16_t c = 0; c < 256; c++) /* 8x14 MDA in 8x8 cell (lines 8-13 + padding lines) */
                for (uint8_t d = 0; d < 8; d++)
                    fontdatm[c][d + 8] = fgetc(fp) & 0xff;
            (void) fseek(fp, 4096 + 2048, SEEK_SET);
            for (uint16_t c = 0; c < 256; c++) /* 8x8 CGA (thick, primary) */
                for (uint8_t d = 0; d < 8; d++)
                    fontdat[c][d] = fgetc(fp) & 0xff;
            break;

        case 1: /* PC200 */
            for (uint8_t d = 0; d < 4; d++) {
                /* There are 4 fonts in the ROM */
                for (uint16_t c = 0; c < 256; c++) /* 8x14 MDA in 8x16 cell */
                    (void) !fread(&fontdatm[256 * d + c][0], 1, 16, fp);
                for (uint16_t c = 0; c < 256; c++) { /* 8x8 CGA in 8x16 cell */
                    (void) !fread(&fontdat[256 * d + c][0], 1, 8, fp);
                    fseek(fp, 8, SEEK_CUR);
                }
            }
            break;

        default:
        case 2: /* CGA */
            for (uint16_t c = 0; c < 256; c++)
                for (uint8_t d = 0; d < 8; d++)
                    fontdat[c][d] = fgetc(fp) & 0xff;
            break;

        case 3: /* Wyse 700 */
            for (uint16_t c = 0; c < 512; c++)
                for (uint8_t d = 0; d < 32; d++)
                    fontdatw[c][d] = fgetc(fp) & 0xff;
            break;

        case 4: /* MDSI Genius */
            for (uint16_t c = 0; c < 256; c++)
                for (uint8_t d = 0; d < 16; d++)
                    fontdat8x12[c][d] = fgetc(fp) & 0xff;
            break;

        case 5: /* Toshiba 3100e */
            for (uint16_t d = 0; d < 2048; d += 512) { /* Four languages... */
                for (uint16_t c = d; c < d + 256; c++) {
                    (void) !fread(&fontdatm[c][8], 1, 8, fp);
                }
                for (uint32_t c = d + 256; c < d + 512; c++) {
                    (void) !fread(&fontdatm[c][8], 1, 8, fp);
                }
                for (uint32_t c = d; c < d + 256; c++) {
                    (void) !fread(&fontdatm[c][0], 1, 8, fp);
                }
                for (uint32_t c = d + 256; c < d + 512; c++) {
                    (void) !fread(&fontdatm[c][0], 1, 8, fp);
                }
                fseek(fp, 4096, SEEK_CUR); /* Skip blank section */
                for (uint32_t c = d; c < d + 256; c++) {
                    (void) !fread(&fontdat[c][0], 1, 8, fp);
                }
                for (uint32_t c = d + 256; c < d + 512; c++) {
                    (void) !fread(&fontdat[c][0], 1, 8, fp);
                }
            }
            break;

        case 6: /* Korean KSC-5601 */
            if (!fontdatksc5601)
                fontdatksc5601 = malloc(16384 * sizeof(dbcs_font_t));

            if (!fontdatksc5601_user)
                fontdatksc5601_user = malloc(192 * sizeof(dbcs_font_t));

            for (uint32_t c = 0; c < 16384; c++) {
                for (uint8_t d = 0; d < 32; d++)
                    fontdatksc5601[c].chr[d] = fgetc(fp) & 0xff;
            }
            break;

        case 7: /* Sigma Color 400 */
            /* The first 4k of the character ROM holds an 8x8 font */
            for (uint16_t c = 0; c < 256; c++) {
                (void) !fread(&fontdat[c][0], 1, 8, fp);
                fseek(fp, 8, SEEK_CUR);
            }
            /* The second 4k holds an 8x16 font */
            for (uint16_t c = 0; c < 256; c++) {
                if (fread(&fontdatm[c][0], 1, 16, fp) != 16)
                    fatal("loadfont(): Error reading 8x16 font in Sigma Color 400 mode, c = %i\n", c);
            }
            break;

        case 8: /* Amstrad PC1512, Toshiba T1000/T1200 */
            for (uint16_t c = 0; c < 2048; c++) /* Allow up to 2048 chars */
                for (uint8_t d = 0; d < 8; d++)
                    fontdat[c][d] = fgetc(fp) & 0xff;
            break;

        case 9: /* Image Manager 1024 native font */
            for (uint16_t c = 0; c < 256; c++)
                (void) !fread(&fontdat12x18[c][0], 1, 36, fp);
            break;

        case 10: /* Pravetz */
            for (uint16_t c = 0; c < 1024; c++) /* Allow up to 1024 chars */
                for (uint8_t d = 0; d < 8; d++)
                    fontdat[c][d] = fgetc(fp) & 0xff;
            break;

        case 11: /* PC200 */
            for (uint8_t d = 0; d < 4; d++) {
                /* There are 4 fonts in the ROM */
                for (uint16_t c = 0; c < 256; c++) /* 8x14 MDA in 8x16 cell */
                    (void) !fread(&fontdatm2[256 * d + c][0], 1, 16, fp);
                for (uint16_t c = 0; c < 256; c++) { /* 8x8 CGA in 8x16 cell */
                    (void) !fread(&fontdat2[256 * d + c][0], 1, 8, fp);
                    fseek(fp, 8, SEEK_CUR);
                }
            }
            break;
    }

    (void) fclose(fp);
}

void
loadfont_ex(char *fn, int format, int offset)
{
    FILE *fp;

    fp = rom_fopen(fn, "rb");
    if (fp == NULL)
        return;

    fseek(fp, offset, SEEK_SET);
    loadfont_common(fp, format);
}

void
loadfont(char *fn, int format)
{
    loadfont_ex(fn, format, 0);
}

uint32_t
video_color_transform(uint32_t color)
{
    uint8_t *clr8 = (uint8_t *) &color;
#if 0
    if (!video_grayscale && !invert_display)
        return color;
#endif
    if (video_grayscale) {
        if (video_graytype) {
            if (video_graytype == 1)
                color = ((54 * (uint32_t) clr8[2]) + (183 * (uint32_t) clr8[1]) + (18 * (uint32_t) clr8[0])) / 255;
            else
                color = ((uint32_t) clr8[2] + (uint32_t) clr8[1] + (uint32_t) clr8[0]) / 3;
        } else
            color = ((76 * (uint32_t) clr8[2]) + (150 * (uint32_t) clr8[1]) + (29 * (uint32_t) clr8[0])) / 255;
        switch (video_grayscale) {
            case 2:
            case 3:
            case 4:
                color = shade[video_grayscale][color];
                break;
            default:
                clr8[3] = 0;
                clr8[0] = color;
                clr8[1] = clr8[2] = clr8[0];
                break;
        }
    }
    if (invert_display)
        color ^= 0x00ffffff;
    return color;
}
