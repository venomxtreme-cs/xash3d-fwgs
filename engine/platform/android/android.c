/*
android_nosdl.c - android backend
Copyright (C) 2016-2019 mittorn

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#include "platform/platform.h"
#include "input.h"
#include "client.h"
#include "sound.h"
#include "errno.h"
#include <pthread.h>
#include <sys/prctl.h>

#include <android/log.h>
#include <jni.h>
#if XASH_SDL
#include <SDL.h>
#endif // XASH_SDL

struct jnimethods_s
{
	JNIEnv *env;
	jobject activity;
	jclass actcls;
	jmethodID loadAndroidID;
	jmethodID getAndroidID;
	jmethodID saveAndroidID;
	jmethodID getWindowInsets;
} jni;

void Android_Init( void )
{
	memset( &jni, 0, sizeof( jni ));

#if XASH_SDL
	jni.env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	jni.activity = (jobject)SDL_AndroidGetActivity();
	jni.actcls = (*jni.env)->GetObjectClass( jni.env, jni.activity );
	jni.loadAndroidID = (*jni.env)->GetMethodID( jni.env, jni.actcls, "loadAndroidID", "()Ljava/lang/String;" );
	jni.getAndroidID = (*jni.env)->GetMethodID( jni.env, jni.actcls, "getAndroidID", "()Ljava/lang/String;" );
	jni.saveAndroidID = (*jni.env)->GetMethodID( jni.env, jni.actcls, "saveAndroidID", "(Ljava/lang/String;)V" );
	jni.getWindowInsets = (*jni.env)->GetMethodID( jni.env, jni.actcls, "getWindowInsets", "()[F" );
#endif // !XASH_SDL
}

/*
========================
Android_GetNativeObject
========================
*/

void *Android_GetNativeObject( const char *name )
{
	if( !strcasecmp( name, "JNIEnv" ) )
	{
		return (void *)jni.env;
	}
	else if( !strcasecmp( name, "ActivityClass" ) )
	{
		return (void *)jni.actcls;
	}

	return NULL;
}

/*
========================
Android_GetAndroidID
========================
*/
const char *Android_GetAndroidID( void )
{
	static char id[32];

	if( !COM_StringEmpty( id ))
		return id;

	jstring resultJNIStr = (*jni.env)->CallObjectMethod( jni.env, jni.activity, jni.getAndroidID );
	const char *resultCStr = (*jni.env)->GetStringUTFChars( jni.env, resultJNIStr, NULL );
	Q_strncpy( id, resultCStr, sizeof( id ) );
	(*jni.env)->ReleaseStringUTFChars( jni.env, resultJNIStr, resultCStr );
	(*jni.env)->DeleteLocalRef( jni.env, resultJNIStr );

	return id;
}

/*
========================
Android_LoadID
========================
*/
const char *Android_LoadID( void )
{
	static char id[32];
	jstring resultJNIStr = (*jni.env)->CallObjectMethod( jni.env, jni.activity, jni.loadAndroidID );
	const char *resultCStr = (*jni.env)->GetStringUTFChars( jni.env, resultJNIStr, NULL );
	Q_strncpy( id, resultCStr, sizeof( id ) );
	(*jni.env)->ReleaseStringUTFChars( jni.env, resultJNIStr, resultCStr );
	(*jni.env)->DeleteLocalRef( jni.env, resultJNIStr );

	return id;
}

/*
========================
Android_SaveID
========================
*/
void Android_SaveID( const char *id )
{
	jstring JStr = (*jni.env)->NewStringUTF( jni.env, id );
	(*jni.env)->CallVoidMethod( jni.env, jni.activity, jni.saveAndroidID, JStr );
	(*jni.env)->DeleteLocalRef( jni.env, JStr );
}

/*
========================
Android_ShellExecute
========================
*/
void Platform_ShellExecute( const char *path, const char *parms )
{
#if XASH_SDL
	SDL_OpenURL( path );
#endif // XASH_SDL
}

/*
========================
Android_GetWindowInsets
========================
*/
void Android_GetWindowInsets( float insets[4] )
{
	// A7 build 125 (user: "still 200-300 fps, want 1000 always"):
	// VID_UpdateWindowInsets() calls this EVERY frame (host_state.c ->
	// Host_Frame), which was a full JNI round-trip per frame:
	// CallObjectMethod + GetFloatArrayRegion + DeleteLocalRef. Safe-area
	// insets cannot meaningfully change 4x per second, so cache the four
	// floats in C and re-query Java only every 0.25 s (host.realtime).
	// Cost per frame drops from a JNI bridge call to one compare.
	static float cached[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	static double next_refresh = 0.0;
	static qboolean logged = false;
	jfloatArray result;

	if( host.realtime < next_refresh )
	{
		insets[0] = cached[0];
		insets[1] = cached[1];
		insets[2] = cached[2];
		insets[3] = cached[3];
		return;
	}

	next_refresh = host.realtime + 0.25;
	result = (*jni.env)->CallObjectMethod( jni.env, jni.activity, jni.getWindowInsets );

	if( !result || (*jni.env)->ExceptionCheck( jni.env ))
	{
		(*jni.env)->ExceptionClear( jni.env );
		if( result )
			(*jni.env)->DeleteLocalRef( jni.env, result );
		return; // keep last cached values
	}

	(*jni.env)->GetFloatArrayRegion( jni.env, result, 0, 4, cached );
	(*jni.env)->DeleteLocalRef( jni.env, result );

	if( !logged )
	{
		logged = true;
		Con_Printf( "[A7] build125: window insets JNI cached (1 call per 0.25s instead of every frame) %.3f %.3f %.3f %.3f\n",
			cached[0], cached[1], cached[2], cached[3] );
	}

	insets[0] = cached[0];
	insets[1] = cached[1];
	insets[2] = cached[2];
	insets[3] = cached[3];
}

/*
========================
Android_SteamFriends_f

A7: "steamfriends" - open the Steam friends WebView on top of the running
game (su.xash.engine.SteamFriendsActivity, which signs in to
steamcommunity and saves the friend list for the menu and the scoreboard).

Everything is resolved per call: Android_Init() is never invoked by this
engine, so there is no cached JNI state to depend on, and SDL attaches
whatever thread runs the command for us.
========================
*/
static void Android_SteamFriends_f( void )
{
#if XASH_SDL
	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	jobject activity = (jobject)SDL_AndroidGetActivity();
	jclass cls;
	jmethodID open;

	if( !env || !activity )
		return;

	cls = (*env)->GetObjectClass( env, activity );
	open = (*env)->GetMethodID( env, cls, "openSteamFriends", "()V" );

	// SDL's context is expected to be the XashActivity instance; if some
	// build hands us an Application instead, drop the pending exception
	// so the next JNI call does not trip over it
	if( (*env)->ExceptionCheck( env ))
	{
		(*env)->ExceptionClear( env );
		open = NULL;
	}

	if( open )
		(*env)->CallVoidMethod( env, activity, open );

	if( (*env)->ExceptionCheck( env ))
		(*env)->ExceptionClear( env );

	(*env)->DeleteLocalRef( env, cls );
	(*env)->DeleteLocalRef( env, activity );
#endif // XASH_SDL
}

/*
========================
Android_Loading* bridge

A7 build 97: the activity draws the loading window above the GL surface
(full-screen shade, logo, status line, segmented bar, download block with
its own bar, Cancel), so every load phase starts on that window instead of
potentially black frames. Everything is resolved per call, same rules as
the SteamFriends entry above: Android_Init() is never invoked by this
engine, there is no cached JNI state to depend on, and SDL attaches
whatever thread calls this for us.

g_loading_shown keeps the polling callers cheap - once the window is down
the per-frame status feed returns before touching JNI at all.
========================
*/
static qboolean g_loading_shown = false;
static volatile qboolean g_loading_cancelled = false;

// A7 build 98: one-shot loud marker when the JNI lookup fails, so a self
// install can tell from engine.log exactly why the overlay never appeared
// (ProGuard stripped the method, wrong APK pairing, etc.) instead of a
// silent black screen.
static qboolean g_loading_method_missing_logged = false;

struct android_loading_call_s
{
	JNIEnv *env;
	jobject activity;
	jclass cls;
	jmethodID mid;
};

static qboolean Android_JniBegin( struct android_loading_call_s *c, const char *subsystem, const char *name, const char *sig, qboolean *already_logged )
{
	c->env = NULL;
	c->activity = NULL;
	c->cls = NULL;
	c->mid = NULL;

#if XASH_SDL
	c->env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	c->activity = (jobject)SDL_AndroidGetActivity();

	if( !c->env || !c->activity )
		return false;

	c->cls = (*c->env)->GetObjectClass( c->env, c->activity );
	c->mid = (*c->env)->GetMethodID( c->env, c->cls, name, sig );

	// a failed lookup leaves a pending exception; clear it so nothing
	// downstream trips over it
	if( (*c->env)->ExceptionCheck( c->env ))
	{
		(*c->env)->ExceptionClear( c->env );
		c->mid = NULL;
	}

	// A7 build 98: surface the miss loudly (once) for on-device diagnosis
	if( !c->mid && already_logged && !*already_logged )
	{
		*already_logged = true;
		Con_Printf( S_ERROR "[A7] %s: XashActivity.%s(%s) not found - "
			"that feature is disabled. Engine APK out of sync with "
			"the game APK, or ProGuard stripped the JNI method.\n",
			subsystem ? subsystem : "jni", name, sig );
	}
#endif // XASH_SDL

	return c->mid != NULL;
}

static qboolean Android_LoadingBegin( struct android_loading_call_s *c, const char *name, const char *sig )
{
	return Android_JniBegin( c, "loading", name, sig, &g_loading_method_missing_logged );
}

static void Android_LoadingEnd( struct android_loading_call_s *c )
{
	if( (*c->env)->ExceptionCheck( c->env ))
		(*c->env)->ExceptionClear( c->env );

	if( c->cls )
		(*c->env)->DeleteLocalRef( c->env, c->cls );
	if( c->activity )
		(*c->env)->DeleteLocalRef( c->env, c->activity );
}

/*
========================
Android_GetDisplayModes
Android_SetPreferredDisplayMode

A7 build 153 (round 152, user: "that's not real screen resolution, it just
change hud_scale"): REAL panel resolution switching. The build-152 buffer
ladder only resized the GL surface buffer and the proportional HUD scale
absorbed it. These wrap XashActivity.getDisplayModes / setPreferredDisplayMode
(Display.Mode + preferredDisplayModeId, API 23+): the engine lists the
panel's actual modes for the Video-modes picker and applies the chosen one -
the panel itself switches and the surface follows with a normal
surfaceChanged. vid_sdl2.c consumes them.
========================
*/
static qboolean g_vidmode_method_missing_logged = false;

void Android_GetDisplayModes( char *out, size_t size )
{
	struct android_loading_call_s c;
	jstring jstr;
	const char *str;

	out[0] = '\0';

	if( !Android_JniBegin( &c, "vidmode", "getDisplayModes", "()Ljava/lang/String;", &g_vidmode_method_missing_logged ))
		return;

	jstr = (*c.env)->CallObjectMethod( c.env, c.activity, c.mid );

	if( !jstr || (*c.env)->ExceptionCheck( c.env ))
	{
		(*c.env)->ExceptionClear( c.env );
		Android_LoadingEnd( &c );
		return;
	}

	str = (*c.env)->GetStringUTFChars( c.env, jstr, NULL );

	if( str )
	{
		Q_strncpy( out, str, size );
		(*c.env)->ReleaseStringUTFChars( c.env, jstr, str );
	}

	(*c.env)->DeleteLocalRef( c.env, jstr );
	Android_LoadingEnd( &c );
}

qboolean Android_SetPreferredDisplayMode( int w, int h )
{
	struct android_loading_call_s c;
	jboolean ok;

	if( !Android_JniBegin( &c, "vidmode", "setPreferredDisplayMode", "(II)Z", &g_vidmode_method_missing_logged ))
		return false;

	ok = (*c.env)->CallBooleanMethod( c.env, c.activity, c.mid, (jint)w, (jint)h );
	Android_LoadingEnd( &c );

	return ok != JNI_FALSE;
}

// A7 build 98: is the platform overlay actually up? one source of truth
// (g_loading_shown) so the engine's per-frame auto-heal can re-show a
// window that a missed call site never raised.
qboolean Android_LoadingShown( void )
{
	return g_loading_shown;
}

void Android_LoadingShow( const char *serveraddr )
{
	struct android_loading_call_s c;
	jstring jstr;

	if( !Android_LoadingBegin( &c, "loadingShow", "(Ljava/lang/String;)V" ))
		return;

	jstr = (*c.env)->NewStringUTF( c.env, COM_StringEmpty( serveraddr ) ? "" : serveraddr );

	if( jstr )
	{
		(*c.env)->CallVoidMethod( c.env, c.activity, c.mid, jstr );
		(*c.env)->DeleteLocalRef( c.env, jstr );
	}

	Android_LoadingEnd( &c );
	g_loading_shown = true;
	Con_Printf( "[A7] loading: overlay shown (server=\"%s\")\n",
		COM_StringEmpty( serveraddr ) ? "" : serveraddr );
}

void Android_LoadingStatus( const char *text, float percent )
{
	struct android_loading_call_s c;
	jstring jstr;

	// window is down: nothing to update
	if( !g_loading_shown || COM_StringEmpty( text ))
		return;

	if( !Android_LoadingBegin( &c, "loadingStatus", "(Ljava/lang/String;F)V" ))
		return;

	jstr = (*c.env)->NewStringUTF( c.env, text );

	if( jstr )
	{
		(*c.env)->CallVoidMethod( c.env, c.activity, c.mid, jstr, percent );
		(*c.env)->DeleteLocalRef( c.env, jstr );
	}

	Android_LoadingEnd( &c );
}

void Android_LoadingDownload( const char *file, float percent, const char *status, const char *footer )
{
	struct android_loading_call_s c;
	jstring jfile = NULL, jstatus = NULL, jfooter = NULL;

	if( !g_loading_shown )
		return;

	if( !Android_LoadingBegin( &c, "loadingDownload", "(Ljava/lang/String;FLjava/lang/String;Ljava/lang/String;)V" ))
		return;

	if( !COM_StringEmptyOrNULL( file ))
	{
		jfile = (*c.env)->NewStringUTF( c.env, file );

		if( !jfile )
		{
			Android_LoadingEnd( &c );
			return;
		}
	}

	if( jfile && !COM_StringEmptyOrNULL( status ))
		jstatus = (*c.env)->NewStringUTF( c.env, status );

	if( jfile && !COM_StringEmptyOrNULL( footer ))
		jfooter = (*c.env)->NewStringUTF( c.env, footer );

	(*c.env)->CallVoidMethod( c.env, c.activity, c.mid, jfile, percent, jstatus, jfooter );

	if( jfile )
		(*c.env)->DeleteLocalRef( c.env, jfile );
	if( jstatus )
		(*c.env)->DeleteLocalRef( c.env, jstatus );
	if( jfooter )
		(*c.env)->DeleteLocalRef( c.env, jfooter );

	Android_LoadingEnd( &c );
}

void Android_LoadingHide( void )
{
	struct android_loading_call_s c;

	if( !g_loading_shown )
		return;

	if( !Android_LoadingBegin( &c, "loadingHide", "()V" ))
		return;

	(*c.env)->CallVoidMethod( c.env, c.activity, c.mid );
	Android_LoadingEnd( &c );

	g_loading_shown = false;
	Con_Printf( "[A7] loading: overlay hidden\n" );
}

qboolean Android_LoadingCancelled( void )
{
	qboolean cancelled = g_loading_cancelled;

	g_loading_cancelled = false;
	return cancelled;
}

// Cancel on the loading window (UI thread). Static native, resolved by
// symbol lookup in the loaded native libraries like the rest of the
// activity's native calls.
JNIEXPORT void JNICALL Java_su_xash_engine_XashActivity_nativeLoadingCancelled( JNIEnv *env, jclass clazz )
{
	(void)env;
	(void)clazz;
	g_loading_cancelled = true;
}

// the avatar helpers print into the game console, so a phone-only user
// can see WHY a photo never arrived (engine.log carries it too)
JNIEXPORT void JNICALL Java_su_xash_engine_XashActivity_nativeConsolePrintf( JNIEnv *env, jclass clazz, jstring jmsg )
{
	const char *msg;

	(void)clazz;

	if( !jmsg )
		return;

	msg = (*env)->GetStringUTFChars( env, jmsg, NULL );

	if( !msg )
		return;

	Con_Printf( "%s\n", msg );
	(*env)->ReleaseStringUTFChars( env, jmsg, msg );
}

// A7 build 111: the avatar pick/import thread (XashActivity.writeAvatarPair)
// just saved the new logo into media/avatars - raise the flag from THAT
// thread; the engine frame consumes it once per frame (CL_CheckAvatarImported
// in client/cl_main.c: drops the cached textures + bumps @cl_avatargen so the
// Options preview and scoreboards refresh live instead of after a rejoin)
static volatile int s_avatar_saved_pending;

JNIEXPORT void JNICALL Java_su_xash_engine_XashActivity_nativeAvatarSaved( JNIEnv *env, jclass clazz )
{
	(void)env;
	(void)clazz;
	s_avatar_saved_pending = 1;
}

qboolean Android_AvatarSavedPending( void )
{
	if( !s_avatar_saved_pending )
		return false;

	s_avatar_saved_pending = 0;
	return true;
}

/*
=======================
Android_AvatarFetch

A7 round 100: the activity pulls this steam profile's public photo into
<game>/media/avatars/av_<account>.png, which the client scoreboard draws
for that player's row (the client re-checks the file on its own, so a
fetch landing mid-server still shows). Called once per id while
userinfo is parsed.
=======================
*/
void Android_AvatarFetch( uint64_t steamid64 )
{
	static qboolean method_missing_logged;
	struct android_loading_call_s c;

	if( !Android_JniBegin( &c, "avatar", "avatarFetch", "(J)V", &method_missing_logged ))
		return;

	(*c.env)->CallVoidMethod( c.env, c.activity, c.mid, (jlong)steamid64 );
	Android_LoadingEnd( &c );
}

/*
=======================
Android_AvatarPick

A7 round 100: "avatar_pick" from the options menu - the system image
chooser saves the picture as <game>/media/avatars/avatar_custom.png, the
first entry of my row's logo chain.
=======================
*/
void Android_AvatarPick( void )
{
	static qboolean method_missing_logged;
	struct android_loading_call_s c;

	if( !Android_JniBegin( &c, "avatar", "avatarPick", "()V", &method_missing_logged ))
		return;

	(*c.env)->CallVoidMethod( c.env, c.activity, c.mid );
	Android_LoadingEnd( &c );
}

static void Android_AvatarPick_f( void )
{
#if XASH_ANDROID
	Android_AvatarPick();
#else
	Con_Printf( "avatar_pick is only available on Android\n" );
#endif
}

/*
=======================
Android_AvatarImport

build 101: "avatar_import <path>" from the options Import image button.
The in-app file dialog (mainui) lists the bmp/png/jpg the game folders
hold - PC-style - and the activity decodes that path into
<game>/media/avatars/avatar_custom.png, exactly like the picker flow.
=======================
*/
void Android_AvatarImport( const char *path )
{
	static qboolean method_missing_logged;
	struct android_loading_call_s c;
	jstring jpath;

	if( !path || !path[0] )
		return;

	if( !Android_JniBegin( &c, "avatar", "avatarImport", "(Ljava/lang/String;)V", &method_missing_logged ))
		return;

	jpath = (*c.env)->NewStringUTF( c.env, path );
	(*c.env)->CallVoidMethod( c.env, c.activity, c.mid, jpath );

	if( jpath )
		(*c.env)->DeleteLocalRef( c.env, jpath );

	Android_LoadingEnd( &c );
}

static void Android_AvatarImport_f( void )
{
#if XASH_ANDROID
	if( Cmd_Argc( ) < 2 )
	{
		Con_Printf( "usage: avatar_import <file>\n" );
		return;
	}

	Android_AvatarImport( Cmd_Argv( 1 ));
#else
	Con_Printf( "avatar_import is only available on Android\n" );
#endif
}

/*
========================
Android_RegisterCommands
========================
*/
void Android_RegisterCommands( void )
{
	Cmd_AddCommand( "steamfriends", Android_SteamFriends_f, "open the Steam friends screen (sign in, friend list)" );
	Cmd_AddCommand( "avatar_pick", Android_AvatarPick_f, "pick a scoreboard logo image from the gallery" );
	Cmd_AddCommand( "avatar_import", Android_AvatarImport_f, "import a scoreboard logo image listed by the in-app file dialog" );
}
