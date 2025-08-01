/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Trident TGUI9400CXi and TGUI9440/96x0 emulation.
 *
 *          TGUI9400CXi has extended write modes, controlled by extended
 *          GDC registers :
 *
 *            GDC[0x10] - Control
 *                bit 0 - pixel width (1 = 16 bit, 0 = 8 bit)
 *                bit 1 - mono->colour expansion (1 = enabled,
 *                        0 = disabled)
 *                bit 2 - mono->colour expansion transparency
 *                       (1 = transparent, 0 = opaque)
 *                bit 3 - extended latch copy
 *            GDC[0x11] - Background colour (low byte)
 *            GDC[0x12] - Background colour (high byte)
 *            GDC[0x14] - Foreground colour (low byte)
 *            GDC[0x15] - Foreground colour (high byte)
 *            GDC[0x17] - Write mask (low byte)
 *            GDC[0x18] - Write mask (high byte)
 *
 *          Mono->colour expansion will expand written data 8:1 to 8/16
 *          consecutive bytes.
 *          MSB is processed first. On word writes, low byte is processed
 *          first. 1 bits write foreground colour, 0 bits write background
 *          colour unless transparency is enabled.
 *          If the relevant bit is clear in the write mask then the data
 *          is not written.
 *
 *          With 16-bit pixel width, each bit still expands to one byte,
 *          so the TGUI driver doubles up monochrome data.
 *
 *          While there is room in the register map for three byte colours,
 *          I don't believe 24-bit colour is supported. The TGUI9440
 *          blitter has the same limitation.
 *
 *          I don't think double word writes are supported.
 *
 *          Extended latch copy uses an internal 16 byte latch. Reads load
 *          the latch, writing writes out 16 bytes. I don't think the
 *          access size or host data has any affect, but the Windows 3.1
 *          driver always reads bytes and write words of 0xffff.
 *
 *
 *
 * Authors: Sarah Walker, <https://pcem-emulator.co.uk/>
 *          Miran Grca, <mgrca8@gmail.com>
 *
 *          Copyright 2008-2019 Sarah Walker.
 *          Copyright 2016-2019 Miran Grca.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <86box/86box.h>
#include <86box/io.h>
#include <86box/timer.h>
#include <86box/mem.h>
#include <86box/pci.h>
#include <86box/rom.h>
#include <86box/device.h>
#include "cpu.h"
#include <86box/plat.h>
#include <86box/video.h>
#include <86box/i2c.h>
#include <86box/vid_ddc.h>
#include <86box/vid_xga.h>
#include <86box/vid_svga.h>
#include <86box/vid_svga_render.h>

#define ROM_TGUI_9400CXI          "roms/video/tgui9440/9400CXI.VBI"
#define ROM_TGUI_9440_VLB         "roms/video/tgui9440/trident_9440_vlb.bin"
#define ROM_TGUI_9440_PCI         "roms/video/tgui9440/BIOS.BIN"
#define ROM_TGUI_96xx             "roms/video/tgui9660/Union.VBI"

#define EXT_CTRL_16BIT            0x01
#define EXT_CTRL_MONO_EXPANSION   0x02
#define EXT_CTRL_MONO_TRANSPARENT 0x04
#define EXT_CTRL_LATCH_COPY       0x08

enum {
    TGUI_9400CXI = 0,
    TGUI_9440,
    TGUI_9660,
    TGUI_9680
};

#define ONBOARD 0x0100

typedef struct tgui_t {
    mem_mapping_t linear_mapping;
    mem_mapping_t accel_mapping;
    mem_mapping_t mmio_mapping;

    rom_t bios_rom;

    svga_t svga;
    int    pci;

    uint8_t pci_slot;
    uint8_t irq_state;

    int type;

    uint8_t int_line;
    uint8_t pci_regs[256];

    struct {
        int16_t  src_x, src_y;
        int16_t  src_x_clip, src_y_clip;
        int16_t  dst_x, dst_y;
        int16_t  dst_y_clip, dst_x_clip;
        int16_t  size_x, size_y;
        uint16_t sv_size_y;
        uint16_t patloc;
        uint32_t fg_col, bg_col;
        uint32_t style, ckey;
        uint8_t  rop;
        uint32_t flags;
        uint8_t  pattern[0x80];
        uint8_t  pattern_32bpp[0x100];
        int      command;
        int      offset;
        uint16_t ger22;

        int16_t  err;
        int16_t  top, left, bottom, right;
        int16_t  x, y, cx, cy, dx, dy;
        uint32_t src, dst, src_old, dst_old;
        int      pat_x, pat_y;
        int      use_src;

        int      pitch, bpp;
        uint32_t fill_pattern[8 * 8];
        uint32_t mono_pattern[8 * 8];
        uint32_t pattern_8[8 * 8];
        uint32_t pattern_16[8 * 8];
        uint32_t pattern_32[8 * 8];
        int pattern_32_idx;
    } accel;

    uint8_t copy_latch[16]; /*TGUI9400CXi only*/

    uint8_t tgui_3d8, tgui_3d9;
    int     oldmode;
    uint8_t oldctrl1;
    uint8_t oldctrl2, newctrl2;
    uint8_t oldgr0e, newgr0e;

    uint32_t linear_base, linear_size, ge_base,
        mmio_base;
    uint32_t hwc_fg_col, hwc_bg_col;

    int     ramdac_state;
    uint8_t ramdac_ctrl;
    uint8_t alt_clock;

    int clock_m, clock_n, clock_k;

    uint32_t vram_size, vram_mask;

    volatile int write_blitter;
    void        *i2c, *ddc;

    int has_bios;
} tgui_t;

video_timings_t timing_tgui_vlb = { .type = VIDEO_BUS, .write_b = 4, .write_w = 8, .write_l = 16, .read_b = 4, .read_w = 8, .read_l = 16 };
video_timings_t timing_tgui_pci = { .type = VIDEO_PCI, .write_b = 4, .write_w = 8, .write_l = 16, .read_b = 4, .read_w = 8, .read_l = 16 };

static void    tgui_out(uint16_t addr, uint8_t val, void *priv);
static uint8_t tgui_in(uint16_t addr, void *priv);

static void tgui_recalcmapping(tgui_t *tgui);

static void     tgui_accel_out(uint16_t addr, uint8_t val, void *priv);
static void     tgui_accel_out_w(uint16_t addr, uint16_t val, void *priv);
static void     tgui_accel_out_l(uint16_t addr, uint32_t val, void *priv);
static uint8_t  tgui_accel_in(uint16_t addr, void *priv);
static uint16_t tgui_accel_in_w(uint16_t addr, void *priv);
static uint32_t tgui_accel_in_l(uint16_t addr, void *priv);

static uint8_t  tgui_accel_read(uint32_t addr, void *priv);
static uint16_t tgui_accel_read_w(uint32_t addr, void *priv);
static uint32_t tgui_accel_read_l(uint32_t addr, void *priv);

static void tgui_accel_write(uint32_t addr, uint8_t val, void *priv);
static void tgui_accel_write_w(uint32_t addr, uint16_t val, void *priv);
static void tgui_accel_write_l(uint32_t addr, uint32_t val, void *priv);

static void tgui_accel_write_fb_b(uint32_t addr, uint8_t val, void *priv);
static void tgui_accel_write_fb_w(uint32_t addr, uint16_t val, void *priv);
static void tgui_accel_write_fb_l(uint32_t addr, uint32_t val, void *priv);

static uint8_t tgui_ext_linear_read(uint32_t addr, void *priv);
static void    tgui_ext_linear_write(uint32_t addr, uint8_t val, void *priv);
static void    tgui_ext_linear_writew(uint32_t addr, uint16_t val, void *priv);
static void    tgui_ext_linear_writel(uint32_t addr, uint32_t val, void *priv);

static uint8_t tgui_ext_read(uint32_t addr, void *priv);
static void    tgui_ext_write(uint32_t addr, uint8_t val, void *priv);
static void    tgui_ext_writew(uint32_t addr, uint16_t val, void *priv);
static void    tgui_ext_writel(uint32_t addr, uint32_t val, void *priv);

/*Remap address for chain-4/doubleword style layout*/
static __inline uint32_t
dword_remap(UNUSED(svga_t *svga), uint32_t in_addr)
{
    return ((in_addr << 2) & 0x3fff0) | ((in_addr >> 14) & 0xc) | (in_addr & ~0x3fffc);
}

static void
tgui_update_irqs(tgui_t *tgui)
{
    if (!tgui->pci)
        return;

    if (!(tgui->oldctrl1 & 0x40))
        pci_set_irq(tgui->pci_slot, PCI_INTA, &tgui->irq_state);
    else
        pci_clear_irq(tgui->pci_slot, PCI_INTA, &tgui->irq_state);
}

static void
tgui_remove_io(tgui_t *tgui)
{
    io_removehandler(0x03c0, 0x0020, tgui_in, NULL, NULL, tgui_out, NULL, NULL, tgui);
    if (tgui->type >= TGUI_9440) {
        io_removehandler(0x43c6, 0x0004, tgui_in, NULL, NULL, tgui_out, NULL, NULL, tgui);
        io_removehandler(0x83c6, 0x0003, tgui_in, NULL, NULL, tgui_out, NULL, NULL, tgui);
        io_removehandler(0x2120, 0x0001, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x2122, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x2124, 0x0001, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x2127, 0x0001, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x2128, 0x0004, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x212c, 0x0004, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x2130, 0x0004, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x2134, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x2138, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x213a, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x213c, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x213e, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x2140, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x2142, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x2144, 0x0004, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x2148, 0x0004, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x2168, 0x0004, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x2178, 0x0004, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x217c, 0x0004, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_removehandler(0x2180, 0x0080, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
    }
}

static void
tgui_set_io(tgui_t *tgui)
{
    tgui_remove_io(tgui);

    io_sethandler(0x03c0, 0x0020, tgui_in, NULL, NULL, tgui_out, NULL, NULL, tgui);
    if (tgui->type >= TGUI_9440) {
        io_sethandler(0x43c6, 0x0004, tgui_in, NULL, NULL, tgui_out, NULL, NULL, tgui);
        io_sethandler(0x83c6, 0x0003, tgui_in, NULL, NULL, tgui_out, NULL, NULL, tgui);
        io_sethandler(0x2120, 0x0001, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x2122, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x2124, 0x0001, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x2127, 0x0001, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x2128, 0x0004, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x212c, 0x0004, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x2130, 0x0004, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x2134, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x2138, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x213a, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x213c, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x213e, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x2140, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x2142, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x2144, 0x0004, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x2148, 0x0004, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x2168, 0x0004, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x2178, 0x0004, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x217c, 0x0004, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
        io_sethandler(0x2180, 0x0080, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
    }
}

static void
tgui_out(uint16_t addr, uint8_t val, void *priv)
{
    tgui_t *tgui = (tgui_t *) priv;
    svga_t *svga = &tgui->svga;
    uint8_t old, o;

    if (((addr & 0xFFF0) == 0x3D0 || (addr & 0xFFF0) == 0x3B0) && !(svga->miscout & 1))
        addr ^= 0x60;

    switch (addr) {
        case 0x3C5:
            switch (svga->seqaddr) {
                case 0xB:
                    tgui->oldmode = 1;
                    break;
                case 0xC:
                    if (svga->seqregs[0x0e] & 0x80)
                        svga->seqregs[0x0c] = val;
                    break;
                case 0xd:
                    if (tgui->oldmode)
                        tgui->oldctrl2 = val;
                    else
                        tgui->newctrl2 = val;
                    break;
                case 0xE:
                    if (tgui->oldmode) {
                        tgui->oldctrl1 = val;
                        tgui_update_irqs(tgui);
                        svga->write_bank = (tgui->oldctrl1) * 65536;
                    } else {
                        svga->seqregs[0xe] = val ^ 2;
                        svga->write_bank   = (svga->seqregs[0xe]) * 65536;
                    }
                    if (!(svga->gdcreg[0xf] & 1))
                        svga->read_bank = svga->write_bank;
                    return;

                case 0x5a:
                case 0x5b:
                case 0x5c:
                case 0x5d:
                case 0x5e:
                case 0x5f:
                    svga->seqregs[svga->seqaddr] = val;
                    return;

                default:
                    break;
            }
            break;

        case 0x3C6:
            if (tgui->type == TGUI_9400CXI) {
                tkd8001_ramdac_out(addr, val, svga->ramdac, svga);
                return;
            }
            if (tgui->ramdac_state == 4) {
                tgui->ramdac_state = 0;
                tgui->ramdac_ctrl  = val;
#if 0
                pclog("TGUI ramdac ctrl=%02x.\n", (tgui->ramdac_ctrl >> 4) & 0x0f);
#endif
                svga_recalctimings(svga);
                return;
            }
            break;

        case 0x3C7:
        case 0x3C8:
        case 0x3C9:
            if (tgui->type == TGUI_9400CXI) {
                tkd8001_ramdac_out(addr, val, svga->ramdac, svga);
                return;
            }
            tgui->ramdac_state = 0;
            break;

        case 0x3CF:
            o = svga->gdcreg[svga->gdcaddr];
            switch (svga->gdcaddr) {
                case 2:
                    svga->colourcompare = val;
                    break;
                case 4:
                    svga->readplane = val & 3;
                    break;
                case 5:
                    svga->writemode   = val & 3;
                    svga->readmode    = val & 8;
                    svga->chain2_read = val & 0x10;
                    break;
                case 6:
                    if (svga->gdcreg[6] != val) {
                        svga->gdcreg[6] = val;
                        tgui_recalcmapping(tgui);
                    }
                    break;
                case 7:
                    svga->colournocare = val;
                    break;

                case 0x0e:
                    svga->gdcreg[0xe] = val ^ 2;
                    if ((svga->gdcreg[0xf] & 1) == 1)
                        svga->read_bank = (svga->gdcreg[0xe]) * 65536;
                    break;

                case 0x0f:
                    if (val & 1)
                        svga->read_bank = (svga->gdcreg[0xe]) * 65536;
                    else {
                        if (tgui->oldmode)
                            svga->read_bank = (tgui->oldctrl1) * 65536;
                        else
                            svga->read_bank = (svga->seqregs[0xe]) * 65536;
                    }

                    if (tgui->oldmode)
                        svga->write_bank = (tgui->oldctrl1) * 65536;
                    else
                        svga->write_bank = (svga->seqregs[0xe]) * 65536;
                    break;

                case 0x23:
                    svga->dpms = !!(val & 0x03);
                    svga_recalctimings(svga);
                    break;

                case 0x2f:
                case 0x5a:
                case 0x5b:
                case 0x5c:
                case 0x5d:
                case 0x5e:
                case 0x5f:
                    svga->gdcreg[svga->gdcaddr] = val;
                    break;

                default:
                    break;
            }
            svga->gdcreg[svga->gdcaddr] = val;

            if (tgui->type == TGUI_9400CXI) {
                if ((svga->gdcaddr >= 0x10) && (svga->gdcaddr <= 0x1f)) {
                    tgui_recalcmapping(tgui);
                    return;
                }
            }
            svga->fast = (svga->gdcreg[8] == 0xff && !(svga->gdcreg[3] & 0x18) && !svga->gdcreg[1]) && ((svga->chain4 && (svga->packed_chain4 || svga->force_old_addr)) || svga->fb_only);
            if (((svga->gdcaddr == 5) && ((val ^ o) & 0x70)) || ((svga->gdcaddr == 6) && ((val ^ o) & 1)))
                svga_recalctimings(svga);
            return;
        case 0x3D4:
            svga->crtcreg = val;
            return;
        case 0x3D5:
            if (!(svga->seqregs[0x0e] & 0x80) && !tgui->oldmode) {
                switch (svga->crtcreg) {
                    case 0x21:
                    case 0x29:
                    case 0x2a:
                    case 0x38:
                    case 0x39:
                    case 0x3b:
                    case 0x3c:
                        return;
                    default:
                        break;
                }
            }
            if ((svga->crtcreg < 7) && (svga->crtc[0x11] & 0x80))
                return;
            if ((svga->crtcreg == 7) && (svga->crtc[0x11] & 0x80))
                val = (svga->crtc[7] & ~0x10) | (val & 0x10);
            old                       = svga->crtc[svga->crtcreg];
            svga->crtc[svga->crtcreg] = val;
            switch (svga->crtcreg) {
                case 0x1e:
                    svga->vram_display_mask = (val & 0x80) ? tgui->vram_mask : 0x3ffff;
                    break;

                case 0x21:
                    if (!tgui->pci) {
                        tgui->linear_base = ((val & 0xc0) << 18) | ((val & 0x0f) << 20);
                        tgui->linear_size = (val & 0x10) ? 0x200000 : 0x100000;
                        svga->decode_mask = (val & 0x10) ? 0x1fffff : 0xfffff;
                    }
                    tgui_recalcmapping(tgui);
                    break;

                case 0x34:
                case 0x35:
                    if (tgui->type >= TGUI_9440) {
                        tgui->ge_base = ((svga->crtc[0x35] << 0x18) | (svga->crtc[0x34] << 0x10));
                        tgui_recalcmapping(tgui);
                    }
                    break;

                case 0x36:
                case 0x39:
                    tgui_recalcmapping(tgui);
                    break;

                case 0x37:
                    if (tgui->type >= TGUI_9440)
                        i2c_gpio_set(tgui->i2c, (val & 0x02) || !(val & 0x04), (val & 0x01) || !(val & 0x08));
                    break;

                case 0x40:
                case 0x41:
                case 0x42:
                case 0x43:
                case 0x44:
                case 0x45:
                case 0x46:
                case 0x47:
                    if (tgui->type >= TGUI_9440) {
                        svga->hwcursor.x = (svga->crtc[0x40] | (svga->crtc[0x41] << 8)) & 0x7ff;
                        svga->hwcursor.y = (svga->crtc[0x42] | (svga->crtc[0x43] << 8)) & 0x7ff;

                        if ((tgui->accel.ger22 & 0xff) == 8) {
                            if (svga->bpp != 24) {
                                svga->hwcursor.x <<= 1;
                                svga_recalctimings(svga);
                                if ((svga->vdisp == 1022) && svga->interlace)
                                    svga->hwcursor.x >>= 1;
                            }
                        }

                        svga->hwcursor.xoff = svga->crtc[0x46] & 0x3f;
                        svga->hwcursor.yoff = svga->crtc[0x47] & 0x3f;
                        svga->hwcursor.addr = (svga->crtc[0x44] << 10) | ((svga->crtc[0x45] & 0x0f) << 18) | (svga->hwcursor.yoff * 8);
                    }
                    break;

                case 0x50:
                    if (tgui->type >= TGUI_9440) {
                        svga->hwcursor.ena       = !!(val & 0x80);
                        svga->hwcursor.cur_xsize = svga->hwcursor.cur_ysize = ((val & 1) ? 64 : 32);
                    }
                    break;

                default:
                    break;
            }

            if (old != val) {
                if (svga->crtcreg < 0xe || svga->crtcreg > 0x10) {
                    if ((svga->crtcreg == 0xc) || (svga->crtcreg == 0xd)) {
                        svga->fullchange = 3;
                        svga->memaddr_latch   = ((svga->crtc[0xc] << 8) | svga->crtc[0xd]) + ((svga->crtc[8] & 0x60) >> 5);
                    } else {
                        svga->fullchange = svga->monitor->mon_changeframecount;
                        svga_recalctimings(svga);
                    }
                }
            }
            return;

        case 0x3D8:
            tgui->tgui_3d8 = val;
            if (svga->gdcreg[0xf] & 4) {
                svga->write_bank = (val & 0x3f) * 65536;
                if (!(svga->gdcreg[0xf] & 1)) {
                    svga->read_bank = (val & 0x3f) * 65536;
                }
            }
            return;
        case 0x3D9:
            tgui->tgui_3d9 = val;
            if ((svga->gdcreg[0xf] & 5) == 5)
                svga->read_bank = (val & 0x3f) * 65536;
            return;

        case 0x3DB:
            tgui->alt_clock = val & 0xe3;
            return;

        case 0x43c8:
            tgui->clock_n = val & 0x7f;
            tgui->clock_m = (tgui->clock_m & ~1) | (val >> 7);
            break;
        case 0x43c9:
            tgui->clock_m = (tgui->clock_m & ~0x1e) | ((val << 1) & 0x1e);
            tgui->clock_k = (val & 0x10) >> 4;
            break;

        default:
            break;
    }
    svga_out(addr, val, svga);
}

static uint8_t
tgui_in(uint16_t addr, void *priv)
{
    tgui_t *tgui = (tgui_t *) priv;
    svga_t *svga = &tgui->svga;
    uint8_t temp;

    if (((addr & 0xFFF0) == 0x3D0 || (addr & 0xFFF0) == 0x3B0) && !(svga->miscout & 1))
        addr ^= 0x60;

    switch (addr) {
        case 0x3C5:
            if (svga->seqaddr == 9) {
                if (tgui->type == TGUI_9680)
                    return 0x01; /*TGUI9680XGi*/
            }
            if (svga->seqaddr == 0x0b) {
                tgui->oldmode = 0;
                switch (tgui->type) {
                    case TGUI_9400CXI:
                        return 0x93; /*TGUI9400CXi*/
                    case TGUI_9440:
                        return 0xe3; /*TGUI9440AGi*/
                    case TGUI_9660:
                    case TGUI_9680:
                        return 0xd3; /*TGUI9660XGi*/

                    default:
                        break;
                }
            }
            if (svga->seqaddr == 0x0d) {
                if (tgui->oldmode)
                    return tgui->oldctrl2;
                return tgui->newctrl2;
            }
            if (svga->seqaddr == 0x0c) {
                if (svga->seqregs[0x0e] & 0x80)
                    return svga->seqregs[0x0c];
            }
            if (svga->seqaddr == 0x0e) {
                if (tgui->oldmode)
                    return tgui->oldctrl1 | 0x88;
                return svga->seqregs[0x0e];
            }
            if ((svga->seqaddr >= 0x5a) && (svga->seqaddr <= 0x5f))
                return svga->seqregs[svga->seqaddr];
            break;

        case 0x3C6:
            if (tgui->type == TGUI_9400CXI)
                return tkd8001_ramdac_in(addr, svga->ramdac, svga);
            if (tgui->ramdac_state == 4)
                return tgui->ramdac_ctrl;
            tgui->ramdac_state++;
            break;

        case 0x3C7:
        case 0x3C8:
        case 0x3C9:
            if (tgui->type == TGUI_9400CXI)
                return tkd8001_ramdac_in(addr, svga->ramdac, svga);
            tgui->ramdac_state = 0;
            break;

        case 0x3CF:
            if (svga->gdcaddr >= 0x5a && svga->gdcaddr <= 0x5f)
                return svga->gdcreg[svga->gdcaddr];
            if (svga->gdcaddr == 0x2f)
                return svga->gdcreg[svga->gdcaddr];
            break;
        case 0x3D4:
            return svga->crtcreg;
        case 0x3D5:
            temp = svga->crtc[svga->crtcreg];
            if ((svga->crtcreg == 0x37) && (tgui->type >= TGUI_9440)) {
                if (!(temp & 0x04)) {
                    temp &= ~0x02;
                    if (i2c_gpio_get_scl(tgui->i2c))
                        temp |= 0x02;
                }
                if (!(temp & 0x08)) {
                    temp &= ~0x01;
                    if (i2c_gpio_get_sda(tgui->i2c))
                        temp |= 0x01;
                }
            }
            return temp;
        case 0x3d8:
            return tgui->tgui_3d8;
        case 0x3d9:
            return tgui->tgui_3d9;
        case 0x3db:
            return tgui->alt_clock;

        default:
            break;
    }
    return svga_in(addr, svga);
}

void
tgui_recalctimings(svga_t *svga)
{
    const tgui_t *tgui       = (tgui_t *) svga->priv;
    uint8_t       ger22lower = (tgui->accel.ger22 & 0xff);
    uint8_t       ger22upper = (tgui->accel.ger22 >> 8);

    if (tgui->type >= TGUI_9440) {
        if ((svga->crtc[0x38] & 0x19) == 0x09)
            svga->bpp = 32;
        else {
            switch ((tgui->ramdac_ctrl >> 4) & 0x0f) {
                case 0x01:
                    svga->bpp = 15;
                    break;
                case 0x03:
                    svga->bpp = 16;
                    break;
                case 0x0d:
                    svga->bpp = 24;
                    break;
                default:
                    svga->bpp = 8;
                    break;
            }
        }
    }

    if ((tgui->type >= TGUI_9440) && (svga->bpp >= 24))
        svga->hdisp = (svga->crtc[1] + 1) << 3;

    if (((svga->crtc[0x29] & 0x30) && (svga->bpp >= 15)) || !svga->rowoffset)
        svga->rowoffset |= 0x100;

#if 0
    pclog("BPP=%d, DataWidth=%02x, CRTC29 bit 4-5=%02x, pixbusmode=%02x, rowoffset=%02x, doublerowoffset=%x.\n", svga->bpp, svga->crtc[0x2a] & 0x40, svga->crtc[0x29] & 0x30, svga->crtc[0x38], svga->rowoffset, svga->gdcreg[0x2f] & 4);
#endif

    if ((svga->crtc[0x1e] & 0xA0) == 0xA0)
        svga->memaddr_latch |= 0x10000;
    if (svga->crtc[0x27] & 0x01)
        svga->memaddr_latch |= 0x20000;
    if (svga->crtc[0x27] & 0x02)
        svga->memaddr_latch |= 0x40000;
    if (svga->crtc[0x27] & 0x04)
        svga->memaddr_latch |= 0x80000;

    if (svga->crtc[0x27] & 0x08)
        svga->split |= 0x400;
    if (svga->crtc[0x27] & 0x10)
        svga->dispend |= 0x400;

    if (svga->crtc[0x27] & 0x20)
        svga->vsyncstart |= 0x400;
    if (svga->crtc[0x27] & 0x40)
        svga->vblankstart |= 0x400;
    if (svga->crtc[0x27] & 0x80)
        svga->vtotal |= 0x400;

    if (tgui->oldctrl2 & 0x10) {
        svga->rowoffset <<= 1;
        svga->lowres = 0;
    }

    svga->interlace = !!(svga->crtc[0x1e] & 4);
    if (svga->interlace && (tgui->type < TGUI_9440))
        svga->rowoffset >>= 1;

    if (svga->vdisp == 1020)
        svga->vdisp += 2;

    if (tgui->oldctrl2 & 0x10)
        svga->memaddr_latch <<= 1;

    svga->lowres = !(svga->crtc[0x2a] & 0x40);

    if (tgui->type >= TGUI_9440) {
        if (svga->miscout & 8)
            svga->clock = (cpuclock * (double) (1ULL << 32)) / (((tgui->clock_n + 8) * 14318180.0) / ((tgui->clock_m + 2) * (1 << tgui->clock_k)));

        if (svga->gdcreg[0xf] & 0x08)
            svga->clock *= 2;
        else if (svga->gdcreg[0xf] & 0x40)
            svga->clock *= 3;
    } else {
        switch (((svga->miscout >> 2) & 3) | ((tgui->newctrl2 << 2) & 4) | ((tgui->newctrl2 >> 3) & 8)) {
            case 0x02:
                svga->clock = (cpuclock * (double) (1ULL << 32)) / 44900000.0;
                break;
            case 0x03:
                svga->clock = (cpuclock * (double) (1ULL << 32)) / 36000000.0;
                break;
            case 0x04:
                svga->clock = (cpuclock * (double) (1ULL << 32)) / 57272000.0;
                break;
            case 0x05:
                svga->clock = (cpuclock * (double) (1ULL << 32)) / 65000000.0;
                break;
            case 0x06:
                svga->clock = (cpuclock * (double) (1ULL << 32)) / 50350000.0;
                break;
            case 0x07:
                svga->clock = (cpuclock * (double) (1ULL << 32)) / 40000000.0;
                break;
            case 0x08:
                svga->clock = (cpuclock * (double) (1ULL << 32)) / 88000000.0;
                break;
            case 0x09:
                svga->clock = (cpuclock * (double) (1ULL << 32)) / 98000000.0;
                break;
            case 0x0a:
                svga->clock = (cpuclock * (double) (1ULL << 32)) / 118800000.0;
                break;
            case 0x0b:
                svga->clock = (cpuclock * (double) (1ULL << 32)) / 108000000.0;
                break;
            case 0x0c:
                svga->clock = (cpuclock * (double) (1ULL << 32)) / 72000000.0;
                break;
            case 0x0d:
                svga->clock = (cpuclock * (double) (1ULL << 32)) / 77000000.0;
                break;
            case 0x0e:
                svga->clock = (cpuclock * (double) (1ULL << 32)) / 80000000.0;
                break;
            case 0x0f:
                svga->clock = (cpuclock * (double) (1ULL << 32)) / 75000000.0;
                break;

            default:
                break;
        }

        if (svga->gdcreg[0xf] & 0x08) {
            svga->htotal <<= 1;
            svga->hdisp <<= 1;
            svga->hdisp_time <<= 1;
        }
    }

    if ((tgui->oldctrl2 & 0x10) || (svga->crtc[0x2a] & 0x40)) {
        switch (svga->bpp) {
            case 8:
                svga->render = svga_render_8bpp_highres;
                if (svga->vdisp == 1022) {
                    if (svga->interlace)
                        svga->dispend++;
                    else
                        svga->dispend += 2;
                }
                if (tgui->type >= TGUI_9660) {
                    switch (svga->vdisp) {
                        case 1024:
                        case 1200:
                            svga->htotal <<= 1;
                            svga->hdisp <<= 1;
                            svga->hdisp_time <<= 1;
                            break;
                        default:
                            break;
                    }
#if OLD_CODE
                    if (svga->dispend == ((1024 >> 1) - 2))
                        svga->dispend += 2;
                    if (svga->dispend == (1024 >> 1))
                        svga->hdisp <<= 1;
                    else if ((svga->hdisp == (1600 >> 1)) && (svga->dispend == (1200 >> 1)) && svga->interlace)
                        svga->hdisp <<= 1;
                    else if (svga->hdisp == (1024 >> 1)) {
                        if (svga->interlace && (svga->dispend == (768 >> 1)))
                            svga->hdisp <<= 1;
                        else if (!svga->interlace && (svga->dispend == 768))
                            svga->hdisp <<= 1;
                    }
#endif

                    if (ger22upper & 0x80) {
                        svga->htotal <<= 1;
                        svga->hdisp <<= 1;
                        svga->hdisp_time <<= 1;
                    }
                    switch (svga->hdisp) {
                        case 640:
                            if (!ger22lower)
                                svga->rowoffset = 0x50;
                            break;

                        default:
                            break;
                    }
                }
                break;
            case 15:
                svga->render = svga_render_15bpp_highres;
                if (tgui->type < TGUI_9440)
                    svga->hdisp >>= 1;
                break;
            case 16:
                svga->render = svga_render_16bpp_highres;
                if (tgui->type < TGUI_9440)
                    svga->hdisp >>= 1;
                break;
            case 24:
                svga->render = svga_render_24bpp_highres;
                if (tgui->type < TGUI_9440)
                    svga->hdisp = (svga->hdisp << 1) / 3;
                break;
            case 32:
                if (svga->rowoffset == 0x100)
                    svga->rowoffset <<= 1;

                svga->render = svga_render_32bpp_highres;
                break;

            default:
                break;
        }
    }
}

static void
tgui_recalcmapping(tgui_t *tgui)
{
    svga_t *svga = &tgui->svga;
    xga_t  *xga  = (xga_t *) svga->xga;

    if (tgui->type == TGUI_9400CXI) {
        if (svga->gdcreg[0x10] & EXT_CTRL_LATCH_COPY) {
            mem_mapping_set_handler(&tgui->linear_mapping,
                                    tgui_ext_linear_read, NULL, NULL,
                                    tgui_ext_linear_write, tgui_ext_linear_writew, tgui_ext_linear_writel);
            mem_mapping_set_handler(&svga->mapping,
                                    tgui_ext_read, NULL, NULL,
                                    tgui_ext_write, tgui_ext_writew, tgui_ext_writel);
        } else if (svga->gdcreg[0x10] & EXT_CTRL_MONO_EXPANSION) {
            mem_mapping_set_handler(&tgui->linear_mapping,
                                    svga_read_linear, svga_readw_linear, svga_readl_linear,
                                    tgui_ext_linear_write, tgui_ext_linear_writew, tgui_ext_linear_writel);
            mem_mapping_set_handler(&svga->mapping,
                                    svga_read, svga_readw, svga_readl,
                                    tgui_ext_write, tgui_ext_writew, tgui_ext_writel);
        } else {
            mem_mapping_set_handler(&tgui->linear_mapping,
                                    svga_read_linear, svga_readw_linear, svga_readl_linear,
                                    svga_write_linear, svga_writew_linear, svga_writel_linear);
            mem_mapping_set_handler(&svga->mapping,
                                    svga_read, svga_readw, svga_readl,
                                    svga_write, svga_writew, svga_writel);
        }
    }

    if (tgui->pci && !(tgui->pci_regs[PCI_REG_COMMAND] & PCI_COMMAND_MEM)) {
        mem_mapping_disable(&svga->mapping);
        mem_mapping_disable(&tgui->linear_mapping);
        mem_mapping_disable(&tgui->accel_mapping);
        mem_mapping_disable(&tgui->mmio_mapping);
        return;
    }

    if (svga->crtc[0x21] & 0x20) {
        mem_mapping_disable(&svga->mapping);
        mem_mapping_set_addr(&tgui->linear_mapping, tgui->linear_base, tgui->linear_size);
        if (tgui->type >= TGUI_9440) {
            if ((svga->crtc[0x36] & 0x03) == 0x01)
                mem_mapping_set_addr(&tgui->accel_mapping, 0xb4000, 0x4000);
            else if ((svga->crtc[0x36] & 0x03) == 0x02)
                mem_mapping_set_addr(&tgui->accel_mapping, 0xbc000, 0x4000);
            else if ((svga->crtc[0x36] & 0x03) == 0x03)
                mem_mapping_set_addr(&tgui->accel_mapping, tgui->ge_base, 0x4000);
        } else {
            switch (svga->gdcreg[6] & 0xC) {
                case 0x0: /*128k at A0000*/
                    mem_mapping_set_addr(&svga->mapping, 0xa0000, 0x20000);
                    svga->banked_mask = 0xffff;
                    break;
                case 0x4: /*64k at A0000*/
                    mem_mapping_set_addr(&svga->mapping, 0xa0000, 0x10000);
                    svga->banked_mask = 0xffff;
                    if (xga_active && (svga->xga != NULL)) {
                        xga->on = 0;
                        mem_mapping_set_handler(&svga->mapping, svga->read, svga->readw, svga->readl, svga->write, svga->writew, svga->writel);
                    }
                    break;
                case 0x8: /*32k at B0000*/
                    mem_mapping_set_addr(&svga->mapping, 0xb0000, 0x08000);
                    svga->banked_mask = 0x7fff;
                    break;
                case 0xC: /*32k at B8000*/
                    mem_mapping_set_addr(&svga->mapping, 0xb8000, 0x08000);
                    svga->banked_mask = 0x7fff;
                    break;

                default:
                    break;
            }
        }
    } else {
        mem_mapping_disable(&tgui->linear_mapping);
        switch (svga->gdcreg[6] & 0xC) {
            case 0x0: /*128k at A0000*/
                mem_mapping_set_addr(&svga->mapping, 0xa0000, 0x20000);
                svga->banked_mask = 0xffff;
                break;
            case 0x4: /*64k at A0000*/
                mem_mapping_set_addr(&svga->mapping, 0xa0000, 0x10000);
                svga->banked_mask = 0xffff;
                if (xga_active && (svga->xga != NULL)) {
                    xga->on = 0;
                    mem_mapping_set_handler(&svga->mapping, svga->read, svga->readw, svga->readl, svga->write, svga->writew, svga->writel);
                }
                break;
            case 0x8: /*32k at B0000*/
                mem_mapping_set_addr(&svga->mapping, 0xb0000, 0x08000);
                svga->banked_mask = 0x7fff;
                break;
            case 0xC: /*32k at B8000*/
                mem_mapping_set_addr(&svga->mapping, 0xb8000, 0x08000);
                svga->banked_mask = 0x7fff;
                break;

            default:
                break;
        }

        if (tgui->pci && tgui->linear_base) /*Assume that, with PCI, linear addressing is always enabled.*/
            mem_mapping_set_addr(&tgui->linear_mapping, tgui->linear_base, tgui->linear_size);

        if ((svga->crtc[0x36] & 0x03) == 0x01)
            mem_mapping_set_addr(&tgui->accel_mapping, 0xb4000, 0x4000);
        else if ((svga->crtc[0x36] & 0x03) == 0x02)
            mem_mapping_set_addr(&tgui->accel_mapping, 0xbc000, 0x4000);
        else if ((svga->crtc[0x36] & 0x03) == 0x03)
            mem_mapping_set_addr(&tgui->accel_mapping, tgui->ge_base, 0x4000);
        else
            mem_mapping_disable(&tgui->accel_mapping);
    }

    if (tgui->type >= TGUI_9440) {
        if ((tgui->mmio_base != 0x00000000) && (svga->crtc[0x39] & 0x01))
            mem_mapping_set_addr(&tgui->mmio_mapping, tgui->mmio_base, 0x10000);
        else
            mem_mapping_disable(&tgui->mmio_mapping);
    }
}

static void
tgui_hwcursor_draw(svga_t *svga, int displine)
{
    uint32_t dat[2];
    int      offset = svga->hwcursor_latch.x - svga->hwcursor_latch.xoff;
    int      pitch  = (svga->hwcursor_latch.cur_xsize == 64) ? 16 : 8;

    if (svga->interlace && svga->hwcursor_oddeven)
        svga->hwcursor_latch.addr += pitch;

    dat[0] = (svga->vram[svga->hwcursor_latch.addr] << 24) | (svga->vram[svga->hwcursor_latch.addr + 1] << 16) | (svga->vram[svga->hwcursor_latch.addr + 2] << 8) | svga->vram[svga->hwcursor_latch.addr + 3];
    dat[1] = (svga->vram[svga->hwcursor_latch.addr + 4] << 24) | (svga->vram[svga->hwcursor_latch.addr + 5] << 16) | (svga->vram[svga->hwcursor_latch.addr + 6] << 8) | svga->vram[svga->hwcursor_latch.addr + 7];
    for (uint8_t xx = 0; xx < 32; xx++) {
        if (svga->crtc[0x50] & 0x40) {
            if (offset >= svga->hwcursor_latch.x) {
                if (dat[0] & 0x80000000)
                    (buffer32->line[displine])[svga->x_add + offset] = (dat[1] & 0x80000000) ? 0xffffff : 0;
            }
        } else {
            if (offset >= svga->hwcursor_latch.x) {
                if (!(dat[0] & 0x80000000))
                    (buffer32->line[displine])[svga->x_add + offset] = (dat[1] & 0x80000000) ? 0xffffff : 0;
                else if (dat[1] & 0x80000000)
                    (buffer32->line[displine])[svga->x_add + offset] ^= 0xffffff;
            }
        }
        offset++;
        dat[0] <<= 1;
        dat[1] <<= 1;
    }
    svga->hwcursor_latch.addr += pitch;

    if (svga->interlace && !svga->hwcursor_oddeven)
        svga->hwcursor_latch.addr += pitch;
}

uint8_t
tgui_pci_read(UNUSED(int func), int addr, void *priv)
{
    const tgui_t *tgui = (tgui_t *) priv;

    switch (addr) {
        case 0x00:
            return 0x23; /*Trident*/
        case 0x01:
            return 0x10;

        case 0x02:
            return (tgui->type == TGUI_9440) ? 0x40 : 0x60; /*TGUI9440AGi or TGUI96x0XGi*/
        case 0x03:
            return (tgui->type == TGUI_9440) ? 0x94 : 0x96;

        case PCI_REG_COMMAND:
            return tgui->pci_regs[PCI_REG_COMMAND] | 0x80; /*Respond to IO and memory accesses*/

        case 0x07:
            return 1 << 1; /*Medium DEVSEL timing*/

        case 0x08:
            return 0; /*Revision ID*/
        case 0x09:
            return 0; /*Programming interface*/

        case 0x0a:
            return 0x01; /*Supports VGA interface, XGA compatible*/
        case 0x0b:
            return 0x03;

        case 0x10:
            return 0x00; /*Linear frame buffer address*/
        case 0x11:
            return 0x00;
        case 0x12:
            return tgui->linear_base >> 16;
        case 0x13:
            return tgui->linear_base >> 24;

        case 0x14:
            return 0x00; /*MMIO address*/
        case 0x15:
            return 0x00;
        case 0x16:
            return tgui->mmio_base >> 16;
        case 0x17:
            return tgui->mmio_base >> 24;

        case 0x30:
            return tgui->has_bios ? (tgui->pci_regs[0x30] & 0x01) : 0x00; /*BIOS ROM address*/
        case 0x31:
            return 0x00;
        case 0x32:
            return tgui->has_bios ? tgui->pci_regs[0x32] : 0x00;
        case 0x33:
            return tgui->has_bios ? tgui->pci_regs[0x33] : 0x00;

        case 0x3c:
            return tgui->int_line;
        case 0x3d:
            return PCI_INTA;

        default:
            break;
    }
    return 0;
}

void
tgui_pci_write(UNUSED(int func), int addr, uint8_t val, void *priv)
{
    tgui_t *tgui = (tgui_t *) priv;
    svga_t *svga = &tgui->svga;

    switch (addr) {
        case PCI_REG_COMMAND:
            tgui->pci_regs[PCI_REG_COMMAND] = val & 0x23;
            if (val & PCI_COMMAND_IO)
                tgui_set_io(tgui);
            else
                tgui_remove_io(tgui);

            tgui_recalcmapping(tgui);
            break;

        case 0x12:
            if (tgui->type >= TGUI_9660)
                tgui->linear_base = (tgui->linear_base & 0xff000000) | ((val & 0xc0) << 16);
            else
                tgui->linear_base = (tgui->linear_base & 0xff000000) | ((val & 0xe0) << 16);
            tgui->linear_size = tgui->vram_size;
            svga->decode_mask = tgui->vram_mask;
            tgui_recalcmapping(tgui);
            break;
        case 0x13:
            if (tgui->type >= TGUI_9660)
                tgui->linear_base = (tgui->linear_base & 0xc00000) | (val << 24);
            else
                tgui->linear_base = (tgui->linear_base & 0xe00000) | (val << 24);
            tgui->linear_size = tgui->vram_size;
            svga->decode_mask = tgui->vram_mask;
            tgui_recalcmapping(tgui);
            break;

        case 0x16:
            if (tgui->type >= TGUI_9660)
                tgui->mmio_base = (tgui->mmio_base & 0xff000000) | ((val & 0xc0) << 16);
            else
                tgui->mmio_base = (tgui->mmio_base & 0xff000000) | ((val & 0xe0) << 16);
            tgui_recalcmapping(tgui);
            break;
        case 0x17:
            if (tgui->type >= TGUI_9660)
                tgui->mmio_base = (tgui->mmio_base & 0x00c00000) | (val << 24);
            else
                tgui->mmio_base = (tgui->mmio_base & 0x00e00000) | (val << 24);
            tgui_recalcmapping(tgui);
            break;

        case 0x30:
        case 0x32:
        case 0x33:
            if (tgui->has_bios) {
                tgui->pci_regs[addr] = val;
                if (tgui->pci_regs[0x30] & 0x01) {
                    uint32_t biosaddr = (tgui->pci_regs[0x32] << 16) | (tgui->pci_regs[0x33] << 24);
                    mem_mapping_set_addr(&tgui->bios_rom.mapping, biosaddr, 0x8000);
                } else {
                    mem_mapping_disable(&tgui->bios_rom.mapping);
                }
            }
            return;

        case 0x3c:
            tgui->int_line = val;
            return;

        default:
            break;
    }
}

static uint8_t
tgui_ext_linear_read(uint32_t addr, void *priv)
{
    svga_t *svga = (svga_t *) priv;
    tgui_t *tgui = (tgui_t *) svga->priv;

    cycles -= svga->monitor->mon_video_timing_read_b;

    addr &= svga->decode_mask;
    if (addr >= svga->vram_max)
        return 0xff;

    addr &= svga->vram_mask;
    addr &= ~0x0f;
    addr  = dword_remap(svga, addr);

    for (int i = 0; i < 16; i++) {
        tgui->copy_latch[i] = svga->vram[addr];
        addr += ((i & 3) == 3) ? 0x0d : 0x01;
    }

    addr &= svga->vram_mask;

    return svga->vram[addr];
}

static uint8_t
tgui_ext_read(uint32_t addr, void *priv)
{
    svga_t *svga = (svga_t *) priv;

    addr = (addr & svga->banked_mask) + svga->read_bank;

    return tgui_ext_linear_read(addr, svga);
}

static void
tgui_ext_linear_write(uint32_t addr, uint8_t val, void *priv)
{
    svga_t       *svga = (svga_t *) priv;
    const tgui_t *tgui = (tgui_t *) svga->priv;
    int           bpp = (svga->gdcreg[0x10] & EXT_CTRL_16BIT);
    uint8_t       fg[2] = { svga->gdcreg[0x14], svga->gdcreg[0x15] };
    uint8_t       bg[2] = { svga->gdcreg[0x11], svga->gdcreg[0x12] };

    cycles -= svga->monitor->mon_video_timing_write_b;

    addr &= svga->decode_mask;
    if (addr >= svga->vram_max)
        return;

    addr &= svga->vram_mask;
    addr &= (svga->gdcreg[0x10] & EXT_CTRL_LATCH_COPY) ? ~0x0f : ~0x07;
    addr = dword_remap(svga, addr);

    svga->changedvram[addr >> 12] = svga->monitor->mon_changeframecount;

    if (svga->gdcreg[0x10] & EXT_CTRL_LATCH_COPY) {
        for (int i = 0; i < 8; i++) {
            if (val & (0x80 >> i))
                svga->vram[addr] = tgui->copy_latch[i];

            addr += ((i & 3) == 3) ? 0x0d : 0x01;
            addr &= svga->vram_mask;
        }
    } else {
        if (svga->gdcreg[0x10] & EXT_CTRL_MONO_TRANSPARENT) {
            if (bpp) {
                for (int i = 0; i < 8; i++) {
                    if (val & (0x80 >> i))
                        svga->vram[addr] = fg[i & 1];

                    addr += ((i & 3) == 3) ? 0x0d : 0x01;
                    addr &= svga->vram_mask;
                }
            } else {
                for (int i = 0; i < 8; i++) {
                    if (val & (0x80 >> i))
                        svga->vram[addr] = fg[0];

                    addr += ((i & 3) == 3) ? 0x0d : 0x01;
                    addr &= svga->vram_mask;
                }
            }
        } else {
            if (bpp) {
                for (int i = 0; i < 8; i++) {
                    if (val & (0x80 >> i)) {
                        if (svga->gdcreg[0x17] & (0x80 >> i))
                            svga->vram[addr] = fg[i & 1];
                    } else {
                        if (svga->gdcreg[0x17] & (0x80 >> i))
                            svga->vram[addr] = bg[i & 1];
                    }
                    addr += ((i & 3) == 3) ? 0x0d : 0x01;
                    addr &= svga->vram_mask;
                }
            } else {
                for (int i = 0; i < 8; i++) {
                    if (val & (0x80 >> i)) {
                        if (svga->gdcreg[0x17] & (0x80 >> i))
                            svga->vram[addr] = fg[0];
                    } else {
                        if (svga->gdcreg[0x17] & (0x80 >> i))
                            svga->vram[addr] = bg[0];
                    }
                    addr += ((i & 3) == 3) ? 0x0d : 0x01;
                    addr &= svga->vram_mask;
                }
            }
        }
    }
}

static void
tgui_ext_linear_writew(uint32_t addr, uint16_t val, void *priv)
{
    svga_t       *svga = (svga_t *) priv;
    const tgui_t *tgui = (tgui_t *) svga->priv;
    int           bpp = (svga->gdcreg[0x10] & EXT_CTRL_16BIT);
    uint8_t       fg[2] = { svga->gdcreg[0x14], svga->gdcreg[0x15] };
    uint8_t       bg[2] = { svga->gdcreg[0x11], svga->gdcreg[0x12] };
    uint16_t      mask  = svga->gdcreg[0x18] | (svga->gdcreg[0x17] << 8);

    cycles -= svga->monitor->mon_video_timing_write_w;

    addr &= svga->decode_mask;
    if (addr >= svga->vram_max)
        return;

    addr &= svga->vram_mask;
    addr &= ~0x0f;
    addr = dword_remap(svga, addr);

    svga->changedvram[addr >> 12] = svga->monitor->mon_changeframecount;
    val = (val >> 8) | (val << 8);

    if (svga->gdcreg[0x10] & EXT_CTRL_LATCH_COPY) {
        for (int i = 0; i < 16; i++) {
            if (val & (0x8000 >> i))
                svga->vram[addr] = tgui->copy_latch[i];

            addr += ((i & 3) == 3) ? 0x0d : 0x01;
            addr &= svga->vram_mask;
        }
    } else {
        if (svga->gdcreg[0x10] & EXT_CTRL_MONO_TRANSPARENT) {
            if (bpp) {
                for (int i = 0; i < 16; i++) {
                    if (val & (0x8000 >> i))
                        svga->vram[addr] = fg[i & 1];

                    addr += ((i & 3) == 3) ? 0x0d : 0x01;
                    addr &= svga->vram_mask;
                }
            } else {
                for (int i = 0; i < 16; i++) {
                    if (val & (0x8000 >> i))
                        svga->vram[addr] = fg[0];

                    addr += ((i & 3) == 3) ? 0x0d : 0x01;
                    addr &= svga->vram_mask;
                }
            }
        } else {
            if (bpp) {
                for (int i = 0; i < 16; i++) {
                    if (val & (0x8000 >> i)) {
                        if (mask & (0x8000 >> i))
                            svga->vram[addr] = fg[i & 1];
                    } else {
                        if (mask & (0x8000 >> i))
                            svga->vram[addr] = bg[i & 1];
                    }
                    addr += ((i & 3) == 3) ? 0x0d : 0x01;
                    addr &= svga->vram_mask;
                }
            } else {
                for (int i = 0; i < 16; i++) {
                    if (val & (0x8000 >> i)) {
                        if (mask & (0x8000 >> i))
                            svga->vram[addr] = fg[0];
                    } else {
                        if (mask & (0x8000 >> i))
                            svga->vram[addr] = bg[0];
                    }
                    addr += ((i & 3) == 3) ? 0x0d : 0x01;
                    addr &= svga->vram_mask;
                }
            }
        }
    }
}

static void
tgui_ext_linear_writel(uint32_t addr, uint32_t val, void *priv)
{
    tgui_ext_linear_writew(addr, val, priv);
}

static void
tgui_ext_write(uint32_t addr, uint8_t val, void *priv)
{
    svga_t *svga = (svga_t *) priv;

    addr = (addr & svga->banked_mask) + svga->write_bank;

    tgui_ext_linear_write(addr, val, svga);
}
static void
tgui_ext_writew(uint32_t addr, uint16_t val, void *priv)
{
    svga_t *svga = (svga_t *) priv;

    addr = (addr & svga->banked_mask) + svga->write_bank;

    tgui_ext_linear_writew(addr, val, svga);
}
static void
tgui_ext_writel(uint32_t addr, uint32_t val, void *priv)
{
    svga_t *svga = (svga_t *) priv;

    addr = (addr & svga->banked_mask) + svga->write_bank;

    tgui_ext_linear_writel(addr, val, svga);
}

enum {
    TGUI_BITBLT        = 1,
    TGUI_SCANLINE      = 3,
    TGUI_BRESENHAMLINE = 4,
    TGUI_SHORTVECTOR   = 5,
    TGUI_FASTLINE      = 6
};

enum {
    TGUI_SRCCPU    = 0,
    TGUI_SRCPAT    = 0x02,   /*Source is from pattern*/
    TGUI_SRCDISP   = 0x04,   /*Source is from display*/
    TGUI_PATMONO   = 0x20,   /*Pattern is monochrome and needs expansion*/
    TGUI_SRCMONO   = 0x40,   /*Source is monochrome from CPU and needs expansion*/
    TGUI_TRANSENA  = 0x1000, /*Transparent (no draw when source == bg col)*/
    TGUI_TRANSREV  = 0x2000, /*Reverse fg/bg for transparent*/
    TGUI_SOLIDFILL = 0x4000, /*Pattern set to foreground color*/
    TGUI_STENCIL   = 0x8000  /*Stencil*/
};

#define READ(addr, dat)                                \
    if (tgui->accel.bpp == 0)                          \
        dat = svga->vram[(addr) &tgui->vram_mask];     \
    else if (tgui->accel.bpp == 1)                     \
        dat = vram_w[(addr) & (tgui->vram_mask >> 1)]; \
    else                                               \
        dat = vram_l[(addr) & (tgui->vram_mask >> 2)];

#define ROPMIX(R, D, P, S, out)                    \
    {                                              \
        switch (R) {                               \
            case 0x00:                             \
                out = 0;                           \
                break;                             \
            case 0x01:                             \
                out = ~(D | (P | S));              \
                break;                             \
            case 0x02:                             \
                out = D & ~(P | S);                \
                break;                             \
            case 0x03:                             \
                out = ~(P | S);                    \
                break;                             \
            case 0x04:                             \
                out = S & ~(D | P);                \
                break;                             \
            case 0x05:                             \
                out = ~(D | P);                    \
                break;                             \
            case 0x06:                             \
                out = ~(P | ~(D ^ S));             \
                break;                             \
            case 0x07:                             \
                out = ~(P | (D & S));              \
                break;                             \
            case 0x08:                             \
                out = S & (D & ~P);                \
                break;                             \
            case 0x09:                             \
                out = ~(P | (D ^ S));              \
                break;                             \
            case 0x0a:                             \
                out = D & ~P;                      \
                break;                             \
            case 0x0b:                             \
                out = ~(P | (S & ~D));             \
                break;                             \
            case 0x0c:                             \
                out = S & ~P;                      \
                break;                             \
            case 0x0d:                             \
                out = ~(P | (D & ~S));             \
                break;                             \
            case 0x0e:                             \
                out = ~(P | ~(D | S));             \
                break;                             \
            case 0x0f:                             \
                out = ~P;                          \
                break;                             \
            case 0x10:                             \
                out = P & ~(D | S);                \
                break;                             \
            case 0x11:                             \
                out = ~(D | S);                    \
                break;                             \
            case 0x12:                             \
                out = ~(S | ~(D ^ P));             \
                break;                             \
            case 0x13:                             \
                out = ~(S | (D & P));              \
                break;                             \
            case 0x14:                             \
                out = ~(D | ~(P ^ S));             \
                break;                             \
            case 0x15:                             \
                out = ~(D | (P & S));              \
                break;                             \
            case 0x16:                             \
                out = P ^ (S ^ (D & ~(P & S)));    \
                break;                             \
            case 0x17:                             \
                out = ~(S ^ ((S ^ P) & (D ^ S)));  \
                break;                             \
            case 0x18:                             \
                out = (S ^ P) & (P ^ D);           \
                break;                             \
            case 0x19:                             \
                out = ~(S ^ (D & ~(P & S)));       \
                break;                             \
            case 0x1a:                             \
                out = P ^ (D | (S & P));           \
                break;                             \
            case 0x1b:                             \
                out = ~(S ^ (D & (P ^ S)));        \
                break;                             \
            case 0x1c:                             \
                out = P ^ (S | (D & P));           \
                break;                             \
            case 0x1d:                             \
                out = ~(D ^ (S & (P ^ D)));        \
                break;                             \
            case 0x1e:                             \
                out = P ^ (D | S);                 \
                break;                             \
            case 0x1f:                             \
                out = ~(P & (D | S));              \
                break;                             \
            case 0x20:                             \
                out = D & (P & ~S);                \
                break;                             \
            case 0x21:                             \
                out = ~(S | (D ^ P));              \
                break;                             \
            case 0x22:                             \
                out = D & ~S;                      \
                break;                             \
            case 0x23:                             \
                out = ~(S | (P & ~D));             \
                break;                             \
            case 0x24:                             \
                out = (S ^ P) & (D ^ S);           \
                break;                             \
            case 0x25:                             \
                out = ~(P ^ (D & ~(S & P)));       \
                break;                             \
            case 0x26:                             \
                out = S ^ (D | (P & S));           \
                break;                             \
            case 0x27:                             \
                out = S ^ (D | ~(P ^ S));          \
                break;                             \
            case 0x28:                             \
                out = D & (P ^ S);                 \
                break;                             \
            case 0x29:                             \
                out = ~(P ^ (S ^ (D | (P & S))));  \
                break;                             \
            case 0x2a:                             \
                out = D & ~(P & S);                \
                break;                             \
            case 0x2b:                             \
                out = ~(S ^ ((S ^ P) & (P ^ D)));  \
                break;                             \
            case 0x2c:                             \
                out = S ^ (P & (D | S));           \
                break;                             \
            case 0x2d:                             \
                out = P ^ (S | ~D);                \
                break;                             \
            case 0x2e:                             \
                out = P ^ (S | (D ^ P));           \
                break;                             \
            case 0x2f:                             \
                out = ~(P & (S | ~D));             \
                break;                             \
            case 0x30:                             \
                out = P & ~S;                      \
                break;                             \
            case 0x31:                             \
                out = ~(S | (D & ~P));             \
                break;                             \
            case 0x32:                             \
                out = S ^ (D | (P | S));           \
                break;                             \
            case 0x33:                             \
                out = ~S;                          \
                break;                             \
            case 0x34:                             \
                out = S ^ (P | (D & S));           \
                break;                             \
            case 0x35:                             \
                out = S ^ (P | ~(D ^ S));          \
                break;                             \
            case 0x36:                             \
                out = S ^ (D | P);                 \
                break;                             \
            case 0x37:                             \
                out = ~(S & (D | P));              \
                break;                             \
            case 0x38:                             \
                out = P ^ (S & (D | P));           \
                break;                             \
            case 0x39:                             \
                out = S ^ (P | ~D);                \
                break;                             \
            case 0x3a:                             \
                out = S ^ (P | (D ^ S));           \
                break;                             \
            case 0x3b:                             \
                out = ~(S & (P | ~D));             \
                break;                             \
            case 0x3c:                             \
                out = P ^ S;                       \
                break;                             \
            case 0x3d:                             \
                out = S ^ (P | ~(D | S));          \
                break;                             \
            case 0x3e:                             \
                out = S ^ (P | (D & ~S));          \
                break;                             \
            case 0x3f:                             \
                out = ~(P & S);                    \
                break;                             \
            case 0x40:                             \
                out = P & (S & ~D);                \
                break;                             \
            case 0x41:                             \
                out = ~(D | (P ^ S));              \
                break;                             \
            case 0x42:                             \
                out = (S ^ D) & (P ^ D);           \
                break;                             \
            case 0x43:                             \
                out = ~(S ^ (P & ~(D & S)));       \
                break;                             \
            case 0x44:                             \
                out = S & ~D;                      \
                break;                             \
            case 0x45:                             \
                out = ~(D | (P & ~S));             \
                break;                             \
            case 0x46:                             \
                out = D ^ (S | (P & D));           \
                break;                             \
            case 0x47:                             \
                out = ~(P ^ (S & (D ^ P)));        \
                break;                             \
            case 0x48:                             \
                out = S & (D ^ P);                 \
                break;                             \
            case 0x49:                             \
                out = ~(P ^ (D ^ (S | (P & D))));  \
                break;                             \
            case 0x4a:                             \
                out = D ^ (P & (S | D));           \
                break;                             \
            case 0x4b:                             \
                out = P ^ (D | ~S);                \
                break;                             \
            case 0x4c:                             \
                out = S & ~(D & P);                \
                break;                             \
            case 0x4d:                             \
                out = ~(S ^ ((S ^ P) | (D ^ S)));  \
                break;                             \
            case 0x4e:                             \
                out = P ^ (D | (S ^ P));           \
                break;                             \
            case 0x4f:                             \
                out = ~(P & (D | ~S));             \
                break;                             \
            case 0x50:                             \
                out = P & ~D;                      \
                break;                             \
            case 0x51:                             \
                out = ~(D | (S & ~P));             \
                break;                             \
            case 0x52:                             \
                out = D ^ (P | (S & D));           \
                break;                             \
            case 0x53:                             \
                out = ~(S ^ (P & (D ^ S)));        \
                break;                             \
            case 0x54:                             \
                out = ~(D | ~(P | S));             \
                break;                             \
            case 0x55:                             \
                out = ~D;                          \
                break;                             \
            case 0x56:                             \
                out = D ^ (P | S);                 \
                break;                             \
            case 0x57:                             \
                out = ~(D & (P | S));              \
                break;                             \
            case 0x58:                             \
                out = P ^ (D & (S | P));           \
                break;                             \
            case 0x59:                             \
                out = D ^ (P | ~S);                \
                break;                             \
            case 0x5a:                             \
                out = D ^ P;                       \
                break;                             \
            case 0x5b:                             \
                out = D ^ (P | ~(S | D));          \
                break;                             \
            case 0x5c:                             \
                out = D ^ (P | (S ^ D));           \
                break;                             \
            case 0x5d:                             \
                out = ~(D & (P | ~S));             \
                break;                             \
            case 0x5e:                             \
                out = D ^ (P | (S & ~D));          \
                break;                             \
            case 0x5f:                             \
                out = ~(D & P);                    \
                break;                             \
            case 0x60:                             \
                out = P & (D ^ S);                 \
                break;                             \
            case 0x61:                             \
                out = ~(D ^ (S ^ (P | (D & S))));  \
                break;                             \
            case 0x62:                             \
                out = D ^ (S & (P | D));           \
                break;                             \
            case 0x63:                             \
                out = S ^ (D | ~P);                \
                break;                             \
            case 0x64:                             \
                out = S ^ (D & (P | S));           \
                break;                             \
            case 0x65:                             \
                out = D ^ (S | ~P);                \
                break;                             \
            case 0x66:                             \
                out = D ^ S;                       \
                break;                             \
            case 0x67:                             \
                out = S ^ (D | ~(P | S));          \
                break;                             \
            case 0x68:                             \
                out = ~(D ^ (S ^ (P | ~(D | S)))); \
                break;                             \
            case 0x69:                             \
                out = ~(P ^ (D ^ S));              \
                break;                             \
            case 0x6a:                             \
                out = D ^ (P & S);                 \
                break;                             \
            case 0x6b:                             \
                out = ~(P ^ (S ^ (D & (P | S))));  \
                break;                             \
            case 0x6c:                             \
                out = S ^ (D & P);                 \
                break;                             \
            case 0x6d:                             \
                out = ~(P ^ (D ^ (S & (P | D))));  \
                break;                             \
            case 0x6e:                             \
                out = S ^ (D & (P | ~S));          \
                break;                             \
            case 0x6f:                             \
                out = ~(P & ~(D ^ S));             \
                break;                             \
            case 0x70:                             \
                out = P & ~(D & S);                \
                break;                             \
            case 0x71:                             \
                out = ~(S ^ ((S ^ D) & (P ^ D)));  \
                break;                             \
            case 0x72:                             \
                out = S ^ (D | (P ^ S));           \
                break;                             \
            case 0x73:                             \
                out = ~(S & (D | ~P));             \
                break;                             \
            case 0x74:                             \
                out = D ^ (S | (P ^ D));           \
                break;                             \
            case 0x75:                             \
                out = ~(D & (S | ~P));             \
                break;                             \
            case 0x76:                             \
                out = S ^ (D | (P & ~S));          \
                break;                             \
            case 0x77:                             \
                out = ~(D & S);                    \
                break;                             \
            case 0x78:                             \
                out = P ^ (D & S);                 \
                break;                             \
            case 0x79:                             \
                out = ~(D ^ (S ^ (P & (D | S))));  \
                break;                             \
            case 0x7a:                             \
                out = D ^ (P & (S | ~D));          \
                break;                             \
            case 0x7b:                             \
                out = ~(S & ~(D ^ P));             \
                break;                             \
            case 0x7c:                             \
                out = S ^ (P & (D | ~S));          \
                break;                             \
            case 0x7d:                             \
                out = ~(D & ~(P ^ S));             \
                break;                             \
            case 0x7e:                             \
                out = (S ^ P) | (D ^ S);           \
                break;                             \
            case 0x7f:                             \
                out = ~(D & (P & S));              \
                break;                             \
            case 0x80:                             \
                out = D & (P & S);                 \
                break;                             \
            case 0x81:                             \
                out = ~((S ^ P) | (D ^ S));        \
                break;                             \
            case 0x82:                             \
                out = D & ~(P ^ S);                \
                break;                             \
            case 0x83:                             \
                out = ~(S ^ (P & (D | ~S)));       \
                break;                             \
            case 0x84:                             \
                out = S & ~(D ^ P);                \
                break;                             \
            case 0x85:                             \
                out = ~(P ^ (D & (S | ~P)));       \
                break;                             \
            case 0x86:                             \
                out = D ^ (S ^ (P & (D | S)));     \
                break;                             \
            case 0x87:                             \
                out = ~(P ^ (D & S));              \
                break;                             \
            case 0x88:                             \
                out = D & S;                       \
                break;                             \
            case 0x89:                             \
                out = ~(S ^ (D | (P & ~S)));       \
                break;                             \
            case 0x8a:                             \
                out = D & (S | ~P);                \
                break;                             \
            case 0x8b:                             \
                out = ~(D ^ (S | (P ^ D)));        \
                break;                             \
            case 0x8c:                             \
                out = S & (D | ~P);                \
                break;                             \
            case 0x8d:                             \
                out = ~(S ^ (D | (P ^ S)));        \
                break;                             \
            case 0x8e:                             \
                out = S ^ ((S ^ D) & (P ^ D));     \
                break;                             \
            case 0x8f:                             \
                out = ~(P & ~(D & S));             \
                break;                             \
            case 0x90:                             \
                out = P & ~(D ^ S);                \
                break;                             \
            case 0x91:                             \
                out = ~(S ^ (D & (P | ~S)));       \
                break;                             \
            case 0x92:                             \
                out = D ^ (P ^ (S & (D | P)));     \
                break;                             \
            case 0x93:                             \
                out = ~(S ^ (P & D));              \
                break;                             \
            case 0x94:                             \
                out = P ^ (S ^ (D & (P | S)));     \
                break;                             \
            case 0x95:                             \
                out = ~(D ^ (P & S));              \
                break;                             \
            case 0x96:                             \
                out = D ^ (P ^ S);                 \
                break;                             \
            case 0x97:                             \
                out = P ^ (S ^ (D | ~(P | S)));    \
                break;                             \
            case 0x98:                             \
                out = ~(S ^ (D | ~(P | S)));       \
                break;                             \
            case 0x99:                             \
                out = ~(D ^ S);                    \
                break;                             \
            case 0x9a:                             \
                out = D ^ (P & ~S);                \
                break;                             \
            case 0x9b:                             \
                out = ~(S ^ (D & (P | S)));        \
                break;                             \
            case 0x9c:                             \
                out = S ^ (P & ~D);                \
                break;                             \
            case 0x9d:                             \
                out = ~(D ^ (S & (P | D)));        \
                break;                             \
            case 0x9e:                             \
                out = D ^ (S ^ (P | (D & S)));     \
                break;                             \
            case 0x9f:                             \
                out = ~(P & (D ^ S));              \
                break;                             \
            case 0xa0:                             \
                out = D & P;                       \
                break;                             \
            case 0xa1:                             \
                out = ~(P ^ (D | (S & ~P)));       \
                break;                             \
            case 0xa2:                             \
                out = D & (P | ~S);                \
                break;                             \
            case 0xa3:                             \
                out = ~(D ^ (P | (S ^ D)));        \
                break;                             \
            case 0xa4:                             \
                out = ~(P ^ (D | ~(S | P)));       \
                break;                             \
            case 0xa5:                             \
                out = ~(P ^ D);                    \
                break;                             \
            case 0xa6:                             \
                out = D ^ (S & ~P);                \
                break;                             \
            case 0xa7:                             \
                out = ~(P ^ (D & (S | P)));        \
                break;                             \
            case 0xa8:                             \
                out = D & (P | S);                 \
                break;                             \
            case 0xa9:                             \
                out = ~(D ^ (P | S));              \
                break;                             \
            case 0xaa:                             \
                out = D;                           \
                break;                             \
            case 0xab:                             \
                out = D | ~(P | S);                \
                break;                             \
            case 0xac:                             \
                out = S ^ (P & (D ^ S));           \
                break;                             \
            case 0xad:                             \
                out = ~(D ^ (P | (S & D)));        \
                break;                             \
            case 0xae:                             \
                out = D | (S & ~P);                \
                break;                             \
            case 0xaf:                             \
                out = D | ~P;                      \
                break;                             \
            case 0xb0:                             \
                out = P & (D | ~S);                \
                break;                             \
            case 0xb1:                             \
                out = ~(P ^ (D | (S ^ P)));        \
                break;                             \
            case 0xb2:                             \
                out = S ^ ((S ^ P) | (D ^ S));     \
                break;                             \
            case 0xb3:                             \
                out = ~(S & ~(D & P));             \
                break;                             \
            case 0xb4:                             \
                out = P ^ (S & ~D);                \
                break;                             \
            case 0xb5:                             \
                out = ~(D ^ (P & (S | D)));        \
                break;                             \
            case 0xb6:                             \
                out = D ^ (P ^ (S | (D & P)));     \
                break;                             \
            case 0xb7:                             \
                out = ~(S & (D ^ P));              \
                break;                             \
            case 0xb8:                             \
                out = P ^ (S & (D ^ P));           \
                break;                             \
            case 0xb9:                             \
                out = ~(D ^ (S | (P & D)));        \
                break;                             \
            case 0xba:                             \
                out = D | (P & ~S);                \
                break;                             \
            case 0xbb:                             \
                out = D | ~S;                      \
                break;                             \
            case 0xbc:                             \
                out = S ^ (P & ~(D & S));          \
                break;                             \
            case 0xbd:                             \
                out = ~((S ^ D) & (P ^ D));        \
                break;                             \
            case 0xbe:                             \
                out = D | (P ^ S);                 \
                break;                             \
            case 0xbf:                             \
                out = D | ~(P & S);                \
                break;                             \
            case 0xc0:                             \
                out = P & S;                       \
                break;                             \
            case 0xc1:                             \
                out = ~(S ^ (P | (D & ~S)));       \
                break;                             \
            case 0xc2:                             \
                out = ~(S ^ (P | ~(D | S)));       \
                break;                             \
            case 0xc3:                             \
                out = ~(P ^ S);                    \
                break;                             \
            case 0xc4:                             \
                out = S & (P | ~D);                \
                break;                             \
            case 0xc5:                             \
                out = ~(S ^ (P | (D ^ S)));        \
                break;                             \
            case 0xc6:                             \
                out = S ^ (D & ~P);                \
                break;                             \
            case 0xc7:                             \
                out = ~(P ^ (S & (D | P)));        \
                break;                             \
            case 0xc8:                             \
                out = S & (D | P);                 \
                break;                             \
            case 0xc9:                             \
                out = ~(S ^ (P | D));              \
                break;                             \
            case 0xca:                             \
                out = D ^ (P & (S ^ D));           \
                break;                             \
            case 0xcb:                             \
                out = ~(S ^ (P | (D & S)));        \
                break;                             \
            case 0xcc:                             \
                out = S;                           \
                break;                             \
            case 0xcd:                             \
                out = S | ~(D | P);                \
                break;                             \
            case 0xce:                             \
                out = S | (D & ~P);                \
                break;                             \
            case 0xcf:                             \
                out = S | ~P;                      \
                break;                             \
            case 0xd0:                             \
                out = P & (S | ~D);                \
                break;                             \
            case 0xd1:                             \
                out = ~(P ^ (S | (D ^ P)));        \
                break;                             \
            case 0xd2:                             \
                out = P ^ (D & ~S);                \
                break;                             \
            case 0xd3:                             \
                out = ~(S ^ (P & (D | S)));        \
                break;                             \
            case 0xd4:                             \
                out = S ^ ((S ^ P) & (P ^ D));     \
                break;                             \
            case 0xd5:                             \
                out = ~(D & ~(P & S));             \
                break;                             \
            case 0xd6:                             \
                out = P ^ (S ^ (D | (P & S)));     \
                break;                             \
            case 0xd7:                             \
                out = ~(D & (P ^ S));              \
                break;                             \
            case 0xd8:                             \
                out = P ^ (D & (S ^ P));           \
                break;                             \
            case 0xd9:                             \
                out = ~(S ^ (D | (P & S)));        \
                break;                             \
            case 0xda:                             \
                out = D ^ (P & ~(S & D));          \
                break;                             \
            case 0xdb:                             \
                out = ~((S ^ P) & (D ^ S));        \
                break;                             \
            case 0xdc:                             \
                out = S | (P & ~D);                \
                break;                             \
            case 0xdd:                             \
                out = S | ~D;                      \
                break;                             \
            case 0xde:                             \
                out = S | (D ^ P);                 \
                break;                             \
            case 0xdf:                             \
                out = S | ~(D & P);                \
                break;                             \
            case 0xe0:                             \
                out = P & (D | S);                 \
                break;                             \
            case 0xe1:                             \
                out = ~(P ^ (D | S));              \
                break;                             \
            case 0xe2:                             \
                out = D ^ (S & (P ^ D));           \
                break;                             \
            case 0xe3:                             \
                out = ~(P ^ (S | (D & P)));        \
                break;                             \
            case 0xe4:                             \
                out = S ^ (D & (P ^ S));           \
                break;                             \
            case 0xe5:                             \
                out = ~(P ^ (D | (S & P)));        \
                break;                             \
            case 0xe6:                             \
                out = S ^ (D & ~(P & S));          \
                break;                             \
            case 0xe7:                             \
                out = ~((S ^ P) & (P ^ D));        \
                break;                             \
            case 0xe8:                             \
                out = S ^ ((S ^ P) & (D ^ S));     \
                break;                             \
            case 0xe9:                             \
                out = ~(D ^ (S ^ (P & ~(D & S)))); \
                break;                             \
            case 0xea:                             \
                out = D | (P & S);                 \
                break;                             \
            case 0xeb:                             \
                out = D | ~(P ^ S);                \
                break;                             \
            case 0xec:                             \
                out = S | (D & P);                 \
                break;                             \
            case 0xed:                             \
                out = S | ~(D ^ P);                \
                break;                             \
            case 0xee:                             \
                out = D | S;                       \
                break;                             \
            case 0xef:                             \
                out = S | (D | ~P);                \
                break;                             \
            case 0xf0:                             \
                out = P;                           \
                break;                             \
            case 0xf1:                             \
                out = P | ~(D | S);                \
                break;                             \
            case 0xf2:                             \
                out = P | (D & ~S);                \
                break;                             \
            case 0xf3:                             \
                out = P | ~S;                      \
                break;                             \
            case 0xf4:                             \
                out = P | (S & ~D);                \
                break;                             \
            case 0xf5:                             \
                out = P | ~D;                      \
                break;                             \
            case 0xf6:                             \
                out = P | (D ^ S);                 \
                break;                             \
            case 0xf7:                             \
                out = P | ~(D & S);                \
                break;                             \
            case 0xf8:                             \
                out = P | (D & S);                 \
                break;                             \
            case 0xf9:                             \
                out = P | ~(D ^ S);                \
                break;                             \
            case 0xfa:                             \
                out = D | P;                       \
                break;                             \
            case 0xfb:                             \
                out = D | (P | ~S);                \
                break;                             \
            case 0xfc:                             \
                out = P | S;                       \
                break;                             \
            case 0xfd:                             \
                out = P | (S | ~D);                \
                break;                             \
            case 0xfe:                             \
                out = D | (P | S);                 \
                break;                             \
            case 0xff:                             \
                out = ~0;                          \
                break;                             \
        }                                          \
    }

#define MIX()                                                    \
    do {                                                         \
        out = 0;                                                 \
        ROPMIX(tgui->accel.rop, dst_dat, pat_dat, src_dat, out); \
    } while (0)

#define WRITE(addr, dat)                                                                                  \
    if (tgui->accel.bpp == 0) {                                                                           \
        svga->vram[(addr) &tgui->vram_mask]                   = dat;                                      \
        svga->changedvram[((addr) & (tgui->vram_mask)) >> 12] = svga->monitor->mon_changeframecount;      \
    } else if (tgui->accel.bpp == 1) {                                                                    \
        vram_w[(addr) & (tgui->vram_mask >> 1)]                    = dat;                                 \
        svga->changedvram[((addr) & (tgui->vram_mask >> 1)) >> 11] = svga->monitor->mon_changeframecount; \
    } else {                                                                                              \
        vram_l[(addr) & (tgui->vram_mask >> 2)]                    = dat;                                 \
        svga->changedvram[((addr) & (tgui->vram_mask >> 2)) >> 10] = svga->monitor->mon_changeframecount; \
    }

static void
tgui_accel_command(int count, uint32_t cpu_dat, tgui_t *tgui)
{
    svga_t         *svga = &tgui->svga;
    const uint32_t *pattern_data;
    int             x;
    int             y;
    uint32_t        out;
    uint32_t        src_dat   = 0;
    uint32_t        dst_dat;
    uint32_t        pat_dat;
    int             xdir      = (tgui->accel.flags & 0x200) ? -1 : 1;
    int             ydir      = (tgui->accel.flags & 0x100) ? -1 : 1;
    uint32_t        trans_col = (tgui->accel.flags & TGUI_TRANSREV) ? tgui->accel.fg_col : tgui->accel.bg_col;
    uint16_t       *vram_w    = (uint16_t *) svga->vram;
    uint32_t       *vram_l    = (uint32_t *) svga->vram;

    if (tgui->accel.bpp == 0) {
        trans_col &= 0xff;
    } else if (tgui->accel.bpp == 1) {
        trans_col &= 0xffff;
    }

    if ((count != -1) && !tgui->accel.x && (tgui->accel.flags & TGUI_SRCMONO)) {
        count -= (tgui->accel.flags >> 24) & 7;
        cpu_dat <<= (tgui->accel.flags >> 24) & 7;
    }

    if (count == -1)
        tgui->accel.x = tgui->accel.y = 0;

    tgui->accel.pattern_32_idx = 0;

    if (tgui->accel.flags & TGUI_SOLIDFILL) {
        for (y = 0; y < 8; y++) {
            for (x = 0; x < 8; x++) {
                tgui->accel.fill_pattern[(y * 8) + (7 - x)] = tgui->accel.fg_col;
            }
        }
        pattern_data = tgui->accel.fill_pattern;
    } else if (tgui->accel.flags & TGUI_PATMONO) {
        for (y = 0; y < 8; y++) {
            for (x = 0; x < 8; x++) {
                tgui->accel.mono_pattern[(y * 8) + (7 - x)] = (tgui->accel.pattern[y] & (1 << x)) ? tgui->accel.fg_col : tgui->accel.bg_col;
            }
        }
        pattern_data = tgui->accel.mono_pattern;
    } else {
        if (tgui->accel.bpp == 0) {
            for (y = 0; y < 8; y++) {
                for (x = 0; x < 8; x++) {
                    tgui->accel.pattern_8[(y * 8) + x] = tgui->accel.pattern[x + y * 8];
                }
            }
            pattern_data = tgui->accel.pattern_8;
        } else if (tgui->accel.bpp == 1) {
            for (y = 0; y < 8; y++) {
                for (x = 0; x < 8; x++) {
                    tgui->accel.pattern_16[(y * 8) + x] = tgui->accel.pattern[x * 2 + y * 16] | (tgui->accel.pattern[x * 2 + y * 16 + 1] << 8);
                }
            }
            pattern_data = tgui->accel.pattern_16;
        } else {
            for (y = 0; y < 8; y++) {
                for (x = 0; x < 8; x++) {
                    tgui->accel.pattern_32[(y * 8) + x] = tgui->accel.pattern_32bpp[x * 4 + y * 32] | (tgui->accel.pattern_32bpp[x * 4 + y * 32 + 1] << 8) | (tgui->accel.pattern_32bpp[x * 4 + y * 32 + 2] << 16) | (tgui->accel.pattern_32bpp[x * 4 + y * 32 + 3] << 24);
                }
            }
            pattern_data = tgui->accel.pattern_32;
        }
    }

    /* See Linux kernel drivers/video/tridentfb.c for the pitch */
    tgui->accel.pitch = svga->rowoffset;

    switch (svga->bpp) {
        case 8:
        case 24:
            tgui->accel.pitch <<= 3;
            break;
        case 15:
        case 16:
            tgui->accel.pitch <<= 2;
            break;
        case 32:
            tgui->accel.pitch <<= 1;
            break;
        default:
            break;
    }
#if 0
    pclog("TGUI accel command = %x, ger22 = %04x, hdisp = %d, dispend = %d, vtotal = %d, rowoffset = %d, svgabpp = %d, interlace = %d, accelbpp = %d, pitch = %d.\n", tgui->accel.command, tgui->accel.ger22, svga->hdisp, svga->dispend, svga->vtotal, svga->rowoffset, svga->bpp, svga->interlace, tgui->accel.bpp, tgui->accel.pitch);
#endif

    switch (tgui->accel.command) {
        case TGUI_BITBLT:
            if (count == -1) {
                tgui->accel.src_old = tgui->accel.src_x + (tgui->accel.src_y * tgui->accel.pitch);
                tgui->accel.src     = tgui->accel.src_old;

                tgui->accel.dst_old = tgui->accel.dst_x + (tgui->accel.dst_y * tgui->accel.pitch);
                tgui->accel.dst     = tgui->accel.dst_old;

                tgui->accel.pat_x = tgui->accel.dst_x;
                tgui->accel.pat_y = tgui->accel.dst_y;

                tgui->accel.dx = tgui->accel.dst_x & 0xfff;
                tgui->accel.dy = tgui->accel.dst_y & 0xfff;

                tgui->accel.left   = tgui->accel.src_x_clip & 0xfff;
                tgui->accel.right  = tgui->accel.dst_x_clip & 0xfff;
                tgui->accel.top    = tgui->accel.src_y_clip & 0xfff;
                tgui->accel.bottom = tgui->accel.dst_y_clip & 0xfff;

                if (tgui->accel.bpp == 1) {
                    tgui->accel.left >>= 1;
                    tgui->accel.right >>= 1;
                } else if (tgui->accel.bpp == 3) {
                    tgui->accel.left >>= 2;
                    tgui->accel.right >>= 2;
                }
            }

            switch (tgui->accel.flags & (TGUI_SRCMONO | TGUI_SRCDISP)) {
                case TGUI_SRCCPU:
                    if (count == -1) {
                        if (svga->crtc[0x21] & 0x20)
                            tgui->write_blitter = 1;
                        if (tgui->accel.use_src)
                            return;
                    } else
                        count >>= 3;

                    while (count) {
                        if ((tgui->type == TGUI_9440) || ((tgui->type >= TGUI_9660) && tgui->accel.dx >= tgui->accel.left && tgui->accel.dx <= tgui->accel.right && tgui->accel.dy >= tgui->accel.top && tgui->accel.dy <= tgui->accel.bottom)) {
                            if (tgui->accel.bpp == 0) {
                                src_dat = cpu_dat >> 24;
                                cpu_dat <<= 8;
                            } else if (tgui->accel.bpp == 1) {
                                src_dat = (cpu_dat >> 24) | ((cpu_dat >> 8) & 0xff00);
                                cpu_dat <<= 16;
                                count--;
                            } else {
                                src_dat = (cpu_dat >> 24) | ((cpu_dat >> 8) & 0x0000ff00) | ((cpu_dat << 8) & 0x00ff0000);
                                cpu_dat <<= 16;
                                count -= 3;
                            }


                            READ(tgui->accel.dst, dst_dat);

                            pat_dat = pattern_data[((tgui->accel.pat_y & 7) * 8) + (tgui->accel.pat_x & 7)];

                            if (tgui->accel.bpp == 0)
                                pat_dat &= 0xff;
                            else if (tgui->accel.bpp == 1)
                                pat_dat &= 0xffff;

                            if ((((tgui->accel.flags & (TGUI_PATMONO | TGUI_TRANSENA)) == (TGUI_TRANSENA | TGUI_PATMONO)) && (pat_dat != trans_col)) || !(tgui->accel.flags & TGUI_PATMONO) || ((tgui->accel.flags & (TGUI_PATMONO | TGUI_TRANSENA)) == TGUI_PATMONO) || (tgui->accel.ger22 & 0x200)) {
                                MIX();

                                WRITE(tgui->accel.dst, out);
                            }
                        }

                        tgui->accel.src += xdir;
                        tgui->accel.dst += xdir;
                        tgui->accel.pat_x += xdir;
                        if (tgui->type >= TGUI_9660)
                            tgui->accel.dx += xdir;

                        tgui->accel.x++;
                        if (tgui->accel.x > tgui->accel.size_x) {
                            tgui->accel.x = 0;

                            tgui->accel.pat_x = tgui->accel.dst_x;
                            tgui->accel.pat_y += ydir;

                            if (tgui->type >= TGUI_9660) {
                                tgui->accel.dx = tgui->accel.dst_x & 0xfff;
                                tgui->accel.dy += ydir;
                            }

                            tgui->accel.src_old += (ydir * tgui->accel.pitch);
                            tgui->accel.dst_old += (ydir * tgui->accel.pitch);

                            tgui->accel.src = tgui->accel.src_old;
                            tgui->accel.dst = tgui->accel.dst_old;

                            tgui->accel.y++;

                            if (tgui->accel.y > tgui->accel.size_y) {
                                if (svga->crtc[0x21] & 0x20)
                                    tgui->write_blitter = 0;
                                return;
                            }
                            if (tgui->accel.use_src)
                                return;
                        }
                        count--;
                    }
                    break;

                case TGUI_SRCMONO | TGUI_SRCCPU:
                    if (count == -1) {
                        if (svga->crtc[0x21] & 0x20)
                            tgui->write_blitter = 1;
                        if (tgui->accel.use_src)
                            return;
                    }

                    while (count--) {
                        if ((tgui->type == TGUI_9440) || ((tgui->type >= TGUI_9660) && tgui->accel.dx >= tgui->accel.left && tgui->accel.dx <= tgui->accel.right && tgui->accel.dy >= tgui->accel.top && tgui->accel.dy <= tgui->accel.bottom)) {
                            src_dat = ((cpu_dat >> 31) ? tgui->accel.fg_col : tgui->accel.bg_col);
                            if (tgui->accel.bpp == 0)
                                src_dat &= 0xff;
                            else if (tgui->accel.bpp == 1)
                                src_dat &= 0xffff;

                            READ(tgui->accel.dst, dst_dat);

                            pat_dat = pattern_data[((tgui->accel.pat_y & 7) * 8) + (tgui->accel.pat_x & 7)];

                            if (tgui->accel.bpp == 0)
                                pat_dat &= 0xff;
                            else if (tgui->accel.bpp == 1)
                                pat_dat &= 0xffff;

                            if (!(tgui->accel.flags & TGUI_TRANSENA) || (src_dat != trans_col)) {
                                MIX();

                                WRITE(tgui->accel.dst, out);
                            }
                        }

                        cpu_dat <<= 1;
                        tgui->accel.src += xdir;
                        tgui->accel.dst += xdir;
                        tgui->accel.pat_x += xdir;
                        if (tgui->type >= TGUI_9660)
                            tgui->accel.dx += xdir;

                        tgui->accel.x++;
                        if (tgui->accel.x > tgui->accel.size_x) {
                            tgui->accel.x = 0;

                            tgui->accel.pat_x = tgui->accel.dst_x;
                            tgui->accel.pat_y += ydir;

                            if (tgui->type >= TGUI_9660) {
                                tgui->accel.dx = tgui->accel.dst_x & 0xfff;
                                tgui->accel.dy += ydir;
                            }

                            tgui->accel.src = tgui->accel.src_old = tgui->accel.src_old + (ydir * tgui->accel.pitch);
                            tgui->accel.dst = tgui->accel.dst_old = tgui->accel.dst_old + (ydir * tgui->accel.pitch);

                            tgui->accel.y++;

                            if (tgui->accel.y > tgui->accel.size_y) {
                                if (svga->crtc[0x21] & 0x20)
                                    tgui->write_blitter = 0;
                                return;
                            }
                            if (tgui->accel.use_src)
                                return;
                        }
                    }
                    break;

                default:
                    while (count--) {
                        READ(tgui->accel.src, src_dat);
                        READ(tgui->accel.dst, dst_dat);

                        pat_dat = pattern_data[((tgui->accel.pat_y & 7) * 8) + (tgui->accel.pat_x & 7)];

                        if (tgui->accel.bpp == 0)
                            pat_dat &= 0xff;
                        else if (tgui->accel.bpp == 1)
                            pat_dat &= 0xffff;

                        if (!(tgui->accel.flags & TGUI_TRANSENA) || (src_dat != trans_col)) {
                            MIX();

                            WRITE(tgui->accel.dst, out);
                        }

                        tgui->accel.src += xdir;
                        tgui->accel.dst += xdir;
                        tgui->accel.pat_x += xdir;

                        tgui->accel.x++;
                        if (tgui->accel.x > tgui->accel.size_x) {
                            tgui->accel.x = 0;
                            tgui->accel.y++;

                            tgui->accel.pat_x = tgui->accel.dst_x;
                            tgui->accel.pat_y += ydir;

                            tgui->accel.src = tgui->accel.src_old = tgui->accel.src_old + (ydir * tgui->accel.pitch);
                            tgui->accel.dst = tgui->accel.dst_old = tgui->accel.dst_old + (ydir * tgui->accel.pitch);

                            if (tgui->accel.y > tgui->accel.size_y)
                                return;
                        }
                    }
                    break;
            }
            break;

        case TGUI_SCANLINE:
            if (count == -1) {
                tgui->accel.src_old = tgui->accel.src_x + (tgui->accel.src_y * tgui->accel.pitch);
                tgui->accel.src     = tgui->accel.src_old;

                tgui->accel.dst_old = tgui->accel.dst_x + (tgui->accel.dst_y * tgui->accel.pitch);
                tgui->accel.dst     = tgui->accel.dst_old;

                tgui->accel.pat_x = tgui->accel.dst_x;
                tgui->accel.pat_y = tgui->accel.dst_y;
            }

            while (count--) {
                READ(tgui->accel.src, src_dat);
                READ(tgui->accel.dst, dst_dat);

                pat_dat = pattern_data[((tgui->accel.pat_y & 7) * 8) + (tgui->accel.pat_x & 7)];

                if (tgui->accel.bpp == 0)
                    pat_dat &= 0xff;
                else if (tgui->accel.bpp == 1)
                    pat_dat &= 0xffff;

                if (!(tgui->accel.flags & TGUI_TRANSENA) || (src_dat != trans_col)) {
                    MIX();

                    WRITE(tgui->accel.dst, out);
                }

                tgui->accel.src += xdir;
                tgui->accel.dst += xdir;
                tgui->accel.pat_x += xdir;

                tgui->accel.x++;
                if (tgui->accel.x > tgui->accel.size_x) {
                    tgui->accel.x = 0;

                    tgui->accel.pat_x = tgui->accel.dst_x;
                    tgui->accel.src = tgui->accel.src_old = tgui->accel.src_old + (ydir * tgui->accel.pitch);
                    tgui->accel.dst = tgui->accel.dst_old = tgui->accel.dst_old + (ydir * tgui->accel.pitch);
                    tgui->accel.pat_y += ydir;
                    return;
                }
            }
            break;

        case TGUI_BRESENHAMLINE:
            if (count == -1) {
                tgui->accel.dx = tgui->accel.dst_x & 0xfff;
                tgui->accel.dy = tgui->accel.dst_y & 0xfff;
                tgui->accel.y = tgui->accel.size_y;

                tgui->accel.left   = tgui->accel.src_x_clip & 0xfff;
                tgui->accel.right  = tgui->accel.dst_x_clip & 0xfff;
                tgui->accel.top    = tgui->accel.src_y_clip & 0xfff;
                tgui->accel.bottom = tgui->accel.dst_y_clip & 0xfff;

                if (tgui->accel.bpp == 1) {
                    tgui->accel.left >>= 1;
                    tgui->accel.right >>= 1;
                } else if (tgui->accel.bpp == 3) {
                    tgui->accel.left >>= 2;
                    tgui->accel.right >>= 2;
                }
            }

            while (count--) {
                /*Note by TC1995: I suppose the x/y clipping max is always more than 0 in the TGUI 96xx, but the TGUI 9440 lacks clipping*/
                if ((tgui->type == TGUI_9440) || ((tgui->type >= TGUI_9660) && ((tgui->accel.dx & 0xfff) >= tgui->accel.left) && ((tgui->accel.dx & 0xfff) <= tgui->accel.right) && ((tgui->accel.dy & 0xfff) >= tgui->accel.top) && ((tgui->accel.dy & 0xfff) <= tgui->accel.bottom))) {
                    READ(tgui->accel.dx + (tgui->accel.dy * tgui->accel.pitch), dst_dat);

                    pat_dat = tgui->accel.fg_col;

                    MIX();

                    WRITE(tgui->accel.dx + (tgui->accel.dy * tgui->accel.pitch), out);
                }

                if (!tgui->accel.y)
                    break;

                if (tgui->accel.size_x >= 0) {
                    tgui->accel.size_x += tgui->accel.src_x;
                    /*Step minor axis*/
                    switch ((tgui->accel.flags >> 8) & 7) {
                        case 0:
                        case 2:
                            tgui->accel.dy++;
                            break;
                        case 1:
                        case 3:
                            tgui->accel.dy--;
                            break;
                        case 4:
                        case 5:
                            tgui->accel.dx++;
                            break;
                        case 6:
                        case 7:
                            tgui->accel.dx--;
                            break;

                        default:
                            break;
                        }
                } else
                    tgui->accel.size_x += tgui->accel.src_y;

                /*Step major axis*/
                switch ((tgui->accel.flags >> 8) & 7) {
                    case 0:
                    case 1:
                        tgui->accel.dx++;
                        break;
                    case 2:
                    case 3:
                        tgui->accel.dx--;
                        break;
                    case 4:
                    case 6:
                        tgui->accel.dy++;
                        break;
                    case 5:
                    case 7:
                        tgui->accel.dy--;
                        break;

                    default:
                        break;
                }

                tgui->accel.y--;
                tgui->accel.dx &= 0xfff;
                tgui->accel.dy &= 0xfff;
            }
            break;

        case TGUI_SHORTVECTOR:
            if (count == -1) {
                tgui->accel.dx = tgui->accel.dst_x & 0xfff;
                tgui->accel.dy = tgui->accel.dst_y & 0xfff;
                tgui->accel.y = tgui->accel.sv_size_y & 0xfff;

                tgui->accel.left   = tgui->accel.src_x_clip & 0xfff;
                tgui->accel.right  = tgui->accel.dst_x_clip & 0xfff;
                tgui->accel.top    = tgui->accel.src_y_clip & 0xfff;
                tgui->accel.bottom = tgui->accel.dst_y_clip & 0xfff;

                if (tgui->accel.bpp == 1) {
                    tgui->accel.left >>= 1;
                    tgui->accel.right >>= 1;
                } else if (tgui->accel.bpp == 3) {
                    tgui->accel.left >>= 2;
                    tgui->accel.right >>= 2;
                }
            }

            while (count--) {
                /*Note by TC1995: I suppose the x/y clipping max is always more than 0 in the TGUI 96xx, but the TGUI 9440 lacks clipping*/
                if ((tgui->type == TGUI_9440) || ((tgui->type >= TGUI_9660) && ((tgui->accel.dx & 0xfff) >= tgui->accel.left) && ((tgui->accel.dx & 0xfff) <= tgui->accel.right) && ((tgui->accel.dy & 0xfff) >= tgui->accel.top) && ((tgui->accel.dy & 0xfff) <= tgui->accel.bottom))) {
                    READ(tgui->accel.dx + (tgui->accel.dy * tgui->accel.pitch), dst_dat);

                    pat_dat = tgui->accel.fg_col;

                    MIX();

                    WRITE(tgui->accel.dx + (tgui->accel.dy * tgui->accel.pitch), out);
                }

                if (!tgui->accel.y)
                    break;

                switch ((tgui->accel.sv_size_y >> 8) & 0xe0) {
                    case 0x00:
                        tgui->accel.dx++;
                        break;
                    case 0x20:
                        tgui->accel.dx++;
                        tgui->accel.dy--;
                        break;
                    case 0x40:
                        tgui->accel.dy--;
                        break;
                    case 0x60:
                        tgui->accel.dx--;
                        tgui->accel.dy--;
                        break;
                    case 0x80:
                        tgui->accel.dx--;
                        break;
                    case 0xa0:
                        tgui->accel.dx--;
                        tgui->accel.dy++;
                        break;
                    case 0xc0:
                        tgui->accel.dy++;
                        break;
                    case 0xe0:
                        tgui->accel.dx++;
                        tgui->accel.dy++;
                        break;

                    default:
                        break;
                }

                tgui->accel.y--;
                tgui->accel.dx &= 0xfff;
                tgui->accel.dy &= 0xfff;
            }
            break;

        case TGUI_FASTLINE:
            if (tgui->type < TGUI_9660)
                break;

            if (count == -1) {
                tgui->accel.dx = tgui->accel.dst_x & 0xfff;
                tgui->accel.dy = tgui->accel.dst_y & 0xfff;
                tgui->accel.y = tgui->accel.size_y;

                tgui->accel.left   = tgui->accel.src_x_clip & 0xfff;
                tgui->accel.right  = tgui->accel.dst_x_clip & 0xfff;
                tgui->accel.top    = tgui->accel.src_y_clip & 0xfff;
                tgui->accel.bottom = tgui->accel.dst_y_clip & 0xfff;

                if (tgui->accel.bpp == 1) {
                    tgui->accel.left >>= 1;
                    tgui->accel.right >>= 1;
                } else if (tgui->accel.bpp == 3) {
                    tgui->accel.left >>= 2;
                    tgui->accel.right >>= 2;
                }
            }

            while (count--) {
                /*Note by TC1995: I suppose the x/y clipping max is always more than 0 in the TGUI 96xx, but the TGUI 9440 lacks clipping*/
                if ((tgui->type == TGUI_9440) || ((tgui->type >= TGUI_9660) && ((tgui->accel.dx & 0xfff) >= tgui->accel.left) && ((tgui->accel.dx & 0xfff) <= tgui->accel.right) && ((tgui->accel.dy & 0xfff) >= tgui->accel.top) && ((tgui->accel.dy & 0xfff) <= tgui->accel.bottom))) {
                    READ(tgui->accel.dx + (tgui->accel.dy * tgui->accel.pitch), dst_dat);

                    pat_dat = tgui->accel.fg_col;

                    MIX();

                    WRITE(tgui->accel.dx + (tgui->accel.dy * tgui->accel.pitch), out);
                }

                if (!tgui->accel.y)
                    break;

                switch ((tgui->accel.size_y >> 8) & 0xe0) {
                    case 0x00:
                        tgui->accel.dx++;
                        break;
                    case 0x20:
                        tgui->accel.dx++;
                        tgui->accel.dy--;
                        break;
                    case 0x40:
                        tgui->accel.dy--;
                        break;
                    case 0x60:
                        tgui->accel.dx--;
                        tgui->accel.dy--;
                        break;
                    case 0x80:
                        tgui->accel.dx--;
                        break;
                    case 0xa0:
                        tgui->accel.dx--;
                        tgui->accel.dy++;
                        break;
                    case 0xc0:
                        tgui->accel.dy++;
                        break;
                    case 0xe0:
                        tgui->accel.dx++;
                        tgui->accel.dy++;
                        break;

                    default:
                        break;
                }

                tgui->accel.y--;
                tgui->accel.dx &= 0xfff;
                tgui->accel.dy &= 0xfff;
            }
            break;

        default:
            break;
    }
}

static void
tgui_accel_out(uint16_t addr, uint8_t val, void *priv)
{
    tgui_t *tgui = (tgui_t *) priv;
    svga_t *svga = &tgui->svga;

    switch (addr) {
        case 0x2122:
            tgui->accel.ger22 = (tgui->accel.ger22 & 0xff00) | val;
            switch (svga->bpp) {
                case 8:
                case 24:
                    tgui->accel.bpp = 0;
                    break;
                case 15:
                case 16:
                    tgui->accel.bpp = 1;
                    break;
                case 32:
                    tgui->accel.bpp = 3;
                    break;

                default:
                    break;
            }
            break;

        case 0x2123:
            tgui->accel.ger22 = (tgui->accel.ger22 & 0xff) | (val << 8);
#if 0
            pclog("Pitch IO23: val = %02x, rowoffset = %x.\n", tgui->accel.ger22, svga->crtc[0x13]);
#endif
            switch (svga->bpp) {
                case 8:
                case 24:
                    tgui->accel.bpp = 0;
                    break;
                case 15:
                case 16:
                    tgui->accel.bpp = 1;
                    break;
                case 32:
                    tgui->accel.bpp = 3;
                    break;
            }
            break;

        case 0x2124: /*Command*/
            tgui->accel.command = val;
            tgui_accel_command(-1, 0, tgui);
            break;

        case 0x2127: /*ROP*/
            tgui->accel.rop     = val;
            tgui->accel.use_src = (val & 0x33) ^ ((val >> 2) & 0x33);
            break;

        case 0x2128: /*Flags*/
            tgui->accel.flags = (tgui->accel.flags & 0xffffff00) | val;
            break;
        case 0x2129: /*Flags*/
            tgui->accel.flags = (tgui->accel.flags & 0xffff00ff) | (val << 8);
            break;
        case 0x212a: /*Flags*/
            tgui->accel.flags = (tgui->accel.flags & 0xff00ffff) | (val << 16);
            break;
        case 0x212b: /*Flags*/
            tgui->accel.flags = (tgui->accel.flags & 0x0000ffff) | (val << 24);
            break;

        case 0x212c: /*Foreground colour*/
        case 0x2178:
            tgui->accel.fg_col = (tgui->accel.fg_col & 0xffffff00) | val;
            break;
        case 0x212d: /*Foreground colour*/
        case 0x2179:
            tgui->accel.fg_col = (tgui->accel.fg_col & 0xffff00ff) | (val << 8);
            break;
        case 0x212e: /*Foreground colour*/
        case 0x217a:
            tgui->accel.fg_col = (tgui->accel.fg_col & 0xff00ffff) | (val << 16);
            break;
        case 0x212f: /*Foreground colour*/
        case 0x217b:
            tgui->accel.fg_col = (tgui->accel.fg_col & 0x00ffffff) | (val << 24);
            break;

        case 0x2130: /*Background colour*/
        case 0x217c:
            tgui->accel.bg_col = (tgui->accel.bg_col & 0xffffff00) | val;
            break;
        case 0x2131: /*Background colour*/
        case 0x217d:
            tgui->accel.bg_col = (tgui->accel.bg_col & 0xffff00ff) | (val << 8);
            break;
        case 0x2132: /*Background colour*/
        case 0x217e:
            tgui->accel.bg_col = (tgui->accel.bg_col & 0xff00ffff) | (val << 16);
            break;
        case 0x2133: /*Background colour*/
        case 0x217f:
            tgui->accel.bg_col = (tgui->accel.bg_col & 0x00ffffff) | (val << 24);
            break;

        case 0x2134: /*Pattern location*/
            tgui->accel.patloc = (tgui->accel.patloc & 0xff00) | val;
            break;
        case 0x2135: /*Pattern location*/
            tgui->accel.patloc = (tgui->accel.patloc & 0xff) | (val << 8);
            break;

        case 0x2138: /*Dest X*/
            tgui->accel.dst_x = (tgui->accel.dst_x & 0xff00) | val;
            break;
        case 0x2139: /*Dest X*/
            tgui->accel.dst_x = (tgui->accel.dst_x & 0xff) | (val << 8);
            break;
        case 0x213a: /*Dest Y*/
            tgui->accel.dst_y = (tgui->accel.dst_y & 0xff00) | val;
            break;
        case 0x213b: /*Dest Y*/
            tgui->accel.dst_y = (tgui->accel.dst_y & 0xff) | (val << 8);
            break;

        case 0x213c: /*Src X, Diagonal Step Constant*/
            tgui->accel.src_x = (tgui->accel.src_x & 0x3f00) | val;
            break;
        case 0x213d: /*Src X, Diagonal Step Constant*/
            tgui->accel.src_x = (tgui->accel.src_x & 0xff) | ((val & 0x3f) << 8);
            if (val & 0x20)
                tgui->accel.src_x |= ~0x3fff;
            break;
        case 0x213e: /*Src Y, Axial Step Constant*/
            tgui->accel.src_y = (tgui->accel.src_y & 0x3f00) | val;
            break;
        case 0x213f: /*Src Y, Axial Step Constant*/
            tgui->accel.src_y = (tgui->accel.src_y & 0xff) | ((val & 0x3f) << 8);
            if (val & 0x20)
                tgui->accel.src_y |= ~0x3fff;
            break;

        case 0x2140: /*Size X, Line Error Term*/
            tgui->accel.size_x = (tgui->accel.size_x & 0x3f00) | val;
            break;
        case 0x2141: /*Size X, Line Error Term*/
            tgui->accel.size_x = (tgui->accel.size_x & 0xff) | ((val & 0x3f) << 8);
            if (val & 0x20)
                tgui->accel.size_x |= ~0x1fff;
            break;
        case 0x2142: /*Size Y, Major Axis Pixel Count*/
            tgui->accel.size_y    = (tgui->accel.size_y & 0xf00) | val;
            tgui->accel.sv_size_y = (tgui->accel.sv_size_y & 0xff00) | val;
            break;
        case 0x2143: /*Size Y, Major Axis Pixel Count*/
            tgui->accel.size_y    = (tgui->accel.size_y & 0xff) | ((val & 0x0f) << 8);
            tgui->accel.sv_size_y = (tgui->accel.sv_size_y & 0xff) | (val << 8);
            break;

        case 0x2144: /*Style*/
            tgui->accel.style = (tgui->accel.style & 0xffffff00) | val;
            break;
        case 0x2145: /*Style*/
            tgui->accel.style = (tgui->accel.style & 0xffff00ff) | (val << 8);
            break;
        case 0x2146: /*Style*/
            tgui->accel.style = (tgui->accel.style & 0xff00ffff) | (val << 16);
            break;
        case 0x2147: /*Style*/
            tgui->accel.style = (tgui->accel.style & 0x00ffffff) | (val << 24);
            break;

        case 0x2148: /*Clip Src X*/
            tgui->accel.src_x_clip = (tgui->accel.src_x_clip & 0xff00) | val;
            break;
        case 0x2149: /*Clip Src X*/
            tgui->accel.src_x_clip = (tgui->accel.src_x_clip & 0xff) | (val << 8);
            break;
        case 0x214a: /*Clip Src Y*/
            tgui->accel.src_y_clip = (tgui->accel.src_y_clip & 0xff00) | val;
            break;
        case 0x214b: /*Clip Src Y*/
            tgui->accel.src_y_clip = (tgui->accel.src_y_clip & 0xff) | (val << 8);
            break;

        case 0x214c: /*Clip Dest X*/
            tgui->accel.dst_x_clip = (tgui->accel.dst_x_clip & 0xff00) | val;
            break;
        case 0x214d: /*Clip Dest X*/
            tgui->accel.dst_x_clip = (tgui->accel.dst_x_clip & 0xff) | (val << 8);
            break;
        case 0x214e: /*Clip Dest Y*/
            tgui->accel.dst_y_clip = (tgui->accel.dst_y_clip & 0xff00) | val;
            break;
        case 0x214f: /*Clip Dest Y*/
            tgui->accel.dst_y_clip = (tgui->accel.dst_y_clip & 0xff) | (val << 8);
            break;

        case 0x2168: /*CKey*/
            tgui->accel.ckey = (tgui->accel.ckey & 0xffffff00) | val;
            break;
        case 0x2169: /*CKey*/
            tgui->accel.ckey = (tgui->accel.ckey & 0xffff00ff) | (val << 8);
            break;
        case 0x216a: /*CKey*/
            tgui->accel.ckey = (tgui->accel.ckey & 0xff00ffff) | (val << 16);
            break;
        case 0x216b: /*CKey*/
            tgui->accel.ckey = (tgui->accel.ckey & 0x00ffffff) | (val << 24);
            break;

        case 0x2180:
        case 0x2181:
        case 0x2182:
        case 0x2183:
        case 0x2184:
        case 0x2185:
        case 0x2186:
        case 0x2187:
        case 0x2188:
        case 0x2189:
        case 0x218a:
        case 0x218b:
        case 0x218c:
        case 0x218d:
        case 0x218e:
        case 0x218f:
        case 0x2190:
        case 0x2191:
        case 0x2192:
        case 0x2193:
        case 0x2194:
        case 0x2195:
        case 0x2196:
        case 0x2197:
        case 0x2198:
        case 0x2199:
        case 0x219a:
        case 0x219b:
        case 0x219c:
        case 0x219d:
        case 0x219e:
        case 0x219f:
        case 0x21a0:
        case 0x21a1:
        case 0x21a2:
        case 0x21a3:
        case 0x21a4:
        case 0x21a5:
        case 0x21a6:
        case 0x21a7:
        case 0x21a8:
        case 0x21a9:
        case 0x21aa:
        case 0x21ab:
        case 0x21ac:
        case 0x21ad:
        case 0x21ae:
        case 0x21af:
        case 0x21b0:
        case 0x21b1:
        case 0x21b2:
        case 0x21b3:
        case 0x21b4:
        case 0x21b5:
        case 0x21b6:
        case 0x21b7:
        case 0x21b8:
        case 0x21b9:
        case 0x21ba:
        case 0x21bb:
        case 0x21bc:
        case 0x21bd:
        case 0x21be:
        case 0x21bf:
        case 0x21c0:
        case 0x21c1:
        case 0x21c2:
        case 0x21c3:
        case 0x21c4:
        case 0x21c5:
        case 0x21c6:
        case 0x21c7:
        case 0x21c8:
        case 0x21c9:
        case 0x21ca:
        case 0x21cb:
        case 0x21cc:
        case 0x21cd:
        case 0x21ce:
        case 0x21cf:
        case 0x21d0:
        case 0x21d1:
        case 0x21d2:
        case 0x21d3:
        case 0x21d4:
        case 0x21d5:
        case 0x21d6:
        case 0x21d7:
        case 0x21d8:
        case 0x21d9:
        case 0x21da:
        case 0x21db:
        case 0x21dc:
        case 0x21dd:
        case 0x21de:
        case 0x21df:
        case 0x21e0:
        case 0x21e1:
        case 0x21e2:
        case 0x21e3:
        case 0x21e4:
        case 0x21e5:
        case 0x21e6:
        case 0x21e7:
        case 0x21e8:
        case 0x21e9:
        case 0x21ea:
        case 0x21eb:
        case 0x21ec:
        case 0x21ed:
        case 0x21ee:
        case 0x21ef:
        case 0x21f0:
        case 0x21f1:
        case 0x21f2:
        case 0x21f3:
        case 0x21f4:
        case 0x21f5:
        case 0x21f6:
        case 0x21f7:
        case 0x21f8:
        case 0x21f9:
        case 0x21fa:
        case 0x21fb:
        case 0x21fc:
        case 0x21fd:
        case 0x21fe:
        case 0x21ff:
            tgui->accel.pattern[addr & 0x7f] = val;
            tgui->accel.pattern_32bpp[tgui->accel.pattern_32_idx] = val;
            tgui->accel.pattern_32_idx = (tgui->accel.pattern_32_idx + 1) & 0xff;
            break;

        default:
            break;
    }
}

static void
tgui_accel_out_w(uint16_t addr, uint16_t val, void *priv)
{
    tgui_t *tgui = (tgui_t *) priv;
    tgui_accel_out(addr, val, tgui);
    tgui_accel_out(addr + 1, val >> 8, tgui);
}

static void
tgui_accel_out_l(uint16_t addr, uint32_t val, void *priv)
{
    tgui_t *tgui = (tgui_t *) priv;

    switch (addr) {
        case 0x2124: /*Long version of Command and ROP together*/
            tgui->accel.command = val & 0xff;
            tgui->accel.rop     = val >> 24;
            tgui->accel.use_src = (tgui->accel.rop & 0x33) ^ ((tgui->accel.rop >> 2) & 0x33);
            tgui_accel_command(-1, 0, tgui);
            break;

        default:
            tgui_accel_out(addr, val, tgui);
            tgui_accel_out(addr + 1, val >> 8, tgui);
            tgui_accel_out(addr + 2, val >> 16, tgui);
            tgui_accel_out(addr + 3, val >> 24, tgui);
            break;
    }
}

static uint8_t
tgui_accel_in(uint16_t addr, void *priv)
{
    const tgui_t *tgui = (tgui_t *) priv;

    switch (addr) {
        case 0x2120: /*Status*/
            return 0;

        case 0x2122:
            return tgui->accel.ger22 & 0xff;
        case 0x2123:
            return tgui->accel.ger22 >> 8;

        case 0x2127: /*ROP*/
            return tgui->accel.rop;

        case 0x2128: /*Flags*/
            return tgui->accel.flags & 0xff;
        case 0x2129: /*Flags*/
            return tgui->accel.flags >> 8;
        case 0x212a: /*Flags*/
            return tgui->accel.flags >> 16;
        case 0x212b:
            return tgui->accel.flags >> 24;

        case 0x212c: /*Foreground colour*/
        case 0x2178:
            return tgui->accel.fg_col & 0xff;
        case 0x212d: /*Foreground colour*/
        case 0x2179:
            return tgui->accel.fg_col >> 8;
        case 0x212e: /*Foreground colour*/
        case 0x217a:
            return tgui->accel.fg_col >> 16;
        case 0x212f: /*Foreground colour*/
        case 0x217b:
            return tgui->accel.fg_col >> 24;

        case 0x2130: /*Background colour*/
        case 0x217c:
            return tgui->accel.bg_col & 0xff;
        case 0x2131: /*Background colour*/
        case 0x217d:
            return tgui->accel.bg_col >> 8;
        case 0x2132: /*Background colour*/
        case 0x217e:
            return tgui->accel.bg_col >> 16;
        case 0x2133: /*Background colour*/
        case 0x217f:
            return tgui->accel.bg_col >> 24;

        case 0x2134: /*Pattern location*/
            return tgui->accel.patloc & 0xff;
        case 0x2135: /*Pattern location*/
            return tgui->accel.patloc >> 8;

        case 0x2138: /*Dest X*/
            return tgui->accel.dst_x & 0xff;
        case 0x2139: /*Dest X*/
            return tgui->accel.dst_x >> 8;
        case 0x213a: /*Dest Y*/
            return tgui->accel.dst_y & 0xff;
        case 0x213b: /*Dest Y*/
            return tgui->accel.dst_y >> 8;

        case 0x213c: /*Src X*/
            return tgui->accel.src_x & 0xff;
        case 0x213d: /*Src X*/
            return tgui->accel.src_x >> 8;
        case 0x213e: /*Src Y*/
            return tgui->accel.src_y & 0xff;
        case 0x213f: /*Src Y*/
            return tgui->accel.src_y >> 8;

        case 0x2140: /*Size X*/
            return tgui->accel.size_x & 0xff;
        case 0x2141: /*Size X*/
            return tgui->accel.size_x >> 8;
        case 0x2142: /*Size Y*/
            return tgui->accel.size_y & 0xff;
        case 0x2143: /*Size Y*/
            return tgui->accel.size_y >> 8;

        case 0x2144: /*Style*/
            return tgui->accel.style & 0xff;
        case 0x2145: /*Style*/
            return tgui->accel.style >> 8;
        case 0x2146: /*Style*/
            return tgui->accel.style >> 16;
        case 0x2147: /*Style*/
            return tgui->accel.style >> 24;

        case 0x2148: /*Clip Src X*/
            return tgui->accel.src_x_clip & 0xff;
        case 0x2149: /*Clip Src X*/
            return tgui->accel.src_x_clip >> 8;
        case 0x214a: /*Clip Src Y*/
            return tgui->accel.src_y_clip & 0xff;
        case 0x214b: /*Clip Src Y*/
            return tgui->accel.src_y_clip >> 8;

        case 0x214c: /*Clip Dest X*/
            return tgui->accel.dst_x_clip & 0xff;
        case 0x214d: /*Clip Dest X*/
            return tgui->accel.dst_x_clip >> 8;
        case 0x214e: /*Clip Dest Y*/
            return tgui->accel.dst_y_clip & 0xff;
        case 0x214f: /*Clip Dest Y*/
            return tgui->accel.dst_y_clip >> 8;

        case 0x2168: /*CKey*/
            return tgui->accel.ckey & 0xff;
        case 0x2169: /*CKey*/
            return tgui->accel.ckey >> 8;
        case 0x216a: /*CKey*/
            return tgui->accel.ckey >> 16;
        case 0x216b: /*CKey*/
            return tgui->accel.ckey >> 24;

        case 0x2180:
        case 0x2181:
        case 0x2182:
        case 0x2183:
        case 0x2184:
        case 0x2185:
        case 0x2186:
        case 0x2187:
        case 0x2188:
        case 0x2189:
        case 0x218a:
        case 0x218b:
        case 0x218c:
        case 0x218d:
        case 0x218e:
        case 0x218f:
        case 0x2190:
        case 0x2191:
        case 0x2192:
        case 0x2193:
        case 0x2194:
        case 0x2195:
        case 0x2196:
        case 0x2197:
        case 0x2198:
        case 0x2199:
        case 0x219a:
        case 0x219b:
        case 0x219c:
        case 0x219d:
        case 0x219e:
        case 0x219f:
        case 0x21a0:
        case 0x21a1:
        case 0x21a2:
        case 0x21a3:
        case 0x21a4:
        case 0x21a5:
        case 0x21a6:
        case 0x21a7:
        case 0x21a8:
        case 0x21a9:
        case 0x21aa:
        case 0x21ab:
        case 0x21ac:
        case 0x21ad:
        case 0x21ae:
        case 0x21af:
        case 0x21b0:
        case 0x21b1:
        case 0x21b2:
        case 0x21b3:
        case 0x21b4:
        case 0x21b5:
        case 0x21b6:
        case 0x21b7:
        case 0x21b8:
        case 0x21b9:
        case 0x21ba:
        case 0x21bb:
        case 0x21bc:
        case 0x21bd:
        case 0x21be:
        case 0x21bf:
        case 0x21c0:
        case 0x21c1:
        case 0x21c2:
        case 0x21c3:
        case 0x21c4:
        case 0x21c5:
        case 0x21c6:
        case 0x21c7:
        case 0x21c8:
        case 0x21c9:
        case 0x21ca:
        case 0x21cb:
        case 0x21cc:
        case 0x21cd:
        case 0x21ce:
        case 0x21cf:
        case 0x21d0:
        case 0x21d1:
        case 0x21d2:
        case 0x21d3:
        case 0x21d4:
        case 0x21d5:
        case 0x21d6:
        case 0x21d7:
        case 0x21d8:
        case 0x21d9:
        case 0x21da:
        case 0x21db:
        case 0x21dc:
        case 0x21dd:
        case 0x21de:
        case 0x21df:
        case 0x21e0:
        case 0x21e1:
        case 0x21e2:
        case 0x21e3:
        case 0x21e4:
        case 0x21e5:
        case 0x21e6:
        case 0x21e7:
        case 0x21e8:
        case 0x21e9:
        case 0x21ea:
        case 0x21eb:
        case 0x21ec:
        case 0x21ed:
        case 0x21ee:
        case 0x21ef:
        case 0x21f0:
        case 0x21f1:
        case 0x21f2:
        case 0x21f3:
        case 0x21f4:
        case 0x21f5:
        case 0x21f6:
        case 0x21f7:
        case 0x21f8:
        case 0x21f9:
        case 0x21fa:
        case 0x21fb:
        case 0x21fc:
        case 0x21fd:
        case 0x21fe:
        case 0x21ff:
            return tgui->accel.pattern[addr & 0x7f];

        default:
            break;
    }
    return 0;
}

static uint16_t
tgui_accel_in_w(uint16_t addr, void *priv)
{
    tgui_t *tgui = (tgui_t *) priv;
    return tgui_accel_in(addr, tgui) | (tgui_accel_in(addr + 1, tgui) << 8);
}

static uint32_t
tgui_accel_in_l(uint16_t addr, void *priv)
{
    tgui_t *tgui = (tgui_t *) priv;
    return tgui_accel_in_w(addr, tgui) | (tgui_accel_in_w(addr + 2, tgui) << 16);
}

static void
tgui_accel_write(uint32_t addr, uint8_t val, void *priv)
{
    tgui_t *tgui = (tgui_t *) priv;
    svga_t *svga = &tgui->svga;

    if ((svga->crtc[0x36] & 0x03) == 0x02) {
        if ((addr & ~0xff) != 0xbff00)
            return;
    } else if ((svga->crtc[0x36] & 0x03) == 0x01) {
        if ((addr & ~0xff) != 0xb7f00)
            return;
    }

    tgui_accel_out((addr & 0xff) + 0x2100, val, tgui);
}

static void
tgui_accel_write_w(uint32_t addr, uint16_t val, void *priv)
{
    tgui_t *tgui = (tgui_t *) priv;

    tgui_accel_write(addr, val, tgui);
    tgui_accel_write(addr + 1, val >> 8, tgui);
}

static void
tgui_accel_write_l(uint32_t addr, uint32_t val, void *priv)
{
    tgui_t       *tgui = (tgui_t *) priv;
    const svga_t *svga = &tgui->svga;

    switch (addr & 0xff) {
        case 0x24: /*Long version of Command and ROP together*/
            if ((svga->crtc[0x36] & 0x03) == 0x02) {
                if ((addr & ~0xff) != 0xbff00)
                    return;
            } else if ((svga->crtc[0x36] & 0x03) == 0x01) {
                if ((addr & ~0xff) != 0xb7f00)
                    return;
            }
            tgui->accel.command = val & 0xff;
            tgui->accel.rop     = val >> 24;
            tgui->accel.use_src = ((val >> 24) & 0x33) ^ (((val >> 24) >> 2) & 0x33);
            tgui_accel_command(-1, 0, tgui);
            break;

        default:
            tgui_accel_write_w(addr, val, tgui);
            tgui_accel_write_w(addr + 2, val >> 16, tgui);
            break;
    }
}

static uint8_t
tgui_accel_read(uint32_t addr, void *priv)
{
    const tgui_t *tgui = (tgui_t *) priv;
    const svga_t *svga = &tgui->svga;

    if ((svga->crtc[0x36] & 0x03) == 0x02) {
        if ((addr & ~0xff) != 0xbff00)
            return 0xff;
    } else if ((svga->crtc[0x36] & 0x03) == 0x01) {
        if ((addr & ~0xff) != 0xb7f00)
            return 0xff;
    }

    switch (addr & 0xff) {
        case 0x20: /*Status*/
            return 0;

        case 0x22:
            return tgui->accel.ger22 & 0xff;
        case 0x23:
            return tgui->accel.ger22 >> 8;

        case 0x27: /*ROP*/
            return tgui->accel.rop;

        case 0x28: /*Flags*/
            return tgui->accel.flags & 0xff;
        case 0x29: /*Flags*/
            return tgui->accel.flags >> 8;
        case 0x2a: /*Flags*/
            return tgui->accel.flags >> 16;
        case 0x2b:
            return tgui->accel.flags >> 24;

        case 0x2c: /*Foreground colour*/
        case 0x78:
            return tgui->accel.fg_col & 0xff;
        case 0x2d: /*Foreground colour*/
        case 0x79:
            return tgui->accel.fg_col >> 8;
        case 0x2e: /*Foreground colour*/
        case 0x7a:
            return tgui->accel.fg_col >> 16;
        case 0x2f: /*Foreground colour*/
        case 0x7b:
            return tgui->accel.fg_col >> 24;

        case 0x30: /*Background colour*/
        case 0x7c:
            return tgui->accel.bg_col & 0xff;
        case 0x31: /*Background colour*/
        case 0x7d:
            return tgui->accel.bg_col >> 8;
        case 0x32: /*Background colour*/
        case 0x7e:
            return tgui->accel.bg_col >> 16;
        case 0x33: /*Background colour*/
        case 0x7f:
            return tgui->accel.bg_col >> 24;

        case 0x34: /*Pattern location*/
            return tgui->accel.patloc & 0xff;
        case 0x35: /*Pattern location*/
            return tgui->accel.patloc >> 8;

        case 0x38: /*Dest X*/
            return tgui->accel.dst_x & 0xff;
        case 0x39: /*Dest X*/
            return tgui->accel.dst_x >> 8;
        case 0x3a: /*Dest Y*/
            return tgui->accel.dst_y & 0xff;
        case 0x3b: /*Dest Y*/
            return tgui->accel.dst_y >> 8;

        case 0x3c: /*Src X*/
            return tgui->accel.src_x & 0xff;
        case 0x3d: /*Src X*/
            return tgui->accel.src_x >> 8;
        case 0x3e: /*Src Y*/
            return tgui->accel.src_y & 0xff;
        case 0x3f: /*Src Y*/
            return tgui->accel.src_y >> 8;

        case 0x40: /*Size X*/
            return tgui->accel.size_x & 0xff;
        case 0x41: /*Size X*/
            return tgui->accel.size_x >> 8;
        case 0x42: /*Size Y*/
            return tgui->accel.size_y & 0xff;
        case 0x43: /*Size Y*/
            return tgui->accel.size_y >> 8;

        case 0x44: /*Style*/
            return tgui->accel.style & 0xff;
        case 0x45: /*Style*/
            return tgui->accel.style >> 8;
        case 0x46: /*Style*/
            return tgui->accel.style >> 16;
        case 0x47: /*Style*/
            return tgui->accel.style >> 24;

        case 0x48: /*Clip Src X*/
            return tgui->accel.src_x_clip & 0xff;
        case 0x49: /*Clip Src X*/
            return tgui->accel.src_x_clip >> 8;
        case 0x4a: /*Clip Src Y*/
            return tgui->accel.src_y_clip & 0xff;
        case 0x4b: /*Clip Src Y*/
            return tgui->accel.src_y_clip >> 8;

        case 0x4c: /*Clip Dest X*/
            return tgui->accel.dst_x_clip & 0xff;
        case 0x4d: /*Clip Dest X*/
            return tgui->accel.dst_x_clip >> 8;
        case 0x4e: /*Clip Dest Y*/
            return tgui->accel.dst_y_clip & 0xff;
        case 0x4f: /*Clip Dest Y*/
            return tgui->accel.dst_y_clip >> 8;

        case 0x68: /*CKey*/
            return tgui->accel.ckey & 0xff;
        case 0x69: /*CKey*/
            return tgui->accel.ckey >> 8;
        case 0x6a: /*CKey*/
            return tgui->accel.ckey >> 16;
        case 0x6b: /*CKey*/
            return tgui->accel.ckey >> 24;

        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x84:
        case 0x85:
        case 0x86:
        case 0x87:
        case 0x88:
        case 0x89:
        case 0x8a:
        case 0x8b:
        case 0x8c:
        case 0x8d:
        case 0x8e:
        case 0x8f:
        case 0x90:
        case 0x91:
        case 0x92:
        case 0x93:
        case 0x94:
        case 0x95:
        case 0x96:
        case 0x97:
        case 0x98:
        case 0x99:
        case 0x9a:
        case 0x9b:
        case 0x9c:
        case 0x9d:
        case 0x9e:
        case 0x9f:
        case 0xa0:
        case 0xa1:
        case 0xa2:
        case 0xa3:
        case 0xa4:
        case 0xa5:
        case 0xa6:
        case 0xa7:
        case 0xa8:
        case 0xa9:
        case 0xaa:
        case 0xab:
        case 0xac:
        case 0xad:
        case 0xae:
        case 0xaf:
        case 0xb0:
        case 0xb1:
        case 0xb2:
        case 0xb3:
        case 0xb4:
        case 0xb5:
        case 0xb6:
        case 0xb7:
        case 0xb8:
        case 0xb9:
        case 0xba:
        case 0xbb:
        case 0xbc:
        case 0xbd:
        case 0xbe:
        case 0xbf:
        case 0xc0:
        case 0xc1:
        case 0xc2:
        case 0xc3:
        case 0xc4:
        case 0xc5:
        case 0xc6:
        case 0xc7:
        case 0xc8:
        case 0xc9:
        case 0xca:
        case 0xcb:
        case 0xcc:
        case 0xcd:
        case 0xce:
        case 0xcf:
        case 0xd0:
        case 0xd1:
        case 0xd2:
        case 0xd3:
        case 0xd4:
        case 0xd5:
        case 0xd6:
        case 0xd7:
        case 0xd8:
        case 0xd9:
        case 0xda:
        case 0xdb:
        case 0xdc:
        case 0xdd:
        case 0xde:
        case 0xdf:
        case 0xe0:
        case 0xe1:
        case 0xe2:
        case 0xe3:
        case 0xe4:
        case 0xe5:
        case 0xe6:
        case 0xe7:
        case 0xe8:
        case 0xe9:
        case 0xea:
        case 0xeb:
        case 0xec:
        case 0xed:
        case 0xee:
        case 0xef:
        case 0xf0:
        case 0xf1:
        case 0xf2:
        case 0xf3:
        case 0xf4:
        case 0xf5:
        case 0xf6:
        case 0xf7:
        case 0xf8:
        case 0xf9:
        case 0xfa:
        case 0xfb:
        case 0xfc:
        case 0xfd:
        case 0xfe:
        case 0xff:
            return tgui->accel.pattern[addr & 0x7f];

        default:
            break;
    }
    return 0xff;
}

static uint16_t
tgui_accel_read_w(uint32_t addr, void *priv)
{
    tgui_t *tgui = (tgui_t *) priv;

    return tgui_accel_read(addr, tgui) | (tgui_accel_read(addr + 1, tgui) << 8);
}

static uint32_t
tgui_accel_read_l(uint32_t addr, void *priv)
{
    tgui_t *tgui = (tgui_t *) priv;

    return tgui_accel_read_w(addr, tgui) | (tgui_accel_read_w(addr + 2, tgui) << 16);
}

static void
tgui_accel_write_fb_b(uint32_t addr, uint8_t val, void *priv)
{
    svga_t *svga = (svga_t *) priv;
    tgui_t *tgui = (tgui_t *) svga->priv;

    if (tgui->write_blitter) {
        tgui_accel_command(8, val << 24, tgui);
    } else
        svga_write_linear(addr, val, svga);
}

static void
tgui_accel_write_fb_w(uint32_t addr, uint16_t val, void *priv)
{
    svga_t *svga = (svga_t *) priv;
    tgui_t *tgui = (tgui_t *) svga->priv;

    if (tgui->write_blitter)
        tgui_accel_command(16, (((val & 0xff00) >> 8) | ((val & 0x00ff) << 8)) << 16, tgui);
    else
        svga_writew_linear(addr, val, svga);
}

static void
tgui_accel_write_fb_l(uint32_t addr, uint32_t val, void *priv)
{
    svga_t *svga = (svga_t *) priv;
    tgui_t *tgui = (tgui_t *) svga->priv;

    if (tgui->write_blitter)
        tgui_accel_command(32, ((val & 0xff000000) >> 24) | ((val & 0x00ff0000) >> 8) | ((val & 0x0000ff00) << 8) | ((val & 0x000000ff) << 24), tgui);
    else
        svga_writel_linear(addr, val, svga);
}

static void
tgui_mmio_write(uint32_t addr, uint8_t val, void *priv)
{
    const tgui_t *tgui = (tgui_t *) priv;
    const svga_t *svga = &tgui->svga;

    addr &= 0x0000ffff;

    if (((svga->crtc[0x36] & 0x03) == 0x00) && (addr >= 0x2100 && addr <= 0x21ff))
        tgui_accel_out(addr, val, priv);
    else if (((svga->crtc[0x36] & 0x03) > 0x00) && (addr <= 0xff))
        tgui_accel_write(addr, val, priv);
    else
        tgui_out(addr, val, priv);
}

static void
tgui_mmio_write_w(uint32_t addr, uint16_t val, void *priv)
{
    const tgui_t *tgui = (tgui_t *) priv;
    const svga_t *svga = &tgui->svga;

    addr &= 0x0000ffff;

    if (((svga->crtc[0x36] & 0x03) == 0x00) && (addr >= 0x2100 && addr <= 0x21ff))
        tgui_accel_out_w(addr, val, priv);
    else if (((svga->crtc[0x36] & 0x03) > 0x00) && (addr <= 0xff))
        tgui_accel_write_w(addr, val, priv);
    else {
        tgui_out(addr, val & 0xff, priv);
        tgui_out(addr + 1, val >> 8, priv);
    }
}

static void
tgui_mmio_write_l(uint32_t addr, uint32_t val, void *priv)
{
    const tgui_t *tgui = (tgui_t *) priv;
    const svga_t *svga = &tgui->svga;

    addr &= 0x0000ffff;

    if (((svga->crtc[0x36] & 0x03) == 0x00) && (addr >= 0x2100 && addr <= 0x21ff))
        tgui_accel_out_l(addr, val, priv);
    else if (((svga->crtc[0x36] & 0x03) > 0x00) && (addr <= 0xff))
        tgui_accel_write_l(addr, val, priv);
    else {
        tgui_out(addr, val & 0xff, priv);
        tgui_out(addr + 1, val >> 8, priv);
        tgui_out(addr + 2, val >> 16, priv);
        tgui_out(addr + 3, val >> 24, priv);
    }
}

static uint8_t
tgui_mmio_read(uint32_t addr, void *priv)
{
    const tgui_t *tgui = (tgui_t *) priv;
    const svga_t *svga = &tgui->svga;

    uint8_t ret = 0xff;

    addr &= 0x0000ffff;

    if (((svga->crtc[0x36] & 0x03) == 0x00) && (addr >= 0x2100 && addr <= 0x21ff))
        ret = tgui_accel_in(addr, priv);
    else if (((svga->crtc[0x36] & 0x03) > 0x00) && (addr <= 0xff))
        ret = tgui_accel_read(addr, priv);
    else
        ret = tgui_in(addr, priv);

    return ret;
}

static uint16_t
tgui_mmio_read_w(uint32_t addr, void *priv)
{
    const tgui_t *tgui = (tgui_t *) priv;
    const svga_t *svga = &tgui->svga;
    uint16_t      ret  = 0xffff;

    addr &= 0x0000ffff;

    if (((svga->crtc[0x36] & 0x03) == 0x00) && (addr >= 0x2100 && addr <= 0x21ff))
        ret = tgui_accel_in_w(addr, priv);
    else if (((svga->crtc[0x36] & 0x03) > 0x00) && (addr <= 0xff))
        ret = tgui_accel_read_w(addr, priv);
    else
        ret = tgui_in(addr, priv) | (tgui_in(addr + 1, priv) << 8);

    return ret;
}

static uint32_t
tgui_mmio_read_l(uint32_t addr, void *priv)
{
    const tgui_t *tgui = (tgui_t *) priv;
    const svga_t *svga = &tgui->svga;
    uint32_t      ret  = 0xffffffff;

    addr &= 0x0000ffff;

    if (((svga->crtc[0x36] & 0x03) == 0x00) && (addr >= 0x2100 && addr <= 0x21ff))
        ret = tgui_accel_in_l(addr, priv);
    else if (((svga->crtc[0x36] & 0x03) > 0x00) && (addr <= 0xff))
        ret = tgui_accel_read_l(addr, priv);
    else
        ret = tgui_in(addr, priv) | (tgui_in(addr + 1, priv) << 8) | (tgui_in(addr + 2, priv) << 16) | (tgui_in(addr + 3, priv) << 24);

    return ret;
}

static void *
tgui_init(const device_t *info)
{
    const char *bios_fn;

    tgui_t *tgui = malloc(sizeof(tgui_t));
    svga_t *svga = &tgui->svga;
    memset(tgui, 0, sizeof(tgui_t));

    tgui->vram_size = device_get_config_int("memory") << 20;
    tgui->vram_mask = tgui->vram_size - 1;

    tgui->type = info->local & 0xff;

    tgui->pci = !!(info->flags & DEVICE_PCI);

    switch (tgui->type) {
        case TGUI_9400CXI:
            bios_fn = ROM_TGUI_9400CXI;
            break;
        case TGUI_9440:
            if (tgui->pci)
                bios_fn = (info->local & ONBOARD) ? NULL : ROM_TGUI_9440_PCI;
            else
                bios_fn = ROM_TGUI_9440_VLB;
            break;
        case TGUI_9660:
        case TGUI_9680:
            bios_fn = (info->local & ONBOARD) ? NULL : ROM_TGUI_96xx;
            break;
        default:
            free(tgui);
            return NULL;
    }

    tgui->has_bios = (bios_fn != NULL);

    if (tgui->has_bios) {
        rom_init(&tgui->bios_rom, bios_fn, 0xc0000, 0x8000, 0x7fff, 0, MEM_MAPPING_EXTERNAL);
        if (tgui->pci)
            mem_mapping_disable(&tgui->bios_rom.mapping);
    }

    if (tgui->pci)
        video_inform(VIDEO_FLAG_TYPE_SPECIAL, &timing_tgui_pci);
    else
        video_inform(VIDEO_FLAG_TYPE_SPECIAL, &timing_tgui_vlb);

    svga_init(info, svga, tgui, tgui->vram_size,
              tgui_recalctimings,
              tgui_in, tgui_out,
              tgui_hwcursor_draw,
              NULL);

    if (tgui->type == TGUI_9400CXI)
        svga->ramdac = device_add(&tkd8001_ramdac_device);

    mem_mapping_add(&tgui->linear_mapping, 0, 0, svga_read_linear, svga_readw_linear, svga_readl_linear, tgui_accel_write_fb_b, tgui_accel_write_fb_w, tgui_accel_write_fb_l, NULL, MEM_MAPPING_EXTERNAL, svga);
    mem_mapping_add(&tgui->accel_mapping, 0, 0, tgui_accel_read, tgui_accel_read_w, tgui_accel_read_l, tgui_accel_write, tgui_accel_write_w, tgui_accel_write_l, NULL, MEM_MAPPING_EXTERNAL, tgui);
    if (tgui->type >= TGUI_9440)
        mem_mapping_add(&tgui->mmio_mapping, 0, 0, tgui_mmio_read, tgui_mmio_read_w, tgui_mmio_read_l, tgui_mmio_write, tgui_mmio_write_w, tgui_mmio_write_l, NULL, MEM_MAPPING_EXTERNAL, tgui);
    mem_mapping_disable(&tgui->accel_mapping);
    mem_mapping_disable(&tgui->mmio_mapping);

    if (tgui->vram_size == (2 << 20))
        svga->crtc[0x21] |= 0x10;

    tgui_set_io(tgui);

    if (tgui->pci && (tgui->type >= TGUI_9440)) {
        if (tgui->has_bios)
            pci_add_card(PCI_ADD_NORMAL, tgui_pci_read, tgui_pci_write, tgui, &tgui->pci_slot);
        else
            pci_add_card(PCI_ADD_VIDEO | PCI_ADD_STRICT, tgui_pci_read, tgui_pci_write, tgui, &tgui->pci_slot);
    }

    tgui->pci_regs[PCI_REG_COMMAND] = 0x83;

    if (tgui->has_bios) {
        tgui->pci_regs[0x30] = 0x00;
        tgui->pci_regs[0x32] = 0x0c;
        tgui->pci_regs[0x33] = 0x00;
    }

    if (tgui->type >= TGUI_9440) {
        svga->packed_chain4 = 1;
        tgui->i2c = i2c_gpio_init("ddc_tgui");
        tgui->ddc = ddc_init(i2c_gpio_get_bus(tgui->i2c));
    }

    return tgui;
}

static int
tgui9400cxi_available(void)
{
    return rom_present(ROM_TGUI_9400CXI);
}

static int
tgui9440_vlb_available(void)
{
    return rom_present(ROM_TGUI_9440_VLB);
}

static int
tgui9440_pci_available(void)
{
    return rom_present(ROM_TGUI_9440_PCI);
}

static int
tgui96xx_available(void)
{
    return rom_present(ROM_TGUI_96xx);
}

void
tgui_close(void *priv)
{
    tgui_t *tgui = (tgui_t *) priv;

    svga_close(&tgui->svga);

    if (tgui->type >= TGUI_9440) {
        ddc_close(tgui->ddc);
        i2c_gpio_close(tgui->i2c);
    }

    free(tgui);
}

void
tgui_speed_changed(void *priv)
{
    tgui_t *tgui = (tgui_t *) priv;

    svga_recalctimings(&tgui->svga);
}

void
tgui_force_redraw(void *priv)
{
    tgui_t *tgui = (tgui_t *) priv;

    tgui->svga.fullchange = tgui->svga.monitor->mon_changeframecount;
}

// clang-format off
static const device_config_t tgui9440_config[] = {
    {
        .name           = "memory",
        .description    = "Memory size",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 2,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "1 MB", .value = 1 },
            { .description = "2 MB", .value = 2 },
            { .description = ""                 }
        },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
};

static const device_config_t tgui96xx_config[] = {
    {
        .name           = "memory",
        .description    = "Memory size",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 4,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "1 MB", .value = 1 },
            { .description = "2 MB", .value = 2 },
            { .description = "4 MB", .value = 4 },
            { .description = ""                 }
        },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
};
// clang-format on

const device_t tgui9400cxi_device = {
    .name          = "Trident TGUI 9400CXi",
    .internal_name = "tgui9400cxi_vlb",
    .flags         = DEVICE_VLB,
    .local         = TGUI_9400CXI,
    .init          = tgui_init,
    .close         = tgui_close,
    .reset         = NULL,
    .available     = tgui9400cxi_available,
    .speed_changed = tgui_speed_changed,
    .force_redraw  = tgui_force_redraw,
    .config        = tgui9440_config
};

const device_t tgui9440_vlb_device = {
    .name          = "Trident TGUI 9440AGi VLB",
    .internal_name = "tgui9440_vlb",
    .flags         = DEVICE_VLB,
    .local         = TGUI_9440,
    .init          = tgui_init,
    .close         = tgui_close,
    .reset         = NULL,
    .available     = tgui9440_vlb_available,
    .speed_changed = tgui_speed_changed,
    .force_redraw  = tgui_force_redraw,
    .config        = tgui9440_config
};

const device_t tgui9440_pci_device = {
    .name          = "Trident TGUI 9440AGi PCI",
    .internal_name = "tgui9440_pci",
    .flags         = DEVICE_PCI,
    .local         = TGUI_9440,
    .init          = tgui_init,
    .close         = tgui_close,
    .reset         = NULL,
    .available     = tgui9440_pci_available,
    .speed_changed = tgui_speed_changed,
    .force_redraw  = tgui_force_redraw,
    .config        = tgui9440_config
};

const device_t tgui9440_onboard_pci_device = {
    .name          = "Trident TGUI 9440AGi On-Board PCI",
    .internal_name = "tgui9440_onboard_pci",
    .flags         = DEVICE_PCI,
    .local         = TGUI_9440 | ONBOARD,
    .init          = tgui_init,
    .close         = tgui_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = tgui_speed_changed,
    .force_redraw  = tgui_force_redraw,
    .config        = tgui9440_config
};

const device_t tgui9660_pci_device = {
    .name          = "Trident TGUI 9660XGi PCI",
    .internal_name = "tgui9660_pci",
    .flags         = DEVICE_PCI,
    .local         = TGUI_9660,
    .init          = tgui_init,
    .close         = tgui_close,
    .reset         = NULL,
    .available     = tgui96xx_available,
    .speed_changed = tgui_speed_changed,
    .force_redraw  = tgui_force_redraw,
    .config        = tgui96xx_config
};

const device_t tgui9660_onboard_pci_device = {
    .name          = "Trident TGUI 9660XGi On-Board PCI",
    .internal_name = "tgui9660_onboard_pci",
    .flags         = DEVICE_PCI,
    .local         = TGUI_9660 | ONBOARD,
    .init          = tgui_init,
    .close         = tgui_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = tgui_speed_changed,
    .force_redraw  = tgui_force_redraw,
    .config        = tgui96xx_config
};

const device_t tgui9680_pci_device = {
    .name          = "Trident TGUI 9680XGi PCI",
    .internal_name = "tgui9680_pci",
    .flags         = DEVICE_PCI,
    .local         = TGUI_9680,
    .init          = tgui_init,
    .close         = tgui_close,
    .reset         = NULL,
    .available     = tgui96xx_available,
    .speed_changed = tgui_speed_changed,
    .force_redraw  = tgui_force_redraw,
    .config        = tgui96xx_config
};
