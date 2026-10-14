/*
crash_posix.c - advanced crashhandler
Copyright (C) 2016 Mittorn

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

#if XASH_FREEBSD || XASH_NETBSD || XASH_OPENBSD || XASH_ANDROID || XASH_LINUX || XASH_APPLE
#include <signal.h>
#include <sys/mman.h>
#if XASH_ANDROID
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <android/log.h>
// self-contained crash forensics. The NDK
// libbacktrace unwinder often stops at the signal-delivery frames
// (libsigchain/vdso) and reports nothing useful about the fault itself.
// These helpers read the crashed context straight from ucontext (PC, LR,
// SP resolved to module+offset through dladdr), dump the loaded module
// map and walk the stack with _Unwind_Backtrace. That is enough to tell
// WHICH library (engine, metamod, amxx module, game dll) faulted.
#include <dlfcn.h>
#include <sys/ucontext.h>
#include <link.h>
#include <unwind.h>
#endif
#include "library.h"
#include "input.h"
#include "crash.h"

#if XASH_ANDROID
static char crashlog_path[MAX_OSPATH];
static char enginelog_path[MAX_OSPATH];
#endif

static qboolean have_libbacktrace = false;
static char crash_message[8192];

#if XASH_ANDROID
#define CRASH_MAX_FRAMES 32

static int crash_logfd = -1;
static int crash_fd = -1;

static void Sys_CrashEmit( const char *line, size_t len )
{
	ssize_t unused;

	if( len <= 0 )
		return;

	unused = write( STDERR_FILENO, line, len );

	if( crash_logfd >= 0 )
		unused = write( crash_logfd, line, len );

	if( crash_fd >= 0 )
		unused = write( crash_fd, line, len );
	(void)unused;
}

typedef struct
{
	uintptr_t pcs[CRASH_MAX_FRAMES];
	int count;
	int skip;
} crash_unwind_t;

static _Unwind_Reason_Code Sys_CrashUnwindCb( struct _Unwind_Context *ctx, void *arg )
{
	crash_unwind_t *st = (crash_unwind_t *)arg;
	uintptr_t pc = _Unwind_GetIP( ctx );

	if( !pc )
		return _URC_NO_REASON;

	if( st->skip > 0 )
	{
		st->skip--;
		return _URC_NO_REASON;
	}

	if( st->count >= CRASH_MAX_FRAMES )
		return _URC_END_OF_STACK;

	st->pcs[st->count++] = pc;
	return _URC_NO_REASON;
}

// resolve an instruction address to "module.so + 0x1234 (symbol)"
static int Sys_CrashFormatModule( char *buf, size_t size, int idx, uintptr_t pc )
{
	Dl_info info;
	uintptr_t base = 0;
	const char *name = NULL;
	int len;

	if( dladdr(( void * )pc, &info ) && info.dli_fname )
	{
		name = Q_strrchr( info.dli_fname, '/' );
		name = name ? name + 1 : info.dli_fname;
		base = ( uintptr_t )info.dli_fbase;

		if( info.dli_sname && info.dli_saddr )
			len = Q_snprintf( buf, size, " %02d: 0x%016lx  %s+0x%lx  (%s+0x%lx)\n",
				idx, ( unsigned long )pc, info.dli_sname,
				( unsigned long )( pc - ( uintptr_t )info.dli_saddr ),
				name, ( unsigned long )( pc - base ));
		else
			len = Q_snprintf( buf, size, " %02d: 0x%016lx  %s+0x%lx\n",
				idx, ( unsigned long )pc, name, ( unsigned long )( pc - base ));
	}
	else
	{
		len = Q_snprintf( buf, size, " %02d: 0x%016lx  (unknown module)\n", idx, ( unsigned long )pc );
	}

	return len < 0 ? 0 : len;
}

static int Sys_CrashModuleMapCb( struct dl_phdr_info *info, size_t size, void *data )
{
	char line[512];
	int len;

	(void)size;
	(void)data;

	if( !info->dlpi_name || !info->dlpi_name[0] )
		return 0;

	// keep the report small: only real game-relevant libraries, skip
	// system/framework noise (libc, art, boot oats etc.)
	if( !Q_strstr( info->dlpi_name, ".so" ))
		return 0;

	if( Q_strstr( info->dlpi_name, "/apex/" ) || Q_strstr( info->dlpi_name, "/system/" ) || Q_strstr( info->dlpi_name, "/vendor/" ))
		return 0;

	len = Q_snprintf( line, sizeof( line ), " module: 0x%016lx %s\n",
		( unsigned long )info->dlpi_addr, info->dlpi_name );

	if( len > 0 )
		Sys_CrashEmit( line, ( size_t )len );

	return 0;
}

// dump the precise crashed context (PC/LR resolved to library+offset
// from ucontext), the module map and an unwind walk. Names the guilty
// library even when the normal unwinder cannot pass the signal frame.
static void Sys_CrashAndroidForensics( int signal, siginfo_t *si, void *context, int logfd, int crashfd )
{
	ucontext_t *uc = (ucontext_t *)context;
	uintptr_t pc = 0, lr = 0, sp = 0;
	char line[512];
	int len;
	int i;

	crash_logfd = logfd;
	crash_fd = crashfd;

#if defined( __aarch64__ )
	if( uc )
	{
		pc = ( uintptr_t )uc->uc_mcontext.pc;
		sp = ( uintptr_t )uc->uc_mcontext.sp;
		lr = ( uintptr_t )uc->uc_mcontext.regs[30];
	}
#elif defined( __arm__ )
	if( uc )
	{
		pc = ( uintptr_t )uc->uc_mcontext.arm_pc;
		sp = ( uintptr_t )uc->uc_mcontext.arm_sp;
		lr = ( uintptr_t )uc->uc_mcontext.arm_lr;
	}
#else
	(void)uc;
#endif

	len = Q_snprintf( line, sizeof( line ), "Crash: signal %d errno %d code %d faultaddr %p\n",
		signal, si ? si->si_errno : 0, si ? si->si_code : 0, si ? si->si_addr : NULL );
	Sys_CrashEmit( line, ( size_t )len );

	// the faulting instruction itself: names the guilty library even
	// when the stack cannot be unwound at all
	if( pc )
	{
		len = Sys_CrashFormatModule( line, sizeof( line ), 0, pc );
		Sys_CrashEmit( line, ( size_t )len );
	}

	if( lr )
	{
		len = Sys_CrashFormatModule( line, sizeof( line ), 1, lr );
		Sys_CrashEmit( line, ( size_t )len );
	}

	if( pc && sp )
	{
		len = Q_snprintf( line, sizeof( line ), " PC=0x%016lx LR=0x%016lx SP=0x%016lx\n",
			( unsigned long )pc, ( unsigned long )lr, ( unsigned long )sp );
		Sys_CrashEmit( line, ( size_t )len );
	}

	// loaded module map: base addresses for offset -> symbol conversion
	// against the unstripped libs shipped as build artifacts
	dl_iterate_phdr( Sys_CrashModuleMapCb, NULL );

	// stack walk (best effort; libbacktrace often stops early here)
	{
		crash_unwind_t st;
		memset( &st, 0, sizeof( st ));

		st.skip = 2; // skip the handler + trampoline frames
		_Unwind_Backtrace( Sys_CrashUnwindCb, &st );

		for( i = 0; i < st.count; i++ )
		{
			len = Sys_CrashFormatModule( line, sizeof( line ), i + 2, st.pcs[i] );
			Sys_CrashEmit( line, ( size_t )len );
		}
	}

	len = Q_snprintf( line, sizeof( line ), "=== end of crash forensics ===\n" );
	Sys_CrashEmit( line, ( size_t )len );

	crash_logfd = -1;
	crash_fd = -1;
}
#endif // XASH_ANDROID

static void Sys_Crash( int signal, siginfo_t *si, void *context )
{
	// safe actions first, stack and memory may be corrupted
	int len = Q_snprintf( crash_message, sizeof( crash_message ), "Ver: " XASH_ENGINE_NAME " " XASH_VERSION " (build %i-%s-%s, %s-%s)\n",
		Q_buildnum(), g_buildcommit, g_buildbranch, Q_buildos(), Q_buildarch() );

#if !XASH_FREEBSD && !XASH_NETBSD && !XASH_OPENBSD && !XASH_APPLE
	len += Q_snprintf( crash_message + len, sizeof( crash_message ) - len, "Crash: signal %d errno %d with code %d at %p %p\n", signal, si->si_errno, si->si_code, si->si_addr, si->si_ptr );
#else
	len += Q_snprintf( crash_message + len, sizeof( crash_message ) - len, "Crash: signal %d errno %d with code %d at %p\n", signal, si->si_errno, si->si_code, si->si_addr );
#endif

	ssize_t unused = write( STDERR_FILENO, crash_message, len );

#if XASH_ANDROID
	__android_log_write( ANDROID_LOG_FATAL, "Xash", crash_message );
#endif

	// now get log fd and write trace directly to log
	int logfd = Sys_LogFileNo();
	if( logfd >= 0 )
		unused = write( logfd, crash_message, len );
	(void)unused;

#if XASH_ANDROID
	// precise fault context + module map + unwind walk
	{
		int crashfd = -1;

		if( crashlog_path[0] )
			crashfd = open( crashlog_path, O_WRONLY|O_CREAT|O_TRUNC, 0644 );

		Sys_CrashAndroidForensics( signal, si, context, logfd, crashfd );

		if( crashfd >= 0 )
			close( crashfd );
	}
#endif

#if HAVE_LIBBACKTRACE
	qboolean detailed_message = false;
	if( have_libbacktrace && !detailed_message )
	{
		len = Sys_CrashDetailsLibbacktrace( logfd, crash_message, len, sizeof( crash_message ));
		detailed_message = true;
	}
#endif // HAVE_LIBBACKTRACE

#if XASH_ANDROID
	// also write to a dedicated crash report file the Java side picks up on next launch
	if( crashlog_path[0] )
	{
		int crashfd = open( crashlog_path, O_WRONLY|O_CREAT|O_TRUNC, 0644 );
		if( crashfd >= 0 )
		{
			write( crashfd, crash_message, len );
			close( crashfd );
		}
	}

	// make a copy of engine.log in staging directory
	// TODO: dump log from console buffers, if -log not enabled
	if( logfd >= 0 && enginelog_path[0] && lseek( logfd, 0, SEEK_SET ) == 0 )
	{
		int outfd = open( enginelog_path, O_WRONLY|O_CREAT|O_TRUNC, 0644 );
		if( outfd >= 0 )
		{
			static char buf[8192];
			while( 1 )
			{
				ssize_t n = read( logfd, buf, sizeof( buf ));
				if( n <= 0 )
					break;
				if( write( outfd, buf, (size_t)n ) != n )
					break;
			}
			close( outfd );
		}
	}

	// JNI/SDL calls aren't safe from a signal handler on Android
	_exit( 128 + signal );
#else
#if !XASH_DEDICATED
	IN_SetMouseGrab( false );
#endif
	host.status = HOST_CRASHED;

	// put MessageBox as Sys_Error
	Platform_MessageBox( "Xash Error", crash_message, false );

	// log saved, now we can try to save configs and close log correctly, it may crash
	if( host.type == HOST_NORMAL )
		CL_Crashed();

	Sys_Quit( "crashed" );
#endif // XASH_ANDROID
}

static struct sigaction old_segv_act;
static struct sigaction old_abrt_act;
static struct sigaction old_bus_act;
static struct sigaction old_ill_act;

void Sys_SetupCrashHandler( const char *argv0 )
{
	struct sigaction act =
	{
		.sa_sigaction = Sys_Crash,
		.sa_flags = SA_SIGINFO | SA_ONSTACK,
	};

#if XASH_ANDROID
	const char *crashdir = getenv( "XASH3D_CRASH_DIR" );

	if( !COM_StringEmptyOrNULL( crashdir ))
	{
		Q_snprintf( crashlog_path, sizeof( crashlog_path ), "%s/crash.log", crashdir );
		Q_snprintf( enginelog_path, sizeof( enginelog_path ), "%s/engine.log", crashdir );
	}

	// unblock the engine/SDL_main thread just in case
	sigset_t set;
	sigemptyset( &set );
	sigaddset( &set, SIGSEGV );
	sigaddset( &set, SIGABRT );
	sigaddset( &set, SIGBUS );
	sigaddset( &set, SIGILL );
	pthread_sigmask( SIG_UNBLOCK, &set, NULL );
#endif

#if HAVE_LIBBACKTRACE
	have_libbacktrace = Sys_SetupLibbacktrace( argv0 );
#endif // HAVE_LIBBACKTRACE

	sigaction( SIGSEGV, &act, &old_segv_act );
	sigaction( SIGABRT, &act, &old_abrt_act );
	sigaction( SIGBUS,  &act, &old_bus_act );
	sigaction( SIGILL,  &act, &old_ill_act );
}

void Sys_RestoreCrashHandler( void )
{
	sigaction( SIGSEGV, &old_segv_act, NULL );
	sigaction( SIGABRT, &old_abrt_act, NULL );
	sigaction( SIGBUS,  &old_bus_act, NULL );
	sigaction( SIGILL,  &old_ill_act, NULL );
}

#endif // XASH_FREEBSD || XASH_NETBSD || XASH_OPENBSD || XASH_ANDROID || XASH_LINUX
