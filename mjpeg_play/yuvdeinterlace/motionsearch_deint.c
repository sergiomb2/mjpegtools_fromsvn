/***********************************************************
 * YUVDEINTERLACER for the mjpegtools                      *
 * ------------------------------------------------------- *
 * (C) 2001-2004 Stefan Fendt                              *
 *                                                         *
 * Licensed and protected by the GNU-General-Public-       *
 * License version 2 or if you prefer any later version of *
 * that license). See the file LICENSE for detailed infor- *
 * mation.                                                 *
 *                                                         *
 * FILE: motionsearch_deint.c                              *
 *                                                         *
 ***********************************************************/

#include <string.h>
#include <stdio.h>
#include "config.h"
#include "mjpeg_types.h"
#include "motionsearch_deint.h"
#include "mjpeg_logging.h"
#include "motionsearch.h"
#include "transform_block.h"
#include "vector.h"

extern uint8_t *lp0;
extern uint8_t *lp1;
extern uint8_t *lp2;

void
lowpass_plane_2D (uint8_t * d, uint8_t * s, int w, int h)
{
  int x, y, v;

  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
	v  = *(s + (x) + (y - 6) * w)*1;
	v += *(s + (x) + (y - 5) * w)*6;
	v += *(s + (x) + (y - 4) * w)*11;
	v += *(s + (x) + (y - 3) * w)*17;
	v += *(s + (x) + (y - 2) * w)*21;
	v += *(s + (x) + (y - 1) * w)*24;
	v += *(s + (x) + (y - 0) * w)*25;
	v += *(s + (x) + (y + 1) * w)*24;
	v += *(s + (x) + (y + 2) * w)*21;
	v += *(s + (x) + (y + 3) * w)*17;
	v += *(s + (x) + (y + 4) * w)*11;
	v += *(s + (x) + (y + 5) * w)*6;
	v += *(s + (x) + (y + 6) * w)*1;
	v /= 185;
	
	*(d + x + y * w) = v;

      }

  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
	v  = *(d + (x - 6) + y * w)*1;
	v += *(d + (x - 5) + y * w)*6;
	v += *(d + (x - 4) + y * w)*11;
	v += *(d + (x - 3) + y * w)*17;
	v += *(d + (x - 2) + y * w)*21;
	v += *(d + (x - 1) + y * w)*24;
	v += *(d + (x - 0) + y * w)*25;
	v += *(d + (x + 1) + y * w)*24;
	v += *(d + (x + 2) + y * w)*21;
	v += *(d + (x + 3) + y * w)*17;
	v += *(d + (x + 4) + y * w)*11;
	v += *(d + (x + 5) + y * w)*6;
	v += *(d + (x + 6) + y * w)*1;
	v /= 185;
	
	*(d + x + y * w) = v;

      }

}

void
lowpass_plane_1D (uint8_t * d, uint8_t * s, int w, int h)
{
  int x, y, v;

  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
	v  = *(s + (x    ) + (y -2 ) * w);
	v += *(s + (x    ) + (y -1 ) * w);
	v += *(s + (x    ) + (y    ) * w);
	v += *(s + (x    ) + (y +1 ) * w);
	v += *(s + (x    ) + (y +2 ) * w);
	v /= 5;
	
	*(d + x + y * w) = v;
      }
}

void
motion_compensate (uint8_t * r, uint8_t * f0, uint8_t * f1, uint8_t * f2,
		   int w, int h, int field)
{
  int x, y;
  int dx, dy;
  int fx, fy, ifx, ify;
  int bx, by, ibx, iby;
  uint32_t sad;
  uint32_t fmin, bmin, min;
  int a, b, c, d;

  static uint32_t mean=0;
  static int mcnt = 0;
  static uint32_t thres=0;

#if 0
  // fill top out-off-range lines to avoid ringing
  for (y = 1; y < 10; y++)
    {
      memcpy (f0 - w * y, f0, w);
      memcpy (f1 - w * y, f0, w);
      memcpy (f2 - w * y, f0, w);
    }
  // fill bottom out-off-range lines to avoid ringing
  for (y = 0; y < 10; y++)
    {
      memcpy (f0 + w * (h + y), f0 + w * (h - 1), w);
      memcpy (f1 + w * (h + y), f0 + w * (h - 1), w);
      memcpy (f2 + w * (h + y), f0 + w * (h - 1), w);
    }

  lowpass_plane_2D (lp0, f0, w, h);
  lowpass_plane_2D (lp1, f1, w, h);
  lowpass_plane_2D (lp2, f2, w, h);
#endif

#if 1
  for (y = 0; y < (h/2); y += 4)
    {
      for (x = 0; x < w; x += 8)
	{
		x -= 0;
		y += (h/2)-2;

	  fx = bx = ifx = ibx = 0;
	  fy = by = ify = iby = 0;

	  fmin = psad_sub22
	    (f1 + (x) + (y) * w, f0 + (x) + (y) * w, w, 8);

	  bmin = psad_sub22
	    (f1 + (x) + (y) * w, f2 + (x) + (y) * w, w, 8);

	  fmin *= 50;
	  fmin /= 100;

	  bmin *= 50;
	  bmin /= 100;

	  if(bmin>(thres*2) || fmin>(thres*2))
	  for (dy = -4; dy <= +4; dy++)
	    for (dx = -8; dx <= +8; dx++)
	      {
		sad = psad_sub22
		  (f1 + (x) + (y) * w,
		   f0 + (x + dx) + (y + dy) * w, w, 8);

		if (sad < fmin)
		  {
		    fmin = sad;
		    fx = dx;
		    fy = dy;
		  }

		sad = psad_sub22
		  (f1 + (x) + (y) * w,
		   f2 + (x + dx) + (y + dy) * w, w, 8);

		if (sad < bmin)
		  {
		    bmin = sad;
		    bx = dx;
		    by = dy;
		  }

	      }
		x += 0;
		y -= (h/2)-2;
		
	mean += (bmin<fmin)? bmin:fmin;
	mcnt += 1;

	  for (dy = 0; dy < 4; dy++)
	    for (dx = 0; dx < 8; dx++)
	      {
		a  = *(f0 + (x + dx + fx) + (y + dy + fy) * w);

		b  = *(f2 + (x + dx + bx) + (y + dy + by) * w);

		d = *(f1 + (x + dx ) + (y + dy ) * w);

		c = (a+b)/2;

		if(field==0)
			{
			  *(r + (x + dx) + (y + dy ) * w*2  ) = c;
			  *(r + (x + dx) + (y + dy ) * w*2+w) = d;
			}
		else
			{
			  *(r + (x + dx) + (y + dy ) * w*2+w) = c;
			  *(r + (x + dx) + (y + dy ) * w*2  ) = d;
			}
	      }
	}
    }
    if (mcnt>32000)
	{
	    thres = mean/mcnt;
	    mcnt=mean=0;
	}

	//fprintf(stderr,"thres:%i mcnt:%i\n",thres,mcnt);
#endif

#if 1
  for (y = field; y < h; y +=2)
    {
      for (x = 0; x < w; x++)
	{
	  a = *(r + x + y * w-w);
	  b = *(r + x + y * w  );
	  c = *(r + x + y * w+w);

	  if( (a>(b+16) && (b+16)<c) || (a<(b-16) && (b-16)>c) )
		  *(r + x + y * w) = (a+c)/2;
	}
    }
#endif
#if 1
// finally do a linear blend to get rid of the last combing and video artefacts
  for (y = 1; y < (h - 1); y++)
    {
      for (x = 0; x < w; x++)
	{
  	*(r + x + y * w) =
			(
			*(r + x + y * w - w) * 1 +
	     		*(r + x + y * w)     * 1 + 
			*(r + x + y * w + w) * 1
			) / 3;
	}
    }
#endif
}
