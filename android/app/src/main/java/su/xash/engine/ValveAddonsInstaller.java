package su.xash.engine;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Extracts assets/valve-addons.zip into the Half-Life (valve) game tree.
 *
 * The engine APK embeds the AMX Mod X / Metamod add-on bundle for the valve
 * game dir, but unlike the cs16client app (which unpacks addons.zip through
 * its MainActivity) NOTHING ever wrote it to disk: the on-disk
 * <basedir>/valve/addons was only ever populated once by hand and an app
 * update could never refresh it. That left a stale libamxx_sqlite .so in
 * place which crashed the game on shutdown.
 *
 * This installer closes the gap. It unpacks the bundle at launch - before the
 * engine starts - gated by a version key plus an on-disk marker, always
 * refreshes the shipped native modules (a stale .so is exactly the bug being
 * fixed) and never touches the user's own configs or saved plugin data.
 *
 * The zip carries "valve/..." entries and is unpacked straight into the
 * basedir so they land under <basedir>/valve/....
 *
 * build 137 (items 3+4): the exception above ("always refresh the shipped
 * native modules") is now retired - NATIVE LIBRARIES ARE NEVER WRITTEN TO
 * /sdcard ANY MORE. Metamod, the AMX Mod X core and every libamxx_* module
 * ship in this APK's nativeLibraryDir (staged by build_native.sh) and the
 * engine sources them from XASH3D_GAMELIBDIR, which by construction holds
 * exactly one ABI. Extraction now skips every .so/.dll entry (the zip stays
 * byte-identical - only the EXTRACTION changes), a purge in install()
 * removes the leftovers older builds wrote to disk, and both are ELF-verified
 * against this APK's own ABI so the log states which architecture was lying
 * around. Restarting the AMXX chain from the clean gate: ADDONS_VERSION
 * r6 -> r7 and the marker is now the ABI-neutral addons/metamod/plugins.ini.
 */
final class ValveAddonsInstaller {
	private static final String TAG = "ValveAddonsInstaller";

	// bump when the bundled valve add-on set changes. The engine app's
	// versionName is git-hash based and does NOT change per build, so the
	// add-ons need their own key to force a re-extract.
	// r2: adds the weaponmod.games gamedata (weaponmod will not attach
	//     without it) - a build-23 install already has this key set and
	//     would otherwise never pick the new files up.
	// 5314: fakemeta/hamsandwich pdata arm64 table gains the m_signals
	//     sub-fields (m_flSignal/m_flState), fixing the "untranslated arm64
	//     cell key=240" plugin offsets seen with heavy mods.
	// r3: build 48 adds valve/resource (original CS 1.6 menu collage:
	//     BackgroundLayout.txt + background/*.tga) - an install from an
	//     earlier build has this key set and would never extract the
	//     new files, leaving the menu on the flat-colour fallback.
	// r5: build 129 (round 130) always refreshes resource/*_english.txt
	//     (a stale localization file blanks every menu label - the
	//     round-130 "menu text is blank" report) - bump so a current
	//     install picks the shipped files up exactly once.
	// r6: build 130 (round 131) ships gfx/shell/colors.lst with the coded
	//     palette and always refreshes it - a stale on-device colors file
	//     repaints the menu orange (the round-131 screenshots).
	// r7: build 137 (round 136 items 3+4) - the bundle's .so/.dll entries
	//     are no longer extracted and every native library already on disk
	//     under valve/addons is purged (libraries now load from this APK's
	//     nativeLibraryDir), so ONE extract of the metadata runs here.
	private static final String ADDONS_VERSION = "amxx-1.9.0.5314-r7";

	private static final String ZIP_ASSET = "valve-addons.zip";
	// A7 build 137: was valve/addons/metamod/dlls/libmetamod_android_arm64.so
	// - an arm64-only native library can never be the marker again once no
	// .so is written (the gate would re-extract on every 32-bit device).
	private static final String MARKER_REL =
		"valve/addons/metamod/plugins.ini";

	private ValveAddonsInstaller() {
	}

	/**
	 * Called once at launch, before the engine starts, with the resolved
	 * basedir. Never throws: a failed add-on refresh must not block the game.
	 */
	static void install( Context ctx, File basedir ) {
		try {
			SharedPreferences prefs = ctx.getSharedPreferences(
				"xash_preferences", Context.MODE_PRIVATE );

			// A7 build 137: BEFORE the gate - a warm (unchanged) install must
			// clean the old .so too, else only a bundle change would ever purge
			purgeNativeLibs( ctx, basedir );

			File marker = new File( basedir, MARKER_REL );
			if( ADDONS_VERSION.equals(
					prefs.getString( "valve_addons_version", null ) )
					&& marker.isFile() )
				return;	// already current for this add-on set

			int n = extract( ctx, basedir );
			prefs.edit().putString( "valve_addons_version", ADDONS_VERSION ).apply();
			Log.i( TAG, "valve add-ons refreshed: " + n + " files -> "
				+ basedir.getAbsolutePath() );
		} catch( Throwable t ) {
			Log.w( TAG, "valve add-on extract failed", t );
		}
	}

	// ------------------------------------------------------------------
	// A7 build 137 (item 4): remove the native libraries older builds wrote
	// under valve/addons. Every candidate is read as an ELF header first
	// (e_machine/EI_CLASS) and compared against the ABI this APK packages,
	// so the log proves which architecture was sitting on disk - that is the
	// "reinstall does mixed and puts dll/.so on xash" report: the zips
	// carried _arm64 AND _armv7a files side by side and both were unwrapped.
	// Once per process (a second call in the same run has nothing left).
	// ------------------------------------------------------------------
	private static volatile boolean sPurged = false;

	/** "arm64" / "armv7a" from the ELF header, or null when not ARM so. */
	private static String detectAbi( File f ) {
		java.io.FileInputStream in = null;
		try {
			in = new java.io.FileInputStream( f );
			byte[] h = new byte[20];
			int got = 0;
			while( got < 20 ) {
				int n = in.read( h, got, 20 - got );
				if( n <= 0 )
					break;
				got += n;
			}
			if( got < 20 )
				return null;
			if( h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F' )
				return null;
			int cls = h[4] & 0xff;		// 1 = 32-bit, 2 = 64-bit
			int machine = ( h[18] & 0xff ) | (( h[19] & 0xff ) << 8 );
			if( machine == 183 && cls == 2 )
				return "arm64";
			if( machine == 40 && cls == 1 )
				return "armv7a";
			return null;
		} catch( Throwable t ) {
			return null;
		} finally {
			try { if( in != null ) in.close(); } catch( Throwable t ) {}
		}
	}

	/**
	 * The Modules-tab registry (valve + cstrike apps both write it on the
	 * gamedir). Format: "<kind> <base>" per line - entries are the
	 * USER's own installed modules, so the purge must never remove them.
	 */
	private static java.util.HashSet<String> readCustomBases( File basedir ) {
		java.util.HashSet<String> out = new java.util.HashSet<String>();
		try {
			File f = new File( basedir, "valve/addons/.custom-modules" );
			if( !f.isFile() )
				return out;
			java.io.BufferedReader br =
				new java.io.BufferedReader( new java.io.FileReader( f ));
			String line;
			while(( line = br.readLine() ) != null ) {
				line = line.trim();
				if( line.isEmpty() || line.startsWith( "#" ))
					continue;
				String[] p = line.split( "\\s+" );
				if( p.length >= 2 )
					out.add( p[1].toLowerCase( Locale.US ));
			}
			br.close();
		} catch( Throwable t ) {
		}
		return out;
	}

	/** module base the registry uses: dir, ".so", ABI suffix, lib prefix gone. */
	private static String baseNameOf( String filename ) {
		String s = filename == null ? "" : filename.trim();
		int slash = Math.max( s.lastIndexOf( '/' ), s.lastIndexOf( '\\' ));
		if( slash >= 0 )
			s = s.substring( slash + 1 );
		if( s.toLowerCase( Locale.US ).endsWith( ".so" ))
			s = s.substring( 0, s.length() - 3 );
		String[] suf = { "_arm64", "_armv7a", "_armv7l", "_armv7", "_x86_64", "_amd64", "_x86" };
		boolean changed = true;
		while( changed ) {
			changed = false;
			String low = s.toLowerCase( Locale.US );
			for( String x : suf ) {
				if( s.length() > x.length() && low.endsWith( x )) {
					s = s.substring( 0, s.length() - x.length() );
					changed = true;
				}
			}
		}
		String low = s.toLowerCase( Locale.US );
		if( low.startsWith( "libamxx_" ))
			s = s.substring( 8 );
		else if( low.startsWith( "lib" ))
			s = s.substring( 3 );
		return s.trim();
	}

	/** the single ABI this APK packages, from its own lib dir. */
	private static String detectApkAbi( Context ctx ) {
		try {
			File[] libs = new File( ctx.getApplicationInfo().nativeLibraryDir ).listFiles();
			if( libs != null ) {
				for( File f : libs ) {
					if( f.getName().endsWith( ".so" )) {
						String a = detectAbi( f );
						if( a != null )
							return a;
					}
				}
			}
		} catch( Throwable t ) {
		}
		return null;
	}

	private static void purgeNativeLibs( Context ctx, File basedir ) {
		if( sPurged )
			return;
		sPurged = true;

		String apkAbi = detectApkAbi( ctx );
		java.util.HashSet<String> customs = readCustomBases( basedir );
		String[] roots = {
			"valve/addons/metamod/dlls",
			"valve/addons/amxmodx/dlls",
			"valve/addons/amxmodx/modules",
		};
		int removed = 0, foreign = 0, kept = 0;

		for( String r : roots ) {
			File[] files = new File( basedir, r ).listFiles();
			if( files == null )
				continue;

			for( File f : files ) {
				if( !f.isFile() )
					continue;

				String n = f.getName().toLowerCase( Locale.ROOT );
				if( !n.endsWith( ".so" ) && !n.endsWith( ".dll" ))
					continue;

				// registered by the Modules tab: the user's own file -
				// the shipped-library purge skips it
				if( customs.contains(
						baseNameOf( f.getName()).toLowerCase( Locale.US ))) {
					kept++;
					Log.i( TAG, "[A7] build137: kept user module " + r + "/"
						+ f.getName() );
					continue;
				}

				String abi = detectAbi( f );
				if( apkAbi != null && abi != null && !apkAbi.equals( abi ))
					foreign++;

				Log.i( TAG, "[A7] build137: purge sdcard lib " + r + "/"
					+ f.getName() + " (abi "
					+ ( abi != null ? abi : "unknown" )
					+ ", apk abi " + ( apkAbi != null ? apkAbi : "unknown" ) + ")" );

				if( f.delete() )
					removed++;
				else
					Log.w( TAG, "[A7] build137: could not remove "
						+ f.getAbsolutePath() );
			}
		}

		Log.i( TAG, "[A7] build137: valve purge sdcard lib done - " + removed
			+ " removed, wrong-abi " + foreign + ", customs kept " + kept
			+ ", apk abi " + ( apkAbi != null ? apkAbi : "unknown" ) );
	}

	private static int extract( Context ctx, File basedir ) throws Exception {
		String destCanon = basedir.getCanonicalPath();
		byte[] buf = new byte[65536];
		int count = 0;
		int libSkipped = 0;

		InputStream in = ctx.getAssets().open( ZIP_ASSET );
		ZipInputStream zis = new ZipInputStream( in );
		try {
			ZipEntry entry;
			while(( entry = zis.getNextEntry() ) != null ) {
				if( entry.isDirectory() )
					continue;

				String rel = entry.getName();
				File out = new File( basedir, rel );

				// zip-slip guard
				if( !out.getCanonicalPath().startsWith( destCanon + File.separator )) {
					Log.w( TAG, "skipping suspicious zip entry: " + rel );
					continue;
				}

				// A7 build 137 (item 4): native libraries stay in the APK -
				// the engine resolves them from XASH3D_GAMELIBDIR
				// (nativeLibraryDir, single ABI). The zip entry itself is
				// untouched (content gate), only this write is skipped and
				// any older on-disk copy is dropped.
				if( rel.endsWith( ".so" ) || rel.endsWith( ".dll" )) {
					if( out.isFile() && out.delete() )
						Log.i( TAG, "[A7] build137: removed leftover sdcard ll " + rel );
					libSkipped++;
					zis.closeEntry();
					continue;
				}

				if( !mayOverwrite( out, rel )) {
					zis.closeEntry();
					continue;
				}

				File parent = out.getParentFile();
				if( parent != null && !parent.isDirectory() )
					parent.mkdirs();

				FileOutputStream fos = new FileOutputStream( out );
				try {
					int n;
					while(( n = zis.read( buf )) > 0 )
						fos.write( buf, 0, n );
				} finally {
					fos.close();
				}

				count++;
				zis.closeEntry();
			}
		} finally {
			zis.close();
		}

		// A7 build 137: proof line - how many library entries the bundle
		// carried versus how many landed on /sdcard (always 0 now)
		Log.i( TAG, "[A7] build137: valve addons zip " + libSkipped
			+ " libs served from APK, 0 written to /sdcard" );
		return count;
	}

	/**
	 * The two module lists must always match the bundle; the user's own
	 * configs and saved plugin data are never touched. The .so branch is
	 * gone with build 137: no native library reaches the gamedir at all.
	 */
	private static boolean mayOverwrite( File out, String rel ) {
		if( rel.endsWith( "modules.ini" ) || rel.endsWith( "hamdata.ini" ))
			return true;	// module lists must match the shipped set
		// shipped localization: menu labels resolve through L() against
		// resource/*_english.txt and a stale disk copy blanks them (the
		// round-130 "menu text is blank" report) - app assets, not user
		// data - always refresh
		if( rel.endsWith( "_english.txt" ))
			return true;
		// shipped GameUI palette: UI_ApplyCustomColors() parses
		// gfx/shell/colors.lst once at UI_Init and a stale on-disk copy
		// repaints the menu (the round-131 "rows draw orange" report) -
		// app asset, not user data - always refresh
		if( rel.endsWith( "gfx/shell/colors.lst" ))
			return true;
		if( rel.endsWith( ".ewp" ) && out.isFile() )
			return false;	// user waypoints
		if( rel.contains( "/addons/amxmodx/data/file_vault/" ) && out.isFile() )
			return false;	// saved plugin progress
		// user configs (plugins.ini, users.ini, cvars...) - keep the disk copy
		if( rel.contains( "/addons/amxmodx/configs/" ) && out.isFile() )
			return false;
		return true;
	}
}