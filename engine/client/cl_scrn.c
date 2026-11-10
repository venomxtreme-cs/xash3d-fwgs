/*
cl_scrn.c - refresh screen
Copyright (C) 2007 Uncle Mike

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
#include "client.h"
#include "vgui_draw.h"
#include "qfont.h"
#include "input.h"
#include "library.h"

CVAR_DEFINE_AUTO( scr_centertime, "2.5", 0, "centerprint hold time" );
CVAR_DEFINE_AUTO( scr_loading, "0", 0, "loading bar progress" );
CVAR_DEFINE_AUTO( scr_download, "-1", 0, "downloading bar progress" );
CVAR_DEFINE( scr_viewsize, "viewsize", "120", FCVAR_ARCHIVE, "screen size (quake only)" );
CVAR_DEFINE_AUTO( cl_testlights, "0", FCVAR_CHEAT, "test dynamic lights" );
CVAR_DEFINE( cl_allow_levelshots, "allow_levelshots", "0", FCVAR_ARCHIVE, "allow engine to use indivdual levelshots instead of 'loading' image" );
CVAR_DEFINE_AUTO( cl_levelshot_name, "*black", 0, "contains path to current levelshot" );
static CVAR_DEFINE_AUTO( cl_envshot_size, "256", FCVAR_ARCHIVE, "envshot size of cube side" );
CVAR_DEFINE_AUTO( v_dark, "0", 0, "starts level from dark screen" );
static CVAR_DEFINE_AUTO( net_speeds, "0", FCVAR_ARCHIVE, "show network packets" );
static CVAR_DEFINE_AUTO( cl_showfps, "0", FCVAR_ARCHIVE, "show client fps" );
static CVAR_DEFINE_AUTO( cl_showpos, "0", FCVAR_ARCHIVE, "show local player position and velocity" );
static CVAR_DEFINE_AUTO( cl_showents, "0", FCVAR_ARCHIVE | FCVAR_CHEAT, "show entities information (largely undone)" );
static CVAR_DEFINE_AUTO( cl_showcmd, "0", 0, "visualize usercmd button presses" );

typedef struct
{
	int	x1, y1, x2, y2;
} dirty_t;

static dirty_t	scr_dirty, scr_old_dirty[2];
static qboolean	scr_init = false;

/*
==============
SCR_DrawFPS
==============
*/
void SCR_DrawFPS( int height )
{
	rgba_t		color;
	static double	nexttime = 0, lasttime = 0;
	static double	framerate = 0;
	static int	framecount = 0;
	static int	minfps = 9999;
	static int	maxfps = 0;
	char		fpsstring[128];
	int		offset;

	if( cls.state != ca_active || !cl_showfps.value || cl.background )
		return;

	switch( cls.scrshot_action )
	{
	case scrshot_normal:
	case scrshot_snapshot:
	case scrshot_inactive:
		break;
	default: return;
	}

	double newtime = Platform_DoubleTime();
	if( newtime >= nexttime )
	{
		framerate = framecount / (newtime - lasttime);
		lasttime = newtime;
		nexttime = Q_max( nexttime + 1.0, lasttime - 1.0 );
		framecount = 0;
	}

	float calc = framerate;
	framecount++;

	if( calc < 1.0f )
	{
		if( calc == 0.0f )
			return;

		Q_snprintf( fpsstring, sizeof( fpsstring ), "%4i spf", (int)(1.0f / calc + 0.5f));
		MakeRGBA( color, 255, 0, 0, 255 );
	}
	else
	{
		int	curfps = (int)(calc + 0.5f);

		if( curfps < minfps ) minfps = curfps;
		if( curfps > maxfps ) maxfps = curfps;

		if( cl_showfps.value == 2 )
			Q_snprintf( fpsstring, sizeof( fpsstring ), "fps: ^1%4i min, ^3%4i cur, ^2%4i max", minfps, curfps, maxfps );
		else Q_snprintf( fpsstring, sizeof( fpsstring ), "%4i fps", curfps );
		MakeRGBA( color, 255, 255, 255, 255 );
	}

	// A7 build 125 (user: "still 200-300 fps, want 1000 always"): append the
	// frame-cost split (per-frame averages in ms) while the stat windows are
	// live, so ONE screenshot of the fps number proves where the time goes:
	//   body = inside Host_Frame (engine+render+swap), loop = event pump /
	//   state machine around it, swap = GPU flush + compositor handoff.
	// 1000 fps needs all three together <= 1.00 ms per frame. Hidden as soon
	// as the windows go stale (>2.5 s, e.g. during loading) - never faked.
	if( Platform_DoubleTime() - Q_max( a7_body_time, a7_swap_time ) < 2.5 )
	{
		size_t len = strlen( fpsstring );

		if( len < sizeof( fpsstring ) )
			Q_snprintf( fpsstring + len, sizeof( fpsstring ) - len,
				" ^7b%.1f s%.1f o%.1f", a7_body_ms, a7_swap_ms, a7_loop_ms );
	}

	Con_DrawStringLen( fpsstring, &offset, NULL );
	Con_DrawString( refState.width - offset - 4, height, fpsstring, color );
}

/*
==============
SCR_DrawPos

Draw local player position, angles and velocity
==============
*/
void SCR_DrawPos( void )
{
	static char     msg[MAX_SYSPATH];
	rgba_t color;

	if( cls.state != ca_active || !cl_showpos.value || cl.background )
		return;

	cl_entity_t *ent = CL_GetLocalPlayer();
	float speed = VectorLength( cl.simvel );

	Q_snprintf( msg, MAX_SYSPATH,
		"pos: %.2f %.2f %.2f\n"
		"ang: %.2f %.2f %.2f\n"
		"velocity: %.2f",
		cl.simorg[0], cl.simorg[1], cl.simorg[2],
		// should we use entity angles or viewangles?
		// view isn't always bound to player
		ent->angles[0], ent->angles[1], ent->angles[2],
		speed );

	MakeRGBA( color, 255, 255, 255, 255 );

	Con_DrawString( refState.width / 2, 4, msg, color );
}

/*
==============
SCR_DrawEnts
==============
*/
void SCR_DrawEnts( void )
{
	rgba_t color = { 255, 255, 255, 255 };

	if( cls.state != ca_active || !cl_showents.value || ( cl.maxclients > 1 && !cls.demoplayback ))
		return;

	// this probably better hook CL_AddVisibleEntities
	// as entities might get added by client.dll
	for( int i = 0; i < clgame.maxEntities; i++ )
	{
		const cl_entity_t *ent = &clgame.entities[i];
		string msg;
		vec3_t screen;

		if( ent->curstate.messagenum != cl.parsecount )
			continue;

		vec3_t pos = Vec3( ent->origin );

		if( ent->model != NULL )
		{
			vec3_t v;

			// simple model type filter
			if( cl_showents.value > 1 )
			{
				if( ent->model->type != (modtype_t)( cl_showents.value - 2 ))
					continue;
			}

			VectorAverage( ent->model->mins, ent->model->maxs, v );
			VectorAdd( pos, v, pos );
		}

		if( !ref.dllFuncs.WorldToScreen( pos, screen ))
		{
			Q_snprintf( msg, sizeof( msg ),
				"entity %d\n"
				"model %s\n"
				"movetype %d\n",
				ent->index,
				ent->model ? ent->model->name : "(null)",
				ent->curstate.movetype );

			screen[0] =  0.5f * screen[0] * refState.width;
			screen[1] = -0.5f * screen[1] * refState.height;
			screen[0] += 0.5f * refState.width;
			screen[1] += 0.5f * refState.height;

			Con_DrawString( screen[0], screen[1], msg, color );
		}
	}
}

/*
==============
SCR_DrawUserCmd

another debugging aids, shows pressed buttons
==============
*/
void SCR_DrawUserCmd( void )
{
	runcmd_t *pcmd = &cl.commands[( cls.netchan.outgoing_sequence - 1 ) & CL_UPDATE_MASK];
	struct
	{
		int mask;
		const char *name;
	} buttons[16] =
	{
	{ IN_ATTACK, "attack" },
	{ IN_JUMP, "jump" },
	{ IN_DUCK, "duck" },
	{ IN_FORWARD, "forward" },
	{ IN_BACK, "back" },
	{ IN_USE, "use" },
	{ IN_CANCEL, "cancel" },
	{ IN_LEFT, "left" },
	{ IN_RIGHT, "right" },
	{ IN_MOVELEFT, "moveleft" },
	{ IN_MOVERIGHT, "moveright" },
	{ IN_ATTACK2, "attack2" },
	{ IN_RUN, "run" },
	{ IN_RELOAD, "reload" },
	{ IN_ALT1, "alt1" },
	{ IN_SCORE, "score" },
	};
	cl_font_t *font = Con_GetCurFont();
	string msg;
	int ypos = 100;

	if( cls.state != ca_active || !cl_showcmd.value )
		return;

	for( int i = 0; i < ARRAYSIZE( buttons ); i++ )
	{
		rgba_t rgba;

		rgba[0] = FBitSet( pcmd->cmd.buttons, buttons[i].mask ) ? 0 : 255;
		rgba[1] = FBitSet( pcmd->cmd.buttons, buttons[i].mask ) ? 255 : 0;
		rgba[2] = 0;
		rgba[3] = 255;

		Con_DrawString( 100, ypos, buttons[i].name, rgba );

		ypos += font->charHeight;
	}

	Q_snprintf( msg, sizeof( msg ),
		"F/S/U: %g %g %g\n"
		"impulse: %u\n"
		"msec: %u",
		pcmd->cmd.forwardmove, pcmd->cmd.sidemove, pcmd->cmd.upmove,
		pcmd->cmd.impulse,
		pcmd->cmd.msec );
	Con_DrawString( 100, ypos, msg, g_color_table[7] );
}

/*
==============
SCR_NetSpeeds

same as r_speeds but for network channel
==============
*/
void SCR_NetSpeeds( void )
{
	static char	msg[MAX_SYSPATH];
	float		time = cl.mtime[0];
	static int	min_svfps = 100;
	static int	max_svfps = 0;
	int		cur_svfps = 0;
	static int	min_clfps = 100;
	static int	max_clfps = 0;
	int		cur_clfps = 0;
	rgba_t		color;
	cl_font_t *font = Con_GetCurFont();

	if( !host.allow_console )
		return;

	if( !net_speeds.value || cls.state != ca_active )
		return;

	// prevent to get too big values at max
	if( cl_serverframetime() > 0.0001f )
	{
		cur_svfps = Q_rint( 1.0f / cl_serverframetime( ));
		if( cur_svfps < min_svfps ) min_svfps = cur_svfps;
		if( cur_svfps > max_svfps ) max_svfps = cur_svfps;
	}

	// prevent to get too big values at max
	if( cl_clientframetime() > 0.0001f )
	{
		cur_clfps = Q_rint( 1.0f / cl_clientframetime( ));
		if( cur_clfps < min_clfps ) min_clfps = cur_clfps;
		if( cur_clfps > max_clfps ) max_clfps = cur_clfps;
	}

	Q_snprintf( msg, sizeof( msg ),
		"Updaterate: ^1%2i min, ^3%2i cur, ^2%2i max\n"
		"Client FPS: ^1%i min, ^3%3i cur, ^2%3i max\n"
		"Game Time: %02d:%02d\n"
		"Total received from server: %s\n"
		"Total sent to server: %s\n",
		min_svfps, cur_svfps, max_svfps,
		min_clfps, cur_clfps, max_clfps,
		(int)(time / 60.0f ), (int)fmod( time, 60.0f ),
		Q_memprint( cls.netchan.total_received ),
		Q_memprint( cls.netchan.total_sended )
	);

	int x = refState.width - 320 * font->scale;
	int y = 384;

	MakeRGBA( color, 255, 255, 255, 255 );
	CL_DrawString( x, y, msg, color, font, FONT_DRAW_RESETCOLORONLF );
}

/*
================
SCR_RSpeeds
================
*/
void SCR_RSpeeds( void )
{
	char	msg[2048];

	if( !host.allow_console )
		return;

	if( ref.dllFuncs.R_SpeedsMessage( msg, sizeof( msg )))
	{
		rgba_t	color;
		cl_font_t *font = Con_GetCurFont();

		int x = refState.width - 340 * font->scale;
		int y = 64;

		MakeRGBA( color, 255, 255, 255, 255 );
		CL_DrawString( x, y, msg, color, font, FONT_DRAW_RESETCOLORONLF );
	}
}

/*
================
SCR_MakeLevelShot

creates levelshot at next frame
================
*/
void SCR_MakeLevelShot( void )
{
	if( cls.scrshot_request != scrshot_plaque )
		return;

	// make levelshot at nextframe()
	Cbuf_AddText( "levelshot\n" );
}

/*
===============
VID_WriteOverviewScript

Create overview script file
===============
*/
static void VID_WriteOverviewScript( void )
{
	ref_overview_t	*ov = &clgame.overView;
	string		filename;

	Q_snprintf( filename, sizeof( filename ), "overviews/%s.txt", clgame.mapname );

	file_t		*f = FS_Open( filename, "w", false );
	if( !f )
	{
		Con_Printf( S_ERROR "%s: can't open %s for write\n", __func__, filename );
		return;
	}

	FS_Printf( f, "// overview description file for %s.bsp\n\n", clgame.mapname );
	FS_Print( f, "global\n{\n" );
	FS_Printf( f, "\tZOOM\t%.2f\n", ov->flZoom );
	FS_Printf( f, "\tORIGIN\t%.2f\t%.2f\t%.2f\n", ov->origin[0], ov->origin[1], ov->origin[2] );
	FS_Printf( f, "\tROTATED\t%i\n", ov->rotated ? 1 : 0 );
	FS_Print( f, "}\n\nlayer\n{\n" );
	FS_Printf( f, "\tIMAGE\t\"overviews/%s.bmp\"\n", clgame.mapname );
	FS_Printf( f, "\tHEIGHT\t%.2f\n", ov->zFar );	// ???
	FS_Print( f, "}\n" );

	FS_Close( f );
}

/*
================
SCR_MakeScreenShot

create a requested screenshot type
================
*/
void SCR_MakeScreenShot( void )
{
	qboolean	iRet = false;
	int	viewsize;

	if( cls.scrshot_action == scrshot_inactive )
		return;

	if( cls.envshot_viewsize > 0 )
		viewsize = cls.envshot_viewsize;
	else viewsize = cl_envshot_size.value;

	V_CheckGamma();

	switch( cls.scrshot_action )
	{
	case scrshot_normal:
		iRet = ref.dllFuncs.VID_ScreenShot( cls.shotname, VID_SCREENSHOT );
		break;
	case scrshot_snapshot:
		iRet = ref.dllFuncs.VID_ScreenShot( cls.shotname, VID_SNAPSHOT );
		break;
	case scrshot_plaque:
		iRet = ref.dllFuncs.VID_ScreenShot( cls.shotname, VID_LEVELSHOT );
		break;
	case scrshot_savegame:
		iRet = ref.dllFuncs.VID_ScreenShot( cls.shotname, VID_MINISHOT );
		break;
	case scrshot_envshot:
		iRet = ref.dllFuncs.VID_CubemapShot( cls.shotname, viewsize, cls.envshot_vieworg, false );
		break;
	case scrshot_skyshot:
		iRet = ref.dllFuncs.VID_CubemapShot( cls.shotname, viewsize, cls.envshot_vieworg, true );
		break;
	case scrshot_mapshot:
		iRet = ref.dllFuncs.VID_ScreenShot( cls.shotname, VID_MAPSHOT );
		if( iRet )
			VID_WriteOverviewScript(); // store overview script too
		break;
	}

	// report
	if( iRet )
	{
		// snapshots don't writes message about image
		if( cls.scrshot_action != scrshot_snapshot )
			Con_Reportf( "Write %s\n", cls.shotname );
	}
	else Con_Printf( S_ERROR "Unable to write %s\n", cls.shotname );

	cls.envshot_vieworg = NULL;
	cls.scrshot_action = scrshot_inactive;
	cls.envshot_disable_vis = false;
	cls.envshot_viewsize = 0;
	cls.shotname[0] = '\0';
}

/*
================
SCR_DrawBackdropGradient

A7 build 146 (round 146 item 1, user: "Windows still shows a black
loading screen ... not same like android in logo and others"): the Android
Java loading window is NEVER black - XashActivity.makeLoadingBackground
falls back to the 0xFF2A3140 -> 0xFF14181F vertical gradient when the CS
collage art is not available. Paint the same gradient: used for the
loading-plaque fill (instead of the build-92 plain black) and - through
VID_Init - for the very first present of the window, so the whole
engine-init stretch before the first frame shows loading art, not an
empty black window like a raw GoldSource startup.
================
*/
void SCR_DrawBackdropGradient( void )
{
	int	i, bands = 48;
	int	bandH = ( refState.height + bands - 1 ) / bands;

	for( i = 0; i < bands; i++ )
	{
		float	t = (float)i / (float)( bands - 1 );
		int	y = i * bandH;
		int	h = Q_min( bandH, refState.height - y );

		// top 0x2A3140 -> bottom 0x14181F (Java gradient; both steps are
		// monotonic downwards, so the float->byte math never underflows)
		byte	r = (byte)( 0x2A + (( 0x14 - 0x2A ) * t ));
		byte	g = (byte)( 0x31 + (( 0x18 - 0x31 ) * t ));
		byte	b = (byte)( 0x40 + (( 0x1F - 0x40 ) * t ));

		ref.dllFuncs.FillRGBA( kRenderTransTexture, 0, y, refState.width, h, r, g, b, 255 );
	}
}

/*
================
SCR_DrawPlaque
================
*/
static qboolean SCR_DrawPlaque( void )
{
	int	levelshot = 0;

	// A7: always draw the loading screen. this used to be gated behind
	// allow_levelshots (which defaults to 0), so during the whole connect
	// and precache phase nothing was drawn at all and the screen stayed black
	if(( cl_allow_levelshots.value && !cls.changelevel ) || cl.background )
	{
		// no levelshot file → the name is forced to "*black";
		// skip loading that so the loading screen can draw its own backdrop
		if( Q_stricmp( cl_levelshot_name.string, "*black" ))
			levelshot = ref.dllFuncs.GL_LoadTexture( cl_levelshot_name.string, NULL, 0, TF_IMAGE );
	}

	if( levelshot )
	{
		ref.dllFuncs.GL_SetRenderMode( kRenderNormal );
		ref.dllFuncs.R_DrawStretchPic( 0, 0, refState.width, refState.height, 0, 0, 1, 1, levelshot );
	}
	else
	{
		// A7 build 92: PC GoldSource loads without the olive tint and the
		// centered loading art - the levelshot above still shows when the
		// mod provides one. Build 146: the fallback is NO LONGER plain
		// black - it is the Android loading window's never-black gradient,
		// so a latched frame drawn before the GameUI dialog is up (or with
		// no dialog at all) can never show the user a black screen
		SCR_DrawBackdropGradient();
	}

	// A7 build 95: the bottom LOADING band is gone for every interactive
	// load (local start, connect, fastdl download, changelevel) - the
	// centered GameUI dialog with the backdrop art is the loading view
	// the reference shows (user: "loading ... view that i tell you in
	// picture, not loading in down"). the band stays only for
	// background-map sessions and demo playback, where no dialog is
	// raised at all
	if( cl.background || cls.demoplayback ) CL_DrawHUD( CL_LOADING );

	return true;
}

/*
================
SCR_BeginLoadingPlaque
================
*/
void SCR_BeginLoadingPlaque( qboolean is_background )
{
	S_StopAllSounds( true );
	cl.audio_prepped = false;			// don't play ambients

	if( cls.key_dest == key_menu && !cls.changedemo && !is_background )
	{
		UI_SetActiveMenu( false );
		if( cls.state == ca_disconnected && !(GameState->curstate == STATE_RUNFRAME && GameState->nextstate != STATE_RUNFRAME) )
			SCR_UpdateScreen();
	}

	// A7 build 93: raise the GameUI loading dialog - the PC CS 1.6
	// "Loading..." box from the reference picture (status line, gold
	// segmented bar, Cancel) over the plain plaque backdrop. scr_loading
	// already drives the bar. a remote session is identified by
	// connect_time (CL_Connect_f latches it before calling here); a local
	// listen server reports "Starting local game server..." instead.
	// SCR_EndLoadingPlaque takes the dialog back down.
	if( !is_background && !cls.demoplayback )
	{
		if( cls.connect_time != 0.0f && cls.servername[0] )
			UI_ConnectionProgress_Connect( cls.servername );
		else UI_ConnectionProgress_Connect( NULL );

#if XASH_ANDROID
		// A7 build 113 (user: "return how it was and fix black screen"): the
		// GoldSource-styled platform loading window (CS collage art that is
		// never black + the olive "Loading..." box - the build 97/98/99
		// view) is raised again right here, before any latch, redraw or
		// blocking spawn, so the first frame of every load is game art.
		// connect_time is set for remote sessions; a local listen load
		// shows the plain title. Build 112 wrongly removed THIS window -
		// it is the "loading goldsource" the user meant; what must never
		// show again is only the engine-side xash loading underneath it.
		// The build-112 teardown hides (SCR_EndLoadingPlaque, the freeze
		// watchdog) stay: they are what stop this window's shade from
		// outliving an aborted load ("black screen after changing map").
		Con_DPrintf( "[A7] loading: raising platform loading window (build 113 restore)\n" );
		Android_LoadingShow(( cls.connect_time != 0.0f && cls.servername[0] ) ?
			cls.servername : "" );
#endif // XASH_ANDROID

		// key_menu so taps and ESC reach the dialog's Cancel/Esc while the
		// load runs (an open console keeps its key_dest untouched)
		if( cls.key_dest != key_console )
			Key_SetKeyDest( key_menu );
	}

#if XASH_ANDROID
	// A7 build 144 (round 144 items 2+3): a background-flag load skipped
	// the raise above entirely while still latching the screen below -
	// latched frames draw nothing on their own (V_PreRender refuses, the
	// GameUI cannot redraw mid-load), so that path WAS a black stretch
	// with no touch buttons for its whole duration. Same GoldSource cover
	// for background loads too; never double-raise a window that is up.
	if( is_background && !cls.demoplayback && !Android_LoadingShown( ))
	{
		Con_DPrintf( "[A7] build144: background load raising the loading window\n" );
		Android_LoadingShow(( cls.connect_time != 0.0f && cls.servername[0] ) ?
			cls.servername : "" );
	}
#endif // XASH_ANDROID

	if( cls.state == ca_disconnected || cls.disable_screen )
	{
		// build 90: the freeze must be latched HERE too. a local load starts
		// from the menu (state is still 'disconnected') so this branch always
		// hit, returned, and never set cls.disable_screen — the next frames ran
		// the normal ca_disconnected path and Con_DrawConsole took the screen
		// over (fullscreen console + key_dest forced to key_console)
		cls.disable_screen = host.realtime;

		// A7: a local map load starts from the menu (state is still
		// 'disconnected') and the server spawn that follows is fully
		// synchronous — draw the loading screen right now so it stays up
		// instead of a frozen menu frame followed by black
		if( cls.state == ca_disconnected && GameState->nextstate != STATE_RUNFRAME )
		{
			float		saved_disable = cls.disable_screen;
			qboolean	prepped;

			// build 90: lift the latch only for V_PreRender and put it back
			// before the 2d pass, so V_PostRender → Con_DrawConsole never
			// paints the console over this plaque frame
			cls.disable_screen = 0.0f;
			prepped = V_PreRender( );
			cls.disable_screen = saved_disable;

			if( prepped )
			{
				SCR_DrawPlaque();
				V_PostRender();
			}
		}
		return; // already set
	}

	if( cls.key_dest == key_console )
		return;

	if( is_background ) IN_MouseSavePos( );
	cls.draw_changelevel = !is_background;
	SCR_UpdateScreen();

	// set video_prepped after update screen, so engine can draw last remaining frame
	cl.video_prepped = false;
	cls.disable_screen = host.realtime;
	cl.background = is_background;		// set right state before svc_serverdata is came

//	SNDDMA_LockSound();
}

/*
================
SCR_EndLoadingPlaque
================
*/
// A7 build 150 (round 149 item 2, user: "black screen it only appears
// temporarily when loading"): taking the Java loading window down at the
// canonical end-of-load instant is still one step too early - the first
// post-load world frame is not presented yet, so the reveal can expose the
// surface for the frames the world still needs to start drawing = the
// residual TEMPORARY black flash. The gameplay-start exits arm the defer
// here (SCR_DeferLoadingWindowHide); SCR_EndLoadingPlaque then keeps the
// window up and SCR_UpdateScreen takes it down on the first frame that
// actually drew the world. Every other exit (disconnect, cancel, watchdog)
// is never armed and still hides immediately.
static qboolean s_r150_loadinghide_defer = false;

qboolean SCR_DeferLoadingWindowHide( void )
{
#if XASH_ANDROID
	if( Android_LoadingShown( ))
	{
		s_r150_loadinghide_defer = true;
		Con_Printf( "[A7] build150: loading window hide deferred to first world frame\n" );
		return true;
	}
#endif // XASH_ANDROID
	return false;
}

void SCR_EndLoadingPlaque( void )
{
	cls.disable_screen = 0.0f;
	Con_ClearNotify();

	// A7 build 150 (round 149 item 1, user: "touch buttons are not working
	// when i start game it show then it been hiding"): a load officially
	// ends here with NO client touch menu open - any clientonly / _menu_*
	// state the previous session left latched (chooseteam/buy/cmd cfg menu
	// open across the load, mainui CClientWindow Hide() miss) is stale and
	// would hide every main gameplay button forever. Same class of stale
	// state the build-144 key_dest belts already clean on the load path.
	Touch_CleanStaleClientMenu();

	// A7 build 93: close the GameUI loading dialog that
	// SCR_BeginLoadingPlaque/CL_PrecacheResources raised. HandleDisconnect
	// resets its state (no-op if it never showed); UI_SetActiveMenu(false)
	// drops it from the menu stack - the dialog's own Hide can't remove the
	// last window outside the game (it would bounce key_dest to the console
	// on every frame) - and lands key_dest on the game so input works again
	// right after the load. background-map sessions keep their menu.
	if( !cl.background )
	{
		UI_ConnectionProgress_Disconnect();
		UI_SetActiveMenu( false );
	}

#if XASH_ANDROID
	// A7 build 112: this is THE "load is over" point (signon complete,
	// disconnect, abort) - also take the platform loading window down
	// here so no overlay can ever outlive the load, whatever raised it.
	// A7 build 150: unless the gameplay-start exit armed the defer, in
	// which case SCR_UpdateScreen hides it on the first frame that
	// actually drew the world.
	if( !s_r150_loadinghide_defer )
		Android_LoadingHide();
#endif // XASH_ANDROID
//	SNDDMA_UnlockSound();
}

/*
=================
SCR_AddDirtyPoint
=================
*/
static void SCR_AddDirtyPoint( int x, int y )
{
	if( x < scr_dirty.x1 ) scr_dirty.x1 = x;
	if( x > scr_dirty.x2 ) scr_dirty.x2 = x;
	if( y < scr_dirty.y1 ) scr_dirty.y1 = y;
	if( y > scr_dirty.y2 ) scr_dirty.y2 = y;
}

/*
================
SCR_DirtyScreen
================
*/
void SCR_DirtyScreen( void )
{
	SCR_AddDirtyPoint( 0, 0 );
	SCR_AddDirtyPoint( refState.width - 1, refState.height - 1 );
}

static void R_DrawTileClear( int texnum, int x, int y, int w, int h, float tw, float th )
{
	float s1 = x / tw;
	float t1 = y / th;
	float s2 = ( x + w ) / tw;
	float t2 = ( y + h ) / th;

	ref.dllFuncs.GL_SetRenderMode( kRenderNormal );
	ref.dllFuncs.Color4ub( 255, 255, 255, 255 );
	ref.dllFuncs.R_DrawStretchPic( x, y, w, h, s1, t1, s2, t2, texnum );
}

/*
================
SCR_TileClear
================
*/
void SCR_TileClear( void )
{
	int	i, top, bottom, left, right, texnum;
	dirty_t	clear;

	if( likely( scr_viewsize.value >= 120 ))
		return; // full screen rendering

	if( !cls.tileImage )
	{
		cls.tileImage = ref.dllFuncs.GL_LoadTexture( "gfx/backtile.lmp", NULL, 0, TF_NOMIPMAP );
		if( !cls.tileImage )
			cls.tileImage = -1;
	}

	if( cls.tileImage > 0 )
		texnum = cls.tileImage;
	else texnum = 0;

	// erase rect will be the union of the past three frames
	// so tripple buffering works properly
	clear = scr_dirty;

	for( i = 0; i < 2; i++ )
	{
		if( scr_old_dirty[i].x1 < clear.x1 )
			clear.x1 = scr_old_dirty[i].x1;
		if( scr_old_dirty[i].x2 > clear.x2 )
			clear.x2 = scr_old_dirty[i].x2;
		if( scr_old_dirty[i].y1 < clear.y1 )
			clear.y1 = scr_old_dirty[i].y1;
		if( scr_old_dirty[i].y2 > clear.y2 )
			clear.y2 = scr_old_dirty[i].y2;
	}

	scr_old_dirty[1] = scr_old_dirty[0];
	scr_old_dirty[0] = scr_dirty;

	scr_dirty.x1 = 9999;
	scr_dirty.x2 = -9999;
	scr_dirty.y1 = 9999;
	scr_dirty.y2 = -9999;

	if( clear.y2 <= clear.y1 )
		return; // nothing disturbed

	top = clgame.viewport[1];
	bottom = top + clgame.viewport[3] - 1;
	left = clgame.viewport[0];
	right = left + clgame.viewport[2] - 1;

	float tw = REF_GET_PARM( PARM_TEX_SRC_WIDTH, texnum );
	float th = REF_GET_PARM( PARM_TEX_SRC_HEIGHT, texnum );

	if( clear.y1 < top )
	{
		// clear above view screen
		i = clear.y2 < top-1 ? clear.y2 : top - 1;
		R_DrawTileClear( texnum, clear.x1, clear.y1, clear.x2 - clear.x1 + 1, i - clear.y1 + 1, tw, th );
		clear.y1 = top;
	}

	if( clear.y2 > bottom )
	{
		// clear below view screen
		i = clear.y1 > bottom + 1 ? clear.y1 : bottom + 1;
		R_DrawTileClear( texnum, clear.x1, i, clear.x2 - clear.x1 + 1, clear.y2 - i + 1, tw, th );
		clear.y2 = bottom;
	}

	if( clear.x1 < left )
	{
		// clear left of view screen
		i = clear.x2 < left - 1 ? clear.x2 : left - 1;
		R_DrawTileClear( texnum, clear.x1, clear.y1, i - clear.x1 + 1, clear.y2 - clear.y1 + 1, tw, th );
		clear.x1 = left;
	}

	if( clear.x2 > right )
	{
		// clear left of view screen
		i = clear.x1 > right + 1 ? clear.x1 : right + 1;
		R_DrawTileClear( texnum, i, clear.y1, clear.x2 - i + 1, clear.y2 - clear.y1 + 1, tw, th );
		clear.x2 = right;
	}
}

/*
==================
SCR_UpdateScreen

This is called every frame, and can also be called explicitly to flush
text to the screen.
==================
*/
void SCR_UpdateScreen( void )
{
	qboolean screen_redraw = true; // assume screen has been redrawn

	if( cls.disable_screen )
	{
		// A7: the loading plaque is up — keep drawing it (with the animated
		// progress bar) instead of freezing the previous frame for the whole
		// load. the old freeze-existed because 2d on a half-torn-down world
		// looked ugly on some gpus; the plaque paints a full opaque backdrop
		// first, so there is nothing half-drawn left to see
		if(( host.realtime - cls.disable_screen ) > cl_timeout.value )
		{
			// A7 build 91: never give up on the plaque while a connection
			// is still being negotiated - the connect and the download of
			// custom content can legitimately take longer than cl_timeout,
			// and the loading screen must cover that whole phase. a tick
			// just proved the host is alive, so restart the watchdog.
			if( cls.state == ca_connecting || cls.state == ca_connected || cls.state == ca_validate )
			{
				// A7 build 118 (item 1): PROGRESS-AWARE restart. The netchan
				// timeout in CL_ReadPackets only fires from ca_connected up, so
				// a connect that never delivers a packet (dead server, refused
				// challenge, changelevel handshake that never completes) used to
				// restart this latch FOREVER while the GameUI connection dialog
				// stayed modal on top of the game - "every time i played for a
				// couple of minutes i get this message, i can't press anything".
				// No packet for cl_timeout in a non-local non-demo session =
				// dead: abort through CL_Disconnect, the exact path the
				// dialog's Cancel button takes - it closes the dialog
				// (SCR_EndLoadingPlaque), clears the latch and hands the screen
				// back to the menu. custom-content downloads keep last_received
				// moving (netchan fragments) and local sessions are exempt, the
				// same rules CL_ReadPackets already applies.
				if( !cls.demoplayback && !NET_IsLocalAddress( cls.netchan.remote_address )
					&& ( host.realtime - cls.netchan.last_received ) > cl_timeout.value )
				{
					Con_Printf( "%s: no packets for %.0fs, dropping dead session\n", __func__, cl_timeout.value );
					CL_Disconnect();
				}
				else cls.disable_screen = host.realtime;
			}
			else
			{
				Con_Reportf( "%s: loading plaque timed out\n", __func__ );
				// A7 build 118 (item 1): the old raw latch clear + Android_LoadingHide
				// left the GameUI connection dialog up ("i can't press anything").
				// SCR_EndLoadingPlaque is THE canonical end-of-load path: it clears
				// the latch, closes the dialog (UI_ConnectionProgress_Disconnect +
				// UI_SetActiveMenu(false) -> input works again) and hides the
				// Android loading window (the build-112 belt now lives inside it).
				SCR_EndLoadingPlaque();
			}
		}
		else
		{
			float		saved_disable = cls.disable_screen;
			qboolean	prepped;

			// build 90: lift the latch only for V_PreRender, restore it before
			// the 2d pass — the whole frame must run with cls.disable_screen
			// latched or Con_DrawConsole unveils the console over the plaque
			cls.disable_screen = 0.0f;
			prepped = V_PreRender( );
			cls.disable_screen = saved_disable;

			if( prepped )
			{
				SCR_DrawPlaque();
				V_PostRender();
			}
			return;
		}
	}

	if( !V_PreRender( )) return;

	// A7 build 147 (round 147 item 1, user: "Windows still black screen in
	// loading area ... start game, been black, show loading, return to
	// black again ... also in map changed servers"): EVERY frame that is
	// presented must be fully covered by something opaque, or the swap
	// hands the screen a buffer that was never written this frame and the
	// user sees the undefined (black) contents. The 3D world only writes
	// pixels when it actually renders - V_RenderView bails before a single
	// vertex when cl.video_prepped is false (fresh level in
	// CL_RegisterResources, mid-changelevel on a server) and the UI/menu
	// covers the screen only when it is fully open - so those stretches used
	// to present black between the loading dialog and the first world frame,
	// exactly the Android behavior the Java loading window is there to
	// prevent. Paint the same never-black 0x2A3140 -> 0x14181F gradient
	// under everything as the frame's background; everything else on top
	// (World / plaque / loading dialog / console / menu) draws over it.
	if( cls.state != ca_cinematic && ( cls.state != ca_active || !cl.video_prepped
		|| ( !ui_renderworld.value && UI_IsVisible() && !cl.background )))
	{
		ref.dllFuncs.R_Set2DMode( true ); // R_BeginFrame left 2D on; re-assert
		SCR_DrawBackdropGradient();
	}

	switch( cls.state )
	{
	case ca_disconnected:
		Con_RunConsole ();
		break;
	case ca_connecting:
	case ca_connected:
	case ca_validate:
		screen_redraw = SCR_DrawPlaque();
		break;
	case ca_active:
		Con_RunConsole ();
		V_RenderView();
		break;
	case ca_cinematic:
		SCR_DrawCinematic();
		break;
	default:
		Host_Error( "%s: bad cls.state\n", __func__ );
		break;
	}

#if XASH_ANDROID
	// A7 build 150 (round 149 item 2, user: "black screen it only appears
	// temporarily when loading"): fire the deferred loading-window hide.
	// The world has just been drawn into the buffer that is about to be
	// presented, so taking the Java window down NOW can never expose a
	// black surface. A session that left the game before any world frame
	// hides immediately; a changelevel that went straight into the next
	// load keeps the defer armed so the NEW load gets the same
	// first-world-frame treatment.
	if( s_r150_loadinghide_defer )
	{
		if(( cls.state == ca_active && cl.video_prepped ) || cls.state == ca_disconnected )
		{
			s_r150_loadinghide_defer = false;
			Android_LoadingHide();
		}
	}
#endif // XASH_ANDROID

	// during changelevel we might have a few frames when we have nothing to draw
	// (assuming levelshots are off) and drawing 2d on top of nothing or cleared screen
	// is ugly, specifically with Adreno and ImgTec GPUs
	if( screen_redraw || !cls.changelevel || !cls.changedemo )
		V_PostRender();
}

/*
================
SCR_LoadCreditsFont

INTERNAL RESOURCE
================
*/
void SCR_LoadCreditsFont( void )
{
	cl_font_t *const font = &cls.creditsFont;
	qboolean success = false;
	float scale = hud_fontscale.value;
	dword crc = 0;

	// build 102: the crisp TrueType HUD font is back for EVERY HUD string
	// (build 101 had reverted it to the bitmap face and the text quality
	// dropped - user: "reback that new one of fonts", "hudmessage with good
	// quality like before"). Tahoma is CS 1.6's signature face and lives at
	// <gamedir>/gfx/fonts/tahoma.ttf (staged by the build). HUD text only -
	// the console window keeps Con_GetFont(). On failure we fall through to
	// the classic bitmap chain below.
	// A7 build 110: back to plain TF_FONT (bilinear) - build 108's TF_NEAREST
	// made the text look worse ("you returned quality bad now, reback how it
	// was before"). This is the exact pre-build-108 smoothing.
	if( Con_LoadTTFFont( "gfx/fonts/tahoma.ttf", font, scale, &hud_fontrender, TF_FONT ))
	{
		success = true;
		Con_Printf( "[A7] font: HUD TrueType (gfx/fonts/tahoma.ttf)\n" );
	}
	else if( Con_LoadTTFFont( "gfx/fonts/FiraSans-Regular.ttf", font, scale, &hud_fontrender, TF_FONT ))
	{
		success = true;
		Con_Printf( "[A7] font: HUD TrueType (gfx/fonts/FiraSans-Regular.ttf)\n" );
	}
	else Con_Printf( "[A7] font: TrueType not found, HUD falls back to bitmap\n" );

	// replace default gfx.wad textures by current charset's font
	if( !success && ( !CRC32_File( &crc, "gfx.wad" ) || crc == 0x49eb9f16 ))
	{
		string charsetFnt;

		if( Q_snprintf( charsetFnt, sizeof( charsetFnt ),
			"creditsfont_%s.fnt", Cvar_VariableString( "con_charset" )) > 0 )
		{
			if( FS_FileExists( charsetFnt, false ))
			{
				// A7 build 91: TF_NEAREST keeps the bitmap crisp when
				// hud_fontscale (1.5) upscales it - the bilinear default
				// smeared the glyphs at high font scales
				success = Con_LoadVariableWidthFont( charsetFnt, font, scale, &hud_fontrender, TF_FONT|TF_NEAREST );
			}
		}
	}

	if( !success )
		success = Con_LoadVariableWidthFont( "gfx/creditsfont.fnt", font, scale, &hud_fontrender, TF_FONT|TF_NEAREST );

	// it's a Quake font, we need to keep source texture for it
	if( !success )
		success = Con_LoadFixedWidthFont( "gfx/conchars", font, scale, &hud_fontrender, TF_FONT|TF_NEAREST|TF_KEEP_SOURCE );

	// copy font size for client.dll
	if( success )
	{
		clgame.scrInfo.iCharHeight = cls.creditsFont.charHeight;

		for( int i = 0; i < ARRAYSIZE( cls.creditsFont.charWidths ); i++ )
			clgame.scrInfo.charWidths[i] = cls.creditsFont.charWidths[i];
	}
	else Con_DPrintf( S_ERROR "failed to load HUD font\n" );
}

/*
================
SCR_InstallParticlePalette

INTERNAL RESOURCE
================
*/
static void SCR_InstallParticlePalette( void )
{
	rgbdata_t	*pic;
	int	i;

	// first check 'palette.lmp' then 'palette.pal'
	pic = FS_LoadImage( DEFAULT_INTERNAL_PALETTE, NULL, 0 );
	if( !pic ) pic = FS_LoadImage( DEFAULT_EXTERNAL_PALETTE, NULL, 0 );

	// NOTE: imagelib required this fakebuffer for loading internal palette
	if( !pic ) pic = FS_LoadImage( "#valve.pal", (byte *)&i, 768 );

	if( pic )
	{
		for( i = 0; i < 256; i++ )
		{
			clgame.palette[i].r = pic->palette[i*4+0];
			clgame.palette[i].g = pic->palette[i*4+1];
			clgame.palette[i].b = pic->palette[i*4+2];
		}
		FS_FreeImage( pic );
	}
	else
	{
		// someone deleted internal palette from code...
		for( i = 0; i < 256; i++ )
		{
			clgame.palette[i].r = i;
			clgame.palette[i].g = i;
			clgame.palette[i].b = i;
		}
	}
}

int SCR_LoadPauseIcon( void )
{
	int texnum = 0;

	if( FS_FileExists( "gfx/paused.lmp", false ))
		texnum = ref.dllFuncs.GL_LoadTexture( "gfx/paused.lmp", NULL, 0, TF_IMAGE|TF_ALLOW_NEAREST );
	else if( FS_FileExists( "gfx/pause.lmp", false ))
		texnum = ref.dllFuncs.GL_LoadTexture( "gfx/pause.lmp", NULL, 0, TF_IMAGE|TF_ALLOW_NEAREST );

	return texnum ? texnum : -1;
}

/*
================
SCR_RegisterTextures

INTERNAL RESOURCE
================
*/
void SCR_RegisterTextures( void )
{
	const char *exts[] = { "lmp", "bmp", "png" };
	const char *names[] = { "gfx/lambda", "gfx/loading" };
	uint flags = TF_IMAGE|TF_ALLOW_NEAREST;

	if( cl_allow_levelshots.value )
		SetBits( flags, TF_LUMINANCE );

	for( int i = 0; i < ARRAYSIZE( names ); i++ )
	{
		for( int j = 0; j < ARRAYSIZE( exts ); j++ )
		{
			string path;

			if( Q_snprintf( path, sizeof( path ), "%s.%s", names[i], exts[j] ) < 0 )
				continue;

			if( !FS_FileExists( path, false ))
				continue;

			cls.loadingBar = ref.dllFuncs.GL_LoadTexture( path, NULL, 0, flags );

			if( cls.loadingBar )
				return;
		}
	}

	Con_Printf( S_WARN "%s: failed to load loading image\n", __func__ );
}

/*
=================
SCR_SizeUp_f

Keybinding command
=================
*/
static void SCR_SizeUp_f( void )
{
	Cvar_SetValue( "viewsize", Q_min( scr_viewsize.value + 10, 120 ));
}


/*
=================
SCR_SizeDown_f

Keybinding command
=================
*/
static void SCR_SizeDown_f( void )
{
	Cvar_SetValue( "viewsize", Q_max( scr_viewsize.value - 10, 30 ));
}

/*
==================
SCR_VidInit
==================
*/
void SCR_VidInit( void )
{
	if( !ref.initialized ) // don't call VidInit too soon
		return;

	memset( &clgame.ds, 0, sizeof( clgame.ds )); // reset a draw state
	memset( &gameui.ds, 0, sizeof( gameui.ds )); // reset a draw state
	memset( &clgame.centerPrint, 0, sizeof( clgame.centerPrint ));

	// update screen sizes for menu
	if( gameui.globals )
	{
		gameui.globals->scrWidth = refState.width;
		gameui.globals->scrHeight = refState.height;
	}

	// notify vgui about screen size change
	if( clgame.hInstance )
	{
		VGui_Startup( refState.width, refState.height );
	}

	CL_ClearSpriteTextures(); // now all hud sprites are invalid

	// vid_state has changed
	if( gameui.hInstance ) gameui.dllFuncs.pfnVidInit();
	if( clgame.hInstance ) clgame.dllFuncs.pfnVidInit();

	// restart console size
	Con_VidInit ();
	Touch_NotifyResize();
}

/*
==================
SCR_Init
==================
*/
void SCR_Init( void )
{
	if( scr_init ) return;

	Cvar_RegisterVariable( &scr_centertime );
	Cvar_RegisterVariable( &cl_levelshot_name );
	Cvar_RegisterVariable( &cl_allow_levelshots );
	Cvar_RegisterVariable( &scr_loading );
	Cvar_RegisterVariable( &scr_download );
	Cvar_RegisterVariable( &cl_testlights );
	Cvar_RegisterVariable( &cl_envshot_size );
	Cvar_RegisterVariable( &v_dark );
	Cvar_RegisterVariable( &scr_viewsize );
	Cvar_RegisterVariable( &net_speeds );
	Cvar_RegisterVariable( &cl_showfps );
	Cvar_RegisterVariable( &cl_showpos );
	Cvar_RegisterVariable( &cl_showcmd );
#ifdef _DEBUG
	Cvar_RegisterVariable( &cl_showents );
#endif // NDEBUG

	// register our commands
	Cmd_AddCommand( "skyname", CL_SetSky_f, "set new skybox by basename" );
	Cmd_AddCommand( "loadsky", CL_SetSky_f, "set new skybox by basename" );
	Cmd_AddCommand( "viewpos", SCR_Viewpos_f, "prints current player origin" );
	Cmd_AddCommand( "sizeup", SCR_SizeUp_f, "screen size up to 10 points" );
	Cmd_AddCommand( "sizedown", SCR_SizeDown_f, "screen size down to 10 points" );

	if( !UI_LoadProgs( ))
	{
		Con_Printf( S_ERROR "can't initialize gameui DLL: %s\n", COM_GetLibraryError() ); // there is non fatal for us
		host.allow_console = true; // we need console, because menu is missing
	}

	SCR_VidInit();
	SCR_LoadCreditsFont ();
	SCR_RegisterTextures ();
	SCR_InstallParticlePalette ();
	SCR_InitCinematic();
	CL_InitNetgraph();

	if( host.allow_console && Sys_CheckParm( "-toconsole" ))
		Cbuf_AddText( "toggleconsole\n" );
	else UI_SetActiveMenu( true );

	scr_init = true;
}

void SCR_Shutdown( void )
{
	if( !scr_init ) return;

	Cmd_RemoveCommand( "skyname" );
	Cmd_RemoveCommand( "viewpos" );
	UI_SetActiveMenu( false );
	UI_UnloadProgs();

	scr_init = false;
}
