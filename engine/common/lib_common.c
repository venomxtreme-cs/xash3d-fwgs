/*
lib_common.c - common dynamic library code
Copyright (C) 2018 Flying With Gauss

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
#include "library.h"
#include "filesystem.h"
#include "server.h"
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#if XASH_ANDROID
#include <dirent.h> /* A7 build 137: seed the private module tree from the APK lib dir */
#endif

static char s_szLastError[1024] = "";

const char *COM_GetLibraryError( void )
{
        return s_szLastError;
}

void COM_ResetLibraryError( void )
{
        s_szLastError[0] = 0;
}

void COM_PushLibraryError( const char *error )
{
        if( s_szLastError[0] )
                Q_strncat( s_szLastError, "\n", sizeof( s_szLastError ) );
        Q_strncat( s_szLastError, error, sizeof( s_szLastError ) );
}

void *COM_FunctionFromName_SR( void *hInstance, const char *pName )
{
#ifdef XASH_ALLOW_SAVERESTORE_OFFSETS
        if( !memcmp( pName, "ofs:", 4 ))
                return (byte*)svgame.dllFuncs.pfnGameInit + Q_atoi( pName + 4 );
#endif

#if XASH_POSIX
        size_t numfuncs;
        char **funcs = COM_ConvertToLocalPlatform( MANGLE_ITANIUM, pName, &numfuncs );

        if( funcs )
        {
                void *f = NULL;
                for( size_t i = 0; i < numfuncs; i++ )
                {
                        if( !f )
                                f = COM_FunctionFromName( hInstance, funcs[i] );
                        Z_Free( funcs[i] );
                }
                Z_Free( funcs );

                if( f ) return f;
        }
#elif _MSC_VER
        // TODO: COM_ConvertToLocalPlatform doesn't support MSVC yet
        // also custom loader strips always MSVC mangling, so Win32
        // platforms already use platform-neutral names
        const char *func = COM_GetPlatformNeutralName( pName );

        if( func )
                return COM_FunctionFromName( hInstance, func );
#endif

        return COM_FunctionFromName( hInstance, pName );
}

const char *COM_OffsetNameForFunction( void *function )
{
        static string sname;
        Q_snprintf( sname, MAX_STRING, "ofs:%zu", (size_t)((byte*)function - (byte*)svgame.dllFuncs.pfnGameInit ));
        Con_Reportf( "%s: %s\n", __func__, sname );
        return sname;
}

dll_user_t *FS_FindLibrary( const char *dllname, qboolean directpath )
{
        // no fs loaded yet, but let engine find fs
        if( !g_fsapi.FindLibrary )
        {
                dll_user_t *p = Mem_Calloc( host.mempool, sizeof( dll_user_t ));
                Q_strncpy( p->shortPath, dllname, sizeof( p->shortPath ));
                Q_strncpy( p->fullPath, dllname, sizeof( p->fullPath ));
                Q_strncpy( p->dllName, dllname, sizeof( p->dllName ));

                return p;
        }

        fs_dllinfo_t dllInfo;

        // fs can't find library
        if( !g_fsapi.FindLibrary( dllname, directpath, &dllInfo ))
                return NULL;

        // NOTE: for libraries we not fail even if search is NULL
        // let the OS find library himself
        dll_user_t *p = Mem_Calloc( host.mempool, sizeof( dll_user_t ));
        Q_strncpy( p->shortPath, dllInfo.shortPath, sizeof( p->shortPath ));
        Q_strncpy( p->fullPath, dllInfo.fullPath, sizeof( p->fullPath ));
        Q_strncpy( p->dllName, dllname, sizeof( p->dllName ));
        p->custom_loader = dllInfo.custom_loader;
        p->encrypted = dllInfo.encrypted;

        return p;
}

/*
=============================================================================

        LIBRARY NAMING(see Documentation/library-naming.md for more info)

=============================================================================
*/
/*
==============
COM_GenerateClientLibraryPath

Generates platform-unique and compatible name for client libraries
==============
*/
static void COM_GenerateClientLibraryPath( const char *name, char *out, size_t size )
{
#ifdef XASH_INTERNAL_GAMELIBS // assuming library loader knows where to get libraries
        Q_strncpy( out, name, size );
#else
        string libname;

        COM_GenerateCommonLibraryName( name, libname, sizeof( libname ));

        Q_snprintf( out, size, "%s/%s", GI->dll_path, libname );
#endif
}

/*
==============
COM_GenerateServerLibraryPath

Generates platform-unique and compatible name for server library
==============
*/
static void COM_GenerateServerLibraryPath( const char *alt_dllname, char *out, size_t size )
{
#ifdef XASH_INTERNAL_GAMELIBS // assuming library loader knows where to get libraries
        Q_strncpy( out, "server", size );
#elif XASH_X86 && XASH_WIN32
        Q_strncpy( out, GI->game_dll, size );
#elif XASH_X86 && XASH_APPLE
        Q_strncpy( out, GI->game_dll_osx, size );
#elif XASH_X86 && XASH_LINUX && !XASH_ANDROID
        Q_strncpy( out, GI->game_dll_linux, size );
        COM_StripExtension( out );

        // GoldSrc actually strips everything after '_', causing issues for mods that have '_' in the DLL name on Linux
        // e.g. delta_particles.so becomes delta.so. We're gonna be smarter and just drop the _i?86 if it matches...
        // ... until somebody complains :)
        COM_StripIntelSuffix( out );
        COM_DefaultExtension( out, "." OS_LIB_EXT, size );
#else
        string temp, dir, libname;
        const char *base_dllname;

#if XASH_WIN32
        Q_strncpy( temp, GI->game_dll, sizeof( temp ));
#elif XASH_APPLE
        Q_strncpy( temp, GI->game_dll_osx, sizeof( temp ));
#else
        Q_strncpy( temp, GI->game_dll_linux, sizeof( temp ));
#endif

        // path to the dll directory
        COM_ExtractFilePath( temp, dir );

        if( alt_dllname )
        {
                base_dllname = alt_dllname;
        }
        else
        {
                // cleaned up dll name
                COM_StripExtension( temp );
                COM_StripIntelSuffix( temp );
                base_dllname = COM_FileWithoutPath( temp );
        }

        COM_GenerateCommonLibraryName( base_dllname, libname, sizeof( libname ));

        Q_snprintf( out, size, "%s/%s", dir, libname );
#endif
}

/*
=============================================================================

        AMX MOD X SUPPORT (Android)

        If addons/metamod/dlls/libmetamod_android_<arch>.so exists in the game
        directory, the engine automatically loads Metamod as the server library.
        Metamod then loads AMX Mod X and the original server library chain.

        Modern Android forbids dlopen() from shared (sdcard) storage, so every
        addon library is first copied into the application private directory,
        then loaded from there. The private layout mirrors addons/:

          <privdir>/addons/metamod/dlls/*.so
          <privdir>/addons/amxmodx/dlls/*.so
          <privdir>/addons/amxmodx/modules/*.so

        Path configuration is passed to Metamod and AMX Mod X via environment:

          MM_GAMEDLL      - absolute path of the library the engine would load
                            normally (e.g. YaPB proxy or the real game server lib)
          MM_GAMELIBDIR   - directory with AMX Mod X module libraries
          MM_ADDONS_ROOT  - private copy of addons/, used by metamod plugin loader

=============================================================================
*/
// A7 build 144 (round 144 item 4): the WHOLE AMX Mod X machinery below
// (mkdir -p, copy chains, env handshake MM_* / XASH3D_AMXX_*, the fake
// client chain, the APK libdir seed and COM_AMXX_Setup) is an Android-only
// loader - its POSIX calls (mkdir mode argument, setenv) do not compile
// on the Windows x64 target, and the user's item 4 forbids amxmodx there
// anyway. Its only cross-file callers are comment references; the one real
// call (COM_AMXX_Setup from COM_GetCommonLibraryPath's server branch) is
// inside an existing #if XASH_ANDROID below. Omitted entirely on WIN32.
#if XASH_ANDROID
static void COM_AMXX_MkDirP( char *path )
{
        char *p;

        for( p = path + 1; *p; p++ )
        {
                if( *p == '/' )
                {
                        *p = 0;
                        mkdir( path, 0755 );
                        *p = '/';
                }
        }
        mkdir( path, 0755 );
}

static qboolean COM_AMXX_CopyFile( const char *src, const char *dst )
{
        FILE *out;
        file_t *in;
        char buf[16384];
        fs_offset_t len;

        // read through the engine filesystem so relative search paths
        // (gamedir on sdcard, rodir, etc) resolve correctly
        in = g_fsapi.Open( src, "rb", false );

        if( !in )
                return false;

        out = fopen( dst, "wb" );

        if( !out )
        {
                g_fsapi.Close( in );
                return false;
        }

        while(( len = g_fsapi.Read( in, buf, sizeof( buf ))) > 0 )
        {
                if( fwrite( buf, 1, len, out ) != ( size_t )len )
                {
                        g_fsapi.Close( in );
                        fclose( out );
                        return false;
                }
        }

        g_fsapi.Close( in );
        fclose( out );

        // dlopen requires the executable bit
        chmod( dst, 0755 );

        return true;
}

static qboolean COM_AMXX_CopyTree( const char *privroot )
{
        static const char *subdirs[] =
        {
                "metamod/dlls",
                "amxmodx/dlls",
                "amxmodx/modules",
        };
        int i;

        for( i = 0; i < ( int )sizeof( subdirs ) / sizeof( subdirs[0] ); i++ )
        {
                char pattern[MAX_SYSPATH];
                search_t *search;

                Q_snprintf( pattern, sizeof( pattern ), "addons/%s/*.so", subdirs[i] );

                search = g_fsapi.Search( pattern, false, false );

                if( !search )
                        continue;

                for( int j = 0; j < search->numfilenames; j++ )
                {
                        char src[MAX_SYSPATH];
                        char dst[MAX_SYSPATH];
                        char dstdir[MAX_SYSPATH];
                        char name[MAX_SYSPATH];
                        const char *base = COM_FileWithoutPath( search->filenames[j] );
                        int rename_abi = !Q_strncmp( subdirs[i], "amxmodx", 7 );
                        int matched = 0, other_abi = 0;

                        if( !g_fsapi.GetFullDiskPath( src, sizeof( src ), search->filenames[j], false ))
                                continue;

                        Q_strncpy( name, base, sizeof( name ));

                        // ABI selection: amxmodx module files may carry an
                        // architecture suffix. Copy the one matching this
                        // build under the neutral name, skip foreign arches.
                        if( rename_abi )
                        {
                                const char *ba = Q_buildarch();
                                const char *suffixes[2];
                                int nsuffix = 0;

                                if( !Q_strcmp( ba, "arm64" ))
                                {
                                        // the suffix INCLUDES the .so extension for
                                        // matching, but only the architecture part is
                                        // stripped below: metamod plugins.ini and the
                                        // amxmodx module loader expect lib*_*.so names
                                        suffixes[nsuffix++] = "_arm64.so";
                                        // skip other architectures entirely
                                        if( Q_strstr( name, "_armv7" ) || Q_strstr( name, "_x86.so" ) || Q_strstr( name, "_amd64.so" ))
                                                other_abi = 1;
                                }
                                else
                                {
                                        suffixes[nsuffix++] = "_armv7a.so";
                                        suffixes[nsuffix++] = "_armv7l.so";
                                        if( Q_strstr( name, "_arm64" ) || Q_strstr( name, "_x86.so" ) || Q_strstr( name, "_amd64.so" ))
                                                other_abi = 1;
                                }

                                if( other_abi )
                                        continue;

                                for( int k = 0; k < nsuffix && !matched; k++ )
                                {
                                        size_t slen = Q_strlen( suffixes[k] );
                                        size_t nlen = Q_strlen( name );

                                        if( nlen > slen && !Q_strcmp( name + nlen - slen, suffixes[k] ))
                                        {
                                                // cut the architecture suffix, then
                                                // re-append ".so": metamod plugins.ini
                                                // and the amxmodx module loader expect
                                                // lib*_*.so names
                                                name[nlen - slen] = '\0';
                                                Q_strncat( name, ".so", sizeof( name ));
                                                matched = 1;
                                        }
                                }

                        }
                        else if(( Q_strstr( name, "_armv7" ) && !Q_strcmp( Q_buildarch(), "arm64" )) ||
                                ( Q_strstr( name, "_arm64" ) && Q_strcmp( Q_buildarch(), "arm64" )))
                        {
                                // skip foreign-architecture metamod copies
                                continue;
                        }

                        Q_snprintf( dst, sizeof( dst ), "%s/addons/%s/%s", privroot, subdirs[i], name );

                        // drop stale files or empty directories left by earlier
                        // builds at this path so the copy below can succeed
                        remove( dst );

                        Q_snprintf( dstdir, sizeof( dstdir ), "%s/addons/%s", privroot, subdirs[i] );
                        COM_AMXX_MkDirP( dstdir );

                        if( !COM_AMXX_CopyFile( src, dst ))
                                Con_Printf( S_WARN "%s: failed to copy %s -> %s\n", __func__, src, dst );
                        else
                                Con_Reportf( "%s: copied %s -> %s\n", __func__, src, dst );
                }

                Mem_Free( search );
        }

        return true;
}

/*
===================
COM_AMXX_FakeClientChain

E-BOT expects ReHLDS-like fake-client semantics: pfnCreateFakeClient runs the
full game DLL connect chain (ClientConnect -> ClientPutInServer), so the game
DLL creates the CBasePlayer private data and the bot can leave observer state
and join a team. Without the chain the fake client never gets game-side player
data and the bot stays in spectator forever. YaPB instead drives the game DLL
itself, so the chain must stay OFF when YaPB is the active bot: only enable it
when the metamod plugin list actually selects E-BOT (metamod reads the gamedir
copy of plugins.ini, see "[META] Pluginfile: using gamedir copy").
===================
*/
static void COM_AMXX_FakeClientChain( void )
{
        char inipath[MAX_SYSPATH];
        char line[512];
        FILE *f;

        if( !g_fsapi.GetFullDiskPath( inipath, sizeof( inipath ), "addons/metamod/plugins.ini", false ))
                return;

        f = fopen( inipath, "rb" );
        if( !f )
                return;

        while( fgets( line, sizeof( line ), f ))
        {
                // only an active (non-comment) line naming the E-BOT module;
                // patchPluginsIni comments the entry out when the switch is off
                if( line[0] != ';' && Q_stristr( line, "libebot" ))
                {
                        // overwrite=0: an explicit external setting still wins
                        setenv( "XASH3D_FAKECLIENT_CONNECT", "1", 0 );
                        // per-connect chatter; only emitted above the highest
                        // developer level so the console stays clean
                        if( host_developer.value > DEV_EXTENDED )
                                Con_Reportf( "%s: E-BOT selected in %s, enabling full fake-client connect chain\n", __func__, inipath );
                        break;
                }
        }

        fclose( f );
}

/* A7 build 137 (items 3+4): does this app's own nativeLibraryDir
 * (XASH3D_GAMELIBDIR) carry the server side? Both APKs package
 * libmetamod_android_<arch>.so; only ONE ABI can ever be in that directory
 * (an installed package is extracted for the device's ABI alone), so once
 * this probe hits, sourcing libraries from here is ABI-proof by
 * construction - unlike the addon zips, which carried _arm64 and _armv7a
 * copies side by side on /sdcard (the mixed-ABI reinstall report).
 * Returns false when the directory is missing/unusable so the legacy
 * /sdcard copy chain keeps its turn (pre-build-137 installs). */
static qboolean COM_AMXX_APKLibDir( char *metamod_path, size_t size )
{
        const char *apkdir = getenv( "XASH3D_GAMELIBDIR" );
        const char *archnames[] = { Q_buildarch(), "arm64", "armv7a", "armv7l", "armv7hf" };
        int i;

        if( COM_StringEmptyOrNULL( apkdir ))
                return false;

        for( i = 0; i < ( int )sizeof( archnames ) / sizeof( archnames[0] ); i++ )
        {
                if( COM_StringEmpty( archnames[i] ))
                        continue;

                Q_snprintf( metamod_path, size, "%s/libmetamod_android_%s.so",
                        apkdir, archnames[i] );

                if( access( metamod_path, R_OK ) == 0 )
                        return true;
        }

        metamod_path[0] = '\0';
        return false;
}

#if XASH_ANDROID
/* A7 build 137 (item 4): copy one file when the local copy is missing or a
 * different size (the shipped set changes per build). Returns true when the
 * destination ends up present. Plain stat/fopen - both ends are absolute
 * paths on local storage, no filesystem search paths involved. */
static qboolean COM_AMXX_Mirror( const char *src, const char *dst )
{
        struct stat st_src, st_dst;
        FILE *in, *out;
        char buf[16384];
        size_t n;

        if( stat( src, &st_src ) != 0 )
                return false;

        if( stat( dst, &st_dst ) == 0 && st_dst.st_size == st_src.st_size )
                return true;

        in = fopen( src, "rb" );

        if( !in )
                return false;

        out = fopen( dst, "wb" );

        if( !out )
        {
                fclose( in );
                return false;
        }

        while(( n = fread( buf, 1, sizeof( buf ), in )) > 0 )
                fwrite( buf, 1, n, out );

        fclose( in );
        fclose( out );

        return stat( dst, &st_dst ) == 0;
}

/* mkdir -p for the three private destinations below */
static void COM_AMXX_MakeParent( const char *filepath )
{
        char dir[MAX_SYSPATH];
        char *p;

        Q_strncpy( dir, filepath, sizeof( dir ));
        p = Q_strrchr( dir, '/' );

        if( p && p != dir )
        {
                *p = '\0';
                COM_AMXX_MkDirP( dir );
        }
}

/*
=================
COM_AMXX_SeedFromAPK

A7 build 137 (item 4): the shipped server libraries live in this app's
nativeLibraryDir ONLY now (the addon zips stop unwrapping .so onto
/sdcard and the launchers purge whatever older builds left there). The
AMX Mod X loader takes exactly ONE root (MM_GAMELIBDIR) and the engine
has always handed it a PRIVATE copy of the module dir - .so may not be
dlopened from shared storage - so that private tree is now FED FROM THE
APK LIB DIR instead of from the gamedir. Custom modules the user
installed with the launcher's Modules tab still arrive through
COM_AMXX_CopyTree (they stay on /sdcard: they are user data), and both
kinds of library end up in the same single-ABI private dir exactly like
before. Returns true when the shipped set is present afterwards.
=================
*/
static qboolean COM_AMXX_SeedFromAPK( const char *privroot )
{
        char apkmetamod[MAX_SYSPATH];
        char src[MAX_SYSPATH], dst[MAX_SYSPATH];
        const char *apkdir;
        qboolean ok = true;
        struct dirent *ent;
        DIR *d;

        if( !COM_AMXX_APKLibDir( apkmetamod, sizeof( apkmetamod )))
                return false;

        apkdir = getenv( "XASH3D_GAMELIBDIR" );

        if( COM_StringEmptyOrNULL( apkdir ))
                return false;

        // metamod: the privroot branch probes this exact arch-suffixed name
        Q_snprintf( dst, sizeof( dst ), "%s/addons/metamod/dlls/%s",
                privroot, COM_FileWithoutPath( apkmetamod ));
        COM_AMXX_MakeParent( dst );

        if( !COM_AMXX_Mirror( apkmetamod, dst ))
                ok = false;

        // the AMX Mod X core, under the neutral name plugins.ini names
        Q_snprintf( src, sizeof( src ), "%s/libmm_amxmodx.so", apkdir );
        Q_snprintf( dst, sizeof( dst ), "%s/addons/amxmodx/dlls/libmm_amxmodx.so",
                privroot );
        COM_AMXX_MakeParent( dst );

        if( !COM_AMXX_Mirror( src, dst ))
                ok = false;

        // every libamxx_* module in the APK dir, flat into the module dir
        // (that is exactly how modules.cpp builds the path on Android:
        // MM_GAMELIBDIR "/libamxx_" shortname ".so")
        Q_snprintf( dst, sizeof( dst ), "%s/addons/amxmodx/modules", privroot );
        COM_AMXX_MkDirP( dst );
        d = opendir( apkdir );

        if( d )
        {
                while(( ent = readdir( d )) != NULL )
                {
                        size_t len;

                        if( Q_strncmp( ent->d_name, "libamxx_", 8 ))
                                continue;

                        len = strlen( ent->d_name );

                        if( len < 4 || Q_strcmp( ent->d_name + len - 3, ".so" ))
                                continue;

                        Q_snprintf( src, sizeof( src ), "%s/%s", apkdir, ent->d_name );
                        Q_snprintf( dst, sizeof( dst ), "%s/addons/amxmodx/modules/%s",
                                privroot, ent->d_name );

                        if( !COM_AMXX_Mirror( src, dst ))
                                ok = false;
                }

                closedir( d );
        }

        /* Always Con_Printf (not Con_Reportf): this is THE field-proof line
         * for item 4 in the wire path - on a normal launch the seed branch
         * wins and the later "!ready" marker never runs, so this one has to
         * survive release builds or engine.log carries no evidence at all.
         * Uses the same "build137: server libs from the APK lib dir" needle
         * the scan112 FS/engine bucket looks for. */
        if( ok )
                Con_Printf( "[A7] build137: server libs from the APK lib dir (seeded %s -> %s)\n",
                        apkdir, privroot );
        else
                Con_Printf( S_WARN "%s: incomplete APK seed of %s\n", __func__, privroot );

        return ok;
}
#endif // XASH_ANDROID

static qboolean COM_AMXX_Setup( char *out, size_t size, const char *realdll )
{
        static qboolean prepared = false;
        static qboolean ready = false;
        static char privroot[MAX_SYSPATH];

        const char *rodir = getenv( "XASH3D_RODIR" );
        const char *amxxdir = getenv( "XASH3D_AMXX_LIBDIR" );
        const char *archnames[] = { Q_buildarch(), "arm64", "armv7a", "armv7l", "armv7hf" };
        char testpath[MAX_SYSPATH];
        int i;

        /* v7: per-game toggle from the launcher -- when the user disables
         * AMX Mod X for this game, load the original server library directly.
         * v8.1: use the OrNULL variant -- getenv returns NULL when the var is
         * unset (e.g. cs16client (amxx) launches the engine without extras),
         * and plain COM_StringEmpty dereferences it -> SIGSEGV at 0x0. */
        if( !COM_StringEmptyOrNULL( getenv( "XASH3D_DISABLE_AMXX" )))
        {
                Con_Reportf( "%s: disabled by XASH3D_DISABLE_AMXX, loading %s directly\n", __func__, realdll ? realdll : "<null>" );
                return false;
        }

        if( !prepared )
        {
                prepared = true;

                if( !COM_StringEmptyOrNULL( amxxdir ))
                {
                        Q_strncpy( privroot, amxxdir, sizeof( privroot ));
                        ready = COM_AMXX_CopyTree( privroot );
                }
                else if( !COM_StringEmptyOrNULL( rodir ))
                {
                        // rodir is <files>/gamelibs, use the parent (application private files dir)
                        Q_strncpy( privroot, rodir, sizeof( privroot ));
                        {
                                char *p = Q_strrchr( privroot, '/' );
                                if( p ) *p = 0;
                        }
                        Q_strncat( privroot, "/amxx", sizeof( privroot ));
                        ready = COM_AMXX_CopyTree( privroot );
                }

#if XASH_ANDROID
                // A7 build 137 (items 3+4): since the addon zips stop writing
                // .so to /sdcard (and the launchers purge the leftovers), the
                // shipped metamod / AMX core / libamxx_* modules are seeded
                // into the private tree ABOVE FROM THE APK LIB DIR - one ABI
                // per package, so this copy path can never mix architectures
                // anymore. Custom modules the user installed keep coming from
                // the gamedir copy (COM_AMXX_CopyTree) - both live together
                // in the single MM_GAMELIBDIR root handed to the loader.
                if( privroot[0] && COM_AMXX_SeedFromAPK( privroot ))
                        ready = true;
#endif
        }

        // runs for both branches below: needs only the gamedir plugins.ini
        COM_AMXX_FakeClientChain();

        /* A7 build 137 (items 3+4, user: "fix libs in xash folder should not
         * be there like amxmodx metamod, only inside apk finding gamedir,
         * for all androids ... fix in liblist.gam trying to find regamedll
         * and metamod, make it method easy work without liblist.gam"): serve
         * EVERY server-side library from the APK's own nativeLibraryDir
         * (XASH3D_GAMELIBDIR, set by XashActivity.getArguments() from the
         * "gamelibdir" extra = getApplicationInfo().nativeLibraryDir). The
         * APK packages exactly ONE ABI in that directory - libmetamod_android_*
         * libmm_amxmodx libamxx_* libcs_android_* / libhl_android_* - so this
         * path is single-ABI by construction (no mixing possible) and needs
         * NOTHING on /sdcard: metadata (plugins.ini/modules.ini) stays
         * gamedir-side, libraries never do. Checked before the /sdcard copy
         * chain so a device that still has old .so leftovers ignores them.
         * Only used when the PRIVATE-dir route above is unavailable (neither
         * XASH3D_AMXX_LIBDIR nor XASH3D_RODIR set): normally the seeded
         * private tree answers and MM_GAMELIBDIR points at it, keeping the
         * user's custom modules (Modules tab) loadable alongside.
         */
        if( !ready )
        {
                char apkmetamod[MAX_SYSPATH];
                char gamelib[MAX_SYSPATH];

                if( COM_AMXX_APKLibDir( apkmetamod, sizeof( apkmetamod )))
                {
                        const char *apkdir = getenv( "XASH3D_GAMELIBDIR" );
                        qboolean gamedll_set = false;

                        // hand metamod the regamedll that ships in the same
                        // lib dir (engine-generated name, or the alternate
                        // arch spelling - armv7a vs armv7l naming differs
                        // between build systems)
                        if( !COM_StringEmptyOrNULL( realdll ) && !Q_stristr( realdll, "metamod" ))
                        {
                                const char *bn = COM_FileWithoutPath( realdll );

                                Q_snprintf( gamelib, sizeof( gamelib ), "%s/%s", apkdir, bn );

                                if( access( gamelib, R_OK ) == 0 )
                                {
                                        setenv( "MM_GAMEDLL", gamelib, 1 );
                                        gamedll_set = true;
                                }
                                else
                                {
                                        const char *plat = Q_strstr( bn, "_android_" );

                                        if( plat )
                                        {
                                                int stemlen = ( int )( plat - bn ) + ( int )strlen( "_android_" );

                                                for( int k = 0; k < ( int )sizeof( archnames ) / sizeof( archnames[0] ); k++ )
                                                {
                                                        if( COM_StringEmpty( archnames[k] ))
                                                                continue;

                                                        Q_snprintf( gamelib, sizeof( gamelib ), "%s/%.*s%s.so",
                                                                apkdir, stemlen, bn, archnames[k] );

                                                        if( access( gamelib, R_OK ) == 0 )
                                                        {
                                                                setenv( "MM_GAMEDLL", gamelib, 1 );
                                                                gamedll_set = true;
                                                                break;
                                                        }
                                                }
                                        }
                                }
                        }

                        // flat APK lib dir == the module dir amxx expects
                        // (<root>/libamxx_<module>.so, modules.cpp LoadModule)
                        setenv( "MM_GAMELIBDIR", apkdir, 1 );

                        // MM_ADDONS_ROOT is deliberately NOT set: metamod's
                        // stat_plugin_file() and android_dlopen_fallback()
                        // already fall back to XASH3D_GAMELIBDIR/<basename>
                        // for the gamedir pathnames in plugins.ini

                        setenv( "XASH3D_AMXX_TRIED", "1", 1 );

                        Con_Printf( "[A7] build137: server libs from the APK lib dir (metamod %s, gamedll %s)\n",
                                apkmetamod, gamedll_set ? gamelib : "<generated-name probe>" );

                        Q_strncpy( out, apkmetamod, size );
                        return true;
                }
        }

        if( !ready )
        {
                // no private dir available, try to load metamod directly from the game dir
                // (works only on devices without sdcard dlopen restrictions)
                const char *arch = Q_buildarch();

                Q_snprintf( testpath, sizeof( testpath ), "addons/metamod/dlls/libmetamod_android_%s.so", arch );

                if( g_fsapi.FileExists( testpath, false ))
                {
                        g_fsapi.GetFullDiskPath( out, size, testpath, false );

                        // mark the amxx chain as active so SV_InitGame can
                        // fall back to the original gamedll if this load fails
                        setenv( "XASH3D_AMXX_TRIED", "1", 1 );

                        return true;
                }

                return false;
        }

        // pick the metamod library that matches this build
        for( i = 0; i < ( int )sizeof( archnames ) / sizeof( archnames[0] ); i++ )
        {
                if( COM_StringEmpty( archnames[i] ))
                        continue;

                Q_snprintf( testpath, sizeof( testpath ), "%s/addons/metamod/dlls/libmetamod_android_%s.so", privroot, archnames[i] );

                if( access( testpath, R_OK ) == 0 )
                {
                        char gamelib[MAX_SYSPATH];
                        const char *gamelibdir = getenv( "XASH3D_GAMELIBDIR" );
                        qboolean gamedll_set = false;

                        // pass the original server library to metamod
                        // never hand metamod itself over as the gamedll --
                        // gameinfo.txt entries left by the old manual install
                        // instructions could point realdll back at metamod,
                        // which makes metamod chain into itself and recurse
                        // until the stack overflows
                        if( !COM_StringEmptyOrNULL( realdll ) && !Q_stristr( realdll, "metamod" ))
                        {
                                if( !COM_StringEmptyOrNULL( gamelibdir ))
                                {
                                        Q_snprintf( gamelib, sizeof( gamelib ), "%s/%s", gamelibdir, COM_FileWithoutPath( realdll ));

                                        if( access( gamelib, R_OK ) == 0 )
                                        {
                                                setenv( "MM_GAMEDLL", gamelib, 1 );
                                                gamedll_set = true;
                                        }
                                }

                                if( !gamedll_set && g_fsapi.GetFullDiskPath( gamelib, sizeof( gamelib ), realdll, false ))
                                {
                                        setenv( "MM_GAMEDLL", gamelib, 1 );
                                        gamedll_set = true;
                                }
                        }

                        setenv( "MM_GAMELIBDIR", va( "%s/addons/amxmodx/modules", privroot ), 1 );
                        // MM_ADDONS_ROOT must be the private root itself:
                        // metamod's dlopen/stat fallbacks splice the
                        // "/addons/..." tail of the ini pathname onto it,
                        // so appending "/addons" here produced
                        // .../addons/addons/... and the fallback never hit
                        setenv( "MM_ADDONS_ROOT", privroot, 1 );

                        // mark the amxx chain as active so SV_InitGame can
                        // fall back to the original gamedll if this load fails
                        setenv( "XASH3D_AMXX_TRIED", "1", 1 );

                        Con_Reportf( "%s: AMX Mod X support enabled, metamod at %s, gamedll %s\n",
                                __func__, testpath, gamedll_set ? gamelib : "<autodetect>" );

                        Q_strncpy( out, testpath, size );
                        return true;
                }
        }

        return false;
}
#endif // XASH_ANDROID (build 144: Windows engine builds out the AMXX block)

/*
==============
COM_GetCommonLibraryPath

Generates platform-unique and compatible name for server library
==============
*/
void COM_GetCommonLibraryPath( ECommonLibraryType eLibType, char *out, size_t size )
{
        switch( eLibType )
        {
        case LIBRARY_GAMEUI:
                if( !COM_StringEmpty( host.menulib ))
                {
                        if( host.menulib[0] == '@' )
                                COM_GenerateClientLibraryPath( host.menulib + 1, out, size );
                        else Q_strncpy( out, host.menulib, size );
                }
                else COM_GenerateClientLibraryPath( "menu", out, size );
                break;
        case LIBRARY_CLIENT:
                if( !COM_StringEmpty( host.clientlib ))
                {
                        if( host.clientlib[0] == '@' )
                                COM_GenerateClientLibraryPath( host.clientlib + 1, out, size );
                        else Q_strncpy( out, host.clientlib, size );
                }
                else COM_GenerateClientLibraryPath( "client", out, size );
                break;
        case LIBRARY_SERVER:
                if( !COM_StringEmpty( host.gamedll ))
                {
                        if( host.gamedll[0] == '@' )
                                COM_GenerateServerLibraryPath( host.gamedll + 1, out, size );
                        else Q_strncpy( out, host.gamedll, size );
                }
                else COM_GenerateServerLibraryPath( NULL, out, size );

#if XASH_ANDROID
                /* A7 build 137 (item 3): the generated server library name
                 * (e.g. "dlls/libcs_android_arm64.so") only exists inside the
                 * APK on Android - no gamedir on /sdcard ever carries it (the
                 * addon zips ship no dlls/, and this build writes no .so to
                 * /sdcard at all). Translate it to the absolute file in
                 * XASH3D_GAMELIBDIR (nativeLibraryDir) so a DIRECT load (the
                 * AMX Mod X path disabled, or metamod absent) and the
                 * MM_GAMEDLL hand-off below both work without liblist.gam and
                 * without any library on shared storage. */
                {
                        const char *apkdir = getenv( "XASH3D_GAMELIBDIR" );

                        if( !COM_StringEmptyOrNULL( apkdir ) && out[0] && out[0] != '/' )
                        {
                                char probe[MAX_SYSPATH];

                                Q_snprintf( probe, sizeof( probe ), "%s/%s", apkdir, COM_FileWithoutPath( out ));

                                if( access( probe, R_OK ) == 0 )
                                {
                                        Q_strncpy( out, probe, size );
                                        Con_Reportf( "%s: server library resolved from the APK lib dir: %s\n", __func__, out );
                                }
                        }
                }
#endif

#if XASH_LINUX
                // AMX Mod X support: automatically load metamod instead of the
                // server library if it is installed into addons/metamod.
                // enabled on all Linux builds (not just Android) so the
                // CI arm64 host runner can exercise the exact same chain.
                {
                        char mmpath[MAX_SYSPATH];

                        if( COM_AMXX_Setup( mmpath, sizeof( mmpath ), out ))
                                Q_strncpy( out, mmpath, size );
                }
#endif
                break;
        default:
                ASSERT( 0 );
                out[0] = 0;
                break;
        }
}

/*
=============================================================================

        C++ MANGLE CONVERSION

=============================================================================
*/
#define MAX_NESTED_NAMESPACES 16 /* MSVC limit */

static EFunctionMangleType COM_DetectMangleType( const char *str )
{
        // Itanium C++ ABI mangling always start with _Z
        // namespaces start with N, therefore _ZN
        if( !Q_strncmp( str, "_ZN", 3 ) )
                return MANGLE_ITANIUM;

        // MSVC C++ mangling always start with ? and have
        if( str[0] == '?' && Q_strstr( str, "@@" ))
                return MANGLE_MSVC;

        // allow offsets, we just silently ignore them on conversion
        if( !Q_strncmp( str, "ofs:", 4 ))
                return MANGLE_OFFSET;

        // don't get confused by MSVC C mangling
        if( str[0] != '@' && Q_strchr( str, '@' ))
                return MANGLE_VALVE;

        // not technically an error
        return MANGLE_UNKNOWN;
}

char *COM_GetMSVCName( const char *in_name )
{
        static string out_name;

        if( in_name[0] == '?' )  // is this a MSVC C++ mangled name?
        {
                char *pos = Q_strstr( in_name, "@@" );
                if( pos != NULL )
                {
                        ptrdiff_t len = pos - in_name;

                        // strip off the leading '?'
                        Q_strncpy( out_name, in_name + 1, sizeof( out_name ));
                        out_name[len-1] = 0; // terminate string at the "@@"
                        return out_name;
                }
        }

        Q_strncpy( out_name, in_name, sizeof( out_name ));

        return out_name;
}

static char *COM_GetItaniumName( const char * const in_name )
{
        static string out_name;
        const char *f = in_name;
        string symbols[16];
        uint len = 0;
        int i;
        int remaining;

        remaining = Q_strlen( f );

        if( remaining < 3 )
                goto invalid_format;

        out_name[0] = 0;

        // skip _ZN
        f += 3;
        remaining -= 3;

        for( i = 0; i < MAX_NESTED_NAMESPACES; i++ )
        {
                // parse symbol length marker
                len = 0;
                for( ; isdigit((byte)*f ) && remaining > 0; f++, remaining-- )
                        len = len * 10 + ( *f - '0' );

                // sane value
                len = Q_min( remaining, len );

                if( len == 0 )
                        goto invalid_format;

                Q_strncpy( symbols[i], f, Q_min( len + 1, sizeof( out_name )));
                f += len;
                remaining -= len;

                // end marker
                if( *f == 'E' )
                        break;

                if( !isdigit((byte)*f ) || remaining <= 0 )
                        goto invalid_format;
        }

        if( i == MAX_NESTED_NAMESPACES )
        {
                Con_DPrintf( "%s: too much nested namespaces: %s\n", __func__, in_name );
                return NULL;
        }

        for( ; i >= 0; i-- )
        {
                Q_strncat( out_name, symbols[i], sizeof( out_name ));
                if( i > 0 )
                        Q_strncat( out_name, "@", sizeof( out_name ));
        }

        return out_name;

invalid_format:
        Con_DPrintf( "%s: invalid format: %s\n", __func__, in_name );
        return NULL;
}

char **COM_ConvertToLocalPlatform( EFunctionMangleType to, const char *from, size_t *numfuncs )
{
        // TODO:
        if( to == MANGLE_MSVC )
                return NULL;

        const char *postfix[3];

        switch( to )
        {
        case MANGLE_ITANIUM:
                postfix[0] = "Ev";
                postfix[1] = "EP11CBaseEntity";
                postfix[2] = "EP11CBaseEntityS1_8USE_TYPEf";
                break;
        default:
                ASSERT( 0 );
                return NULL;
        }

        string symbols[MAX_NESTED_NAMESPACES];
        const char *prev = from;
        int i;

        for( i = 0; i < MAX_NESTED_NAMESPACES; i++ )
        {
                const char *at = Q_strchr( prev, '@' );
                uint len;

                if( at ) len = (uint)( at - prev );
                else len = (uint)Q_strlen( prev );

                Q_strncpy( symbols[i], prev, Q_min( len + 1, sizeof( symbols[i] )));

                if( !at )
                        break;

                prev = at + 1;
        }

        if( i == MAX_NESTED_NAMESPACES )
        {
                Con_DPrintf( "%s: too much nested namespaces: %s\n", __func__, from );
                return NULL;
        }

        // only three possible variations
        *numfuncs = ARRAYSIZE( postfix );
        char **ret = Z_Malloc( sizeof( char * ) * ARRAYSIZE( postfix ) );

        string temp, temp2;
        Q_strncpy( temp, "_ZN", sizeof( temp ));

        for( ; i >= 0; i-- )
        {
                Q_snprintf( temp2, sizeof( temp2 ), "%u%s", (uint)Q_strlen( symbols[i] ), symbols[i] );
                Q_strncat( temp, temp2, sizeof( temp ));
        }

        for( i = 0; i < ARRAYSIZE( postfix ); i++ )
        {
                Q_snprintf( temp2, sizeof( temp2 ), "%s%s", temp, postfix[i] );
                ret[i] = copystring( temp2 );
        }

        return ret;
}

const char *COM_GetPlatformNeutralName( const char *in_name )
{
        EFunctionMangleType type = COM_DetectMangleType( in_name );

        switch( type )
        {
        case MANGLE_ITANIUM: return COM_GetItaniumName( in_name );
        case MANGLE_MSVC: return COM_GetMSVCName( in_name );
        default: return in_name;
        }
}

qboolean COM_CheckLibraryDirectDependency( const char *name, const char *depname, qboolean directpath )
{
        dll_user_t *hInst = FS_FindLibrary( name, directpath );

        if( !hInst )
                return false;

        fs_offset_t filesize = 0;
        byte *data;

        // FS_LoadFile could resolve shortPath through a search path FS_FindLibrary skipped
        if( hInst->custom_loader )
                data = FS_LoadFile( hInst->shortPath, &filesize, false );
        else
                data = FS_LoadDirectFile( hInst->fullPath, &filesize );

        // FS_FindLibrary only looks the library up, there's nothing to unload here
        Mem_Free( hInst );

        if( !data )
                return false;

        const qboolean ret = Platform_CheckLibraryDirectDependency( data, (size_t)filesize, depname );

        Mem_Free( data );

        return ret;
}

#if XASH_ENGINE_TESTS
#include "tests.h"

static void Test_DetectMangleType( void )
{
        TASSERT(COM_DetectMangleType( "asdf" ) == MANGLE_UNKNOWN );
        TASSERT(COM_DetectMangleType( "012345" ) == MANGLE_UNKNOWN );
        TASSERT(COM_DetectMangleType( "?asdf" ) == MANGLE_UNKNOWN );
        TASSERT(COM_DetectMangleType( "_Zasdf" ) == MANGLE_UNKNOWN );

        TASSERT(COM_DetectMangleType( "ofs:1234" ) == MANGLE_OFFSET );
        TASSERT(COM_DetectMangleType( "ofs:asdf" ) == MANGLE_OFFSET );
        TASSERT(COM_DetectMangleType( "ofs:" ) == MANGLE_OFFSET );

        TASSERT(COM_DetectMangleType( "_ZN1f1fEv" ) == MANGLE_ITANIUM );
        TASSERT(COM_DetectMangleType( "_ZN3foo3barEv" ) == MANGLE_ITANIUM );

        TASSERT(COM_DetectMangleType( "?f@f@@msvcsucks" ) == MANGLE_MSVC );
        TASSERT(COM_DetectMangleType( "?foo@bar@@IHATEMSVC" ) == MANGLE_MSVC );

        TASSERT(COM_DetectMangleType( "f@f" ) == MANGLE_VALVE );
        TASSERT(COM_DetectMangleType( "foo@bar" ) == MANGLE_VALVE );

        // Xash3D FWGS extensions test
        TASSERT(COM_DetectMangleType( "_ZN1f1f1fEv" ) == MANGLE_ITANIUM );
        TASSERT(COM_DetectMangleType( "_ZN3foo3bar3bazEv" ) == MANGLE_ITANIUM );

        TASSERT(COM_DetectMangleType( "?f@f@f@@msvcsucks" ) == MANGLE_MSVC );
        TASSERT(COM_DetectMangleType( "?foo@bar@@IHATEMSVC" ) == MANGLE_MSVC );

        TASSERT(COM_DetectMangleType( "f@f@f" ) == MANGLE_VALVE );
        TASSERT(COM_DetectMangleType( "foo@bar@baz" ) == MANGLE_VALVE );
}

static void Test_GetMSVCName( void )
{
        const char *symbols[] =
        {
                "", "",
                "?f@f@@XYZA", "f@f",
                "?foo@bar@@QAEXXZ", "foo@bar",
                "foo", "foo",
                "?foo", "?foo",
                "?foo@@", "foo", // not an error?
                "?foo@bar@baz@@gotstrippedanyway","foo@bar@baz"
        };

        for( int i = 0; i < ARRAYSIZE( symbols ); i += 2 )
        {
                Msg( "Checking if MSVC '%s' converts to '%s'...\n", symbols[i], symbols[i+1] );

                TASSERT( !Q_strcmp( COM_GetMSVCName( symbols[i] ), symbols[i+1] ));
        }
}

static void Test_GetItaniumName( void )
{
        const char *symbols[] =
        {
                "", NULL,
                "_", NULL,
                "_Z", NULL,
                "_ZN", NULL,
                "_ZNv", NULL,
                "_ZN4barr3foo", NULL,
                "_ZN3bar3foov", NULL,
                "_ZN4bar3fooEv", NULL,
                "_ZN3bar3fooEv", "foo@bar",
                "_Z3foov", NULL,
                "_ZN3fooEv", "foo", // not possible?
                "_ZN3baz3bar3fooEdontcare", "foo@bar@baz",
        };

        for( int i = 0; i < ARRAYSIZE( symbols ); i += 2 )
        {
                Msg( "Checking if Itanium '%s' converts to '%s'...\n", symbols[i], symbols[i+1] );

                TASSERT( !Q_strcmp( COM_GetItaniumName( symbols[i] ), symbols[i+1] ));
        }
}

static void Test_ConvertFromValveToLocal( void )
{
        const char *symbols[] =
        {
                "", "_ZN",
                "foo", "_ZN3foo",
                "xash3d@fwgs", "_ZN4fwgs6xash3d",
                "foo@bar@bazz", "_ZN4bazz3bar3foo"
        };

        for( int i = 0; i < ARRAYSIZE( symbols ); i += 2 )
        {
                size_t numfuncs;
                size_t symlen = Q_strlen( symbols[i + 1] );

                Msg( "Checking if Valve '%s' converts to Itanium '%s'...\n", symbols[i], symbols[i+1] );

                char **ret = COM_ConvertToLocalPlatform( MANGLE_ITANIUM, symbols[i], &numfuncs );

                TASSERT( numfuncs == 3 );
                TASSERT( !Q_strncmp( ret[0], symbols[i+1], symlen ));
                TASSERT( !Q_strncmp( ret[1], symbols[i+1], symlen ));
                TASSERT( !Q_strncmp( ret[2], symbols[i+1], symlen ));
        }
}

void Test_RunLibCommon( void )
{
        TRUN( Test_DetectMangleType() );
        TRUN( Test_GetMSVCName() );
        TRUN( Test_GetItaniumName() );
        TRUN( Test_ConvertFromValveToLocal() );
}
#endif /* XASH_ENGINE_TESTS */
