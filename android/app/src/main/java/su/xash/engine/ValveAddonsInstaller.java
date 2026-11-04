package su.xash.engine;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Extracts assets/valve-addons.zip into the Half-Life (valve) game tree.
 *
 * The engine APK embeds the AMX Mod X / Metamod add-on bundle for the valve
 * game dir, but unlike the cs16client app (which unpacks addons.zip through
 * its MainActivity) NOTHING ever wrote it to disk: the on-disk
 * &lt;basedir&gt;/valve/addons was only ever populated once by hand and an app
 * update could never refresh it. That left a stale libamxx_sqlite .so in
 * place which crashed the game on shutdown.
 *
 * This installer closes the gap. It unpacks the bundle at launch - before the
 * engine starts - gated by a version key plus an on-disk marker, always
 * refreshes the shipped native modules (a stale .so is exactly the bug being
 * fixed) and never touches the user's own configs or saved plugin data.
 *
 * The zip carries "valve/..." entries and is unpacked straight into the
 * basedir so they land under &lt;basedir&gt;/valve/....
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
	private static final String ADDONS_VERSION = "amxx-1.9.0.5314-r5";

	private static final String ZIP_ASSET = "valve-addons.zip";
	private static final String MARKER_REL =
		"valve/addons/metamod/dlls/libmetamod_android_arm64.so";

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

	private static int extract( Context ctx, File basedir ) throws Exception {
		String destCanon = basedir.getCanonicalPath();
		byte[] buf = new byte[65536];
		int count = 0;

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

				// native libraries may need the exec bit
				if( rel.endsWith( ".so" ))
					out.setExecutable( true, false );

				count++;
				zis.closeEntry();
			}
		} finally {
			zis.close();
		}
		return count;
	}

	/**
	 * The shipped native modules and the two module lists must always match
	 * the bundle (a stale .so is exactly the bug this installer fixes); the
	 * user's own configs and saved plugin data are never touched.
	 */
	private static boolean mayOverwrite( File out, String rel ) {
		if( rel.endsWith( ".so" ))
			return true;	// shipped modules: always refresh
		if( rel.endsWith( "modules.ini" ) || rel.endsWith( "hamdata.ini" ))
			return true;	// module lists must match the .so set
		// shipped localization: menu labels resolve through L() against
		// resource/*_english.txt and a stale disk copy blanks them (the
		// round-130 "menu text is blank" report) - app assets, not user
		// data - always refresh
		if( rel.endsWith( "_english.txt" ))
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
