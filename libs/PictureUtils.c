/* -*-c-*- */
/* Copyright (C) 1993, Robert Nation
 * Copyright (C) 2002  Olivier Chapuis
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* ---------------------------- included header files ----------------------- */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <ctype.h>
#include <time.h>
#include <math.h>

#include <X11/Xlib.h>
#include <X11/Xmd.h>

#include <fvwmlib.h>
#include "PictureBase.h"
#include "PictureUtils.h"
#include "PictureDitherMatrice.h"

/* ---------------------------- local definitions and macro ------------------ */

#define PICTURE_DEBUG_COLORS_ALLOC_FAILURE 0
#define PICTURE_DEBUG_COLORS_PRINT_CMAP    0
#define PICTURE_DEBUG_COLORS_INFO          1

/* form alloc_in_cmap from the xpm lib */
#define XPM_DIST(r1,g1,b1,r2,g2,b2) (long)\
                          (3*(abs((long)r1-(long)r2) + \
			      abs((long)g1-(long)g2) + \
			      abs((long)b1-(long)b2)) + \
			   abs((long)r1 + (long)g1 + (long)b1 - \
			       ((long)r2 +  (long)g2 + (long)b2)))
#define XPM_COLOR_CLOSENESS 40000

#define SQUARE(X) ((X)*(X))

#define TRUE_DIST(r1,g1,b1,r2,g2,b2) (long)\
                   (SQUARE((long)((r1 - r2)>>8)) \
		+   SQUARE((long)((g1 - g2)>>8)) \
                +   SQUARE((long)((b1 - b2)>>8)))

#define FAST_DIST(r1,g1,b1,r2,g2,b2) (long)\
                   (abs((long)(r1 - r2)) \
		+   abs((long)(g1 - g2)) \
                +   abs((double)(b1 - b2)))

#define USED_DIST(r1,g1,b1,r2,g2,b2) TRUE_DIST(r1,g1,b1,r2,g2,b2)

#define PICTURE_COLOR_CLOSENESS USED_DIST(3333,3333,3333,0,0,0)

#define PICTURE_PAllocTable         1000000
#define PICTURE_PUseDynamicColors   100000
#define PICTURE_PStrictColorLimit   10000
#define PICTURE_use_named           1000
#define PICTURE_TABLETYPE_LENGHT    7

/* humm ... dither is probably borken with gamma correction. Anyway I do
 * do think that using gamma correction for the colors cubes is a good
 * idea */
#define USE_GAMMA_CORECTION 0
/* 2.2 is recommanded by the Poynon colors FAQ, some others suggest 1.5 and 2 
 * Use float constants!*/
#define COLOR_GAMMA 1.5
#define GREY_GAMMA  2.0

/* ---------------------------- imports ------------------------------------- */

/* ---------------------------- included code files ------------------------- */

/* ---------------------------- local types --------------------------------- */

typedef struct
{
	XColor color;               /* rgb color info */
	unsigned long alloc_count;  /* nbr of allocation */
} PColor;

typedef struct
{
	/*
	 * info for colors table (depth <= 8)
	 */
	/* color cube used */
	short nr;
	short ng;
	short nb;
	/* grey palette def, nbr of grey = 2^grey_bits */
	short grey_bits;
	/* color cube used for dithering with the named table */
	short d_nr;
	short d_ng;
	short d_nb;
       /* info for depth > 8 */
	int red_shift;
	int green_shift;
	int blue_shift;
	int red_prec;
	int green_prec;
	int blue_prec;
	/* for dithering in depth 15 and 16 */
	unsigned short *red_dither;
	unsigned short *green_dither;
	unsigned short *blue_dither;
} PColorsInfo;

typedef struct {
    int cols_index;
    long closeness;
}      CloseColor;

/* ---------------------------- forward declarations ------------------------ */

/* ---------------------------- local variables ----------------------------- */

static int PColorLimit = 0;
static PColor *Pct = NULL;
static short *PMappingTable = NULL;
static short *PDitherMappingTable = NULL;
static Bool PStrictColorLimit = 0;
static Bool PAllocTable = 0;
static PColorsInfo Pcsi = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, NULL};

/* ---------------------------- exported variables (globals) ---------------- */

/* ---------------------------- local functions ----------------------------- */

/* ***************************************************************************
 * get shift and prec from a mask (depth > 8)
 * ***************************************************************************/
static
void decompose_mask(
	unsigned long mask, int *shift, int *prec)
{
	*shift = 0;
	*prec = 0;

	while (!(mask & 0x1))
	{
		(*shift)++;
		mask >>= 1;
	}

	while (mask & 0x1)
	{
		(*prec)++;
		mask >>= 1;
	}
}

/* ***************************************************************************
 * color allocation in the colormap. strongly inspired by SetCloseColor from
 * the Xpm library (depth <= 8)
 * ***************************************************************************/

static int
closeness_cmp(const void *a, const void *b)
{
    CloseColor *x = (CloseColor *) a, *y = (CloseColor *) b;

    /* cast to int as qsort requires */
    return (int) (x->closeness - y->closeness);
}

static
int alloc_color_in_cmap(XColor *c, Bool force)
{
	static XColor colors[256];
	CloseColor closenesses[256];
	XColor tmp;
	int i,j;
	int map_entries = Pvisual->map_entries;
	time_t current_time;
	time_t last_time = 0;

	map_entries = (map_entries > 256)? 256:map_entries;
	current_time = time(NULL);
	if (current_time - last_time >= 2 || force)
	{
		last_time = current_time;
		for (i = 0; i < map_entries; i++)
		{
			colors[i].pixel = i;
		}
		XQueryColors(Pdpy, Pcmap, colors, map_entries);
	}
	for(i = 0; i < map_entries; i++)
	{
		closenesses[i].cols_index = i;
		closenesses[i].closeness = USED_DIST(
			(int)(c->red),
			(int)(c->green),
			(int)(c->blue),
			(int)(colors[i].red),
			(int)(colors[i].green),
			(int)(colors[i].blue));
	}
	qsort(closenesses, map_entries, sizeof(CloseColor), closeness_cmp);

	i = 0;
	j = closenesses[i].cols_index;
	while (force ||
	       (abs((long)c->red - (long)colors[j].red) <=
		PICTURE_COLOR_CLOSENESS &&
		abs((long)c->green - (long)colors[j].green) <=
		PICTURE_COLOR_CLOSENESS &&
		abs((long)c->blue - (long)colors[j].blue) <=
		PICTURE_COLOR_CLOSENESS))
	{
			tmp.red = colors[j].red;
			tmp.green = colors[j].green;
			tmp.blue = colors[j].blue;
			if (XAllocColor(Pdpy, Pcmap, &tmp))
			{
				c->red = tmp.red;
				c->green = tmp.green;
				c->blue = tmp.blue;
				c->pixel = tmp.pixel;
				return 1;
			}
			else
			{
				i++;
				if (i == map_entries)
					break;
				j = closenesses[i].cols_index;
			}
	}
	return 0;
}

/* ***************************************************************************
 * dithering
 * ***************************************************************************/

static
int my_dither(int x, int y, XColor *c)
{
        /* the dither matrice */
	static const char DM[128][128] = DITHER_MATRICE;
	int index;
	const char *dmp;

	if (Pcsi.grey_bits != 0)
	{
		/* Grey Scale */
		int prec = Pcsi.grey_bits;

		if (Pcsi.grey_bits == 1)
		{
			/* FIXME, can we do a better dithering */
			prec = 2;
		}
		dmp = DM[(0 + y) & (DM_HEIGHT - 1)];
		index = (c->green + ((c->blue + c->red) >> 1)) >> 1;
		index += (dmp[(0 + x) & (DM_WIDTH - 1)] << 2) >> prec;
		index = (index - (index >> prec));
		index = index >> (8 - Pcsi.grey_bits);
	}
	else
	{
		/* color cube */
		int dith, rs, gs, bs, gb, b;

		if (Pcsi.d_nr > 0)
		{
			rs = Pcsi.d_nr - 1;
			gs = Pcsi.d_ng - 1;
			bs = Pcsi.d_nb - 1;
			gb = Pcsi.d_ng*Pcsi.d_nb;
			b = Pcsi.d_nb;
		}
		else
		{
			rs = Pcsi.nr - 1;
			gs = Pcsi.ng - 1;
			bs = Pcsi.nb - 1;
			gb = Pcsi.ng*Pcsi.nb;
			b = Pcsi.nb;
		}
		dmp = DM[(0 + y) & (DM_HEIGHT - 1)];
		dith = (dmp[(0 + x) & (DM_WIDTH - 1)] << 2) | 7;
		c->red = ((c->red * rs) + dith) >> 8;
		c->green = ((c->green * gs) + (262 - dith)) >> 8;
		c->blue = ((c->blue * bs) + dith) >> 8;
		index = c->red * gb + c->green * b + c->blue;
		if (PDitherMappingTable != NULL)
		{
			index = PDitherMappingTable[index];
		}
	}
	return index;
}

static
int my_dither_depth_15_16_init(void)
{
	const unsigned char _dither_44[4][4] =
	{
		{0, 4, 1, 5},
		{6, 2, 7, 3},
		{1, 5, 0, 4},
		{7, 3, 6, 2}
	};
	int y,x,i;
	int rm = 0xf8, re = 0x7, gm = 0xfc, ge = 0x3, bm = 0xf8, be = 0x7;

	if (Pdepth == 16 && (Pvisual->red_mask == 0xf800) &&
	    (Pvisual->green_mask == 0x7e0) &&
	    (Pvisual->blue_mask == 0x1f))
	{
		/* ok */
	}
	else if (Pdepth == 15 && (Pvisual->red_mask == 0x7c00) &&
		 (Pvisual->green_mask == 0x3e0) &&
		 (Pvisual->blue_mask == 0x1f))
	{
		gm = 0xf8; ge = 0x7;
	}
	else
	{
		return 2; /* fail */
	}

	Pcsi.red_dither =
		(unsigned short *)safemalloc(4*4*256*sizeof(unsigned short));
	Pcsi.green_dither =
		(unsigned short *)safemalloc(4*4*256*sizeof(unsigned short));
	Pcsi.blue_dither =
		(unsigned short *)safemalloc(4*4*256*sizeof(unsigned short));

	for (y = 0; y < 4; y++)
	{
		for (x = 0; x < 4; x++)
		{
			for (i = 0; i < 256; i++)
			{
				if ((_dither_44[x][y] < (i & re)) &&
				    (i < (256 - 8)))
				{
					Pcsi.red_dither[
						(x << 10) | (y << 8) | i] =
						((i + 8) & rm) << 8;
				}
				else
				{
					Pcsi.red_dither[
						(x << 10) | (y << 8) | i] =
						(i & rm) << 8;
				}
				if ((_dither_44[x][y] < ((i & ge) << 1))
				    && (i < (256 - 4)))
				{
					Pcsi.green_dither[
						(x << 10) | (y << 8) | i] =
						((i + 4) & gm) << 8;
				}
				else
				{
					Pcsi.green_dither[
						(x << 10) | (y << 8) | i] =
						(i & gm) << 8;
				}
				if ((_dither_44[x][y] < (i & be)) &&
				    (i < (256 - 8)))
				{
					Pcsi.blue_dither[
						(x << 10) | (y << 8) | i] =
						((i + 8) & bm) << 8;
				}
				else
				{
					Pcsi.blue_dither[
						(x << 10) | (y << 8) | i] =
						(i & bm) << 8;
				}
			}
		}
	}
	return 1;
}

/* ***************************************************************************
 * Color allocation in the "palette" (depth <= 8)
 * ***************************************************************************/
static
int alloc_color_in_pct(XColor *c, int index)
{
	if (Pct[index].alloc_count == 0)
	{
		int s = PStrictColorLimit;

		PStrictColorLimit = 0;
		c->red = Pct[index].color.red;
		c->green = Pct[index].color.green;
		c->blue = Pct[index].color.blue;
		PictureAllocColor(Pdpy, Pcmap, c, True); /* WARN (rec) */
		Pct[index].color.pixel = c->pixel;
		Pct[index].alloc_count = 1;
		PStrictColorLimit = s;
	}
	else
	{
		c->red = Pct[index].color.red;
		c->green = Pct[index].color.green;
		c->blue = Pct[index].color.blue;
		c->pixel = Pct[index].color.pixel;
		if (Pct[index].alloc_count < 0xffffffff)
			(Pct[index].alloc_count)++;
	}
	return 1;
}

static
int get_color_index(int r, int g, int b, int is_8)
{
	int index;

	if (!is_8)
	{
		r= r >> 8;
		g= g >> 8;
		b= b >> 8;
	}
	if (Pcsi.grey_bits > 0)
	{
		/* FIXME: Use other proporition ? */
		index = ((r+g+b)/3) >> (8 - Pcsi.grey_bits);
	}
	else
	{
#if 1
		/* exact computation (linear dist) */
		float fr,fg,fb;
		int ir, ig, ib;

		fr = ((float)r * (Pcsi.nr-1))/255;
		fg = ((float)g * (Pcsi.ng-1))/255;
		fb = ((float)b * (Pcsi.nb-1))/255;

		ir = (int)fr + (fr - (int)fr > 0.5);
		ig = (int)fg + (fg - (int)fg > 0.5);
		ib = (int)fb + (fb - (int)fb > 0.5);

		index = ir * Pcsi.ng*Pcsi.nb + ig * Pcsi.nb + ib;
#else
		/* approximation; faster */
		index = ((r * Pcsi.nr)>>8) * Pcsi.ng*Pcsi.nb +
			((g * Pcsi.ng)>>8) * Pcsi.nb +
			((b * Pcsi.nb)>>8);
#endif
		if (PMappingTable != NULL)
		{
			index = PMappingTable[index];
		}
	}
	return index;
}

/* ***************************************************************************
 * local function for building the palette (depth <= 8)
 * ***************************************************************************/
static
XColor *build_mapping_colors(int nr, int ng, int nb)
{
	int r, g, b, i;
	XColor *colors;

	colors = (XColor *)safemalloc(nr*ng*nb * sizeof(XColor));
	i = 0;
	for (r = 0; r < nr; r++)
	{
		for (g = 0; g < ng; g++)
		{
			for (b = 0; b < nb; b++)
			{
				colors[i].red =
					r * 65535 / (nr - 1);
				colors[i].green =
					g * 65535 / (ng - 1);
				colors[i].blue =
					b * 65535 / (nb - 1);
				i++;
			}
		}
	}
	return colors;
}

short *build_mapping_table(int nr, int ng, int nb)
{
	int size = nr*ng*nb;
	XColor *colors_map;
	short *Table;
	int i,j, minind;
	double mindst = 40000;
	double dst;

	colors_map = build_mapping_colors(nr, ng, nb);
	Table = (short *)safemalloc((size+1) * sizeof(short));
	for(i=0; i<size; i++)
	{
		minind = 0;
		for(j=0; j<PColorLimit; j++)
		{
			dst = USED_DIST(colors_map[i].red,
					colors_map[i].green,
					colors_map[i].blue,
					Pct[j].color.red,
					Pct[j].color.green,
					Pct[j].color.blue);
			if (j == 0 || dst < mindst)
			{
				mindst=dst;
				minind=j;
			}
		}
		Table[i] = minind;
	}
	Table[size] = Table[size-1];
	free(colors_map);
	return Table;
}

static
void free_table_colors(PColor *color_table, int npixels)
{
	Pixel pixels[256];
	int i,n=0;

	if (npixels > 0)
	{
		for(i = 0; i < npixels; i++)
		{
			if (color_table[i].alloc_count)
			{
				pixels[n++] = color_table[i].color.pixel;
			}
			color_table[i].alloc_count = 0;
		}
		if (n > 0)
		{
			XFreeColors(Pdpy, Pcmap, pixels, n, 0);
		}
	}
}

static
int get_nbr_of_free_colors(int max_check)
{
	int check = 1;
	Pixel Pixels[256];

	if (max_check < 1)
		return 0;
	if (Pdepth > 8)
		return max_check;
	max_check = (max_check > Pvisual->map_entries) ?
		Pvisual->map_entries:max_check;
	while(1)
	{
		if (XAllocColorCells(
			Pdpy, Pcmap, False, NULL, 0, Pixels, check))
		{
			XFreeColors(Pdpy, Pcmap, Pixels, check, 0);
			check++;
		}
		else
		{
			return check-1;
		}
		if (check > max_check)
		{
			return check-1;
		}
	}
	return check-1;
}

static
PColor *alloc_color_cube(
	int nr, int ng, int nb, int ngrey, int grey_bits, Bool do_allocate)
{
	int r, g, b, grey, i, start_grey, end_grey;
	PColor *color_table;
	XColor color;
	int size;

	size = nr*ng*nb + ngrey + (1 << grey_bits)*(grey_bits != 0);
	if (grey_bits)
	{
		ngrey = (1 << grey_bits);
	}
	if (nr > 0 && ngrey > 0)
	{
		start_grey = 1;
		end_grey = ngrey - 1;
		size = size - 2;
	}
	else
	{
		start_grey = 0;
		end_grey = ngrey;
	}

	color_table = (PColor *)safemalloc((size+1) * sizeof(PColor));

	i = 0;

#if USE_GAMMA_CORECTION
#define CG(x) 65535.0 * pow((x)/65535.0,1/COLOR_GAMMA)
#define GG(x) 65535.0 * pow((x)/65535.0,1/GREY_GAMMA)
#else
#define CG(x) x
#define GG(x) x
#endif

	if (nr > 0)
	{
		for (r = 0; r < nr; r++)
		{
			for (g = 0; g < ng; g++)
			{
				for (b = 0; b < nb; b++)
				{
					color.red = CG(r * 65535 / (nr - 1));
					color.green = CG(g * 65535 / (ng - 1));
					color.blue = CG(b * 65535 / (nb - 1));
					if (do_allocate)
					{
						if (!XAllocColor(Pdpy, Pcmap,
								 &color))
						{
							free_table_colors(
								color_table, i);
							free(color_table);
							return NULL;
						}
						color_table[i].color.pixel =
							color.pixel;
						color_table[i].alloc_count = 1;
					}
					else
					{
						color_table[i].alloc_count = 0;
					}
					color_table[i].color.red = color.red;
					color_table[i].color.green = color.green;
					color_table[i].color.blue = color.blue;
					i++;
				}
			}
		}
	}
	
	if (ngrey > 0)
	{
		for (grey = start_grey; grey < end_grey; grey++)
		{
			color.red = color.green = color.blue =
				GG(grey * 65535 / (ngrey - 1));
			if (do_allocate)
			{
				if (!XAllocColor(Pdpy, Pcmap, &color))
				{
					free_table_colors(color_table, i);
					free(color_table);
					return NULL;
				}
				color_table[i].color.pixel = color.pixel;
				color_table[i].alloc_count = 1;
			}
			else
			{
				color_table[i].alloc_count = 0;
			}
			color_table[i].color.red = color.red;
			color_table[i].color.green = color.green;
			color_table[i].color.blue = color.blue;
			i++;
		}
	}
	color_table[size].color.red = color_table[size-1].color.red;
	color_table[size].color.green = color_table[size-1].color.green;
	color_table[size].color.blue = color_table[size-1].color.blue;
	color_table[size].color.pixel = color_table[size-1].color.pixel;
	color_table[size].alloc_count = 0;
	PColorLimit = size;
	return color_table;
}


static
PColor *alloc_named_ct(int *limit, Bool do_allocate)
{

/* First thing in base array are colors probably already in the color map
   because they have familiar names.
   I pasted them into a xpm and spread them out so that similar colors are
   spread out.
   Toward the end are some colors to fill in the gaps.
   Currently 61 colors in this list.
*/
	char *color_names[] =
	{
		"black",
		"white",
		"grey",
		"green",
		"blue",
		"red",
		"cyan",
		"yellow",
		"magenta",
		"DodgerBlue",
		"SteelBlue",
		"chartreuse",
		"wheat",
		"turquoise",
		"CadetBlue",
		"gray87",
		"CornflowerBlue",
		"YellowGreen",
		"NavyBlue",
		"MediumBlue",
		"plum",
		"aquamarine",
		"orchid",
		"ForestGreen",
		"lightyellow",
		"brown",
		"orange",
		"red3",
		"HotPink",
		"LightBlue",
		"gray47",
		"pink",
		"red4",
		"violet",
		"purple",
		"gray63",
		"gray94",
		"plum1",
		"PeachPuff",
		"maroon",
		"lavender",
		"salmon",           /* for peachpuff, orange gap */
		"blue4",            /* for navyblue/mediumblue gap */
		"PaleGreen4",       /* for forestgreen, yellowgreen gap */
		"#AA7700",          /* brick, no close named color */
		"#11EE88",          /* light green, no close named color */
		"#884466",          /* dark brown, no close named color */
		"#CC8888",          /* light brick, no close named color */
		"#EECC44",          /* gold, no close named color */
		"#AAAA44",          /* dull green, no close named color */
		"#FF1188",          /* pinkish red */
		"#992299",          /* purple */
		"#CCFFAA",          /* light green */
		"#664400",          /* dark brown*/
		"#AADD99",          /* light green */
		"#66CCFF",          /* light blue */
		"#CC2299",          /* dark red */
		"#FF11CC",          /* bright pink */
		"#11CC99",          /* grey/green */
		"#AA77AA",          /* purple/red */
		"#EEBB77"           /* orange/yellow */
	};
	int NColors = sizeof(color_names)/sizeof(char *);
	int i,rc;
	PColor *color_table;
	XColor color;

	*limit = (*limit > NColors)? NColors: *limit;
	color_table = (PColor *)safemalloc((*limit+1) * sizeof(PColor));
	for(i=0; i<*limit; i++)
	{
		rc=XParseColor(Pdpy, Pcmap, color_names[i], &color);
		if (rc==0) {
			fprintf(stderr,"color_to_rgb: can't parse color %s,"
				" rc %d\n", color_names[i], rc);
			free_table_colors(color_table, i);
			free(color_table);
			return NULL;
		}
		if (do_allocate)
		{
			if (!XAllocColor(Pdpy, Pcmap, &color))
			{
				free_table_colors(color_table, i);
				free(color_table);
				return NULL;
			}
			color_table[i].color.pixel = color.pixel;
			color_table[i].alloc_count = 1;
		}
		else
		{
			color_table[i].alloc_count = 0;
		}
		color_table[i].color.red = color.red;
		color_table[i].color.green = color.green;
		color_table[i].color.blue = color.blue;
	}
	color_table[*limit].color.red = color_table[*limit-1].color.red;
	color_table[*limit].color.green = color_table[*limit-1].color.green;
	color_table[*limit].color.blue = color_table[*limit-1].color.blue;
	color_table[*limit].color.pixel = color_table[*limit-1].color.pixel;
	color_table[*limit].alloc_count = 0;
	PColorLimit = *limit;
	return color_table;
}

#if PICTURE_DEBUG_COLORS_PRINT_CMAP
static
void print_colormap(void)
{
	XColor colors[256];
	int i;

	for (i = 0; i < Pvisual->map_entries; i++)
	{
		colors[i].pixel = i;
	}
	XQueryColors(Pdpy, Pcmap, colors, Pvisual->map_entries);
	for (i = 0; i < Pvisual->map_entries; i++)
	{
		fprintf(stderr,"%lu: %x %x %x",
			colors[i].pixel,
			colors[i].red >> 8,
			colors[i].green >> 8,
			colors[i].blue >> 8);
		if (Pct && i < PColorLimit)
		{
			fprintf(stderr,"  / %lu: %x %x %x\n",
				Pct[i].color.pixel,
				Pct[i].color.red >> 8,
				Pct[i].color.green >> 8,
				Pct[i].color.blue >> 8;
		}
		else
		{
			fprintf(stderr,"\n");
		}
	}
}
#endif


static
void create_mapping_table(
	int nr, int ng, int nb, int ngrey, int grey_bits, Bool use_named)
{

	Pcsi.grey_bits = 0;

	/* initialize dithering colors numbers */
	if (!use_named)
	{
		/* */
		Pcsi.d_nr = nr;
		Pcsi.d_ng = ng;
		Pcsi.d_nb = nb;
		Pcsi.grey_bits = grey_bits;
	}
	else
	{
		/* dither table should be small */
		Pcsi.grey_bits = 0;
		if (PColorLimit <= 9)
		{
			Pcsi.d_nr = 3;
			Pcsi.d_ng = 3;
			Pcsi.d_nb = 3;
		}
		else
		{
			Pcsi.d_nr = 4;
			Pcsi.d_ng = 4;
			Pcsi.d_nb = 4;
		}
		PDitherMappingTable = build_mapping_table(
			Pcsi.d_nr, Pcsi.d_ng, Pcsi.d_nb);
	}

	/* initialize colors number fo index computation */
	if (PColorLimit == 2)
	{
		/* ok */
		Pcsi.nr = 0;
		Pcsi.ng = 0;
		Pcsi.nb = 0;
		Pcsi.grey_bits = 1;
	}
	else if (grey_bits > 0)
	{
		Pcsi.nr = 0;
		Pcsi.ng = 0;
		Pcsi.nb = 0;
		Pcsi.grey_bits = grey_bits;
	}
	else if (use_named || (ngrey > 0))
	{
		if (PColorLimit <= 9)
		{
			Pcsi.nr = 8;
			Pcsi.ng = 8;
			Pcsi.nb = 8;
		}
		else
		{
			Pcsi.nr = 16;
			Pcsi.ng = 16;
			Pcsi.nb = 16;
		}
		PMappingTable = build_mapping_table(Pcsi.nr, Pcsi.ng, Pcsi.nb);
	}
	else
	{
		Pcsi.nr = nr;
		Pcsi.ng = ng;
		Pcsi.nb = nb;
		Pcsi.grey_bits = 0;
	}
}

void finish_ct_init(
	int call_type, int ctt, int nr, int ng, int nb, int ngrey, int grey_bits,
	Bool use_named)
{
#if PICTURE_DEBUG_COLORS_PRINT_CMAP
	if (call_type == PICTURE_CALLED_BY_FVWM)
		print_colormap();
#endif

	if (call_type == PICTURE_CALLED_BY_FVWM)
	{
		char *env;

		if (PAllocTable)
		{
			ctt = PICTURE_PAllocTable + ctt;
		}
		if (PUseDynamicColors)
		{
			ctt = PICTURE_PUseDynamicColors + ctt;
		}
		if (PStrictColorLimit)
		{
			ctt = PICTURE_PStrictColorLimit + ctt;
		}
		if (use_named)
		{
			ctt = PICTURE_use_named + ctt;
		}
		else
		{
			ctt++;
		}
		env = safemalloc(21+PICTURE_TABLETYPE_LENGHT+1);
		sprintf(env,"FVWM_COLORTABLE_TYPE=%i",ctt);
		putenv(env);
	}

#if PICTURE_DEBUG_COLORS_INFO
	if (call_type == PICTURE_CALLED_BY_FVWM)
	{
		fprintf(stderr,"[%s][PictureAllocColorTable]: "
			"Info -- "
			"use color table with:\n\t"
			"%i colours (%i,%i)\n",
			"FVWM", PColorLimit,
			get_nbr_of_free_colors(256),ctt);
	}
#endif

	if (Pct)
	{
		if (!PAllocTable && call_type == PICTURE_CALLED_BY_FVWM)
		{
			free_table_colors(Pct, PColorLimit);
		}
		create_mapping_table(nr,ng,nb,ngrey,grey_bits,use_named);
	}
}

/* ---------------------------- interface functions ------------------------- */

int PictureAllocColor(Display *dpy, Colormap cmap, XColor *c, int no_limit)
{

	if (PStrictColorLimit && Pct != NULL)
	{
		no_limit = 0;
	}
	if (Pdepth <= 8 && (no_limit || Pct == NULL) && (Pvisual->class & 1) &&
	    XAllocColor(dpy, cmap, c))
	{
		return 1;
	}
	else if (Pdepth <= 8 && no_limit && (Pvisual->class & 1))
	{
		/* color allocation fail, The Pct != NULL is here to check
		 * that we */
#if PICTURE_DEBUG_COLORS_ALLOC_FAILURE
		fprintf(stderr,"Color allocation fail for %x %x %x\n",
			c->red >> 8, c->green >> 8, c->blue >> 8);
#endif
		if (!alloc_color_in_cmap(c, False))
		{
			int status;

			XGrabServer(dpy);
			status = alloc_color_in_cmap(c, True);
			XUngrabServer(dpy);
#if PICTURE_DEBUG_COLORS_ALLOC_FAILURE
			fprintf(stderr,"\tuse(?) %x %x %x, %i\n",
				c->red >> 8, c->green >> 8, c->blue >> 8,
				status);
#endif
			return status;
		}
		else
		{
#if PICTURE_DEBUG_COLORS_ALLOC_FAILURE
			fprintf(stderr,"\tuse %x %x %x\n",
				c->red >> 8, c->green >> 8, c->blue >> 8);
#endif
			return 1;
		}
	}
	else if (Pct != NULL) /* and PColorLimit > 0 && Pdepth <= 8 &&
				 Pvisual->class & 1*/
	{
		int index;

		index = get_color_index(c->red,c->green,c->blue, False);
		return alloc_color_in_pct(c, index);
	}
	/* Pdepth > 8 or static colors */
	if (Pvisual->class == StaticGray)
	{
		/* FIXME: is this ok in general? */
		c->pixel = ((c->red + c->green + c->blue)/3);
		if (Pdepth < 16)
		{
			c->pixel = c->pixel >> (16 - Pdepth);
		}
		return 1;
	}
	if (Pcsi.red_shift == 0 && Pvisual->class != StaticGray)
	{
		decompose_mask(
			Pvisual->red_mask,&Pcsi.red_shift,&Pcsi.red_prec);
		decompose_mask(
			Pvisual->green_mask,&Pcsi.green_shift,&Pcsi.green_prec);
		decompose_mask(
			Pvisual->blue_mask,&Pcsi.blue_shift,&Pcsi.blue_prec);
	}
	c->pixel = (Pixel)(
		((c->red   >> (16 - Pcsi.red_prec))<< Pcsi.red_shift) +
		((c->green >> (16 - Pcsi.green_prec))<< Pcsi.green_shift) +
		((c->blue  >> (16 - Pcsi.blue_prec))<< Pcsi.blue_shift)
		);
	return 1;
}


int PictureAllocColorAllProp(
	Display *dpy, Colormap cmap, XColor *c, int x, int y,
	Bool no_limit, Bool is_8, Bool do_dither)
{
	int index;
	static int dither_ok = 0; /* not initalized, 1 ok, 2 init fail */
	int switcher =
		(do_dither && dither_ok != 2 && !no_limit &&
		 Pdepth <= 16 && (Pdepth > 8 || Pct != NULL))? Pdepth:0;

	if (dither_ok == 0 && (switcher == 15 || switcher == 16))
	{
		/* init dithering in depth 15 and 16 */
		if (Pcsi.red_shift == 0)
		{
			decompose_mask(
				Pvisual->red_mask,
				&Pcsi.red_shift,
				&Pcsi.red_prec);
			decompose_mask(
				Pvisual->green_mask,
				&Pcsi.green_shift,
				&Pcsi.green_prec);
			decompose_mask(
				Pvisual->blue_mask,
				&Pcsi.blue_shift,
				&Pcsi.blue_prec);
		}
		dither_ok = my_dither_depth_15_16_init();
		if (dither_ok == 2)
		{
			switcher = 0;
		}
	}

	switch(switcher)
	{
	case 0: /* no dithering and maybe no_limit */
		if (Pct == NULL || no_limit)
		{
			if (is_8)
			{
				c->red = c->red*257;
				c->green = c->green*257;
				c->blue = c->blue*257;
			}
			return PictureAllocColor(dpy, cmap, c, no_limit);
		}

		/* Pdepth <= 8, no dither */
		index = get_color_index(c->red,c->green,c->blue, is_8);
		return alloc_color_in_pct(c, index);
		break;
	case 16: /* depth 15 or 16 and dithering */
	case 15:
		if (!is_8)
		{
			c->red = c->red >> 8;
			c->green = c->green >> 8;
			c->blue = c->blue >> 8;
		}
		c->red = Pcsi.red_dither[
			(((x + 0) & 0x3) << 10) | ((y & 0x3) << 8) |
			((c->red) & 0xff)] * 257;
		c->green = Pcsi.green_dither[
			(((x + 0) & 0x3) << 10) | ((y & 0x3) << 8) |
			((c->green) & 0xff)] * 257;
		c->blue = Pcsi.blue_dither[
			(((x + 0) & 0x3) << 10) | ((y & 0x3) << 8) |
			((c->blue) & 0xff)] * 257;
		c->pixel = (Pixel)(
			((c->red >> (16 - Pcsi.red_prec)) << Pcsi.red_shift) +
			((c->green >> (16 - Pcsi.green_prec))
			 << Pcsi.green_shift) +
			((c->blue >> (16 - Pcsi.blue_prec)) << Pcsi.blue_shift)
			);
		return 1;
		break;
	default: /* Pdepth <= 8, dithering */
		if (!is_8)
		{
			c->red = c->red >> 8;
			c->green = c->green >> 8;
			c->blue = c->blue >> 8;
		}
		index = my_dither(x, y, c);
		return alloc_color_in_pct(c, index);
		break;
	}
}

void PictureFreeColors(
	Display *dpy, Colormap cmap, Pixel *pixels, int n, unsigned long planes,
	Bool no_limit)
{
	if (PStrictColorLimit && Pct != NULL)
	{
		no_limit = 0;
	}
	if (Pct != NULL && !no_limit)
	{
		Pixel *p;
		int i,j,do_free;
		int m = 0;

		if (!PUseDynamicColors)
		{
			return;
		}
		p = (Pixel *)safemalloc(n*sizeof(Pixel));
		for(i= 0; i < n; i++)
		{
			do_free = 1;
			for(j=0; j<PColorLimit; j++)
			{
				if (Pct[j].alloc_count &&
				    Pct[j].alloc_count < 0xffffffff &&
				    pixels[i] == Pct[j].color.pixel)
				{
					(Pct[j].alloc_count)--;
					if (Pct[j].alloc_count)
						do_free = 0;
					break;
				}
			}
			if (do_free)
			{
				p[m++] = pixels[i];
			}
		}
		if (m > 0)
		{
			XFreeColors(dpy, cmap, p, m, planes);
		}
		free(p);
		return;
	}
	if ((Pct == NULL || no_limit) && Pdepth <= 8)
	{
		XFreeColors(dpy, cmap, pixels, n, planes);
	}
	return;
}

Pixel PictureGetNextColor(Pixel p, int n)
{
	int i;
	XColor c;

	if (n >= 0)
		n = 1;
	else
		n = -1;

	if (Pct == NULL)
		return p;
	for(i=0; i<PColorLimit; i++)
	{
		if (Pct[i].color.pixel == p)
		{
			if (i == 0 && n < 0)
			{
				c = Pct[PColorLimit-1].color;
				alloc_color_in_pct(&c, PColorLimit-1);
				return Pct[PColorLimit-1].color.pixel;
			}
			else if (i == PColorLimit-1 && n > 0)
			{
				c = Pct[0].color;
				alloc_color_in_pct(&c, 0);
				return Pct[0].color.pixel;
			}
			else
			{
				c = Pct[i+n].color;
				alloc_color_in_pct(&c, i+n);
				return Pct[i+n].color.pixel;
			}
		}
	}
	return p;
}

/* Replace the color in my_color by the closest matching color
   from base_table */
void PictureReduceColorName(char **my_color)
{
	int index;
	XColor rgb;          /* place to calc rgb for each color in xpm */

	if (!XpmSupport)
		return;

	if (!strcasecmp(*my_color,"none")) {
		return; /* do not substitute the "none" color */
	}

	if (!XParseColor(Pdpy, Pcmap, *my_color, &rgb))
	{
		fprintf(stderr,"color_to_rgb: can't parse color %s\n",
			*my_color);
	}
	index = get_color_index(rgb.red,rgb.green,rgb.blue, False);
	/* Finally: replace the color string by the newly determined color
	 * string */
	free(*my_color);                    /* free old color */
	/* area for new color */
	*my_color = safemalloc(8);
	sprintf(*my_color,"#%x%x%x",
		Pct[index].color.red >> 8,
		Pct[index].color.green >> 8,
		Pct[index].color.blue >> 8); /* put it there */
	return;
}

#define PA_COLOR_CUBE (1 << 1)
#define FVWM_COLOR_CUBE (1 << 2)
#define PA_GRAY_SCALE (1 << 3)
#define FVWM_GRAY_SCALE (1 << 4)
#define ANY_COLOR_CUBE (PA_COLOR_CUBE|FVWM_COLOR_CUBE)
#define ANY_GRAY_SCALE (PA_GRAY_SCALE|FVWM_GRAY_SCALE)

int PictureAllocColorTable(char *opt, int call_type)
{
	char *envp;
	int free_colors, map_entries, limit, cc_nbr, i, size;
	int use_named_table = 0;
	int do_allocate = 0;
	int use_default = 1;
	int private_cmap = !(Pdefault);
	int dyn_cl_set = False;
	int strict_cl_set = False;
	int alloc_table_set = False;
	int color_limit = 256; /* not the default at all */
	int pa_type = (Pvisual->class != GrayScale)? PA_COLOR_CUBE:PA_GRAY_SCALE;
	int fvwm_type = (Pvisual->class != GrayScale)?
		FVWM_COLOR_CUBE:FVWM_GRAY_SCALE;
	int cc[][6] =
	{
		/* {nr,ng,nb,ngrey,grey_bits,logic} */

		/* 256 grey scale */
		{0, 0, 0, 0, 8, ANY_GRAY_SCALE},
		/* 244 Xrender XFree-4.2 */
		{6, 6, 6, 30, 0, ANY_COLOR_CUBE},
		/* 216 Xrender XFree-4.2,GTK/QT "default cc" */
		{6, 6, 6, 0, 0, ANY_COLOR_CUBE},
		/* 180 (GTK) */
		{6, 6, 5, 0, 0, ANY_COLOR_CUBE},
		/* 144 (GTK) */
		{6, 6, 4, 0, 0, ANY_COLOR_CUBE},
		/* 128 grey scale */
		{0, 0, 0, 0, 7, ANY_GRAY_SCALE},
		/* 125 GTK mini default cc (may change? 444) */
		{5, 5, 5, 0, 0, ANY_COLOR_CUBE},
		/* 100 (GTK with color limit) */
		{5, 5, 4, 0, 0, ANY_COLOR_CUBE},
		/* 85 Xrender XFree-4.3 */
		{4, 4, 4, 23, 0, ANY_COLOR_CUBE},
		/* 78 (in fact 76)  a good default ??*/
		{4, 4, 4, 16, 0, FVWM_COLOR_CUBE},
		/* 68  a good default ?? */
		{4, 4, 4, 6, 0, ANY_COLOR_CUBE},
		/* 64 Xrender XFree-4.3 (GTK wcl) */
		{4, 4, 4, 0, 0, ANY_COLOR_CUBE},
		/* 64 grey scale */
		{0, 0, 0, 0, 6, ANY_GRAY_SCALE},
		/* 54, maybe a good default? */
		{4, 4, 3, 8, 0, FVWM_COLOR_CUBE},
		/* 48, (GTK wcl) no grey but ok */
		{4, 4, 3, 0, 0, FVWM_COLOR_CUBE},
		/* 32 xrender xfree-4.2 */
		{0, 0, 0, 0, 6, ANY_GRAY_SCALE},
		/* 29 */
		{3, 3, 3, 4, 0, FVWM_COLOR_CUBE},
		/* 27 (xrender in depth 6&7(hypo) GTK wcl) */
		{3, 3, 3, 0, 0, FVWM_COLOR_CUBE|PA_COLOR_CUBE*(Pdepth<8)},
		/* 16 grey scale */
		{0, 0, 0, 0, 4, FVWM_GRAY_SCALE},
		/* 10 */
		{2, 2, 2, 4, 0, FVWM_COLOR_CUBE},
                /* 8 (xrender/qt/gtk wcl) */
		{2, 2, 2, 0, 0, FVWM_COLOR_CUBE},
		/* 8 grey scale Xrender depth 4 and XFree-4.3 */
		{0, 0, 0, 0, 3, FVWM_GRAY_SCALE|PA_GRAY_SCALE*(Pdepth<5)},
		/* 4 grey scale*/
		{0, 0, 0, 0, 2,
		 FVWM_GRAY_SCALE|FVWM_COLOR_CUBE|PA_COLOR_CUBE*(Pdepth<4)},
		/* 2 */
		{0, 0, 0, 0, 1, FVWM_COLOR_CUBE|FVWM_GRAY_SCALE}
	};

	cc_nbr = sizeof(cc)/(sizeof(cc[0]));
	map_entries = Pvisual->map_entries;

	/* by default use dynamic colors */
	PUseDynamicColors = 1;
	PAllocTable = 0;

	/* dynamically changeable visual class are odd numbered */
	if (Pdepth > 8 || !(Pvisual->class & 1))
	{
		PColorLimit = 0;
		PUseDynamicColors = 0;
		if (call_type == PICTURE_CALLED_BY_FVWM &&
		    getenv("FVWM_COLORTABLE_TYPE") != NULL)
		{
			putenv("FVWM_COLORTABLE_TYPE=");
		}
		return 0;
	}

#if PICTURE_DEBUG_COLORS_PRINT_CMAP
	if (module && StrEquals("FVWM",module))
		print_colormap();
#endif

	/* called by a module "allocate" directly */
	if (call_type == PICTURE_CALLED_BY_MODULE &&
	     (envp = getenv("FVWM_COLORTABLE_TYPE")) != NULL)
	{
		int nr = 0, ng = 0, nb = 0, grey_bits = 0, ngrey = 0;
		int ctt = atoi(envp);

		if (ctt >= PICTURE_PAllocTable)
		{
			ctt -= PICTURE_PAllocTable;
			PAllocTable = 1; /* not useful for a module !*/
		}
		if (ctt >= PICTURE_PUseDynamicColors)
		{
			PUseDynamicColors = 1;
			ctt -= PICTURE_PUseDynamicColors;
		}
		if (ctt >= PICTURE_PStrictColorLimit)
		{
			PStrictColorLimit = 1;
			ctt -= PICTURE_PStrictColorLimit;
		}
		if (ctt >= PICTURE_use_named)
		{
			ctt -= PICTURE_use_named;
			Pct = alloc_named_ct(&ctt, False);
			use_named_table = True;
		}
		else if (ctt == 0)
		{
			/* depth <= 8 and no colors limit ! */
			PColorLimit = 0;
			return 0;
		}
		else if (ctt <= cc_nbr)
		{
			ctt--;
			Pct = alloc_color_cube(
				cc[ctt][0], cc[ctt][1], cc[ctt][2], cc[ctt][3],
				cc[ctt][4],
				False);
			nr = cc[ctt][0];
			ng = cc[ctt][1];
			nb = cc[ctt][2];
			ngrey = cc[ctt][3];
			grey_bits = cc[ctt][4];
		}
		if (Pct != NULL)
		{
			/* should always happen */
			finish_ct_init(
				call_type, ctt, nr, ng, nb, ngrey, grey_bits,
				use_named_table);
			return PColorLimit;
		}
	}

	/* parse the color limit env variable */
	if ((envp = opt) != NULL || (envp = getenv("FVWM_COLORLIMIT")) != NULL)
	{
		char *rest, *l;

		rest = GetQuotedString(envp, &l, ":", NULL, NULL, NULL);
		if (l && *l != '\0' && (color_limit = atoi(l)) >= 0)
		{
			use_default = 0;
		}
		if (l != NULL)
		{
			free(l);
		}
		if (color_limit == 9 || color_limit == 61)
		{
			use_named_table = 1;
		}
		if (rest && *rest != '\0')
		{
			if (rest[0] == '1')
			{
				strict_cl_set = True;
				PStrictColorLimit = 1;
			}
			else
			{
				strict_cl_set = True;
				PStrictColorLimit = 0;
			}
			if (strlen(rest) > 1 && rest[1] == '1')
			{
				use_named_table = 1;
			}
			else
			{
				use_named_table = 0;
			}
			if (strlen(rest) > 2 && rest[2] == '1')
			{
				dyn_cl_set = True;
				PUseDynamicColors = 1;
			}
			else
			{
				dyn_cl_set = True;
				PUseDynamicColors = 0;
			}
			if (strlen(rest) > 3 && rest[3] == '1')
			{
				alloc_table_set = True;
				PAllocTable = 1;
			}
			else
			{
				alloc_table_set = True;
				PAllocTable = 0;
			}
		}
	}

	if (color_limit == 0)
	{
		/* should we forbid that ? Yes!*/
		use_default = 1;
		color_limit = 256;
#if 0
		PColorLimit = 0;
		finish_ct_init(
			call_type, 0, 0, 0, 0, 0, 0);
		return PColorLimit;
#endif
	}

	free_colors = get_nbr_of_free_colors(map_entries);

#if PICTURE_DEBUG_COLORS_INFO
	if (call_type == PICTURE_CALLED_BY_FVWM)
		fprintf(stderr,"[%s][PictureAllocColorTable]: "
			"Info -- Nbr of free colors before bulding the "
			"table: %i\n","FVWM", free_colors);
#endif

	/* first try to see if we have a "pre-allocated" color cube.
	 * The bultin RENDER X extension pre-allocate a color cube plus
	 * some grey's (xc/programs/Xserver/render/miindex)
	 * See gdk/gdkrgb.c for the cubes used by gtk+-2, 666 is the default,
	 * 555 is the minimal cc (this may change): if gtk cannot allocate
	 * the 555 cc (or better) a private cmap is used.
	 * for qt-3: see src/kernel/{qapplication.cpp,qimage.cpp,qcolor_x11.c}
	 * the 666 cube is used by default (with approx in the cmap if some
	 * color allocation fail), and some qt app may accept an
	 * --ncols option to limit the nbr of colors, then some "2:3:1"
	 * proportions color cube are used (222, 232, ..., 252, 342, ..., 362,
	 * 452, ...,693, ...)
	 * imlib2 try to allocate the 666 cube if this fail it try more
	 * exotic table (see rend.c and rgba.c) */
	i = 0;
	while(!private_cmap && use_default && i < cc_nbr && Pct == NULL)
	{
		size = cc[i][0]*cc[i][1]*cc[i][2] + cc[i][3] - 2*(cc[i][3] > 0) +
			(1 << cc[i][4])*(cc[i][4] != 0);
		if ((cc[i][5] & pa_type) && size <= map_entries &&
		    free_colors < map_entries - size)
		{
			Pct = alloc_color_cube(
				cc[i][0], cc[i][1], cc[i][2], cc[i][3], cc[i][4],
				True);
		}
		if (Pct != NULL)
		{
			if (free_colors <=
			    get_nbr_of_free_colors(map_entries))
			{
				/* done */
			}
			else
			{
				free_table_colors(Pct, PColorLimit);
				free(Pct);
				Pct = NULL;
			}
		}
		i++;
	}
	if (Pct != NULL)
	{
		if (!dyn_cl_set)
		{
			PUseDynamicColors = 0;
		}
		if (!alloc_table_set)
		{
			PAllocTable = 1;
		}
		i = i - 1;
		finish_ct_init(
			call_type, i, cc[i][0], cc[i][1], cc[i][2], cc[i][3],
			cc[i][4], 0);
		return PColorLimit;
	}

	/*
	 * now use "our" table
	 */

	limit = (color_limit >= map_entries)? map_entries:color_limit;
	if (use_default && !private_cmap)
	{
		/* XRender cvs default: */
#if 0
		if (limit > 100)
			limit = map_entries/3;
		else
			limit = map_entries/2;
		/* depth 8: 85 */
		/* depth 4: 8 */
#endif
		if (limit == 256)
		{
			if (Pvisual->class == GrayScale)
			{
				limit = 64;
			}
			else
			{
				limit = 54; /* 4x4x3 + 6 grey */
				/* other candidate:
				 * limit = 61 (named table)
				 * limit = 85 current XRender default 4cc + 21
				 * limit = 76 future(?) XRender default 4cc + 16
				 * limit = 68 4x4x4 + 4
				 * limit = 64 4x4x4 + 0 */
			}
			
		}
		else if (limit == 128 || limit == 64)
		{
			
			if (Pvisual->class == GrayScale)
			{
				limit = 32;
			}
			else
			{
				limit = 31;
			}
		}
		else if (limit >= 16)
		{
			if (Pvisual->class == GrayScale)
			{
				limit = 8;
			}
			else
			{
				limit = 10;
			}
		}
		else if (limit >= 8)
		{
			limit = 4;
		}
		else
		{
			limit = 2;
		}
	}
	if (limit < 2)
	{
		limit = 2;
	}
	if (PAllocTable)
	{
		do_allocate = 1;
	}
	else
	{
		do_allocate = 0;
	}

	/* use the named table ? */
	if (use_named_table)
	{
		i = limit;
		while(Pct == NULL && i >= 2)
		{
			Pct = alloc_named_ct(&i, do_allocate);
			i--;
		}
	}
	if (Pct != NULL)
	{
		finish_ct_init(
			call_type, PColorLimit, 0, 0, 0, 0, 0, 1);
		return PColorLimit;
	}

	/* color cube or regular grey scale */
	i = 0;
	while(i < cc_nbr && Pct == NULL)
	{
		if ((cc[i][5] & fvwm_type) &&
		    cc[i][0]*cc[i][1]*cc[i][2] + cc[i][3] - 2*(cc[i][3] > 0) +
		    (1 << cc[i][4])*(cc[i][4] != 0) <= limit)
		{
			Pct = alloc_color_cube(
				cc[i][0], cc[i][1], cc[i][2], cc[i][3], cc[i][4],
				do_allocate);
		}
		i++;
	}
	if (Pct != NULL)
	{
		i = i-1;
		finish_ct_init(
			call_type, i, cc[i][0], cc[i][1], cc[i][2], cc[i][3],
			cc[i][4], 0);
		return PColorLimit;
	}

	/* I do not think we can be here */
	Pct = alloc_color_cube(0, 0, 0, 0, 1, False);
	finish_ct_init(call_type, cc_nbr-1, 0, 0, 0, 0, 1, 0);
	return PColorLimit;
}

void PicturePrintColorInfo(int verbose)
{
	fprintf(stderr, "FVWM info on colors\n");
	fprintf(stderr, "  Visual -- ID: 0x%x, default: %s, Class:",
		(int)(Pvisual->visualid),
		(Pdefault)? "Yes":"No");
	if (Pvisual->class == TrueColor)
	{
		fprintf(stderr,"TrueColor");
	}
	else if (Pvisual->class == PseudoColor)
	{
		fprintf(stderr,"PseudoColor");
	}
	else if (Pvisual->class == DirectColor)
	{
		fprintf(stderr,"DirectColor");
	}
	else if (Pvisual->class == StaticColor)
	{
		fprintf(stderr,"StaticColor");
	}
	else if (Pvisual->class == GrayScale)
	{
		fprintf(stderr,"GrayScale");
	}
	else if (Pvisual->class == StaticGray)
	{
		fprintf(stderr,"StaticGray");
	}
	fprintf(stderr, "\n");
	fprintf(stderr, "  Depth: %i, Number of colors: %lu",
		Pdepth, (unsigned long)(1 << Pdepth));
	if (Pdepth > 8)
	{
		fprintf(stderr,  ", No Pallet (as depth > 8)\n");
		return;
	}
	else if (Pct == NULL)
	{
		fprintf(stderr,  ", No Pallet (static colors)\n");
		/* print the Pcmap ? */
		return;
	}
	else
	{
		fprintf(stderr,"\n  Pallet with: %i colors", PColorLimit);
		fprintf(stderr,"  Number of free color cell: %i colors\n",
			get_nbr_of_free_colors(Pvisual->map_entries));
		if (verbose)
		{
			int i;

			fprintf(stderr,"\n");
			for (i = 0; i < PColorLimit; i++)
			{
				fprintf(
					stderr,"    rgb:%i/%i/%i\t%lu\n",
					Pct[i].color.red,
					Pct[i].color.green,
					Pct[i].color.blue,
					Pct[i].alloc_count);
			}
		}
	}

	/* print the default cmap if !Pdefault ? */
}
