/*
android_lib.c - dynamic library code for Android OS
Copyright (C) 2018 Flying With Gauss

This program is free software: you can redistribute it and/sor modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#include <dlfcn.h>
#include "common.h"
#include "library.h"
#include "filesystem.h"
#include "server.h"
#include "platform/android/lib_android.h"
#include "platform/android/dlsym-weak.h" // Android < 5.0

void *ANDROID_LoadLibrary( const char *path )
{
        const char *name = COM_FileWithoutPath( path );
        void *handle;

        Con_Reportf( "%s: loading \"%s\" (name: \"%s\")\n", __func__, path, name );

        // try the path verbatim first. Callers may hand us an absolute
        // path outside every search rule below (e.g. the app-private amxx dir
        // that COM_AMXX_Setup populates). Without this, the final dlopen( name )
        // only searches LD_LIBRARY_PATH and misleadingly reports
        // "library <basename> not found" even though the file exists and is
        // loadable (dlopen from app-private data is allowed by AOSP sepolicy;
        // only execve() is restricted on targetSdk >= 29).
        handle = dlopen( path, RTLD_NOW );

        if( handle )
        {
                Con_Reportf( "%s: loaded from verbatim path\n", __func__ );
                return handle;
        }

        {
                const char *err = dlerror();

                Con_Reportf( "%s: verbatim path failed: %s\n", __func__, err ? err : "unknown error" );
                COM_PushLibraryError( err ? err : "unknown dlopen error" );
        }

        // TODO: remove this once distributing games from APKs will be deprecated
        const char *gamelibdir = getenv( "XASH3D_GAMELIBDIR" );
        if( !COM_StringEmptyOrNULL( gamelibdir ))
        {
                char fullpath[MAX_SYSPATH];
                Q_snprintf( fullpath, sizeof( fullpath ), "%s/%s", gamelibdir, name );

                Con_Reportf( "%s: trying APK path \"%s\"\n", __func__, fullpath );

                handle = dlopen( fullpath, RTLD_NOW );
                if( handle )
                {
                        Con_Reportf( "%s: loaded from APK path\n", __func__ );
                        return handle;
                }

                {
                        const char *err = dlerror();

                        Con_Reportf( "%s: APK path failed: %s\n", __func__, err ? err : "unknown error" );
                        COM_PushLibraryError( err ? err : "unknown dlopen error" );
                }
        }

        // try VFS
        dll_user_t *hInst = FS_FindLibrary( path, false );
        if( hInst )
        {
                Con_Reportf( "%s: VFS found \"%s\"\n", __func__, hInst->fullPath );
                if( !hInst->custom_loader )
                {
                        char libpath[MAX_SYSPATH];
                        Q_strncpy( libpath, hInst->fullPath, sizeof( libpath ));
                        Mem_Free( hInst );

                        handle = dlopen( libpath, RTLD_NOW );
                        if( handle )
                        {
                                Con_Reportf( "%s: loaded from VFS path\n", __func__ );
                                return handle;
                        }
                        {
                                const char *err = dlerror();

                                Con_Reportf( "%s: VFS dlopen failed: %s\n", __func__, err ? err : "unknown error" );
                                COM_PushLibraryError( err ? err : "unknown dlopen error" );
                        }
                }
                else
                {
                        Con_Reportf( "%s: VFS entry has custom loader, skipping\n", __func__ );
                        COM_PushLibraryError( "custom loader not available on Android" );
                        Mem_Free( hInst );
                }
        }

        // find in system search path (APK's LD_LIBRARY_PATH)
        Con_Reportf( "%s: trying LD_LIBRARY_PATH for \"%s\"\n", __func__, name );
        handle = dlopen( name, RTLD_NOW );
        if( handle )
        {
                Con_Reportf( "%s: loaded from LD_LIBRARY_PATH\n", __func__ );
                return handle;
        }

        {
                const char *err = dlerror();

                Con_Reportf( "%s: all paths failed for \"%s\": %s\n", __func__, path, err ? err : "unknown error" );
                COM_PushLibraryError( err ? err : "unknown dlopen error" );
        }

        return NULL;
}

void *ANDROID_GetProcAddress( void *hInstance, const char *name )
{
        void *p = dlsym( hInstance, name );

#ifndef XASH_64BIT
        if( p ) return p;

        p = dlsym_weak( hInstance, name );
#endif // XASH_64BIT

        return p;
}
