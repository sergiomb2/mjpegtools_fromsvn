/*
 * Zoran zr36057/zr36067 PCI controller driver, for the
 * Pinnacle/Miro DC10/DC10+/DC30/DC30+, Iomega Buz, Linux
 * Media Labs LML33/LML33R10.
 *
 * This part handles device access (PCI/I2C/codec/...)
 * 
 * Copyright (C) 2000 Serguei Miridonov <mirsev@cicese.mx>
 *
 * Currently maintained by:
 *   Ronald Bultje    <rbultje@ronald.bitfreak.net>
 *   Laurent Pinchart <laurent.pinchart@skynet.be>
 *   Mailinglist      <mjpeg-users@lists.sf.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/config.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/vmalloc.h>

#include <linux/proc_fs.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/videodev.h>
#include <linux/spinlock.h>
#include <linux/sem.h>

#include <linux/pci.h>
#include <linux/video_decoder.h>
#include <linux/video_encoder.h>
#include <linux/delay.h>

#include "videocodec.h"
#include "zoran.h"
#include "zoran_device.h"

#define IRQ_MASK ( ZR36057_ISR_GIRQ0 | \
		   ZR36057_ISR_GIRQ1 | \
		   ZR36057_ISR_JPEGRepIRQ )

extern const struct zoran_format zoran_formats[];
extern const int zoran_num_formats;

extern int debug;

#define dprintk(num, format, args...) \
	do { \
		if (debug >= num) \
			printk(format, ##args); \
	} while (0)

static int lml33dpath = 0;	/* 1 will use digital path in capture
				 * mode instead of analog. It can be
				 * used for picture adjustments using
				 * tool like xawtv while watching image
				 * on TV monitor connected to the output.
				 * However, due to absence of 75 Ohm
				 * load on Bt819 input, there will be
				 * some image imperfections */

MODULE_PARM(lml33dpath, "i");
MODULE_PARM_DESC(lml33dpath,
		 "Use digital path capture mode (on LML33 cards)");

/*
 * General Purpose I/O and Guest bus access
 */

/*
 * This is a bit tricky. When a board lacks a GPIO function, the corresponding
 * GPIO bit number in the card_info structure is set to 0.
 */

void
GPIO (struct zoran *zr,
      int           bit,
      unsigned int  value)
{
	u32 reg;
	u32 mask;

	/* Make sure the bit number is legal
	 * A bit number of -1 (lacking) gives a mask of 0,
	 * making it harmless */
	mask = (1 << (24 + bit)) & 0xff000000;
	reg = btread(ZR36057_GPPGCR1) & ~mask;
	if (value) {
		reg |= mask;
	}
	btwrite(reg, ZR36057_GPPGCR1);
	udelay(1);
}

/*
 * Wait til post office is no longer busy
 */

int
post_office_wait (struct zoran *zr)
{
	u32 por;

//      while (((por = btread(ZR36057_POR)) & (ZR36057_POR_POPen | ZR36057_POR_POTime)) == ZR36057_POR_POPen) {
	while ((por = btread(ZR36057_POR)) & ZR36057_POR_POPen) {
		/* wait for something to happen */
	}
	if ((por & ZR36057_POR_POTime) && !zr->card->gws_not_connected) {
		/* In LML33/BUZ \GWS line is not connected, so it has always timeout set */
		dprintk(1, KERN_INFO "%s: pop timeout %08x\n", zr->name,
			por);
		return -1;
	}

	return 0;
}

int
post_office_write (struct zoran *zr,
		   unsigned int  guest,
		   unsigned int  reg,
		   unsigned int  value)
{
	u32 por;

	por =
	    ZR36057_POR_PODir | ZR36057_POR_POTime | ((guest & 7) << 20) |
	    ((reg & 7) << 16) | (value & 0xFF);
	btwrite(por, ZR36057_POR);

	return post_office_wait(zr);
}

int
post_office_read (struct zoran *zr,
		  unsigned int  guest,
		  unsigned int  reg)
{
	u32 por;

	por = ZR36057_POR_POTime | ((guest & 7) << 20) | ((reg & 7) << 16);
	btwrite(por, ZR36057_POR);
	if (post_office_wait(zr) < 0) {
		return -1;
	}

	return btread(ZR36057_POR) & 0xFF;
}

/*
 * detect guests
 */

static void
dump_guests (struct zoran *zr)
{
	if (debug > 2) {
		int i, guest[8];

		for (i = 1; i < 8; i++) {	// Don't read jpeg codec here
			guest[i] = post_office_read(zr, i, 0);
		}

		printk(KERN_INFO "%s: Guests:", zr->name);

		for (i = 1; i < 8; i++) {
			printk(" 0x%02x", guest[i]);
		}
		printk("\n");
	}
}

static inline unsigned long
get_time (void)
{
	struct timeval tv;
	do_gettimeofday(&tv);
	return (1000000 * tv.tv_sec + tv.tv_usec);
}

void
detect_guest_activity (struct zoran *zr)
{
	int timeout, i, j, res, guest[8], guest0[8], change[8][3];
	unsigned long t0, t1;

	dump_guests(zr);
	printk(KERN_INFO "%s: Detecting guests activity, please wait...\n",
	       zr->name);
	for (i = 1; i < 8; i++) {	// Don't read jpeg codec here
		guest0[i] = guest[i] = post_office_read(zr, i, 0);
	}

	timeout = 0;
	j = 0;
	t0 = get_time();
	while (timeout < 10000) {
		udelay(10);
		timeout++;
		for (i = 1; (i < 8) && (j < 8); i++) {
			res = post_office_read(zr, i, 0);
			if (res != guest[i]) {
				t1 = get_time();
				change[j][0] = (t1 - t0);
				t0 = t1;
				change[j][1] = i;
				change[j][2] = res;
				j++;
				guest[i] = res;
			}
		}
		if (j >= 8)
			break;
	}
	printk(KERN_INFO "%s: Guests:", zr->name);

	for (i = 1; i < 8; i++) {
		printk(" 0x%02x", guest0[i]);
	}
	printk("\n");
	if (j == 0) {
		printk(KERN_INFO "%s: No activity detected.\n", zr->name);
		return;
	}
	for (i = 0; i < j; i++) {
		printk(KERN_INFO "%s: %6d: %d => 0x%02x\n", zr->name,
		       change[i][0], change[i][1], change[i][2]);
	}
}

/*
 * JPEG Codec access
 */

void
jpeg_codec_sleep (struct zoran *zr,
		  int           sleep)
{
	GPIO(zr, zr->card->gpio[GPIO_JPEG_SLEEP], !sleep);
	if (!sleep) {
		dprintk(3,
			KERN_DEBUG
			"%s: jpeg_codec_sleep() - wake GPIO=0x%08x\n",
			zr->name, btread(ZR36057_GPPGCR1));
		udelay(500);
	} else {
		dprintk(3,
			KERN_DEBUG
			"%s: jpeg_codec_sleep() - sleep GPIO=0x%08x\n",
			zr->name, btread(ZR36057_GPPGCR1));
		udelay(2);
	}
}

int
jpeg_codec_reset (struct zoran *zr)
{
	/* Take the codec out of sleep */
	jpeg_codec_sleep(zr, 0);

	if (zr->card->gpcs[GPCS_JPEG_RESET] != 0xff) {
		post_office_write(zr, zr->card->gpcs[GPCS_JPEG_RESET], 0,
				  0);
		udelay(2);
	} else {
		GPIO(zr, zr->card->gpio[GPIO_JPEG_RESET], 0);
		udelay(2);
		GPIO(zr, zr->card->gpio[GPIO_JPEG_RESET], 1);
		udelay(2);
	}

	return 0;
}

/*
 *   Set the registers for the size we have specified. Don't bother
 *   trying to understand this without the ZR36057 manual in front of
 *   you [AC].
 *
 *   PS: The manual is free for download in .pdf format from
 *   www.zoran.com - nicely done those folks.
 */

static void
zr36057_adjust_vfe (struct zoran          *zr,
		    enum zoran_codec_mode  mode)
{
	u32 reg;

	switch (mode) {
	case BUZ_MODE_MOTION_DECOMPRESS:
		btand(~ZR36057_VFESPFR_ExtFl, ZR36057_VFESPFR);
		reg = btread(ZR36057_VFEHCR);
		if ((reg & (1 << 10)) && zr->card->type != LML33R10) {
			reg += ((1 << 10) | 1);
		}
		btwrite(reg, ZR36057_VFEHCR);
		break;
	case BUZ_MODE_MOTION_COMPRESS:
	case BUZ_MODE_IDLE:
	default:
		if (zr->norm == VIDEO_MODE_NTSC ||
		    (zr->card->type == LML33R10 &&
		     zr->norm == VIDEO_MODE_PAL))
			btand(~ZR36057_VFESPFR_ExtFl, ZR36057_VFESPFR);
		else
			btor(ZR36057_VFESPFR_ExtFl, ZR36057_VFESPFR);
		reg = btread(ZR36057_VFEHCR);
		if (!(reg & (1 << 10)) && zr->card->type != LML33R10) {
			reg -= ((1 << 10) | 1);
		}
		btwrite(reg, ZR36057_VFEHCR);
		break;
	}
}

/*
 * set geometry
 */

static void
zr36057_set_vfe (struct zoran              *zr,
		 int                        video_width,
		 int                        video_height,
		 const struct zoran_format *format)
{
	struct tvnorm *tvn;
	unsigned HStart, HEnd, VStart, VEnd;
	unsigned DispMode;
	unsigned VidWinWid, VidWinHt;
	unsigned hcrop1, hcrop2, vcrop1, vcrop2;
	unsigned Wa, We, Ha, He;
	unsigned X, Y, HorDcm, VerDcm;
	u32 reg;
	unsigned mask_line_size;

	tvn = zr->timing;

	Wa = tvn->Wa;
	Ha = tvn->Ha;

	dprintk(2, KERN_INFO "%s: set_vfe() - width = %d, height = %d\n",
		zr->name, video_width, video_height);

	if (zr->norm != VIDEO_MODE_PAL &&
	    zr->norm != VIDEO_MODE_NTSC &&
	    zr->norm != VIDEO_MODE_SECAM) {
		dprintk(1,
			KERN_ERR "%s: set_vfe() - norm = %d not valid\n",
			zr->name, zr->norm);
		return;
	}
	if (video_width < BUZ_MIN_WIDTH ||
	    video_height < BUZ_MIN_HEIGHT ||
	    video_width > Wa || video_height > Ha) {
		dprintk(1, KERN_ERR "%s: set_vfe: w=%d h=%d not valid\n",
			zr->name, video_width, video_height);
		return;
	}

	/**** zr36057 ****/

	/* horizontal */
	VidWinWid = video_width;
	X = (VidWinWid * 64 + tvn->Wa - 1) / tvn->Wa;
	We = (VidWinWid * 64) / X;
	HorDcm = 64 - X;
	hcrop1 = 2 * ((tvn->Wa - We) / 4);
	hcrop2 = tvn->Wa - We - hcrop1;
	HStart = tvn->HStart | 1;	// | 1 Doesn't have any effect, tested on both a DC10 and a DC10+
/*        if (zr->card->type == LML33)
		HStart += 62;
        if (zr->card->type == BUZ) {   //HStart += 67;
                HStart += 44;
        }
This is not needed anymore (I think) as HSYNC pol has been changed for the BUZ and LML33
*/
	HEnd = HStart + tvn->Wa - 1;
	HStart += hcrop1;
	HEnd -= hcrop2;
	reg = ((HStart & ZR36057_VFEHCR_Hmask) << ZR36057_VFEHCR_HStart)
	    | ((HEnd & ZR36057_VFEHCR_Hmask) << ZR36057_VFEHCR_HEnd);
	if (zr->card->vfe_pol.hsync_pol)
		reg |= ZR36057_VFEHCR_HSPol;
	btwrite(reg, ZR36057_VFEHCR);

	/* Vertical */
	DispMode = !(video_height > BUZ_MAX_HEIGHT / 2);
	VidWinHt = DispMode ? video_height : video_height / 2;
	Y = (VidWinHt * 64 * 2 + tvn->Ha - 1) / tvn->Ha;
	He = (VidWinHt * 64) / Y;
	VerDcm = 64 - Y;
	vcrop1 = (tvn->Ha / 2 - He) / 2;
	vcrop2 = tvn->Ha / 2 - He - vcrop1;
	VStart = tvn->VStart;
	VEnd = VStart + tvn->Ha / 2;	// - 1; FIXME SnapShot times out with -1 in 768*576 on the DC10 - LP
	VStart += vcrop1;
	VEnd -= vcrop2;
	reg = ((VStart & ZR36057_VFEVCR_Vmask) << ZR36057_VFEVCR_VStart)
	    | ((VEnd & ZR36057_VFEVCR_Vmask) << ZR36057_VFEVCR_VEnd);
	if (zr->card->vfe_pol.vsync_pol)
		reg |= ZR36057_VFEVCR_VSPol;
	btwrite(reg, ZR36057_VFEVCR);

	/* scaler and pixel format */
	reg = 0;
	reg |= (HorDcm << ZR36057_VFESPFR_HorDcm);
	reg |= (VerDcm << ZR36057_VFESPFR_VerDcm);
	reg |= (DispMode << ZR36057_VFESPFR_DispMode);
	if (format->palette != VIDEO_PALETTE_YUV422)
		reg |= ZR36057_VFESPFR_LittleEndian;
	/* RJ: I don't know, why the following has to be the opposite
	 * of the corresponding ZR36060 setting, but only this way
	 * we get the correct colors when uncompressing to the screen  */
	//reg |= ZR36057_VFESPFR_VCLKPol; /**/
	/* RJ: Don't know if that is needed for NTSC also */
	if (zr->norm != VIDEO_MODE_NTSC)
		reg |= ZR36057_VFESPFR_ExtFl;	// NEEDED!!!!!!! Wolfgang
	reg |= ZR36057_VFESPFR_TopField;
	switch (format->palette) {

	case VIDEO_PALETTE_YUV422:
		reg |= ZR36057_VFESPFR_YUV422;
		break;

	case VIDEO_PALETTE_RGB555:
		reg |= ZR36057_VFESPFR_RGB555 | ZR36057_VFESPFR_ErrDif;
		break;

	case VIDEO_PALETTE_RGB565:
		reg |= ZR36057_VFESPFR_RGB565 | ZR36057_VFESPFR_ErrDif;
		break;

	case VIDEO_PALETTE_RGB24:
		reg |= ZR36057_VFESPFR_RGB888 | ZR36057_VFESPFR_Pack24;
		break;

	case VIDEO_PALETTE_RGB32:
		reg |= ZR36057_VFESPFR_RGB888;
		break;

	default:
		dprintk(1,
			KERN_INFO "%s: set_vfe() - unknown color_fmt=%x\n",
			zr->name, format->palette);
		return;

	}
	if (HorDcm >= 48) {
		reg |= 3 << ZR36057_VFESPFR_HFilter;	/* 5 tap filter */
	} else if (HorDcm >= 32) {
		reg |= 2 << ZR36057_VFESPFR_HFilter;	/* 4 tap filter */
	} else if (HorDcm >= 16) {
		reg |= 1 << ZR36057_VFESPFR_HFilter;	/* 3 tap filter */
	}
	btwrite(reg, ZR36057_VFESPFR);

	/* display configuration */
	reg = (16 << ZR36057_VDCR_MinPix)
	    | (VidWinHt << ZR36057_VDCR_VidWinHt)
	    | (VidWinWid << ZR36057_VDCR_VidWinWid);
	if (pci_pci_problems & PCIPCI_TRITON)
		// || zr->revision < 1) // Revision 1 has also Triton support
		reg &= ~ZR36057_VDCR_Triton;
	else
		reg |= ZR36057_VDCR_Triton;
	btwrite(reg, ZR36057_VDCR);

	/* (Ronald) don't write this if overlay_mask = NULL */
	if (zr->overlay_mask) {
		/* Write overlay clipping mask data, but don't enable overlay clipping */
		/* RJ: since this makes only sense on the screen, we use 
		 * zr->overlay_settings.width instead of video_width */

		mask_line_size = (BUZ_MAX_WIDTH + 31) / 32;
		reg = virt_to_bus(zr->overlay_mask);
		btwrite(reg, ZR36057_MMTR);
		reg = virt_to_bus(zr->overlay_mask + mask_line_size);
		btwrite(reg, ZR36057_MMBR);
		reg =
		    mask_line_size - (zr->overlay_settings.width +
				      31) / 32;
		if (DispMode == 0)
			reg += mask_line_size;
		reg <<= ZR36057_OCR_MaskStride;
		btwrite(reg, ZR36057_OCR);
	}

	zr36057_adjust_vfe(zr, zr->codec_mode);
}

/*
 * Switch overlay on or off
 */

void
zr36057_overlay (struct zoran *zr,
		 int           on)
{
	const struct zoran_format *format = NULL;
	int fmt, i;
	u32 reg;

	if (on) {
		/* do the necessary settings ... */
		btand(~ZR36057_VDCR_VidEn, ZR36057_VDCR);	/* switch it off first */

		switch (zr->buffer.depth) {
		case 15:
			fmt = VIDEO_PALETTE_RGB555;
			break;
		case 16:
			fmt = VIDEO_PALETTE_RGB565;
			break;
		case 24:
			fmt = VIDEO_PALETTE_RGB24;
			break;
		case 32:
			fmt = VIDEO_PALETTE_RGB32;
			break;
		default:
			fmt = 0;
		}

		if (fmt) {
			for (i = 0; i < zoran_num_formats; i++) {
				if (zoran_formats[i].palette == fmt &&
				    zoran_formats[i].flags & 
				    ZORAN_FORMAT_OVERLAY)
					break;
			}
			if (i != zoran_num_formats) {
				format = &zoran_formats[i];
				zr36057_set_vfe(zr,
						zr->overlay_settings.width,
						zr->overlay_settings.height,
						format);
			}
		}

		/* Start and length of each line MUST be 4-byte aligned.
		 * This should be allready checked before the call to this routine.
		 * All error messages are internal driver checking only! */

		/* video display top and bottom registers */
		reg =
		    (u32) zr->buffer.base +
		    zr->overlay_settings.x * ((format->depth + 7) / 8) +
		    zr->overlay_settings.y * zr->buffer.bytesperline;
		btwrite(reg, ZR36057_VDTR);
		if (reg & 3)
			dprintk(1,
				KERN_ERR
				"%s: zr36057_overlay() - video_address not aligned\n",
				zr->name);
		if (zr->overlay_settings.height > BUZ_MAX_HEIGHT / 2)
			reg += zr->buffer.bytesperline;
		btwrite(reg, ZR36057_VDBR);

		/* video stride, status, and frame grab register */
		reg =
		    zr->buffer.bytesperline -
		    zr->overlay_settings.width * ((format->depth + 7) / 8);
		if (zr->overlay_settings.height > BUZ_MAX_HEIGHT / 2)
			reg += zr->buffer.bytesperline;
		if (reg & 3)
			dprintk(1,
				KERN_ERR
				"%s: zr36057_overlay() - video_stride not aligned\n",
				zr->name);
		reg = (reg << ZR36057_VSSFGR_DispStride);
		reg |= ZR36057_VSSFGR_VidOvf;	/* clear overflow status */
		btwrite(reg, ZR36057_VSSFGR);

		/* Set overlay clipping */
		if (zr->overlay_settings.clipcount > 0)
			btor(ZR36057_OCR_OvlEnable, ZR36057_OCR);

		/* ... and switch it on */
		btor(ZR36057_VDCR_VidEn, ZR36057_VDCR);
	} else {
		/* Switch it off */
		btand(~ZR36057_VDCR_VidEn, ZR36057_VDCR);
	}
}

/*
 * The overlay mask has one bit for each pixel on a scan line,
 *  and the maximum window size is BUZ_MAX_WIDTH * BUZ_MAX_HEIGHT pixels.
 */

void
write_overlay_mask (struct file       *file,
		    struct video_clip *vp,
		    int                count)
{
	struct zoran_fh *fh = file->private_data;
	struct zoran *zr = fh->zr;
	unsigned mask_line_size = (BUZ_MAX_WIDTH + 31) / 32;
	u32 *mask;
	int x, y, width, height;
	unsigned i, j, k;
	u32 reg;

	/* fill mask with one bits */
	memset(fh->overlay_mask, ~0, mask_line_size * 4 * BUZ_MAX_HEIGHT);
	reg = 0;

	for (i = 0; i < count; ++i) {
		/* pick up local copy of clip */
		x = vp[i].x;
		y = vp[i].y;
		width = vp[i].width;
		height = vp[i].height;

		/* trim clips that extend beyond the window */
		if (x < 0) {
			width += x;
			x = 0;
		}
		if (y < 0) {
			height += y;
			y = 0;
		}
		if (x + width > fh->overlay_settings.width) {
			width = fh->overlay_settings.width - x;
		}
		if (y + height > fh->overlay_settings.height) {
			height = fh->overlay_settings.height - y;
		}

		/* ignore degenerate clips */
		if (height <= 0) {
			continue;
		}
		if (width <= 0) {
			continue;
		}

		/* apply clip for each scan line */
		for (j = 0; j < height; ++j) {
			/* reset bit for each pixel */
			/* this can be optimized later if need be */
			mask = fh->overlay_mask + (y + j) * mask_line_size;
			for (k = 0; k < width; ++k) {
				mask[(x + k) / 32] &=
				    ~((u32) 1 << (x + k) % 32);
			}
		}
	}
}

/* Enable/Disable uncompressed memory grabbing of the 36057 */

void
zr36057_set_memgrab (struct zoran *zr,
		     int           mode)
{
	if (mode) {
		if (btread(ZR36057_VSSFGR) &
		    (ZR36057_VSSFGR_SnapShot | ZR36057_VSSFGR_FrameGrab))
			dprintk(1,
				KERN_WARNING
				"%s: zr36057_set_memgrab(1) with SnapShot or FrameGrab on!?\n",
				zr->name);

		/* switch on VSync interrupts */
		btwrite(IRQ_MASK, ZR36057_ISR);	// Clear Interrupts
		btor(zr->card->vsync_int, ZR36057_ICR);	// SW

		/* enable SnapShot */
		btor(ZR36057_VSSFGR_SnapShot, ZR36057_VSSFGR);

		/* Set zr36057 video front end  and enable video */
		zr36057_set_vfe(zr, zr->v4l_settings.width,
				zr->v4l_settings.height,
				zr->v4l_settings.format);

		zr->v4l_memgrab_active = 1;
	} else {
		zr->v4l_memgrab_active = 0;

		/* switch off VSync interrupts */
		btand(~zr->card->vsync_int, ZR36057_ICR);	// SW

		/* reenable grabbing to screen if it was running */
		if (zr->v4l_overlay_active) {
			zr36057_overlay(zr, 1);
		} else {
			btand(~ZR36057_VDCR_VidEn, ZR36057_VDCR);
			btand(~ZR36057_VSSFGR_SnapShot, ZR36057_VSSFGR);
		}
	}
}

int
wait_grab_pending (struct zoran *zr)
{
	unsigned long flags;

	/* wait until all pending grabs are finished */

	if (!zr->v4l_memgrab_active)
		return 0;

	while (zr->v4l_pend_tail != zr->v4l_pend_head) {
		interruptible_sleep_on(&zr->v4l_capq);
		if (signal_pending(current))
			return -ERESTARTSYS;
	}

	spin_lock_irqsave(&zr->spinlock, flags);
	zr36057_set_memgrab(zr, 0);
	spin_unlock_irqrestore(&zr->spinlock, flags);

	return 0;
}

/*****************************************************************************
 *                                                                           *
 *  Set up the Buz-specific MJPEG part                                       *
 *                                                                           *
 *****************************************************************************/

static inline void
set_frame (struct zoran *zr,
	   int           val)
{
	GPIO(zr, zr->card->gpio[GPIO_JPEG_FRAME], val);
}

static void
set_videobus_dir (struct zoran *zr,
		  int           val)
{
	switch (zr->card->type) {
	case LML33:
	case LML33R10:
		if (lml33dpath == 0)
			GPIO(zr, 5, val);
		else
			GPIO(zr, 5, 1);
		break;
	default:
		GPIO(zr, zr->card->gpio[GPIO_VID_DIR],
		     zr->card->gpio_pol[GPIO_VID_DIR] ? !val : val);
		break;
	}
}

static void
init_jpeg_queue (struct zoran *zr)
{
	int i;

	/* re-initialize DMA ring stuff */
	zr->jpg_que_head = 0;
	zr->jpg_dma_head = 0;
	zr->jpg_dma_tail = 0;
	zr->jpg_que_tail = 0;
	zr->jpg_seq_num = 0;
	zr->JPEG_error = 0;
	zr->num_errors = 0;
	zr->jpg_err_seq = 0;
	zr->jpg_err_shift = 0;
	zr->jpg_queued_num = 0;
	for (i = 0; i < zr->jpg_buffers.num_buffers; i++) {
		zr->jpg_buffers.buffer[i].state = BUZ_STATE_USER;	/* nothing going on */
	}
	for (i = 0; i < BUZ_NUM_STAT_COM; i++) {
		zr->stat_com[i] = 1;	/* mark as unavailable to zr36057 */
	}
}

static void
zr36057_set_jpg (struct zoran          *zr,
		 enum zoran_codec_mode  mode)
{
	struct tvnorm *tvn;
	u32 reg;

	tvn = zr->timing;

	/* assert P_Reset, disable code transfer, deassert Active */
	btwrite(0, ZR36057_JPC);

	/* MJPEG compression mode */
	switch (mode) {

	case BUZ_MODE_MOTION_COMPRESS:
	default:
		reg = ZR36057_JMC_MJPGCmpMode;
		break;

	case BUZ_MODE_MOTION_DECOMPRESS:
		reg = ZR36057_JMC_MJPGExpMode;
		reg |= ZR36057_JMC_SyncMstr;
		/* RJ: The following is experimental - improves the output to screen */
		//if(zr->jpg_settings.VFIFO_FB) reg |= ZR36057_JMC_VFIFO_FB; // No, it doesn't. SM
		break;

	case BUZ_MODE_STILL_COMPRESS:
		reg = ZR36057_JMC_JPGCmpMode;
		break;

	case BUZ_MODE_STILL_DECOMPRESS:
		reg = ZR36057_JMC_JPGExpMode;
		break;

	}
	reg |= ZR36057_JMC_JPG;
	if (zr->jpg_settings.field_per_buff == 1)
		reg |= ZR36057_JMC_Fld_per_buff;
	btwrite(reg, ZR36057_JMC);

	/* vertical */
	btor(ZR36057_VFEVCR_VSPol, ZR36057_VFEVCR);
	reg =
	    (6 << ZR36057_VSP_VsyncSize) | (tvn->Ht << ZR36057_VSP_FrmTot);
	btwrite(reg, ZR36057_VSP);
	reg = ((zr->jpg_settings.img_y + tvn->VStart) << ZR36057_FVAP_NAY)
	    | (zr->jpg_settings.img_height << ZR36057_FVAP_PAY);
	btwrite(reg, ZR36057_FVAP);

	/* horizontal */
	btor(ZR36057_VFEHCR_HSPol, ZR36057_VFEHCR);
	reg =
	    ((tvn->HSyncStart) << ZR36057_HSP_HsyncStart) | (tvn->
							     Wt <<
							     ZR36057_HSP_LineTot);
	btwrite(reg, ZR36057_HSP);
	reg =
	    ((zr->jpg_settings.img_x + tvn->HStart +
	      4) << ZR36057_FHAP_NAX)
	    | (zr->jpg_settings.img_width << ZR36057_FHAP_PAX);
	btwrite(reg, ZR36057_FHAP);

	/* field process parameters */
	if (zr->jpg_settings.odd_even)
		reg = ZR36057_FPP_Odd_Even;
	else
		reg = 0;

	btwrite(reg, ZR36057_FPP);

	/* Set proper VCLK Polarity, else colors will be wrong during playback */
	//btor(ZR36057_VFESPFR_VCLKPol, ZR36057_VFESPFR);

	/* code base address */
	reg = virt_to_bus(zr->stat_com);
	btwrite(reg, ZR36057_JCBA);

	/* FIFO threshold (FIFO is 160. double words) */
	/* NOTE: decimal values here */
	switch (mode) {

	case BUZ_MODE_STILL_COMPRESS:
	case BUZ_MODE_MOTION_COMPRESS:
		if (zr->card->type != BUZ)
			reg = 140;
		else
			reg = 60;
		break;

	case BUZ_MODE_STILL_DECOMPRESS:
	case BUZ_MODE_MOTION_DECOMPRESS:
		reg = 20;
		break;

	default:
		reg = 80;
		break;

	}
	btwrite(reg, ZR36057_JCFT);
	zr36057_adjust_vfe(zr, mode);

}

void
print_interrupts (struct zoran *zr)
{
	int res, noerr;
	noerr = 0;
	printk(KERN_INFO "%s: interrupts received:", zr->name);
	if ((res = zr->field_counter) < -1 || res > 1) {
		printk(" FD:%d", res);
	}
	if ((res = zr->intr_counter_GIRQ1) != 0) {
		printk(" GIRQ1:%d", res);
		noerr++;
	}
	if ((res = zr->intr_counter_GIRQ0) != 0) {
		printk(" GIRQ0:%d", res);
		noerr++;
	}
	if ((res = zr->intr_counter_CodRepIRQ) != 0) {
		printk(" CodRepIRQ:%d", res);
		noerr++;
	}
	if ((res = zr->intr_counter_JPEGRepIRQ) != 0) {
		printk(" JPEGRepIRQ:%d", res);
		noerr++;
	}
	if (zr->JPEG_max_missed) {
		printk(" JPEG delays: max=%d min=%d", zr->JPEG_max_missed,
		       zr->JPEG_min_missed);
	}
	if (zr->END_event_missed) {
		printk(" ENDs missed: %d", zr->END_event_missed);
	}
	//if (zr->jpg_queued_num) {
	printk(" queue_state=%ld/%ld/%ld/%ld", zr->jpg_que_tail,
	       zr->jpg_dma_tail, zr->jpg_dma_head, zr->jpg_que_head);
	//}
	if (!noerr) {
		printk(": no interrupts detected.");
	}
	printk("\n");
}

void
clear_interrupt_counters (struct zoran *zr)
{
	zr->intr_counter_GIRQ1 = 0;
	zr->intr_counter_GIRQ0 = 0;
	zr->intr_counter_CodRepIRQ = 0;
	zr->intr_counter_JPEGRepIRQ = 0;
	zr->field_counter = 0;
	zr->IRQ1_in = 0;
	zr->IRQ1_out = 0;
	zr->JPEG_in = 0;
	zr->JPEG_out = 0;
	zr->JPEG_0 = 0;
	zr->JPEG_1 = 0;
	zr->END_event_missed = 0;
	zr->JPEG_missed = 0;
	zr->JPEG_max_missed = 0;
	zr->JPEG_min_missed = 0x7fffffff;
}

static u32
count_reset_interrupt (struct zoran *zr)
{
	u32 isr;
	if ((isr = btread(ZR36057_ISR) & 0x78000000)) {
		if (isr & ZR36057_ISR_GIRQ1) {
			btwrite(ZR36057_ISR_GIRQ1, ZR36057_ISR);
			zr->intr_counter_GIRQ1++;
		}
		if (isr & ZR36057_ISR_GIRQ0) {
			btwrite(ZR36057_ISR_GIRQ0, ZR36057_ISR);
			zr->intr_counter_GIRQ0++;
		}
		if (isr & ZR36057_ISR_CodRepIRQ) {
			btwrite(ZR36057_ISR_CodRepIRQ, ZR36057_ISR);
			zr->intr_counter_CodRepIRQ++;
		}
		if (isr & ZR36057_ISR_JPEGRepIRQ) {
			btwrite(ZR36057_ISR_JPEGRepIRQ, ZR36057_ISR);
			zr->intr_counter_JPEGRepIRQ++;
		}
	}
	return isr;
}

/* hack */
extern void zr36016_write (struct videocodec *codec,
			   u16                reg,
			   u32                val);

void
jpeg_start (struct zoran *zr)
{
	int reg;
	zr->frame_num = 0;

	/* deassert P_reset, disable code transfer, deassert Active */
	btwrite(ZR36057_JPC_P_Reset, ZR36057_JPC);
	/* stop flushing the internal code buffer */
	btand(~ZR36057_MCTCR_CFlush, ZR36057_MCTCR);
	/* enable code transfer */
	btor(ZR36057_JPC_CodTrnsEn, ZR36057_JPC);

	/* clear IRQs */
	btwrite(IRQ_MASK, ZR36057_ISR);
	/* enable the JPEG IRQs */
	btwrite(zr->card->jpeg_int |
			ZR36057_ICR_JPEGRepIRQ |
			ZR36057_ICR_IntPinEn,
		ZR36057_ICR);

	set_frame(zr, 0);	// \FRAME

	/* set the JPEG codec guest ID */
	reg = (zr->card->gpcs[1] << ZR36057_JCGI_JPEGuestID) |
	       (0 << ZR36057_JCGI_JPEGuestReg);
	btwrite(reg, ZR36057_JCGI);

	if (zr->card->video_vfe == CODEC_TYPE_ZR36016 &&
	    zr->card->video_codec == CODEC_TYPE_ZR36050) {
		/* Enable processing on the ZR36016 */
		if (zr->vfe)
			zr36016_write(zr->vfe, 0, 1);

		/* load the address of the GO register in the ZR36050 latch */
		post_office_write(zr, 0, 0, 0);
	}

	/* assert Active */
	btor(ZR36057_JPC_Active, ZR36057_JPC);

	/* enable the Go generation */
	btor(ZR36057_JMC_Go_en, ZR36057_JMC);
	udelay(30);

	set_frame(zr, 1);	// /FRAME

	dprintk(3, KERN_DEBUG "%s: jpeg_start\n", zr->name);
}

void
zr36057_enable_jpg (struct zoran          *zr,
		    enum zoran_codec_mode  mode)
{
	static int zero = 0;
	static int one = 1;
	struct vfe_settings cap;
	int field_size =
	    zr->jpg_buffers.buffer_size / zr->jpg_settings.field_per_buff;

	zr->codec_mode = mode;

	cap.x = zr->jpg_settings.img_x;
	cap.y = zr->jpg_settings.img_y;
	cap.width = zr->jpg_settings.img_width;
	cap.height = zr->jpg_settings.img_height;
	cap.decimation =
	    zr->jpg_settings.HorDcm | (zr->jpg_settings.VerDcm << 8);
	cap.quality = zr->jpg_settings.jpg_comp.quality;

	switch (mode) {

	case BUZ_MODE_MOTION_COMPRESS:
		/* In motion compress mode, the decoder output must be enabled, and
		 * the video bus direction set to input.
		 */
		set_videobus_dir(zr, 0);
		decoder_command(zr, DECODER_ENABLE_OUTPUT, &one);
		encoder_command(zr, ENCODER_SET_INPUT, &zero);

		/* Take the JPEG codec and the VFE out of sleep */
		jpeg_codec_sleep(zr, 0);
		/* Setup the VFE */
		if (zr->vfe) {
			zr->vfe->control(zr->vfe, CODEC_S_JPEG_TDS_BYTE,
					 sizeof(int), &field_size);
			zr->vfe->set_video(zr->vfe, zr->timing, &cap,
					   &zr->card->vfe_pol);
			zr->vfe->set_mode(zr->vfe, CODEC_DO_COMPRESSION);
		}
		/* Setup the JPEG codec */
		zr->codec->control(zr->codec, CODEC_S_JPEG_TDS_BYTE,
				   sizeof(int), &field_size);
		zr->codec->set_video(zr->codec, zr->timing, &cap,
				     &zr->card->vfe_pol);
		zr->codec->set_mode(zr->codec, CODEC_DO_COMPRESSION);

		init_jpeg_queue(zr);
		zr36057_set_jpg(zr, mode);	// \P_Reset, ... Video param, FIFO

		clear_interrupt_counters(zr);
		dprintk(2, KERN_INFO "%s: enable_jpg(MOTION_COMPRESS)\n",
			zr->name);
		break;

	case BUZ_MODE_MOTION_DECOMPRESS:
		/* In motion decompression mode, the decoder output must be disabled, and
		 * the video bus direction set to output.
		 */
		decoder_command(zr, DECODER_ENABLE_OUTPUT, &zero);
		set_videobus_dir(zr, 1);
		encoder_command(zr, ENCODER_SET_INPUT, &one);

		/* Take the JPEG codec and the VFE out of sleep */
		jpeg_codec_sleep(zr, 0);
		/* Setup the VFE */
		if (zr->vfe) {
			zr->vfe->set_video(zr->vfe, zr->timing, &cap,
					   &zr->card->vfe_pol);
			zr->vfe->set_mode(zr->vfe, CODEC_DO_EXPANSION);
		}
		/* Setup the JPEG codec */
		zr->codec->set_video(zr->codec, zr->timing, &cap,
				     &zr->card->vfe_pol);
		zr->codec->set_mode(zr->codec, CODEC_DO_EXPANSION);

		init_jpeg_queue(zr);
		zr36057_set_jpg(zr, mode);	// \P_Reset, ... Video param, FIFO

		clear_interrupt_counters(zr);
		dprintk(2, KERN_INFO "%s: enable_jpg(MOTION_DECOMPRESS)\n",
			zr->name);
		break;

	case BUZ_MODE_IDLE:
	default:
		/* shut down processing */
		btand(~(zr->card->jpeg_int | ZR36057_ICR_JPEGRepIRQ),
		      ZR36057_ICR);
		btwrite(zr->card->jpeg_int | ZR36057_ICR_JPEGRepIRQ,
			ZR36057_ISR);
		btand(~ZR36057_JMC_Go_en, ZR36057_JMC);	// \Go_en

		current->state = TASK_UNINTERRUPTIBLE;
		schedule_timeout(HZ / 20);

		set_videobus_dir(zr, 0);
		set_frame(zr, 1);	// /FRAME
		btor(ZR36057_MCTCR_CFlush, ZR36057_MCTCR);	// /CFlush
		btwrite(0, ZR36057_JPC);	// \P_Reset,\CodTrnsEn,\Active
		btand(~ZR36057_JMC_VFIFO_FB, ZR36057_JMC);
		btand(~ZR36057_JMC_SyncMstr, ZR36057_JMC);
		jpeg_codec_reset(zr);
		jpeg_codec_sleep(zr, 1);
		zr36057_adjust_vfe(zr, mode);

		decoder_command(zr, DECODER_ENABLE_OUTPUT, &one);
		encoder_command(zr, ENCODER_SET_INPUT, &zero);

		dprintk(2, KERN_INFO "%s: enable_jpg(IDLE)\n", zr->name);
		break;

	}
}

/* when this is called the spinlock must be held */
void
zoran_feed_stat_com (struct zoran *zr)
{
	/* move frames from pending queue to DMA */

	int frame, i, max_stat_com;

	max_stat_com =
	    (zr->jpg_settings.TmpDcm ==
	     1) ? BUZ_NUM_STAT_COM : (BUZ_NUM_STAT_COM >> 1);

	while ((zr->jpg_dma_head - zr->jpg_dma_tail) < max_stat_com &&
	       zr->jpg_dma_head < zr->jpg_que_head) {

		frame = zr->jpg_pend[zr->jpg_dma_head & BUZ_MASK_FRAME];
		if (zr->jpg_settings.TmpDcm == 1) {
			/* fill 1 stat_com entry */
			i = (zr->jpg_dma_head -
			     zr->jpg_err_shift) & BUZ_MASK_STAT_COM;
			if (!(zr->stat_com[i] & 1))
				break;
			zr->stat_com[i] =
			    zr->jpg_buffers.buffer[frame].frag_tab_bus;
		} else {
			/* fill 2 stat_com entries */
			i = ((zr->jpg_dma_head -
			      zr->jpg_err_shift) & 1) * 2;
			if (!(zr->stat_com[i] & 1))
				break;
			zr->stat_com[i] =
			    zr->jpg_buffers.buffer[frame].frag_tab_bus;
			zr->stat_com[i + 1] =
			    zr->jpg_buffers.buffer[frame].frag_tab_bus;
		}
		zr->jpg_buffers.buffer[frame].state = BUZ_STATE_DMA;
		zr->jpg_dma_head++;

	}
	if (zr->codec_mode == BUZ_MODE_MOTION_DECOMPRESS)
		zr->jpg_queued_num++;
}

/* when this is called the spinlock must be held */
static void
zoran_reap_stat_com (struct zoran *zr)
{
	/* move frames from DMA queue to done queue */

	int i;
	u32 stat_com;
	unsigned int seq;
	unsigned int dif;
	struct zoran_jpg_buffer *buffer;
	int frame;

	/* In motion decompress we don't have a hardware frame counter,
	 * we just count the interrupts here */

	if (zr->codec_mode == BUZ_MODE_MOTION_DECOMPRESS) {
		zr->jpg_seq_num++;
	}
	while (zr->jpg_dma_tail < zr->jpg_dma_head) {
		if (zr->jpg_settings.TmpDcm == 1)
			i = (zr->jpg_dma_tail -
			     zr->jpg_err_shift) & BUZ_MASK_STAT_COM;
		else
			i = ((zr->jpg_dma_tail -
			      zr->jpg_err_shift) & 1) * 2 + 1;

		stat_com = zr->stat_com[i];

		if ((stat_com & 1) == 0) {
			return;
		}
		frame = zr->jpg_pend[zr->jpg_dma_tail & BUZ_MASK_FRAME];
		buffer = &zr->jpg_buffers.buffer[frame];
		do_gettimeofday(&buffer->bs.timestamp);

		if (zr->codec_mode == BUZ_MODE_MOTION_COMPRESS) {
			buffer->bs.length = (stat_com & 0x7fffff) >> 1;

			/* update sequence number with the help of the counter in stat_com */

			seq = ((stat_com >> 24) + zr->jpg_err_seq) & 0xff;
			dif = (seq - zr->jpg_seq_num) & 0xff;
			zr->jpg_seq_num += dif;
		} else {
			buffer->bs.length = 0;
		}
		buffer->bs.seq =
		    zr->jpg_settings.TmpDcm ==
		    2 ? (zr->jpg_seq_num >> 1) : zr->jpg_seq_num;
		buffer->state = BUZ_STATE_DONE;

		zr->jpg_dma_tail++;
	}
}

static void
error_handler (struct zoran *zr,
	       u32           astat,
	       u32           stat)
{
	/* This is JPEG error handling part */
	if ((zr->codec_mode != BUZ_MODE_MOTION_COMPRESS)
	    && (zr->codec_mode != BUZ_MODE_MOTION_DECOMPRESS)) {
		//dprintk(1, KERN_ERR "%s: Internal error: error handling request in mode %d\n", zr->name, zr->codec_mode);
		return;
	}

	if ((stat & 1) == 0 && zr->codec_mode == BUZ_MODE_MOTION_COMPRESS
	    && zr->jpg_dma_tail - zr->jpg_que_tail >=
	    zr->jpg_buffers.num_buffers) {
		/* No free buffers... */
		zoran_reap_stat_com(zr);
		zoran_feed_stat_com(zr);
		wake_up_interruptible(&zr->jpg_capq);
		zr->JPEG_missed = 0;
		return;
	}
	if (zr->JPEG_error != 1) {
		/*
		 * First entry: error just happened during normal operation
		 * 
		 * In BUZ_MODE_MOTION_COMPRESS:
		 * 
		 * Possible glitch in TV signal. In this case we should
		 * stop the codec and wait for good quality signal before
		 * restarting it to avoid further problems
		 * 
		 * In BUZ_MODE_MOTION_DECOMPRESS:
		 * 
		 * Bad JPEG frame: we have to mark it as processed (codec crashed
		 * and was not able to do it itself), and to remove it from queue.
		 */
		btand(~ZR36057_JMC_Go_en, ZR36057_JMC);
		udelay(1);
		 /*TODO*/ stat = stat | (post_office_read(zr, 7, 0) & 3) << 8;	// | jpeg_codec_read_8(zr, 0x008);
		btwrite(0, ZR36057_JPC);
		btor(ZR36057_MCTCR_CFlush, ZR36057_MCTCR);
		jpeg_codec_reset(zr);
		jpeg_codec_sleep(zr, 1);
		zr->JPEG_error = 1;
		zr->num_errors++;

		/* Report error */
		if (debug > 1 && zr->num_errors <= 8) {
			long frame;
			frame =
			    zr->jpg_pend[zr->
					 jpg_dma_tail & BUZ_MASK_FRAME];
			printk(KERN_ERR
			       "%s: JPEG error stat=0x%08x(0x%08x) queue_state=%ld/%ld/%ld/%ld seq=%ld frame=%ld. Codec stopped. ",
			       zr->name, stat, zr->last_isr,
			       zr->jpg_que_tail, zr->jpg_dma_tail,
			       zr->jpg_dma_head, zr->jpg_que_head,
			       zr->jpg_seq_num, frame);
			printk("stat_com frames:");
			{
				int i, j;
				for (j = 0; j < BUZ_NUM_STAT_COM; j++) {
					for (i = 0;
					     i <
					     zr->jpg_buffers.num_buffers;
					     i++) {
						if (zr->stat_com[j] ==
						    zr->jpg_buffers.
						    buffer[i].
						    frag_tab_bus) {
							printk("% d->%d",
							       j, i);
						}
					}
				}
				printk("\n");
			}
		}

		/* Find an entry in stat_com and rotate contents */
		{
			int i;

			if (zr->jpg_settings.TmpDcm == 1)
				i = (zr->jpg_dma_tail -
				     zr->
				     jpg_err_shift) & BUZ_MASK_STAT_COM;
			else
				i = ((zr->jpg_dma_tail -
				      zr->jpg_err_shift) & 1) * 2;
			if (zr->codec_mode == BUZ_MODE_MOTION_DECOMPRESS) {
				/* Mimic zr36067 operation */
				zr->stat_com[i] |= 1;
				if (zr->jpg_settings.TmpDcm != 1)
					zr->stat_com[i + 1] |= 1;
				/* Refill */
				zoran_reap_stat_com(zr);
				zoran_feed_stat_com(zr);
				wake_up_interruptible(&zr->jpg_capq);
				/* Find an entry in stat_com again after refill */
				if (zr->jpg_settings.TmpDcm == 1)
					i = (zr->jpg_dma_tail -
					     zr->
					     jpg_err_shift) &
					    BUZ_MASK_STAT_COM;
				else
					i = ((zr->jpg_dma_tail -
					      zr->jpg_err_shift) & 1) * 2;
			}
			if (i) {
				/* Rotate stat_comm entries to make current entry first */
				int j;
				u32 bus_addr[BUZ_NUM_STAT_COM];

				memcpy(bus_addr, zr->stat_com,
				       sizeof(bus_addr));
				for (j = 0; j < BUZ_NUM_STAT_COM; j++) {
					zr->stat_com[j] =
					    bus_addr[(i +
						      j) &
						     BUZ_MASK_STAT_COM];
				}
				zr->jpg_err_shift += i;
				zr->jpg_err_shift &= BUZ_MASK_STAT_COM;
			}
			if (zr->codec_mode == BUZ_MODE_MOTION_COMPRESS)
				zr->jpg_err_seq = zr->jpg_seq_num;	/* + 1; */
		}
	}
	/* Now the stat_comm buffer is ready for restart */
	{
		int status;

		status = 0;
		if (zr->codec_mode == BUZ_MODE_MOTION_COMPRESS)
			decoder_command(zr, DECODER_GET_STATUS, &status);
		if (zr->codec_mode == BUZ_MODE_MOTION_DECOMPRESS ||
		    (status & DECODER_STATUS_GOOD)) {
	    /********** RESTART code *************/
			jpeg_codec_reset(zr);
			zr->codec->set_mode(zr->codec, CODEC_DO_EXPANSION);
			zr36057_set_jpg(zr, zr->codec_mode);
			jpeg_start(zr);

			if (zr->num_errors <= 8)
				dprintk(2, KERN_INFO "%s: Restart\n",
					zr->name);

			zr->JPEG_missed = 0;
			zr->JPEG_error = 2;
	    /********** End RESTART code ***********/
		}
	}
}

void
zoran_irq (int             irq,
	   void           *dev_id,
	   struct pt_regs *regs)
{
	u32 stat, astat;
	int count;
	struct zoran *zr;
	unsigned long flags;

	zr = (struct zoran *) dev_id;
	count = 0;

	if (zr->testing) {
		/* Testing interrupts */
		spin_lock_irqsave(&zr->spinlock, flags);
		while ((stat = count_reset_interrupt(zr))) {
			if (count++ > 100) {
				btand(~ZR36057_ICR_IntPinEn, ZR36057_ICR);
				dprintk(1,
					KERN_ERR
					"%s: IRQ lockup while testing, isr=0x%08x, cleared int mask\n",
					zr->name, stat);
				wake_up_interruptible(&zr->test_q);
			}
		}
		zr->last_isr = stat;
		spin_unlock_irqrestore(&zr->spinlock, flags);
		return;
	}

	spin_lock_irqsave(&zr->spinlock, flags);
	while (1) {
		/* get/clear interrupt status bits */
		stat = count_reset_interrupt(zr);
		astat = stat & IRQ_MASK;
		if (!astat) {
			break;
		}
/*              dprintk(4,
			KERN_DEBUG
			"zoran_irq: astat: 0x%08x, mask: 0x%08x\n",
			astat, btread(ZR36057_ICR));*/
		if (astat & zr->card->vsync_int) {	// SW

			if (zr->codec_mode == BUZ_MODE_MOTION_DECOMPRESS ||
			    zr->codec_mode == BUZ_MODE_MOTION_COMPRESS) {
				/* count missed interrupts */
				zr->JPEG_missed++;
			}
			//post_office_read(zr,1,0);
			/* Interrupts may still happen when
			 * zr->v4l_memgrab_active is switched off.
			 * We simply ignore them */

			if (zr->v4l_memgrab_active) {

/* A lot more checks should be here ... */
				if ((btread(ZR36057_VSSFGR) &
				     ZR36057_VSSFGR_SnapShot) == 0)
					dprintk(1,
						KERN_WARNING
						"%s: BuzIRQ with SnapShot off ???\n",
						zr->name);

				if (zr->v4l_grab_frame != NO_GRAB_ACTIVE) {
					/* There is a grab on a frame going on, check if it has finished */

					if ((btread(ZR36057_VSSFGR) &
					     ZR36057_VSSFGR_FrameGrab) ==
					    0) {
						/* it is finished, notify the user */

						zr->v4l_buffers.buffer[zr->
								       v4l_grab_frame].
						    state = BUZ_STATE_DONE;
						zr->v4l_buffers.buffer[zr->
								       v4l_grab_frame].
						    bs.seq =
						    zr->v4l_grab_seq;
						do_gettimeofday(&zr->
								v4l_buffers.
								buffer[zr->
								       v4l_grab_frame].
								bs.
								timestamp);
						zr->v4l_grab_frame =
						    NO_GRAB_ACTIVE;
						zr->v4l_grab_seq++;
						zr->v4l_pend_tail++;
					}
				}

				if (zr->v4l_grab_frame == NO_GRAB_ACTIVE)
					wake_up_interruptible(&zr->
							      v4l_capq);

				/* Check if there is another grab queued */

				if (zr->v4l_grab_frame == NO_GRAB_ACTIVE &&
				    zr->v4l_pend_tail !=
				    zr->v4l_pend_head) {

					int frame =
					    zr->v4l_pend[zr->
							 v4l_pend_tail &
							 V4L_MASK_FRAME];
					u32 reg;

					zr->v4l_grab_frame = frame;

					/* Set zr36057 video front end and enable video */

					/* Buffer address */

					reg =
					    zr->v4l_buffers.buffer[frame].
					    fbuffer_bus;
					btwrite(reg, ZR36057_VDTR);
					if (zr->v4l_settings.height >
					    BUZ_MAX_HEIGHT / 2)
						reg +=
						    zr->v4l_settings.
						    bytesperline;
					btwrite(reg, ZR36057_VDBR);

					/* video stride, status, and frame grab register */
					reg = 0;
					if (zr->v4l_settings.height >
					    BUZ_MAX_HEIGHT / 2)
						reg +=
						    zr->v4l_settings.
						    bytesperline;
					reg =
					    (reg <<
					     ZR36057_VSSFGR_DispStride);
					reg |= ZR36057_VSSFGR_VidOvf;
					reg |= ZR36057_VSSFGR_SnapShot;
					reg |= ZR36057_VSSFGR_FrameGrab;
					btwrite(reg, ZR36057_VSSFGR);

					btor(ZR36057_VDCR_VidEn,
					     ZR36057_VDCR);
				}
			}
		}
#if (IRQ_MASK & ZR36057_ISR_CodRepIRQ)
		if (astat & ZR36057_ISR_CodRepIRQ) {
			zr->intr_counter_CodRepIRQ++;
			IDEBUG(printk
			       (KERN_DEBUG "%s: ZR36057_ISR_CodRepIRQ\n",
				zr->name));
			btand(~ZR36057_ICR_CodRepIRQ, ZR36057_ICR);
		}
#endif				/* (IRQ_MASK & ZR36057_ISR_CodRepIRQ) */

#if (IRQ_MASK & ZR36057_ISR_JPEGRepIRQ)
		if (astat & ZR36057_ISR_JPEGRepIRQ) {

			if (zr->codec_mode == BUZ_MODE_MOTION_DECOMPRESS ||
			    zr->codec_mode == BUZ_MODE_MOTION_COMPRESS) {
				if (debug > 1 &&
				    (!zr->frame_num || zr->JPEG_error)) {
					printk(KERN_INFO
					       "%s: first frame ready: state=0x%08x odd_even=%d field_per_buff=%d delay=%d\n",
					       zr->name, stat,
					       zr->jpg_settings.odd_even,
					       zr->jpg_settings.
					       field_per_buff,
					       zr->JPEG_missed);
					{
						char sc[] = "0000";
						char sv[5];
						int i;
						strcpy(sv, sc);
						for (i = 0; i < 4; i++) {
							if (zr->
							    stat_com[i] &
							    1)
								sv[i] =
								    '1';
						}
						sv[4] = 0;
						printk(KERN_INFO
						       "%s: stat_com=%s queue_state=%ld/%ld/%ld/%ld\n",
						       zr->name, sv,
						       zr->jpg_que_tail,
						       zr->jpg_dma_tail,
						       zr->jpg_dma_head,
						       zr->jpg_que_head);
					}
				} else {
					if (zr->JPEG_missed > zr->JPEG_max_missed)	// Get statistics
						zr->JPEG_max_missed =
						    zr->JPEG_missed;
					if (zr->JPEG_missed <
					    zr->JPEG_min_missed)
						zr->JPEG_min_missed =
						    zr->JPEG_missed;
				}

				if (debug > 2 && zr->frame_num < 6) {
					int i;
					printk("%s: seq=%ld stat_com:",
					       zr->name, zr->jpg_seq_num);
					for (i = 0; i < 4; i++) {
						printk(" %08x",
						       zr->stat_com[i]);
					}
					printk("\n");
				}
				zr->frame_num++;
				zr->JPEG_missed = 0;
				zr->JPEG_error = 0;
				zoran_reap_stat_com(zr);
				zoran_feed_stat_com(zr);
				wake_up_interruptible(&zr->jpg_capq);
			} /*else {
			      dprintk(1,
					KERN_ERR
					"%s: JPEG interrupt while not in motion (de)compress mode!\n",
					zr->name);
			}*/
		}
#endif				/* (IRQ_MASK & ZR36057_ISR_JPEGRepIRQ) */

		if ((astat & zr->card->jpeg_int)	/* DATERR interrupt received                 */
		    ||zr->JPEG_missed > 25	/* Too many fields missed without processing */
		    || zr->JPEG_error == 1	/* We are already in error processing        */
		    || ((zr->codec_mode == BUZ_MODE_MOTION_DECOMPRESS)
			&& (zr->
			    frame_num & (zr->JPEG_missed >
					 zr->jpg_settings.field_per_buff)))
		    /* fields missed during decompression */
		    ) {
			error_handler(zr, astat, stat);
		}

		count++;
		if (count > 10) {
			dprintk(2, KERN_WARNING "%s: irq loop %d\n",
				zr->name, count);
			if (count > 20) {
				btand(~ZR36057_ICR_IntPinEn, ZR36057_ICR);
				dprintk(2,
					KERN_ERR
					"%s: IRQ lockup, cleared int mask\n",
					zr->name);
				break;
			}
		}
		zr->last_isr = stat;
	}
	spin_unlock_irqrestore(&zr->spinlock, flags);
}

void
zoran_set_pci_master (struct zoran *zr,
		      int           set_master)
{
	u16 command;
	if (set_master) {
		pci_set_master(zr->pci_dev);
	} else {
		pci_read_config_word(zr->pci_dev, PCI_COMMAND, &command);
		command &= ~PCI_COMMAND_MASTER;
		pci_write_config_word(zr->pci_dev, PCI_COMMAND, command);
	}
}

void
zoran_init_hardware (struct zoran *zr)
{
	int j, zero = 0;
	/* Enable bus-mastering */
	zoran_set_pci_master(zr, 1);

	/* Initialize the board */
	if (zr->card->init) {
		zr->card->init(zr);
	}

	j = zr->card->input[zr->input].muxsel;

	decoder_command(zr, 0, NULL);
	decoder_command(zr, DECODER_SET_NORM, &zr->norm);
	decoder_command(zr, DECODER_SET_INPUT, &j);

	encoder_command(zr, 0, NULL);
	encoder_command(zr, ENCODER_SET_NORM, &zr->norm);
	encoder_command(zr, ENCODER_SET_INPUT, &zero);

	/* toggle JPEG codec sleep to sync PLL */
	jpeg_codec_sleep(zr, 1);
	jpeg_codec_sleep(zr, 0);

	/* set individual interrupt enables (without GIRQ1)
	 * but don't global enable until zoran_open() */

	//btwrite(IRQ_MASK & ~ZR36057_ISR_GIRQ1, ZR36057_ICR);  // SW
	// It looks like using only JPEGRepIRQEn is not always reliable,
	// may be when JPEG codec crashes it won't generate IRQ? So,
	 /*CP*/			//        btwrite(IRQ_MASK, ZR36057_ICR); // Enable Vsync interrupts too. SM    WHY ? LP
	    zr36057_init_vfe(zr);

	zr36057_enable_jpg(zr, BUZ_MODE_IDLE);

	btwrite(IRQ_MASK, ZR36057_ISR);	// Clears interrupts
}

void
zr36057_restart (struct zoran *zr)
{
	btwrite(0, ZR36057_SPGPPCR);
	mdelay(1);
	btor(ZR36057_SPGPPCR_SoftReset, ZR36057_SPGPPCR);
	mdelay(1);

	/* assert P_Reset */
	btwrite(0, ZR36057_JPC);
	/* set up GPIO direction - all output */
	btwrite(ZR36057_SPGPPCR_SoftReset | 0, ZR36057_SPGPPCR);

	/* set up GPIO pins and guest bus timing */
	btwrite((0x81 << 24) | 0x8888, ZR36057_GPPGCR1);
}

/*
 * initialize video front end
 */

void
zr36057_init_vfe (struct zoran *zr)
{
	u32 reg;
	reg = btread(ZR36057_VFESPFR);
	reg |= ZR36057_VFESPFR_LittleEndian;
	reg &= ~ZR36057_VFESPFR_VCLKPol;
	reg |= ZR36057_VFESPFR_ExtFl;
	reg |= ZR36057_VFESPFR_TopField;
	btwrite(reg, ZR36057_VFESPFR);
	reg = btread(ZR36057_VDCR);
	if (pci_pci_problems & PCIPCI_TRITON)
		// || zr->revision < 1) // Revision 1 has also Triton support
		reg &= ~ZR36057_VDCR_Triton;
	else
		reg |= ZR36057_VDCR_Triton;
	btwrite(reg, ZR36057_VDCR);
}

/*
 * Interface to decoder and encoder chips using i2c bus
 */

int
decoder_command (struct zoran *zr,
		 int           cmd,
		 void         *data)
{
	if (zr->decoder == NULL)
		return -EIO;

	if (zr->card->type == LML33 &&
	    (cmd == DECODER_SET_NORM || DECODER_SET_INPUT)) {
		int res;
		// Bt819 needs to reset its FIFO buffer using #FRST pin and
		// LML33 card uses GPIO(7) for that.
		GPIO(zr, 7, 0);
		res = zr->decoder->driver->command(zr->decoder, cmd, data);
		// Pull #FRST high.
		GPIO(zr, 7, 1);
		return res;
	} else
		return zr->decoder->driver->command(zr->decoder, cmd,
						    data);
}

int
encoder_command (struct zoran *zr,
		 int           cmd,
		 void         *data)
{
	if (zr->encoder == NULL)
		return -1;

	return zr->encoder->driver->command(zr->encoder, cmd, data);
}
