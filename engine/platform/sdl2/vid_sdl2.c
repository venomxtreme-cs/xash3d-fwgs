/*
vid_sdl.c - SDL vid component
Copyright (C) 2018 a1batross

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#include <SDL.h>
#include <SDL_config.h>
#include "common.h"
#include "client.h"
#include "vid_common.h"
#include "platform_sdl2.h"

// include it after because it breaks definitions in net_api.h wtf
#include <SDL_syswm.h>

#if XASH_PSVITA
#include <vrtld.h>
#endif // XASH_PSVITA

#if XASH_ANDROID
#include <android/native_window.h>
#include <ctype.h>
#endif // XASH_ANDROID

static vidmode_t *vidmodes = NULL;
static int num_vidmodes = 0;
static void GL_SetupAttributes( void );
static void VID_ShowWindowOnce( void ); // A7 build 146, defined next to VID_CreateWindow
static struct
{
	int prev_width, prev_height;
} sdlState = { 640, 480 };

#if XASH_ANDROID
// A7 build 152 (round 151, user: "add feature screen resolution for android,
// it doesn't have"): the Android window IS the panel and every SDL size call
// on it is a no-op, so the Video-modes picker had nothing to act on - the
// build-147 native fallback listed a single row and vid_setmode changed
// nothing. The surface BUFFER size is the moving part:
// ANativeWindow_setBuffersGeometry swaps it while SurfaceFlinger keeps
// scaling the smaller buffer over the whole panel (aspect-preserving rows)
// and the engine already carries a window/<->render split for it
// (R_SaveVideoMode's render_w/render_h, the HiDPI path). The picker works
// through the regular vid_setmode flow; the buffer geometry survives no
// surface teardown, so a recreate restore is caught in VID_SaveWindowSize
// and reapplied. s_android_native_* is the panel size seen at window setup,
// before any buffers-geometry call - SDL's window struct only ever moves
// from the Java surfaceChanged path, which reports the panel.
static int s_android_w, s_android_h; // last applied buffer geometry, 0 = native
static int s_android_native_w, s_android_native_h;

static qboolean VID_AndroidGetWindow( ANativeWindow **out )
{
	SDL_SysWMinfo wminfo;

	SDL_VERSION( &wminfo.version );

	if( !host.hWnd || !SDL_GetWindowWMInfo( host.hWnd, &wminfo ))
		return false;

	if( wminfo.subsystem != SDL_SYSWM_ANDROID || !wminfo.info.android.window )
		return false;

	*out = wminfo.info.android.window;
	return true;
}

static qboolean VID_AndroidApplySurfaceResolution( int w, int h )
{
	ANativeWindow *window;

	if( !VID_AndroidGetWindow( &window ))
	{
		Con_Printf( S_ERROR "build152: no ANativeWindow for render resolution %dx%d\n", w, h );
		return false;
	}

	if( ANativeWindow_setBuffersGeometry( window, w, h, 0 ) < 0 )
	{
		Con_Printf( S_ERROR "build152: ANativeWindow_setBuffersGeometry %dx%d failed\n", w, h );
		return false;
	}

	s_android_w = w;
	s_android_h = h;
	Con_Printf( "build152: android render resolution %dx%d applied\n", w, h );
	return true;
}

static qboolean VID_AndroidScaleActive( void )
{
	return s_android_w > 0 && ( s_android_w != s_android_native_w || s_android_h != s_android_native_h );
}

// A7 build 153 (round 152, user: "that's not real screen resolution, it
// just change hud_scale"): REAL panel modes from XashActivity's JNI
// getDisplayModes ("WxH@R;WxH@R...", deduped to distinct sizes). With >= 2
// distinct panel sizes the Video-modes picker lists THEM and a pick goes
// through Android_SetPreferredDisplayMode (preferredDisplayModeId) - the
// panel itself switches, the surface follows with a normal RESIZED event,
// and the pixel density really changes. A single-size device keeps the
// build-152 buffer ladder. The buffer-geometry path is unreachable on
// real-mode devices: a fractional stale-config row would only distort.
// The query is lazy + once: the FIRST VID_SetScreenResolution (window
// creation, boot config apply) runs BEFORE R_InitVideoModes, and a
// persisted real mode must be applied through the panel at boot too.
static int s_android_real_w[16], s_android_real_h[16], s_android_real_rows;
static qboolean s_android_panel_queried = false;

static void VID_AndroidParsePanelModes( const char *list )
{
	const char *p = list;
	int rows = 0;

	s_android_real_rows = 0;

	while( *p && rows < ( int )ARRAYSIZE( s_android_real_w ))
	{
		int w = 0, h = 0;

		while( isdigit(( unsigned char )*p ))
			w = w * 10 + ( *p++ - '0' );

		if( *p == 'x' )
		{
			p++;

			while( isdigit(( unsigned char )*p ))
				h = h * 10 + ( *p++ - '0' );

			while( *p && *p != ';' )
				p++;

			if( *p == ';' )
				p++;

			// one entry per DISTINCT size - the refresh rate is picked
			// Java-side (closest to the mode the panel is running)
			if( w > 0 && h > 0 )
			{
				int j;

				for( j = 0; j < rows; j++ )
				{
					if( s_android_real_w[j] == w && s_android_real_h[j] == h )
						break;
				}

				if( j == rows )
				{
					s_android_real_w[rows] = w;
					s_android_real_h[rows] = h;
					rows++;
				}
			}
		}
		else
		{
			while( *p && *p != ';' )
				p++;

			if( *p == ';' )
				p++;
		}
	}

	s_android_real_rows = rows;
}

static void VID_AndroidQueryPanelModes( void )
{
	char list[1024];

	if( s_android_panel_queried )
		return;

	s_android_panel_queried = true;

	Android_GetDisplayModes( list, sizeof( list ));

	if( list[0] )
		VID_AndroidParsePanelModes( list );
}

static qboolean VID_AndroidRealMode( int w, int h )
{
	int i;

	for( i = 0; i < s_android_real_rows; i++ )
	{
		if( s_android_real_w[i] == w && s_android_real_h[i] == h )
			return true;
	}

	return false;
}
#endif // XASH_ANDROID


struct
{
	SDL_Renderer *renderer;
	SDL_Texture *tex;
	int width, height;
	SDL_Surface *surf;
	SDL_Surface *win;
} sw;

void Platform_Minimize_f( void )
{
	if( host.hWnd )
		SDL_MinimizeWindow( host.hWnd );
}

qboolean SW_CreateBuffer( int width, int height, uint *stride, uint *bpp, uint *r, uint *g, uint *b )
{
	sw.width = width;
	sw.height = height;

	if( sw.renderer )
	{
		unsigned int format = SDL_GetWindowPixelFormat( host.hWnd );
		SDL_RenderSetLogicalSize( sw.renderer, refState.width, refState.height );

		if( sw.tex )
		{
			SDL_DestroyTexture( sw.tex );
			sw.tex = NULL;
		}

		// guess
		if( format == SDL_PIXELFORMAT_UNKNOWN )
		{
			if( refState.desktopBitsPixel == 16 )
				format = SDL_PIXELFORMAT_RGB565;
			else
				format = SDL_PIXELFORMAT_RGBA8888;
		}

		// we can only copy fast 16 or 32 bits
		// SDL_Renderer does not allow zero-copy, so 24 bits will be ineffective
		if( SDL_BYTESPERPIXEL( format ) != 2 && SDL_BYTESPERPIXEL( format ) != 4 )
			format = SDL_PIXELFORMAT_RGBA8888;

		sw.tex = SDL_CreateTexture( sw.renderer, format, SDL_TEXTUREACCESS_STREAMING, width, height );

		// fallback
		if( !sw.tex && format != SDL_PIXELFORMAT_RGBA8888 )
		{
			format = SDL_PIXELFORMAT_RGBA8888;
			sw.tex = SDL_CreateTexture( sw.renderer, format, SDL_TEXTUREACCESS_STREAMING, width, height );
		}

		if( !sw.tex )
		{
			SDL_DestroyRenderer( sw.renderer );
			sw.renderer = NULL;
		}
		else
		{
			void *pixels;
			int pitch;

			if( !SDL_LockTexture( sw.tex, NULL, &pixels, &pitch ))
			{
				// lock successfull, release
				SDL_UnlockTexture( sw.tex );

				// enough for building blitter tables
				int bits;
				uint amask;
				SDL_PixelFormatEnumToMasks( format, &bits, r, g, b, &amask );
				*bpp = SDL_BYTESPERPIXEL( format );
				*stride = pitch / *bpp;

				return true;
			}

			// fallback to surf
			SDL_DestroyTexture( sw.tex );
			sw.tex = NULL;
			SDL_DestroyRenderer( sw.renderer );
			sw.renderer = NULL;
		}
	}

	if( !sw.renderer )
	{
		sw.win = SDL_GetWindowSurface( host.hWnd );

		// sdl will create renderer if hw framebuffer unavailiable, so cannot fallback here
		// if it is failed, it is not possible to draw with SDL in REF_SOFTWARE mode
		if( !sw.win )
		{
			Sys_Warn( "failed to initialize software output, try running with -glblit flag" );
			return false;
		}

		*bpp = sw.win->format->BytesPerPixel;
		*r = sw.win->format->Rmask;
		*g = sw.win->format->Gmask;
		*b = sw.win->format->Bmask;
		*stride = sw.win->pitch / sw.win->format->BytesPerPixel;

		/// TODO: check somehow if ref_soft can handle native format
#if 0
		{
			sw.surf = SDL_CreateRGBSurfaceWithFormat( 0, width, height, 16, SDL_PIXELFORMAT_RGB565 );
			if( !sw.surf )
				Sys_Error( "%s: %s", __func__, SDL_GetError( ));
		}
#endif
		return true;
	}

	// we can't create ref_soft buffer
	return false;
}

void *SW_LockBuffer( void )
{
	if( sw.renderer )
	{
		void *pixels;
		int stride;

		if( SDL_LockTexture(sw.tex, NULL, &pixels, &stride ) < 0 )
			Sys_Error( "%s: %s", __func__, SDL_GetError( ));
		return pixels;
	}

	// ensure it not changed (do we really need this?)
	sw.win = SDL_GetWindowSurface( host.hWnd );
	//if( !sw.win )
		//SDL_GetWindowSurface( host.hWnd );

	// prevent buffer overrun
	if( !sw.win || sw.win->w < sw.width || sw.win->h < sw.height  )
		return NULL;

	if( sw.surf )
	{
		SDL_LockSurface( sw.surf );
		return sw.surf->pixels;
	}
	else
	{
		// real window pixels (x11 shm region, dma buffer, etc)
		// or SDL_Renderer texture if not supported
		SDL_LockSurface( sw.win );
		return sw.win->pixels;
	}
}

void SW_UnlockBuffer( void )
{
	if( sw.renderer )
	{
		SDL_Rect src =
		{
			.w = sw.width,
			.h = sw.height,
		};
		SDL_Rect dst = src;
		SDL_UnlockTexture(sw.tex);

		SDL_SetTextureBlendMode(sw.tex, SDL_BLENDMODE_NONE);


		SDL_RenderCopy(sw.renderer, sw.tex, &src, &dst);
#if !XASH_MOBILE_PLATFORM
		VID_ShowWindowOnce(); // A7 build 146: same as GL_SwapBuffers
#endif
		SDL_RenderPresent(sw.renderer);

		return;
		//Con_Printf("%s\n", SDL_GetError());
	}

	// blit if blitting surface availiable
	if( sw.surf )
	{
		SDL_Rect src =
		{
			.w = sw.width,
			.h = sw.height,
		};
		SDL_Rect dst = src;
		SDL_UnlockSurface( sw.surf );
		SDL_BlitSurface( sw.surf, &src, sw.win, &dst );
		return;
	}

	// already blitted
	SDL_UnlockSurface( sw.win );

#if !XASH_MOBILE_PLATFORM
	VID_ShowWindowOnce(); // A7 build 146: same as GL_SwapBuffers
#endif
	SDL_UpdateWindowSurface( host.hWnd );
}

int R_MaxVideoModes( void )
{
	return num_vidmodes;
}

vidmode_t *R_GetVideoMode( int num )
{
	if( !vidmodes || num < 0 || num >= R_MaxVideoModes() )
	{
		return NULL;
	}

	return vidmodes + num;
}

static void R_InitVideoModes( void )
{
	int display_index = 0;

#if XASH_ANDROID
	// A7 build 153 (round 152, user: "that's not real screen resolution, it
	// just change hud_scale"): the table now prefers the panel's REAL
	// display modes (XashActivity JNI list, >= 2 distinct sizes) - picking
	// one switches the panel itself (preferredDisplayModeId), which is what
	// "screen resolution" means on a phone. The build-152 fraction ladder
	// stays ONLY for devices that expose a single panel size (their only
	// real mode is the panel; the ladder then still saves GPU fill as a
	// render-scale).
	{
		int real_rows;

		num_vidmodes = 0;

		VID_AndroidQueryPanelModes( );
		real_rows = s_android_real_rows;

		if( real_rows >= 2 )
		{
			// REAL resolution table. The panel's CURRENT size goes first so
			// the picker highlights what is actually running; the rest keep
			// the list order the panel reported.
			int order[ARRAYSIZE( s_android_real_w )];
			int rows = 0, i, native_idx = -1;

			vidmodes = Mem_Malloc( host.mempool, sizeof( vidmodes[0] ) * real_rows );

			for( i = 0; i < real_rows; i++ )
			{
				if( s_android_real_w[i] == s_android_native_w && s_android_real_h[i] == s_android_native_h )
				{
					native_idx = i;
					break;
				}
			}

			if( native_idx >= 0 )
				order[rows++] = native_idx;

			for( i = 0; i < real_rows; i++ )
			{
				if( i == native_idx )
					continue;

				order[rows++] = i;
			}

			for( i = 0; i < rows; i++ )
			{
				char buf[MAX_VA_STRING];

				vidmodes[i].width = s_android_real_w[order[i]];
				vidmodes[i].height = s_android_real_h[order[i]];
				Q_snprintf( buf, sizeof( buf ), "%ix%i", vidmodes[i].width, vidmodes[i].height );
				vidmodes[i].desc = copystring( buf );
			}

			num_vidmodes = rows;

			Con_Printf( "[A7] build153: android panel modes %d rows\n", num_vidmodes );

			(void)display_index;
			return;
		}

		s_android_real_rows = 0;
		// single-size panel: the build-152 fraction ladder below is the
		// whole table (see its comment)
	}
#endif // XASH_ANDROID

#if XASH_ANDROID
	// A7 build 152 (round 151, user: "add feature screen resolution for
	// android, it doesn't have"): SDL display-mode lists are meaningless on
	// a phone - the panel is the only mode the OS gives, so the table
	// collapsed to a single native row (build-147 fallback) and picking any
	// ladder entry did nothing. Offer an aspect-preserving fraction ladder
	// off the panel instead: every row is applied as a surface buffer size
	// (see VID_SetScreenResolution) and SurfaceFlinger scales it back over
	// the full screen, so 50% still fills the window - same picture, a
	// quarter of the pixels. The panel row is always first so GetConfig
	// highlights what is actually running.
	{
		const float ladder[] = { 1.0f, 0.75f, 0.5f, 0.375f, 0.25f };
		const int native_w = s_android_native_w;
		const int native_h = s_android_native_h;
		int rows = 0;

		num_vidmodes = 0;

		if( native_w >= VID_MIN_WIDTH && native_h >= VID_MIN_HEIGHT )
		{
			vidmodes = Mem_Malloc( host.mempool, sizeof( vidmodes[0] ) * ARRAYSIZE( ladder ));

			for( size_t i = 0; i < ARRAYSIZE( ladder ); i++ )
			{
				int w = ( int )( native_w * ladder[i] ) & ~1;
				int h = ( int )( native_h * ladder[i] ) & ~1;
				char buf[MAX_VA_STRING];
				int j;

				if( w < VID_MIN_WIDTH || h < VID_MIN_HEIGHT )
					continue;

				for( j = 0; j < rows; j++ )
				{
					if( w == vidmodes[j].width && h == vidmodes[j].height )
						break;
				}

				if( j != rows )
					continue;

				vidmodes[rows].width = w;
				vidmodes[rows].height = h;
				Q_snprintf( buf, sizeof( buf ), "%ix%i", w, h );
				vidmodes[rows].desc = copystring( buf );
				rows++;
			}

			num_vidmodes = rows;
		}

		Con_Printf( "[A7] build152: android resolution ladder %d rows (native %dx%d)\n",
			num_vidmodes, native_w, native_h );

		if( num_vidmodes == 0 )
		{
			// extremely narrow panel: keep the mainui fallback belt alive
			if( vidmodes )
			{
				Mem_Free( vidmodes );
				vidmodes = NULL;
			}
		}

		(void)display_index;
		return;
	}
#endif // XASH_ANDROID

	// A7 round 147 item 8 (user: "return + improve the Android screen-
	// resolution picker (dev's xash remove it) on both platforms"): the
	// table is NEVER allowed to come up empty. SDL on Android commonly
	// reports 0 display modes - that empty table is what the old picker
	// died on - so fall back to the panel's desktop mode and then to the
	// RUNNING window size; and whatever list SDL does give us, the
	// running size is appended if missing so the picker always carries
	// what is actually on screen. mainui keeps its build-143 synthesized
	// ladder as the second belt.
	num_vidmodes = 0;
	int modes = SDL_GetNumDisplayModes( display_index );
	int fb_w = 0, fb_h = 0;
	qboolean fb_fallback = false;

	if( modes <= 0 )
	{
		SDL_DisplayMode dm;
		if( SDL_GetDesktopDisplayMode( display_index, &dm ) == 0 )
		{
			fb_w = dm.w;
			fb_h = dm.h;
		}

		if( fb_w < VID_MIN_WIDTH || fb_h < VID_MIN_HEIGHT )
		{
			fb_w = (int)window_width.value;
			fb_h = (int)window_height.value;
		}

		if( fb_w < VID_MIN_WIDTH || fb_h < VID_MIN_HEIGHT )
			return; // nothing sane to offer: mainui's ladder takes over

		modes = 1;
		fb_fallback = true;
	}

	// +1 slot: the running size row appended below
	vidmodes = Mem_Malloc( host.mempool, ( modes + 1 ) * sizeof( vidmode_t ) );

	char buf[MAX_VA_STRING];

	for( int i = 0; i < modes; i++ )
	{
		SDL_DisplayMode mode;
		int w, h;

		if( fb_fallback )
		{
			w = fb_w;
			h = fb_h;
		}
		else
		{
			if( SDL_GetDisplayMode( display_index, i, &mode ) < 0 )
			{
				Msg( "SDL_GetDisplayMode: %s\n", SDL_GetError() );
				continue;
			}

			if( mode.w < VID_MIN_WIDTH || mode.h < VID_MIN_HEIGHT )
				continue;

			w = mode.w;
			h = mode.h;
		}

		int j;
		for( j = 0; j < num_vidmodes; j++ )
		{
			if( w == vidmodes[j].width && h == vidmodes[j].height )
				break;
		}

		if( j != num_vidmodes )
			continue;

		vidmodes[num_vidmodes].width = w;
		vidmodes[num_vidmodes].height = h;
		Q_snprintf( buf, sizeof( buf ), "%ix%i", w, h );
		vidmodes[num_vidmodes].desc = copystring( buf );

		num_vidmodes++;
	}

	// running size: append when the list does not already carry it, so
	// GetConfig can highlight the mode the player is actually using
	// (windowed custom sizes on Windows, the panel size on Android)
	{
		int cw = (int)window_width.value;
		int ch = (int)window_height.value;
		int j;

		for( j = 0; j < num_vidmodes; j++ )
		{
			if( cw == vidmodes[j].width && ch == vidmodes[j].height )
				break;
		}

		if( j == num_vidmodes && cw >= VID_MIN_WIDTH && ch >= VID_MIN_HEIGHT )
		{
			vidmodes[num_vidmodes].width = cw;
			vidmodes[num_vidmodes].height = ch;
			Q_snprintf( buf, sizeof( buf ), "%ix%i", cw, ch );
			vidmodes[num_vidmodes].desc = copystring( buf );
			num_vidmodes++;
		}
	}

	// one-shot gate marker (round 147, item 8)
	if( fb_fallback )
		Con_Printf( "[A7] build147: resolution picker native fallback active (%ix%i)\n", fb_w, fb_h );
}

static void R_FreeVideoModes( void )
{
	if( !vidmodes )
		return;

	for( int i = 0; i < num_vidmodes; i++ )
		Mem_Free( (char*)vidmodes[i].desc );
	Mem_Free( vidmodes );

	vidmodes = NULL;
}

#if XASH_WIN32
static qboolean WIN_SetWindowIcon( HICON ico )
{
	SDL_SysWMinfo wminfo;

	SDL_VERSION( &wminfo.version );

	if( SDL_GetWindowWMInfo( host.hWnd, &wminfo ) == SDL_TRUE && wminfo.subsystem == SDL_SYSWM_WINDOWS )
	{
		SendMessage( wminfo.info.win.window, WM_SETICON, ICON_SMALL, (LONG_PTR)ico );
		SendMessage( wminfo.info.win.window, WM_SETICON, ICON_BIG, (LONG_PTR)ico );
		return true;
	}

	Con_Reportf( S_ERROR "%s: %s\n", __func__, SDL_GetError( ));
	return false;
}
#endif


/*
=================
GL_GetProcAddress
=================
*/
void *GL_GetProcAddress( const char *name )
{
	void *func = SDL_GL_GetProcAddress( name );

#if XASH_PSVITA
	// try to find in main module
	if( !func )
		func = vrtld_dlsym( NULL, name );
#endif

	if( !func )
	{
		Con_Reportf( S_ERROR "%s failed for %s\n", __func__, name );
	}

	return func;
}

/*
===============
GL_UpdateSwapInterval
===============
*/
void GL_UpdateSwapInterval( void )
{
#if XASH_ANDROID
	// A7 build 121: Android engine-side fps uncap (user: "make all devices
	// work on 1000 fps up... not modified cvars"). The buffer swap is NEVER
	// allowed to wait for the display, so the refresh rate (60/90/120 Hz)
	// cannot cap the framerate. The gl_vsync cvar default is left untouched on
	// purpose - it is just not applied to the swap on Android. A window/context
	// change (FCVAR_CHANGED, set by SetMode) resets the driver-side interval,
	// so re-apply it then.
	//
	// A7 build 122 (user: "features maked engine very lagged to all
	// devices... i want engine very smooth with high fps"): this function is
	// called once PER RENDER FRAME, and the 121 retry ran on every one of
	// them whenever the driver had not accepted yet (no context yet, or a
	// driver that refuses) - per-frame work added on exactly the hot path we
	// are trying to make smooth. The retry is now once a second, and a
	// refusal is printed ONCE with the driver's own words so engine.log says
	// WHY vsync could not be dropped instead of staying silent.
	static qboolean uncap_applied = false;
	static qboolean uncap_failed_logged = false;
	static double uncap_next_retry = 0.0;
	static double uncap_next_check = 0.0;

	if( FBitSet( gl_vsync.flags, FCVAR_CHANGED ))
	{
		ClearBits( gl_vsync.flags, FCVAR_CHANGED );
		uncap_applied = false; // new context: the interval reset with it
		uncap_next_retry = 0.0; // fresh context: try immediately again
		uncap_next_check = 0.0;
	}

	if( !uncap_applied && host.realtime >= uncap_next_retry )
	{
		if( SDL_GL_SetSwapInterval( 0 ) >= 0 )
		{
			uncap_applied = true;
			uncap_next_check = host.realtime + 1.0;
			Con_Printf( "[A7] build 121: engine fps uncap active (vsync bypassed, no frame cap, driver interval %d)\n",
				SDL_GL_GetSwapInterval( ));
		}
		else
		{
			uncap_next_retry = host.realtime + 1.0;

			if( !uncap_failed_logged )
			{
				uncap_failed_logged = true;
				Con_Printf( "[A7] build122: swap interval 0 rejected: %s (retrying once a second)\n",
					SDL_GetError( ));
			}
		}
	}

	// A7 build 123 (user: "now good but make it too high again"): KEEP the
	// ceiling up, not just raise it once. Some drivers/compositors put the
	// swap interval BACK (resume from background, panel/rate switch,
	// context touch) and nothing on our side ever noticed - the engine
	// silently settled at the display refresh again. Verify the interval
	// once a second (one cheap query, never per frame) and force it to 0
	// again the moment it drifts.
	if( uncap_applied && host.realtime >= uncap_next_check )
	{
		int interval = SDL_GL_GetSwapInterval();

		uncap_next_check = host.realtime + 1.0;

		if( interval != 0 )
		{
			uncap_applied = false;
			uncap_next_retry = 0.0;
			Con_Printf( "[A7] build123: swap interval drifted back to %d - re-uncapping\n", interval );
		}
	}
#else
	if( FBitSet( gl_vsync.flags, FCVAR_CHANGED ))
	{
		ClearBits( gl_vsync.flags, FCVAR_CHANGED );

		if( SDL_GL_SetSwapInterval( gl_vsync.value ) < 0 )
			Con_Reportf( S_ERROR  "SDL_GL_SetSwapInterval: %s\n", SDL_GetError( ));
	}
#endif // XASH_ANDROID
}

/*
=================
GL_DeleteContext

always return false
=================
*/
static qboolean GL_DeleteContext( void )
{
	if( glw_state.context )
	{
		SDL_GL_DeleteContext( glw_state.context );
		glw_state.context = NULL;
	}
	return false;
}

static void VID_GetWindowSizeInPixels( SDL_Window *window, SDL_Renderer *renderer, int *w, int *h )
{
#if SDL_VERSION_ATLEAST( 2, 26, 0 )
	SDL_GetWindowSizeInPixels( window, w, h );
#else
	if( glw_state.software )
		SDL_GetRendererOutputSize( renderer, w, h );
	else
		SDL_GL_GetDrawableSize( window, w, h );
#endif
}

void VID_SaveWindowSize( int width, int height )
{
	qboolean maximized = FBitSet( SDL_GetWindowFlags( host.hWnd ), SDL_WINDOW_MAXIMIZED );
	int render_w = width, render_h = height;

#if XASH_ANDROID
	// A7 build 152: with a non-native surface scale latched the buffer (and
	// only the buffer) is at the chosen render size - SDL can't see buffer
	// geometry, so its own numbers are wrong while the override is live.
	if( s_android_native_w && VID_AndroidScaleActive( ))
	{
		render_w = s_android_w;
		render_h = s_android_h;

		// a resize event carrying the PANEL size - the Java surface was
		// recreated (pause/resume, rotation) and the buffer reset to native
		// with it. Apply the scale again right away and save with the
		// applied dims: the reapply never produces its own SDL event.
		// The applied dims go into BOTH slots so window_width/height keep
		// carrying the choice into the next session's config.
		if( width == s_android_native_w && height == s_android_native_h )
		{
			Con_Printf( "build152: surface reset caught - render resolution %dx%d reapplied\n", render_w, render_h );

			if( !VID_AndroidApplySurfaceResolution( render_w, render_h ))
			{
				// surface is gone or refused - fall through and let the
				// native size stand until the next full mode change
				s_android_w = s_android_h = 0;
				VID_GetWindowSizeInPixels( host.hWnd, sw.renderer, &render_w, &render_h );
				VID_SetDisplayTransform( &render_w, &render_h );
				R_SaveVideoMode( width, height, render_w, render_h, maximized );
				return;
			}

			render_w = s_android_w;
			render_h = s_android_h;
			VID_SetDisplayTransform( &render_w, &render_h );
			R_SaveVideoMode( render_w, render_h, render_w, render_h, maximized );
			return;
		}

		VID_SetDisplayTransform( &render_w, &render_h );
		R_SaveVideoMode( width, height, render_w, render_h, maximized );
		return;
	}
#endif // XASH_ANDROID

	VID_GetWindowSizeInPixels( host.hWnd, sw.renderer, &render_w, &render_h );
	VID_SetDisplayTransform( &render_w, &render_h );
	R_SaveVideoMode( width, height, render_w, render_h, maximized );
}

static qboolean VID_GuessFullscreenMode( int display_index, const SDL_DisplayMode *want, SDL_DisplayMode *got )
{
	if( SDL_GetClosestDisplayMode( display_index, want, got ) == NULL )
	{
		Con_Printf( S_ERROR "%s: SDL_GetClosestDisplayMode: %s\n", __func__, SDL_GetError( ));

		// fall back to native mode
		if( SDL_GetDesktopDisplayMode( display_index, got ) < 0 )
		{
			Con_Printf( S_ERROR "%s: SDL_GetDesktopDisplayMode: %s\n", __func__, SDL_GetError( ));

			return false;
		}

		if( got->w != want->w || got->h != want->h )
			Con_Reportf( S_NOTE "Got desktop display mode: %ix%i@%i\n", got->w, got->h, got->refresh_rate );
	}
	else
	{
		if( got->w != want->w || got->h != want->h )
			Con_Reportf( S_NOTE "Got closest display mode: %ix%i@%i\n", got->w, got->h, got->refresh_rate );
	}

	return true;
}

static int VID_GetDisplayIndex( const char *caller, SDL_Window *window )
{
	if( !window )
		return 0;

	int display_index = SDL_GetWindowDisplayIndex( window );

	if( display_index < 0 )
	{
		Con_Printf( S_ERROR "%s: SDL_GetWindowDisplayIndex: %s\n", caller, SDL_GetError());
		display_index = 0;
	}

	return display_index;
}


static void VID_SetWindowIcon( SDL_Window *hWnd )
{
	char iconpath[MAX_STRING];

	Q_strncpy( iconpath, GI->iconpath, sizeof( iconpath ));
	COM_ReplaceExtension( iconpath, ".tga", sizeof( iconpath ));

	rgbdata_t *icon = FS_LoadImage( iconpath, NULL, 0 );

	if( icon )
	{
		SDL_Surface *surface = SDL_CreateRGBSurfaceFrom( icon->buffer,
			icon->width, icon->height, 32, 4 * icon->width,
			0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000 );

		FS_FreeImage( icon );

		if( surface )
		{
			SDL_SetWindowIcon( hWnd, surface );
			SDL_FreeSurface( surface );
			return;
		}
	}

	// ICO support only for Win32
#if XASH_WIN32
	const char *disk_iconpath = FS_GetDiskPath( GI->iconpath, true );

	if( disk_iconpath )
	{
		int len = MultiByteToWideChar( CP_UTF8, 0, disk_iconpath, -1, NULL, 0 );

		if( len >= 0 )
		{
			wchar_t *path = malloc( len * sizeof( *path ));
			HICON ico;

			MultiByteToWideChar( CP_UTF8, 0, disk_iconpath, -1, path, len );
			ico = (HICON)LoadImageW( NULL, path, IMAGE_ICON, 0, 0, LR_LOADFROMFILE|LR_DEFAULTSIZE );

			free( path );

			if( ico && WIN_SetWindowIcon( ico ))
				return;
		}
	}

	WIN_SetWindowIcon( LoadIcon( GetModuleHandle( NULL ), MAKEINTRESOURCE( 101 )));
#endif
}

static qboolean VID_GetDisplayBounds( int display_index, SDL_Window *hWnd, SDL_Rect *rect )
{
	if( SDL_GetDisplayUsableBounds( display_index, rect ) != 0 )
	{
		memset( rect, 0, sizeof( *rect ));
		return false;
	}

	wrect_t wrc = { 0 };
	if( hWnd )
	{
		SDL_GetWindowBordersSize( hWnd, &wrc.top, &wrc.left, &wrc.bottom, &wrc.right );
	}
	else
	{
#if XASH_WIN32
		wrc.left = GetSystemMetrics( SM_CYSIZEFRAME );
		wrc.right = wrc.bottom = wrc.left;
		wrc.top = GetSystemMetrics( SM_CYSMCAPTION ) + wrc.left;
#endif // XASH_WIN32
	}

	rect->x += wrc.left + wrc.right;
	rect->y += wrc.top + wrc.bottom;
	rect->w -= ( wrc.left + wrc.right ) * 2;
	rect->h -= ( wrc.top + wrc.bottom ) * 2;

	return true;
}

static rserr_t VID_SetScreenResolution( int width, int height, window_mode_t window_mode, window_mode_t prev_window_mode )
{
	const int display_index = VID_GetDisplayIndex( __func__, host.hWnd );
	int out_width, out_height;

	switch( window_mode )
	{
	case WINDOW_MODE_BORDERLESS:
	{
		if( SDL_SetWindowFullscreen( host.hWnd, SDL_WINDOW_FULLSCREEN_DESKTOP ) < 0 )
		{
			Con_Printf( S_ERROR "%s: SDL_SetWindowFullscreen (borderless): %s\n", __func__, SDL_GetError( ));

			// there is no "invalid mode" for borderless fullscreen as there is
			// no video mode change to begin with
			return rserr_invalid_fullscreen;
		}
		break;
	}
	case WINDOW_MODE_FULLSCREEN:
	{
		const SDL_DisplayMode want = { .w = width, .h = height };
		SDL_DisplayMode got;

		// return "invalid mode" if we are switching between video modes in fullscreen mode
		// or "invalid fullscreen" if we are switching from windowed to fullscreen
		const rserr_t appropriate_err = prev_window_mode == WINDOW_MODE_WINDOWED ? rserr_invalid_fullscreen : rserr_invalid_mode;

		if( !VID_GuessFullscreenMode( display_index, &want, &got ))
			return appropriate_err;

		if( SDL_SetWindowDisplayMode( host.hWnd, &got ) < 0 )
		{
			Con_Printf( S_ERROR "%s: SDL_SetWindowDisplayMode: %s\n", __func__, SDL_GetError( ));
			return appropriate_err;
		}

		if( SDL_SetWindowFullscreen( host.hWnd, SDL_WINDOW_FULLSCREEN ) < 0 )
		{
			Con_Printf( S_ERROR "%s: SDL_SetWindowFullscreen (fullscreen): %s\n", __func__, SDL_GetError( ));
			return appropriate_err;
		}

		// SDL_SetWindowDisplayMode is broken in SDL2, it changes the display mode but doesn't change window size
		SDL_SetWindowSize( host.hWnd, got.w, got.h );

		break;
	}
	case WINDOW_MODE_WINDOWED:
		if( SDL_SetWindowFullscreen( host.hWnd, 0 ) < 0 )
		{
			Con_Printf( S_ERROR "%s: SDL_SetWindowFullscreen (windowed): %s\n", __func__, SDL_GetError( ));

			// TODO: appropriate error type when going back from fullscreen to windowed?
			return rserr_unknown;
		}

		SDL_SetWindowResizable( host.hWnd, SDL_TRUE );
		SDL_SetWindowBordered( host.hWnd, SDL_TRUE );

		if( !FBitSet( SDL_GetWindowFlags( host.hWnd ), SDL_WINDOW_MAXIMIZED ))
		{
			SDL_DisplayMode dm;
			qboolean center_window = false;

			if( SDL_GetDesktopDisplayMode( display_index, &dm ) >= 0 && width >= dm.w && height >= dm.h )
			{
				SDL_SetWindowSize( host.hWnd, dm.w, dm.h );
				// Con_Printf( "%s: activating fake fullscreen mode\n", __func__ );
				center_window = true;
			}
			else
			{
				SDL_SetWindowSize( host.hWnd, width, height );

				SDL_Rect r;
				if( VID_GetDisplayBounds( display_index, host.hWnd, &r ) >= 0 )
				{
					int x, y;
					SDL_GetWindowPosition( host.hWnd, &x, &y );

					if( x <= r.x || y <= r.y )
						center_window = true;
				}
			}

			if( center_window )
				SDL_SetWindowPosition( host.hWnd, SDL_WINDOWPOS_CENTERED_DISPLAY( display_index ), SDL_WINDOWPOS_CENTERED_DISPLAY( display_index ));
		}

		break;
	}

#if XASH_ANDROID
	// A7 build 152: on Android the window IS the panel - the SDL calls above
	// all succeed without moving anything. The picker's requested size goes
	// to the surface BUFFER instead (SurfaceFlinger scales it fullscreen);
	// the out size below drives the engine's render/window split so VID's
	// save path records exactly what was asked for. Native size is captured
	// here while SDL's window struct still holds the panel (it only ever
	// moves through the Java surfaceChanged path, which always reports the
	// panel, never our buffer geometry).
	{
		int native_w, native_h;

		SDL_GetWindowSize( host.hWnd, &native_w, &native_h );

		if( !s_android_native_w && native_w >= VID_MIN_WIDTH && native_h >= VID_MIN_HEIGHT )
		{
			s_android_native_w = native_w;
			s_android_native_h = native_h;
		}
		else if( native_w > s_android_native_w && native_h > s_android_native_h )
		{
			// a richer panel reading than the one we captured (multi-window
			// started small etc) - the ladder and restore live off this
			s_android_native_w = native_w;
			s_android_native_h = native_h;
		}

		// out size: the requested size when the buffer call succeeds,
		// the untouched panel size otherwise
		out_width = native_w;
		out_height = native_h;

		// A7 build 153: with >= 2 REAL panel modes the picker only offers
		// real sizes and a pick switches the PANEL (Display.Mode via
		// preferredDisplayModeId) - never the buffer geometry (a fractional
		// size would only distort over a differently-shaped surface). The
		// query is lazy (see VID_AndroidQueryPanelModes) so a persisted real
		// mode from the config is applied through the panel at boot too.
		// The panel switch reports itself through a real RESIZED event with
		// the NEW panel size, which VID_SaveWindowSize records on its own.
		VID_AndroidQueryPanelModes( );

		if( s_android_real_rows >= 2 )
		{
			if( VID_AndroidRealMode( width, height ))
			{
				// Java is idempotent ("already at" -> true, no attribute
				// write), so this is safe for the native row as well
				if( Android_SetPreferredDisplayMode( width, height ))
				{
					// drop any buffer scale so the recreate-catch in
					// VID_SaveWindowSize cannot reapply it over the new
					// panel; the panel switch rescales everything itself
					s_android_w = s_android_h = 0;
					out_width = width;
					out_height = height;
					Con_Printf( "build153: panel mode %dx%d applied\n", width, height );
				}
			}

			VID_SaveWindowSize( out_width, out_height );

			// set icon that could've been lost after changing modes
			VID_SetWindowIcon( host.hWnd );

			return rserr_ok;
		}

		if( width == s_android_native_w && height == s_android_native_h )
		{
			// restoring the panel: only when a scale is actually latched -
			// idempotent otherwise
			if( VID_AndroidScaleActive( ))
				VID_AndroidApplySurfaceResolution( width, height );
		}
		else if( VID_AndroidApplySurfaceResolution( width, height ))
		{
			out_width = width;
			out_height = height;
		}

		VID_SaveWindowSize( out_width, out_height );

		// set icon that could've been lost after changing modes
		VID_SetWindowIcon( host.hWnd );

		return rserr_ok;
	}
#endif // XASH_ANDROID

	SDL_GetWindowSize( host.hWnd, &out_width, &out_height );

	Con_Reportf( "%s: Setting video mode to %dx%d %s\n", __func__, out_width, out_height,
		window_mode == WINDOW_MODE_BORDERLESS ? "borderless" :
		window_mode == WINDOW_MODE_FULLSCREEN ? "fullscreen" : "windowed" );

	VID_SaveWindowSize( out_width, out_height );

	// set icon that could've been lost after changing modes
	VID_SetWindowIcon( host.hWnd );

	return rserr_ok;
}

void VID_RestoreScreenResolution( window_mode_t window_mode )
{
	// on mobile platform fullscreen is designed to be always on
	// and code below minimizes our window if we're in full screen
	// don't do that on mobile devices
#if !XASH_MOBILE_PLATFORM
	switch( window_mode )
	{
	case WINDOW_MODE_FULLSCREEN:
		// TODO: we might want to not minimize window if current desktop mode
		// and window mode are the same
		SDL_MinimizeWindow( host.hWnd );
		SDL_SetWindowFullscreen( host.hWnd, 0 );
		break;
	}
#endif // !XASH_MOBILE_PLATFORM
}

// A7 build 146 (round 146 item 1, user: "Windows still shows a black
// loading screen ... not same like android"): the window used to be shown
// the moment it was created - inside ref R_Init, a full ~0.8 s before the
// engine has anything at all to draw - so for that whole stretch the user
// saw an EMPTY BLACK window, the exact GoldSource-style startup the Android
// loading window (collage or its 0xFF2A3140 -> 0xFF14181F gradient) exists
// to replace. Defer the show to the FIRST real present: whatever we draw
// first - on PC that is SCR_DrawBackdropGradient's loading gradient, pushed
// right after R_Init returns - is what the window opens with, so it is
// never on screen as black. Android keeps the old eagerly-shown behavior
// under XASH_ANDROID: there the Java loading window owns the surface and
// this was never the user-visible path.
static qboolean vid_window_shown = false;

static void VID_ShowWindowOnce( void )
{
	if( vid_window_shown || !host.hWnd )
		return;

	vid_window_shown = true;
	SDL_ShowWindow( host.hWnd );
	SDL_RaiseWindow( host.hWnd );
}

/*
=================
VID_CreateWindow
=================
*/
static rserr_t VID_CreateWindow( const int input_width, const int input_height, window_mode_t window_mode )
{
	rserr_t err;
	Uint32 flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE;
	SDL_Rect rect = { SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, input_width, input_height };

	// TODO: disabled for Windows for now
#if !XASH_WIN32
	SetBits( flags, SDL_WINDOW_ALLOW_HIGHDPI );
#endif // !XASH_WIN32

	if( !glw_state.software )
		SetBits( flags, SDL_WINDOW_OPENGL );

	if( vid_maximized.value )
		SetBits( flags, SDL_WINDOW_MAXIMIZED );

	// by default we create window in windowed mode because we don't know
	// if window creation failed because of invalid video mode or any other reason
	host.hWnd = SDL_CreateWindow( GI->title, rect.x, rect.y, rect.w, rect.h, flags );

	if( !host.hWnd )
	{
		err = glw_state.software ? rserr_unknown : rserr_invalid_context;
		goto cleanup;
	}

	SDL_SetWindowMinimumSize( host.hWnd, VID_MIN_WIDTH, VID_MIN_HEIGHT );

	if( window_mode != WINDOW_MODE_WINDOWED )
	{
		err = VID_SetScreenResolution( input_width, input_height, window_mode, WINDOW_MODE_WINDOWED );
		if( err != rserr_ok )
			goto cleanup;
	}

	VID_SetWindowIcon( host.hWnd );
#if XASH_MOBILE_PLATFORM
	SDL_ShowWindow( host.hWnd );
	SDL_RaiseWindow( host.hWnd );
	vid_window_shown = true;
#else
	// keep it HIDDEN until the first present so it can never appear as a
	// black rectangle (see VID_ShowWindowOnce) - a brand new window has to
	// be re-armed for that
	vid_window_shown = false;
#endif

	if( glw_state.software )
	{
		char cmd[64];

		if( Sys_GetParmFromCmdLine( "-sdl_renderer", cmd ))
		{
			int sdl_renderer = Q_max( -1, Q_atoi( cmd ));

			sw.renderer = SDL_CreateRenderer( host.hWnd, sdl_renderer, 0 );

			if( !sw.renderer )
			{
				Con_Printf( S_ERROR "%s: SDL_CreateRenderer: %s\n", __func__, SDL_GetError( ));
				err = rserr_unknown;
				goto cleanup;
			}

			SDL_RendererInfo info;
			SDL_GetRendererInfo( sw.renderer, &info );
			Con_Printf( "SDL_Renderer %s initialized\n", info.name );
		}
	}
	else
	{
		glw_state.context = SDL_GL_CreateContext( host.hWnd );

		if( !glw_state.context )
		{
			Con_Printf( S_ERROR "%s: SDL_GL_CreateContext: %s\n", __func__, SDL_GetError());
			err = rserr_invalid_context;
			goto cleanup;
		}

		if( SDL_GL_MakeCurrent( host.hWnd, glw_state.context ) < 0 )
		{
			Con_Printf( S_ERROR "%s: SDL_GL_MakeCurrent: %s\n", __func__, SDL_GetError( ));
			err = rserr_invalid_context;
			goto cleanup;
		}
	}

	// update window size if it was resized
	SDL_GetWindowSize( host.hWnd, &rect.w, &rect.h );
	VID_SaveWindowSize( rect.w, rect.h );

	VID_Info_f();

	return rserr_ok;

cleanup:
	if( glw_state.context )
	{
		SDL_GL_DeleteContext( glw_state.context );
		glw_state.context = NULL;
	}

	if( sw.renderer )
	{
		SDL_DestroyRenderer( sw.renderer );
		sw.renderer = NULL;
	}

	if( host.hWnd )
	{
		SDL_DestroyWindow( host.hWnd );
		host.hWnd = NULL;
	}

	return err;
}

/*
=================
VID_DestroyWindow
=================
*/
static void VID_DestroyWindow( void )
{
	GL_DeleteContext();

	VID_RestoreScreenResolution( (window_mode_t)vid_fullscreen.value );

	if( host.hWnd )
		SDL_DestroyWindow( host.hWnd );

	host.hWnd = NULL;
	refState.window_mode = WINDOW_MODE_WINDOWED;
}

/*
==================
GL_SetupAttributes
==================
*/
static void GL_SetupAttributes( void )
{
	SDL_GL_ResetAttributes();

	ref.dllFuncs.GL_SetupAttributes( glw_state.safe );
}

// A7 build 125: live swap cost for the on-screen ms readout (SCR_DrawFPS).
// Updated from a 1-second window below; a7_swap_time is the Platform_DoubleTime
// of the last completed window so the draw can hide stale values.
float a7_swap_ms = 0.0f;
double a7_swap_time = -10.0;

void GL_SwapBuffers( void )
{
	// A7 build 124 (user: device reaches only 200-400 fps, wants 1000
	// always): time the swap itself once every 15 seconds. The swap is
	// where GFP driver work is flushed and the buffer is handed to the
	// Android compositor - if THIS is several milliseconds per frame the
	// ceiling is presentation/GPU, and no engine-side change can break it.
	// ~4 arithmetic ops per frame otherwise.
	static double s_sw_win_start = 0.0;
	static double s_sw_sum = 0.0;
	static double s_sw_max = 0.0;
	static int s_sw_n = 0;
	double t0, dt;

	t0 = Sys_DoubleTime();
#if !XASH_MOBILE_PLATFORM
	// A7 build 146: raise the (still hidden) window on the first present,
	// so it opens with loading art instead of sitting black on the desktop
	VID_ShowWindowOnce();
#endif // XASH_MOBILE_PLATFORM

	// A7 build 154 ("black screen in every way - never show up again"):
	// the swap is the ONE place that knows whether the picture is alive -
	// a dead EGL surface (driver hiccup, a surface the OS tore down without
	// a clean Java callback, our own display-mode switch gone wrong) makes
	// every eglSwapBuffers fail while the engine keeps burning frames into
	// nothing. SDL records the failure as its thread error; capture it.
	SDL_ClearError( );
	SDL_GL_SwapWindow( host.hWnd );
	dt = Sys_DoubleTime() - t0;

#if XASH_ANDROID
	{
		const char *swap_err = SDL_GetError( );

		Android_SwapWatchdog( t0, swap_err && swap_err[0] != '\0' );
	}
#endif // XASH_ANDROID

	if( s_sw_win_start == 0.0 )
		s_sw_win_start = host.realtime;

	s_sw_sum += dt;
	s_sw_n++;
	if( dt > s_sw_max )
		s_sw_max = dt;

	if(( host.realtime - s_sw_win_start ) >= 15.0 )
	{
		Con_Printf( "[A7] build124: swap avg %.2f ms, max %.2f ms over %d swaps in %.1f s\n",
			( s_sw_sum / s_sw_n ) * 1000.0, s_sw_max * 1000.0, s_sw_n,
			host.realtime - s_sw_win_start );
		s_sw_win_start = host.realtime;
		s_sw_sum = 0.0;
		s_sw_max = 0.0;
		s_sw_n = 0;
	}

	// A7 build 125: separate 1-second window feeding the ON-SCREEN ms
	// readout next to the fps counter (SCR_DrawFPS) - same accumulator
	// logic as the 15s log line, but refreshed 15x more often so one
	// screenshot shows body vs swap vs loop without engine.log.
	{
		static double s_on_sw_win = 0.0;
		static double s_on_sw_sum = 0.0;
		static int s_on_sw_n = 0;

		if( s_on_sw_win == 0.0 )
			s_on_sw_win = host.realtime;

		s_on_sw_sum += dt;
		s_on_sw_n++;

		if(( host.realtime - s_on_sw_win ) >= 1.0 && s_on_sw_n > 0 )
		{
			a7_swap_ms = (float)(( s_on_sw_sum / s_on_sw_n ) * 1000.0 );
			a7_swap_time = Sys_DoubleTime();

			s_on_sw_win = host.realtime;
			s_on_sw_sum = 0.0;
			s_on_sw_n = 0;
		}
	}
}

int GL_SetAttribute( int attr, int val )
{
	switch( attr )
	{
#define MAP_REF_API_ATTRIBUTE_TO_SDL( name ) case REF_##name: return SDL_GL_SetAttribute( SDL_##name, val );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_RED_SIZE );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_GREEN_SIZE );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_BLUE_SIZE );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_ALPHA_SIZE );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_DOUBLEBUFFER );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_DEPTH_SIZE );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_STENCIL_SIZE );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_MULTISAMPLEBUFFERS );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_MULTISAMPLESAMPLES );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_ACCELERATED_VISUAL );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_CONTEXT_MAJOR_VERSION );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_CONTEXT_MINOR_VERSION );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_CONTEXT_EGL );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_CONTEXT_FLAGS );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_SHARE_WITH_CURRENT_CONTEXT );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_FRAMEBUFFER_SRGB_CAPABLE );
	case REF_GL_CONTEXT_PROFILE_MASK:
#if !XASH_WIN32
#ifdef SDL_HINT_OPENGL_ES_DRIVER
		if( val == REF_GL_CONTEXT_PROFILE_ES )
		{
			SDL_SetHint( SDL_HINT_OPENGL_ES_DRIVER, "1" );
			SDL_SetHint( "SDL_VIDEO_X11_FORCE_EGL", "1" );
		}
#endif // SDL_HINT_OPENGL_ES_DRIVER
#endif
		return SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, val );
#if SDL_VERSION_ATLEAST( 2, 0, 4 )
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_CONTEXT_RELEASE_BEHAVIOR );
#endif
#if SDL_VERSION_ATLEAST( 2, 0, 6 )
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_CONTEXT_RESET_NOTIFICATION );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_CONTEXT_NO_ERROR );
#endif
#undef MAP_REF_API_ATTRIBUTE_TO_SDL
	}

	return -1;
}

int GL_GetAttribute( int attr, int *val )
{
	switch( attr )
	{
#define MAP_REF_API_ATTRIBUTE_TO_SDL( name ) case REF_##name: return SDL_GL_GetAttribute( SDL_##name, val );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_RED_SIZE );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_GREEN_SIZE );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_BLUE_SIZE );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_ALPHA_SIZE );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_DOUBLEBUFFER );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_DEPTH_SIZE );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_STENCIL_SIZE );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_MULTISAMPLEBUFFERS );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_MULTISAMPLESAMPLES );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_ACCELERATED_VISUAL );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_CONTEXT_MAJOR_VERSION );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_CONTEXT_MINOR_VERSION );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_CONTEXT_EGL );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_CONTEXT_FLAGS );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_SHARE_WITH_CURRENT_CONTEXT );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_FRAMEBUFFER_SRGB_CAPABLE );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_CONTEXT_PROFILE_MASK );
#if SDL_VERSION_ATLEAST( 2, 0, 4 )
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_CONTEXT_RELEASE_BEHAVIOR );
#endif
#if SDL_VERSION_ATLEAST( 2, 0, 6 )
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_CONTEXT_RESET_NOTIFICATION );
		MAP_REF_API_ATTRIBUTE_TO_SDL( GL_CONTEXT_NO_ERROR );
#endif
#undef MAP_REF_API_ATTRIBUTE_TO_SDL
	}

	return 0;
}

#ifndef EGL_LIB
#define EGL_LIB NULL
#endif

/*
==================
R_Init_Video
==================
*/
qboolean R_Init_Video( ref_graphic_apis_t type )
{
	string safe;
	SDL_DisplayMode display_mode;
	SDL_GetCurrentDisplayMode( VID_GetDisplayIndex( __func__, NULL ), &display_mode );

	refState.desktopBitsPixel = SDL_BITSPERPIXEL( display_mode.format );

	if( Sys_CheckParm( "-egl" ))
	{
		// EGL doesn't mean we want GLES context
		// so force it only on Windows, where GL is usually created via WGL
#if XASH_WIN32
		SDL_SetHint( SDL_HINT_OPENGL_ES_DRIVER, "1" );
#endif // XASH_WIN32

		SDL_SetHint( SDL_HINT_VIDEO_X11_FORCE_EGL, "1" );
	}

	SDL_SetHint( SDL_HINT_QTWAYLAND_WINDOW_FLAGS, "OverridesSystemGestures" );
	SDL_SetHint( SDL_HINT_QTWAYLAND_CONTENT_ORIENTATION, "landscape" );
	SDL_SetHint( SDL_HINT_VIDEO_X11_XRANDR, "1" );
	SDL_SetHint( SDL_HINT_VIDEO_X11_XVIDMODE, "1" );

	switch( type )
	{
	case REF_SOFTWARE:
		glw_state.software = true;
		break;
	case REF_GL:
		if( !glw_state.safe && Sys_GetParmFromCmdLine( "-safegl", safe ) )
			glw_state.safe = bound( SAFE_NO, Q_atoi( safe ), SAFE_DONTCARE );

		// refdll can request some attributes
		GL_SetupAttributes( );

		if( SDL_GL_LoadLibrary( EGL_LIB ) < 0 )
		{
			Con_Reportf( S_ERROR  "Couldn't initialize OpenGL: %s\n", SDL_GetError());
			return false;
		}
		break;
	default:
		Host_Error( "Can't initialize unknown context type %d!\n", type );
		break;
	}

	if( !VID_SetMode( ))
		return false;

	switch( type )
	{
	case REF_GL:
		// refdll also can check extensions
		ref.dllFuncs.GL_InitExtensions();
		break;
	case REF_SOFTWARE:
	default:
		break;
	}

	R_InitVideoModes();

	host.renderinfo_changed = false;

	return true;
}

rserr_t R_ChangeDisplaySettings( int width, int height, window_mode_t window_mode )
{
	rserr_t err;

	if( !host.hWnd )
		err = VID_CreateWindow( width, height, window_mode );
	else
		err = VID_SetScreenResolution( width, height, window_mode, refState.window_mode );

	if( err != rserr_ok )
		return err;

	SDL_DisplayMode display_mode;
	SDL_GetWindowDisplayMode( host.hWnd, &display_mode );
	refState.desktopBitsPixel = SDL_BITSPERPIXEL( display_mode.format );
	refState.window_mode = window_mode;
	return rserr_ok;
}

/*
==================
VID_SetMode

Set the described video mode
==================
*/
qboolean VID_SetMode( void )
{
	rserr_t	err;
	int width = window_width.value;
	int height = window_height.value;

	// get default resolution if values aren't set
	if( width < VID_MIN_WIDTH || height < VID_MIN_HEIGHT )
	{
#if !defined( DEFAULT_MODE_WIDTH ) || !defined( DEFAULT_MODE_HEIGHT )
		SDL_DisplayMode mode;

		SDL_GetDesktopDisplayMode( 0, &mode );

		width = mode.w;
		height = mode.h;
#else
		width = DEFAULT_MODE_WIDTH;
		height = DEFAULT_MODE_HEIGHT;
#endif
	}

#if XASH_MOBILE_PLATFORM
	if( Q_strcmp( vid_fullscreen.string, DEFAULT_FULLSCREEN ))
	{
		Cvar_DirectSet( &vid_fullscreen, DEFAULT_FULLSCREEN );
		Con_Reportf( S_ERROR "%s: windowed unavailable on this platform\n", __func__ );
	}
#endif

	window_mode_t window_mode = bound( 0, vid_fullscreen.value, WINDOW_MODE_COUNT - 1 );
	SetBits( gl_vsync.flags, FCVAR_CHANGED );

	err = R_ChangeDisplaySettings( width, height, window_mode );

	if( err == rserr_invalid_mode )
	{
		Con_Reportf( S_ERROR "%s: Couldn't set video mode %dx%d, engine will run in %dx%d\n", __func__, width, height, sdlState.prev_width, sdlState.prev_height );
		Sys_Warn( "Couldn't set video mode %dx%d, engine will run in %dx%d", width, height, sdlState.prev_width, sdlState.prev_height );

		width = sdlState.prev_width;
		height = sdlState.prev_height;

		err = R_ChangeDisplaySettings( width, height, window_mode );
	}

	if( err == rserr_invalid_fullscreen )
	{
		Cvar_DirectSetValue( &vid_fullscreen, WINDOW_MODE_WINDOWED );
		Con_Reportf( S_ERROR "%s: fullscreen unavailable in this mode\n", __func__ );
		Sys_Warn( "fullscreen unavailable in this mode!" );
		window_mode = WINDOW_MODE_WINDOWED;
		err = R_ChangeDisplaySettings( width, height, window_mode );
	}

	while( err == rserr_invalid_context )
	{
		Con_Printf( S_ERROR "%s: couldn't create GL context with safegl level %d: %s\n", __func__, glw_state.safe, SDL_GetError());

		glw_state.safe++;

		if( !gl_msaa_samples.value && glw_state.safe == SAFE_NOMSAA )
			glw_state.safe++; // no need to skip msaa, if we already disabled it

		if( glw_state.safe >= SAFE_LAST )
		{
			// window creation has failed...
			return false;
		}

		GL_SetupAttributes();

		// try creating window again
		err = R_ChangeDisplaySettings( width, height, window_mode );
	}

	if( err == rserr_ok )
	{
		sdlState.prev_width = width;
		sdlState.prev_height = height;
		return true;
	}

	Con_Printf( S_ERROR "failed to create window or change video mode: %d", err );

	return false;
}

ref_window_type_t R_GetWindowHandle( void **handle, ref_window_type_t type )
{
	SDL_SysWMinfo wmInfo;

	if( type == REF_WINDOW_TYPE_SDL2 )
	{
		if( handle )
			*handle = (void *)host.hWnd;
		return REF_WINDOW_TYPE_SDL2;
	}

	if( type == REF_WINDOW_TYPE_SDL3 )
		return REF_WINDOW_TYPE_NULL;

	SDL_VERSION( &wmInfo.version );

	if( !SDL_GetWindowWMInfo( host.hWnd, &wmInfo ))
	{
		Con_Reportf( S_ERROR "%s: SDL_GetWindowWMInfo: %s\n", __func__, SDL_GetError( ));
		return REF_WINDOW_TYPE_NULL;
	}

	switch( wmInfo.subsystem )
	{
	case SDL_SYSWM_WINDOWS:
		if( !type || type == REF_WINDOW_TYPE_WIN32 )
		{
#ifdef SDL_VIDEO_DRIVER_WINDOWS
			if( handle )
				*handle = (void *)wmInfo.info.win.window;
			return REF_WINDOW_TYPE_WIN32;
#endif // SDL_VIDEO_DRIVER_WINDOWS
		}
		break;
	case SDL_SYSWM_X11:
		if( !type || type == REF_WINDOW_TYPE_X11 )
		{
#ifdef SDL_VIDEO_DRIVER_X11
			if( handle )
				*handle = (void *)(uintptr_t)wmInfo.info.x11.window;
			return REF_WINDOW_TYPE_X11;
#endif // SDL_VIDEO_DRIVER_X11
		}
		break;
	case SDL_SYSWM_COCOA:
		if( !type || type == REF_WINDOW_TYPE_MACOS )
		{
#ifdef SDL_VIDEO_DRIVER_COCOA
			if( handle )
				*handle = (void *)wmInfo.info.cocoa.window;
			return REF_WINDOW_TYPE_MACOS;
#endif // SDL_VIDEO_DRIVER_COCOA
		}
		break;
	case SDL_SYSWM_WAYLAND:
		if( !type || type == REF_WINDOW_TYPE_WAYLAND )
		{
#ifdef SDL_VIDEO_DRIVER_WAYLAND
			if( handle )
				*handle = (void *)wmInfo.info.wl.surface;
			return REF_WINDOW_TYPE_WAYLAND;
#endif // SDL_VIDEO_DRIVER_WAYLAND
		}
		break;
	}

	return REF_WINDOW_TYPE_NULL;
}

/*
==================
R_Free_Video
==================
*/
void R_Free_Video( void )
{
	GL_DeleteContext ();

	VID_DestroyWindow ();

	R_FreeVideoModes();

	ref.dllFuncs.GL_ClearExtensions();
}

void VID_Info_f( void )
{
	Uint32 flags = SDL_GetWindowFlags( host.hWnd );
	int width, height;
	int render_width, render_height;
	int x, y;

	SDL_GetWindowSize( host.hWnd, &width, &height );
	VID_GetWindowSizeInPixels( host.hWnd, sw.renderer, &render_width, &render_height );
	SDL_GetWindowPosition( host.hWnd, &x, &y );

	Con_Printf( "Video: " S_GREEN "SDL" S_DEFAULT "\n" );
	Con_Printf( "Video driver: " S_GREEN "%s" S_DEFAULT "\n", SDL_GetCurrentVideoDriver( ));
	Con_Printf( "Window size: " S_GREEN "%dx%d" S_DEFAULT " (" S_YELLOW"real %dx%d" S_DEFAULT")\n", width, height, render_width, render_height );
	Con_Printf( "Window position: " S_GREEN "%dx%d" S_DEFAULT "\n", x, y );
	Con_Printf( "Window mode: %s" S_DEFAULT "\n",
		FBitSet( flags, SDL_WINDOW_FULLSCREEN_DESKTOP ) ? S_GREEN "fullscreen desktop" :
		FBitSet( flags, SDL_WINDOW_FULLSCREEN ) ? S_YELLOW "fullscreen" :
		S_CYAN "windowed" );
	Con_Printf( "Window bordered: %s" S_DEFAULT "\n", FBitSet( flags, SDL_WINDOW_BORDERLESS ) ? S_RED "false" : S_GREEN "true" );
	Con_Printf( "Window resizable: %s" S_DEFAULT "\n", FBitSet( flags, SDL_WINDOW_RESIZABLE ) ? S_GREEN "true" : S_RED "false" );
	Con_Printf( "Window maximized: %s" S_DEFAULT "\n", FBitSet( flags, SDL_WINDOW_MAXIMIZED ) ? S_GREEN "true" : S_RED "false" );

	int display_index = SDL_GetWindowDisplayIndex( host.hWnd );
	if( display_index >= 0 )
		Con_Printf( "Window display index: " S_GREEN "%d" S_DEFAULT "\n", display_index );
	else
		Con_Printf( "Window display index: " S_RED "fail: " S_DEFAULT "%s\n", SDL_GetError( ));

	SDL_DisplayMode dm;
	if( SDL_GetWindowDisplayMode( host.hWnd, &dm ) >= 0 )
		Con_Printf( "Window display mode: " S_GREEN "%dx%d@%d" S_DEFAULT "\n", dm.w, dm.h, dm.refresh_rate );
	else
		Con_Printf( "Window display mode: " S_RED "fail: " S_DEFAULT "%s\n", SDL_GetError( ));
}

platform_orientation_t Platform_GetDisplayOrientation( void )
{
	if( host.hWnd )
	{
		int display_index = SDL_GetWindowDisplayIndex( host.hWnd );
		if( display_index >= 0 )
		{
			switch( SDL_GetDisplayOrientation( display_index ) )
			{
			case SDL_ORIENTATION_LANDSCAPE:
				return ORIENTATION_LANDSCAPE;
			case SDL_ORIENTATION_LANDSCAPE_FLIPPED:
				return ORIENTATION_LANDSCAPE_FLIPPED;
			case SDL_ORIENTATION_PORTRAIT:
				return ORIENTATION_PORTRAIT;
			case SDL_ORIENTATION_PORTRAIT_FLIPPED:
				return ORIENTATION_PORTRAIT_FLIPPED;
			default:
				return ORIENTATION_UNKNOWN;
			}
		}
	}

	return ORIENTATION_UNKNOWN;
}
