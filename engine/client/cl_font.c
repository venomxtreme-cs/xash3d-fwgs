/*
cl_font.c - bare bones engine font manager
Copyright (C) 2023 Alibek Omarov

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "common.h"
#include "filesystem.h"
#include "client.h"
#include "qfont.h"
#include "swaplib.h"

// A7 build 98: runtime TTF loader for the in-game HUD font. The engine had
// no .fnt credits fonts anywhere, so every HUD string (round end, center
// prints, scoreboard, buy menu, status lines) fell back to the blocky
// gfx/conchars bitmap. We rasterize a TrueType face (CS 1.6's Tahoma) to an
// alpha atlas at the final device pixel size with stb_truetype and hand it
// to the existing cl_font_t draw path. This header is public domain and
// already vendored in the menu tree; pulled in here for the engine.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

le_struct_begin( charinfo_swap )
	le_struct_field( charinfo, startoffset )
	le_struct_field( charinfo, charwidth )
le_struct_end();

le_struct_begin( qfont_swap )
	le_struct_field( qfont_t, width )
	le_struct_field( qfont_t, height )
	le_struct_field( qfont_t, rowcount )
	le_struct_field( qfont_t, rowheight )
	le_struct_array_child( qfont_t, fontinfo, charinfo_swap, NUM_GLYPHS )
le_struct_end();

qboolean CL_FixedFont( cl_font_t *font )
{
	return font && font->valid && font->type == FONT_FIXED;
}

static int CL_LoadFontTexture( const char *fontname, uint texFlags, int *width )
{
	if( !g_fsapi.FileExists( fontname, false ))
		return 0;

	int tex = ref.dllFuncs.GL_LoadTexture( fontname, NULL, 0, texFlags );
	if( !tex )
		return 0;

	int font_width = REF_GET_PARM( PARM_TEX_WIDTH, tex );
	if( !font_width )
	{
		ref.dllFuncs.GL_FreeTexture( tex );
		return 0;
	}

	*width = font_width;
	return tex;
}

static int CL_FontRenderMode( convar_t *fontrender )
{
	switch((int)fontrender->value )
	{
	case 0:
		return kRenderTransAdd;
	case 1:
		return kRenderTransAlpha;
	case 2:
		return kRenderTransTexture;
	default:
		Cvar_DirectSet( fontrender, fontrender->def_string );
	}

	return kRenderTransTexture;
}

void CL_SetFontRendermode( cl_font_t *font )
{
	ref.dllFuncs.GL_SetRenderMode( CL_FontRenderMode( font->rendermode ));
}

void CL_SetFontColor( cl_font_t *font, const rgba_t color )
{
	ref.dllFuncs.Color4ub( color[0], color[1], color[2], color[3] );
}

qboolean Con_LoadFixedWidthFont( const char *fontname, cl_font_t *font, float scale, convar_t *rendermode, uint texFlags )
{
	int font_width;

	if( !rendermode )
		return false;

	if( font->valid )
		return true; // already loaded

	font->hFontTexture = CL_LoadFontTexture( fontname, texFlags, &font_width );
	if( !font->hFontTexture )
		return false;

	font->type = FONT_FIXED;
	font->valid = true;
	font->scale = scale;
	font->rendermode = rendermode;
	font->charHeight = Q_rint( font_width / 16 * scale );

	for( int i = 0; i < ARRAYSIZE( font->fontRc ); i++ )
	{
		font->fontRc[i].left   = ( i * font_width / 16 ) % font_width;
		font->fontRc[i].right  = font->fontRc[i].left + font_width / 16;
		font->fontRc[i].top    = ( i / 16 ) * ( font_width / 16 );
		font->fontRc[i].bottom = font->fontRc[i].top + font_width / 16;

		font->charWidths[i] = Q_rint( font_width / 16 * scale );
	}

	return true;
}

qboolean Con_LoadVariableWidthFont( const char *fontname, cl_font_t *font, float scale, convar_t *rendermode, uint texFlags )
{
	fs_offset_t length;
	qfont_t src;
	int font_width;

	if( !rendermode )
		return false;

	if( font->valid )
		return true;

	byte *pfile = g_fsapi.LoadFile( fontname, &length, false );
	if( !pfile )
		return false;

	if( length < sizeof( src ))
	{
		Mem_Free( pfile );
		return false;
	}

	memcpy( &src, pfile, sizeof( src ));
	le_struct_swap( qfont_swap, &src );
	Mem_Free( pfile );

	font->hFontTexture = CL_LoadFontTexture( fontname, texFlags, &font_width );
	if( !font->hFontTexture )
		return false;

	font->type = FONT_VARIABLE;
	font->valid = true;
	font->scale = scale ? scale : 1.0f;
	font->rendermode = rendermode;
	font->charHeight = Q_rint( src.rowheight * scale );

	for( int i = 0; i < ARRAYSIZE( font->fontRc ); i++ )
	{
		const charinfo *ci = &src.fontinfo[i];

		font->fontRc[i].left   = (word)ci->startoffset % font_width;
		font->fontRc[i].right  = font->fontRc[i].left + ci->charwidth;
		font->fontRc[i].top    = (word)ci->startoffset / font_width;
		font->fontRc[i].bottom = font->fontRc[i].top + src.rowheight;

		font->charWidths[i] = Q_rint( src.fontinfo[i].charwidth * scale );
	}

	return true;
}

void CL_FreeFont( cl_font_t *font )
{
	if( !font || !font->valid )
		return;

	ref.dllFuncs.GL_FreeTexture( font->hFontTexture );
	memset( font, 0, sizeof( *font ));
}

static int CL_CalcTabStop( const cl_font_t *font, int x )
{
	int space = font->charWidths['0'];
	int tab   = space * 6; // 6 spaces
	int stop  = tab - x % tab;

	if( stop < space )
		return tab * 2 - x % tab; // select next

	return stop;
}

int CL_DrawCharacter( float x, float y, int number, const rgba_t color, cl_font_t *font, int flags )
{
	if( !font || !font->valid || y < -font->charHeight )
		return 0;

	// check if printable
	if( number <= 32 )
	{
		if( number == ' ' )
			return font->charWidths[' '];
		else if( number == '\t' )
			return CL_CalcTabStop( font, x );
		return 0;
	}

	if( FBitSet( flags, FONT_DRAW_UTF8 ))
		number = Con_UtfProcessChar( number & 255 );
	else number &= 255;

	if( !number || !font->charWidths[number])
		return 0;

	int texw, texh;
	R_GetTextureParms( &texw, &texh, font->hFontTexture );
	if( !texw || !texh )
		return font->charWidths[number];

	wrect_t *rc = &font->fontRc[number];
	float half = 0.5f;

	if( font->scale <= 1.f || !REF_GET_PARM( PARM_TEX_FILTERING, font->hFontTexture ))
		half = 0;

	float s1 = ((float)rc->left + half ) / texw;
	float t1 = ((float)rc->top + half ) / texh;
	float s2 = ((float)rc->right - half ) / texw;
	float t2 = ((float)rc->bottom - half ) / texh;
	float w = ( rc->right - rc->left ) * font->scale;
	float h = ( rc->bottom - rc->top ) * font->scale;

	if( FBitSet( flags, FONT_DRAW_HUD ))
		SPR_AdjustSize( &x, &y, &w, &h );

	if( !FBitSet( flags, FONT_DRAW_NORENDERMODE ))
		CL_SetFontRendermode( font );

	if( !FBitSet( flags, FONT_DRAW_NOCOLOR ))
		CL_SetFontColor( font, color );

	ref.dllFuncs.R_DrawStretchPic( x, y, w, h, s1, t1, s2, t2, font->hFontTexture );

	return font->charWidths[number];
}

int CL_DrawString( float x, float y, const char *s, const rgba_t color, cl_font_t *font, int flags )
{
	int draw_len = 0;

	if( !font || !font->valid )
		return 0;

	if( FBitSet( flags, FONT_DRAW_UTF8 ))
		Con_UtfProcessChar( 0 ); // clear utf state

	if( !FBitSet( flags, FONT_DRAW_NORENDERMODE ))
		CL_SetFontRendermode( font );

	CL_SetFontColor( font, color );

	SetBits( flags, FONT_DRAW_NOCOLOR | FONT_DRAW_NORENDERMODE );

	while( *s )
	{
		if( *s == '\n' )
		{
			s++;

			if( !*s )
				break;

			// some client functions ignore newlines
			if( !FBitSet( flags, FONT_DRAW_NOLF ))
			{
				draw_len = 0;
				y += font->charHeight;
			}

			if( FBitSet( flags, FONT_DRAW_RESETCOLORONLF ))
				 CL_SetFontColor( font, color );
			continue;
		}

		if( IsColorString( s ))
		{
			// don't copy alpha
			if( !FBitSet( flags, FONT_DRAW_FORCECOL ))
				CL_SetFontColor( font, g_color_table[ColorIndex(*( s + 1 ))] );

			s += 2;
			continue;
		}

		// skip setting rendermode, it was changed for this string already
		draw_len += CL_DrawCharacter( x + draw_len, y, (byte)*s, NULL, font, flags );

		s++;
	}

	return draw_len;
}

int CL_DrawStringf( cl_font_t *font, float x, float y, const rgba_t color, int flags, const char *fmt, ... )
{
	va_list va;
	char buf[MAX_VA_STRING];

	va_start( va, fmt );
	Q_vsnprintf( buf, sizeof( buf ), fmt, va );
	va_end( va );

	return CL_DrawString( x, y, buf, color, font, flags );
}

void CL_DrawCharacterLen( cl_font_t *font, int number, int *width, int *height )
{
	if( !font || !font->valid ) return;
	if( width )
	{
		if( number == '\t' )
			*width = CL_CalcTabStop( font, 0 ); // at least return max tabstop
		else *width = font->charWidths[number & 255];
	}
	if( height ) *height = font->charHeight;
}

void CL_DrawStringLen( cl_font_t *font, const char *s, int *width, int *height, int flags )
{
	int draw_len = 0;

	if( !font || !font->valid )
		return;

	if( height )
		*height = font->charHeight;

	if( width )
		*width = 0;

	if( COM_StringEmptyOrNULL( s ))
		return;

	if( FBitSet( flags, FONT_DRAW_UTF8 ))
		Con_UtfProcessChar( 0 ); // reset utf state

	while( *s )
	{
		int number;

		if( *s == '\n' )
		{
			// BUG: no check for end string here
			// but high chances somebody's relying on this
			s++;
			draw_len = 0;
			if( !FBitSet( flags, FONT_DRAW_NOLF ))
			{
				if( height )
					*height += font->charHeight;
			}
			continue;
		}
		else if( *s == '\t' )
		{
			draw_len += CL_CalcTabStop( font, 0 ); // at least return max tabstop
			s++;
			continue;
		}

		if( IsColorString( s ))
		{
			s += 2;
			continue;
		}

		if( FBitSet( flags, FONT_DRAW_UTF8 ))
			number = Con_UtfProcessChar( (byte)*s );
		else number = (byte)*s;

		if( number )
		{
			draw_len += font->charWidths[number];

			if( width )
			{
				if( draw_len > *width )
					*width = draw_len;
			}
		}

		s++;
	}
}

// A7 build 98: next power of two (atlas must be safe on every GLES driver)
static int CL_NextPow2( int v )
{
	int p = 1;
	while( p < v ) p <<= 1;
	return p;
}

// A7 build 103 (user: "there's fonts missing which is points not showing
// in hud / check all of them and make it show"): HUD strings are NOT raw
// ASCII - the client dll converts every one through ConvertToFontCharset()
// -> Con_UtfProcessChar() into the active console codepage first
// (cp1251 by default), so bullets, dashes, quotes, accented letters and
// the whole Cyrillic range arrive as bytes 127..255. The atlas stopped at
// 126, so every one of those cells was empty and the character silently
// did not render. This maps a single-byte codepage value back to the
// Unicode codepoint stb_truetype expects.
static int A7_FontCodepoint( int ch, int codepage )
{
	if( ch < 0x80 )
		return ch; // ASCII (DEL 0x7F has no glyph, handled by '?' below)

	if( codepage == 1251 )
	{
		// exact reverse of Q_UnicodeToCP1251(): 0x80..0xBF come from this
		// table, 0xC0..0xDF are U+0410..U+042F (A..Ya) and
		// 0xE0..0xFF are U+0430..U+044F (a..ya)
		static const uint16_t table_cp1251[64] = {
			0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
			0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
			0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
			0x007F, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
			0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
			0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
			0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
			0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457
		};

		if( ch >= 0xC0 && ch <= 0xDF )
			return 0x0410 + ( ch - 0xC0 );
		if( ch >= 0xE0 )
			return 0x0430 + ( ch - 0xE0 );
		if( ch >= 0x80 )
			return table_cp1251[ch - 0x80];
		return ch;
	}

	// cp1252 (and codepage 0): 0xA0..0xFF is Latin-1 = Unicode as-is, so
	// only the 0x80..0x9F window needs the Windows punctuation mapping
	static const uint16_t table_cp1252[32] = {
		0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
		0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
		0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
		0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
	}; // 0x81/0x8D/0x8F/0x90/0x9D are unmapped controls -> they fall to '?'

	if( ch >= 0x80 && ch < 0xA0 )
	{
		uint16_t u = table_cp1252[ch - 0x80];
		return u ? u : ch;
	}

	return ch;
}

/*
=============
Con_LoadTTFFont

A7 build 98: rasterize a TrueType face to an alpha atlas at the final
device pixel size and fill a variable-width cl_font_t. Honours the
hud_fontscale cvar by folding it into the raster pixel size (so glyphs are
never upscaled by the GPU - no blur). build 103: the atlas holds the FULL
single-byte range 32..255 in the active console codepage (A7_FontCodepoint)
instead of ASCII 32..126, so no HUD character can land in an empty cell and
a codepoint the face lacks falls back to '?' instead of vanishing. On any
failure it returns false and the caller falls back to the bitmap credits
font chain.
=============
*/
qboolean Con_LoadTTFFont( const char *ttfpath, cl_font_t *font, float scale, convar_t *rendermode, uint texFlags )
{
	fs_offset_t	length;
	stbtt_fontinfo	info;
	byte		*file, *rgba;
	int		i, px, ascentPx, linePx, cellW, cellH, atlasW, atlasH, cols, rows;
	int		advanceMax = 0;
	int		codepage;
	float		sscale;

	if( !rendermode || !font || font->valid )
		return font && font->valid;

	if( !g_fsapi.FileExists( ttfpath, false ))
		return false;

	file = g_fsapi.LoadFile( ttfpath, &length, false );
	if( !file )
		return false;

	if( length < 12 || !stbtt_InitFont( &info, file, 0 ))
	{
		Mem_Free( file );
		return false;
	}

	// build 103: the codepage the client dll converts HUD strings into
	codepage = Con_GetCodepage();

	// final raster size: a readable base tied to the HUD font scale, so a
	// bigger hud_fontscale re-rasters sharper instead of upscaling a small
	// atlas. clamped to a sane range.
	px = Q_rint( 18.0f * ( scale > 0.0f ? scale : 1.0f ));
	px = bound( 8, px, 64 );

	sscale = stbtt_ScaleForPixelHeight( &info, px );

	{
		int asc, desc, gap;
		stbtt_GetFontVMetrics( &info, &asc, &desc, &gap );
		ascentPx = Q_rint( asc * sscale );
		linePx   = Q_rint(( asc - desc ) * sscale );
	}
	if( linePx < 1 ) linePx = px;

	// widest advance across the whole single-byte range -> uniform cell
	// width. build 103: measured through the codepage mapping, with a '?'
	// stand-in whenever the face has no advance for the byte
	for( i = 32; i < 256; i++ )
	{
		int adv, lsb;
		int cp = A7_FontCodepoint( i, codepage );

		stbtt_GetCodepointHMetrics( &info, cp, &adv, &lsb );

		if( !adv )
		{
			stbtt_GetCodepointHMetrics( &info, '?', &adv, &lsb );
			if( !adv ) adv = px / 2;
		}

		if( adv > advanceMax ) advanceMax = adv;
	}

	{
		const int pad = 3;
		cellW = Q_rint( advanceMax * sscale ) + pad * 2;
		cellH = linePx + pad * 2;
		if( cellW < 1 ) cellW = 1;
		if( cellH < 1 ) cellH = 1;
	}

	cols = 16;
	// build 103: cells 32..255 = 224 slots (ASCII was 95), 16 per row
	rows = ( 224 + cols - 1 ) / cols;
	atlasW = CL_NextPow2( cols * cellW );
	atlasH = CL_NextPow2( rows * cellH );

	rgba = Z_Calloc( atlasW * atlasH * 4 );
	if( !rgba )
	{
		Mem_Free( file );
		return false;
	}

	// white RGB, alpha = glyph coverage. Color comes from the rendermode
	// tint (Color4ub) at draw time, so one atlas serves every HUD color.
	for( i = 32; i < 256; i++ )
	{
		int	col = ( i - 32 ) % cols;
		int	row = ( i - 32 ) / cols;
		int	x0  = col * cellW;
		int	y0  = row * cellH;
		int	cp  = A7_FontCodepoint( i, codepage );
		int	gw = 0, gh = 0, xoff = 0, yoff = 0, adv = 0, lsb = 0;
		byte	*glyph;
		int	gx, gy;

		stbtt_GetCodepointHMetrics( &info, cp, &adv, &lsb );
		glyph = stbtt_GetCodepointBitmap( &info, sscale, sscale, cp, &gw, &gh, &xoff, &yoff );

		// build 103: a cell must never come out blank - if the face has
		// no glyph for this codepage byte (control codes, unmapped
		// slots), swap in '?' so the character still shows instead of
		// disappearing from the HUD ("points not showing"). the space
		// cell is the one intentional empty: its width is fixed up below.
		if( i != ' ' && ( !glyph || !adv ))
		{
			if( glyph )
				stbtt_FreeBitmap( glyph, NULL );

			cp = '?';
			stbtt_GetCodepointHMetrics( &info, cp, &adv, &lsb );
			glyph = stbtt_GetCodepointBitmap( &info, sscale, sscale, cp, &gw, &gh, &xoff, &yoff );
		}

		if( !glyph )
			continue;

		// baseline sits at (ascentPx) from the top of the cell; yoff is
		// the glyph's offset from that baseline (usually negative = up)
		gx = x0 + 3 + xoff;
		gy = y0 + 3 + ascentPx + yoff;

		for( int yy = 0; yy < gh; yy++ )
		{
			int	dy = gy + yy;
			if( dy < 0 || dy >= atlasH ) continue;

			for( int xx = 0; xx < gw; xx++ )
			{
				int	dx = gx + xx;
				byte	a;

				if( dx < 0 || dx >= atlasW ) continue;

				a = glyph[yy * gw + xx];
				if( !a ) continue;

				// coverage drives BOTH rgb and alpha: additive (the
				// default hud_fontrender=0, same as classic conchars)
				// blends on rgb so the black cell background adds
				// nothing, while the alpha modes blend on a. one atlas
				// serves every render mode and every tint color.
				rgba[( dy * atlasW + dx ) * 4 + 0] = a;
				rgba[( dy * atlasW + dx ) * 4 + 1] = a;
				rgba[( dy * atlasW + dx ) * 4 + 2] = a;
				rgba[( dy * atlasW + dx ) * 4 + 3] = a;
			}
		}

		stbtt_FreeBitmap( glyph, NULL );

		// full-cell rect: consistent line height/baseline for multi-line
		// HUD strings. horizontal spacing uses the real advance below.
		font->fontRc[i].left   = (word)x0;
		font->fontRc[i].top    = (word)y0;
		font->fontRc[i].right  = (word)( x0 + cellW );
		font->fontRc[i].bottom = (word)( y0 + cellH );

		font->charWidths[i] = (byte)bound( 1, Q_rint( adv * sscale ), 255 );
	}

	Mem_Free( file );

	font->hFontTexture = ref.dllFuncs.GL_CreateTexture( "*a7hudfont", atlasW, atlasH,
		rgba, texFlags | TF_HAS_ALPHA );
	Mem_Free( rgba );

	if( !font->hFontTexture )
		return false;

	font->type = FONT_VARIABLE;
	font->valid = true;
	font->scale = 1.0f; // rasterized at final size, no GPU scaling
	font->rendermode = rendermode;
	font->charHeight = linePx;

	// space needs an explicit width (its bitmap is empty)
	if( !font->charWidths[' '] )
		font->charWidths[' '] = (byte)bound( 1, cellW / 3, 255 );

	Con_DPrintf( "HUD TTF: %s -> atlas %dx%d, cell %dx%d, px %d\n",
		ttfpath, atlasW, atlasH, cellW, cellH, px );

	return true;
}
