package su.xash.engine;

import android.annotation.SuppressLint;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings.Secure;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.libsdl.app.SDLActivity;

import su.xash.engine.util.CrashReports;
import su.xash.engine.util.SoftKeyboardPan;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Arrays;
import java.util.List;

public class XashActivity extends SDLActivity {
	private boolean mUseVolumeKeys;
	private volatile float[] mWindowInsets = new float[4];
	private String mPackageName;
	private static final String TAG = "XashActivity";

	// resolved XASH3D_BASEDIR; reused by the add-on installer and getArguments
	private File resolveBaseDir() {
		Intent intent = getIntent();
		String basedir = intent != null ? intent.getStringExtra("basedir") : null;
		if (basedir != null)
			return new File(basedir);
		return new File(Environment.getExternalStorageDirectory(), "xash");
	}

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		// refresh the bundled Half-Life add-ons BEFORE the engine starts: the
		// on-disk valve/addons modules must match this APK (a stale sqlite .so
		// crashed on shutdown and nothing else ever wrote the bundle to disk).
		// Marker-gated, so this is a no-op once current. Runs pre-super so it is
		// guaranteed to finish before the native engine loads metamod/AMXX.
		ValveAddonsInstaller.install(this, resolveBaseDir());

		super.onCreate(savedInstanceState);

		setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
			//getWindow().addFlags(WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES);
			getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
		}

		SoftKeyboardPan.assistActivity(this);

		// mSurface is null if SDL failed to start and shows its error dialog
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && mSurface != null) {
			mSurface.setOnApplyWindowInsetsListener((view, insets) -> {
				updateWindowInsets(view, insets);
				return insets;
			});
			mSurface.addOnLayoutChangeListener((view, l, t, r, b, ol, ot, or, ob) ->
				updateWindowInsets(view, view.getRootWindowInsets()));
			mSurface.requestApplyInsets();
		}
	}

	// JNI reads a snapshot; all View access stays on Android's UI thread.
	private float[] getWindowInsets() {
		return mWindowInsets;
	}

	private void updateWindowInsets(View surface, WindowInsets insets) {
		// Full screen: always report zero safe-area insets so the menu, console and
		// touch controls span the entire display and ignore the punch-hole cutout.
		mWindowInsets = new float[] { 0f, 0f, 0f, 0f };
	}

	@Override
	public void onDestroy() {
		super.onDestroy();

		// Now that we don't exit from native code, we need to exit here, resetting
		// application state (actually global variables that we don't cleanup on exit)
		//
		// When the issue with global variables will be resolved, remove that exit() call
		System.exit(0);
	}

	@Override
	protected String[] getLibraries() {
		return new String[]{"SDL2", "xash"};
	}

	@SuppressLint("HardwareIds")
	private String getAndroidID() {
		return Secure.getString(getContentResolver(), Secure.ANDROID_ID);
	}

	@SuppressLint("ApplySharedPref")
	private void saveAndroidID(String id) {
		getSharedPreferences("xash_preferences", MODE_PRIVATE).edit().putString("xash_id", id).commit();
	}

	private String loadAndroidID() {
		return getSharedPreferences("xash_preferences", MODE_PRIVATE).getString("xash_id", "");
	}

	// ---------------------------------------------------------------------
	// A7 build 97: full-screen loading window
	//
	// The engine raises it from the loading plaque (connect, local server
	// start, changelevel), so every load phase begins on this window - a
	// striped shade with the logo, the classic olive panel, a segmented
	// progress bar and a Cancel button - instead of the GL surface, which
	// is still presenting black frames while the signon runs. It sits above
	// the surface (addContentView), so nothing the engine draws underneath
	// can leak through. The transfer block below the main bar opens while a
	// server file is coming down.
	// ---------------------------------------------------------------------

	// A7 build 99: the loading dialog wears the REAL CS 1.6 GameUI palette
	// (cs16.css tokens sampled from the menu retheme) with a classic
	// raised/sunken bevel - the goldsource window look, not a flat box.
	private static final int LOADING_SHADE = 0xE6000000;
	private static final int LOADING_PANEL_BG = 0xFF4C5844;     // #4c5844 panel bg
	private static final int LOADING_SECONDARY_BG = 0xFF3E4637; // #3e4637 input/list bg
	private static final int LOADING_BORDER_LT = 0xFF889180;    // #889180 raised top/left
	private static final int LOADING_BORDER_DK = 0xFF282E22;    // #282e22 raised bottom/right
	private static final int LOADING_ACCENT = 0xFFC4B550;       // #c4b550 gold accent
	private static final int LOADING_TITLE = 0xFFEAF0E6;        // window caption (bright)
	private static final int LOADING_TEXT = 0xFFD8DED3;         // #d8ded3 control labels
	private static final int LOADING_BAR_TRACK = 0xFF282E22;    // sunken bar track
	private static final int LOADING_BAR_BORDER = 0xFF889180;
	private static final int LOADING_BAR_FILL = 0xFFC4B550;     // gold segments

	private FrameLayout mLoadingOverlay;
	private LoadingBar mLoadingBar;
	private TextView mLoadingStatus;
	// build 106 (user: "remove loading percent only"): the percent number
	// beside the main bar is gone for good - the bar still fills, and the
	// transfer row keeps its bare file counter (no % sign)
	private TextView mLoadingDlCount;
	private int mLoadingDlFiles;
	private String mLoadingServer = "";

	// Cancel rides beside the main bar, or beside the transfer bar while
	// one runs - like the pc dialog
	private LinearLayout mLoadingBarRow;
	private LinearLayout mLoadingDlRow;
	private Button mLoadingCancel;

	// transfer block: current file, its bar, status/footer lines, only
	// visible while a server file is being fetched
	private TextView mLoadingDlName;
	private TextView mLoadingDlStatus;
	private LoadingBar mLoadingDlBar;
	private TextView mLoadingDlFooter;

	private int dp(int v) {
		return Math.round(v * getResources().getDisplayMetrics().density);
	}

	/** the segmented yellow bar.
	 *  build 103 (user: "fix loading return how it was"): back to the
	 *  build-100 behaviour - the gold segments fill from the left up to the
	 *  percent the engine reports, and the number beside it reads the same
	 *  value. The build-101 marching-sweep busy indicator is gone. */
	private static class LoadingBar extends View {
		private float mPercent = 0f;

		LoadingBar(Context c) { super(c); }

		void setPercent(float p) {
			if (p < 0f) p = 0f;
			if (p > 100f) p = 100f;
			if (Float.compare(p, mPercent) != 0) {
				mPercent = p;
				invalidate(); // repaint with the new fill
			}
		}

		float getPercent() {
			return mPercent;
		}

		@Override
		protected void onDraw(Canvas canvas) {
			float w = getWidth(), h = getHeight();
			Paint p = new Paint();

			p.setColor(LOADING_BAR_TRACK);
			canvas.drawRect(0, 0, w, h, p);

			final int segments = 18;
			float gap = Math.max(2, w * 0.012f);
			float segW = (w - gap * (segments + 1)) / segments;
			float insetY = Math.max(2, h * 0.16f);

			// build 103: real fill again - left to right up to mPercent, the
			// build-100 way. setPercent() invalidates on every change, no
			// animation loop of its own
			p.setColor(LOADING_BAR_FILL);
			int lit = Math.round(mPercent * segments / 100f);
			if (lit > segments) lit = segments;
			for (int i = 0; i < lit; i++) {
				float x = gap + i * (segW + gap);
				canvas.drawRect(x, insetY, x + segW, h - insetY, p);
			}

			// A7 build 99: sunken bevel frame instead of a flat stroke -
			// dark top/left, light bottom/right = the classic GoldSource
			// progress-bar trough (cs16.css).
			float t = Math.max(1f, getResources().getDisplayMetrics().density);
			p.setStyle(Paint.Style.FILL);
			p.setColor(LOADING_BORDER_DK);
			canvas.drawRect(0, 0, w, t, p);              // top
			canvas.drawRect(0, 0, t, h, p);              // left
			p.setColor(LOADING_BAR_BORDER);
			canvas.drawRect(0, h - t, w, h, p);          // bottom
			canvas.drawRect(w - t, 0, w, h, p);          // right
		}
	}

	// JNI entry points called from the engine thread (all View work is
	// pushed onto the UI thread; every callback tolerates a hidden window)

	public void loadingShow(final String serverAddr) {
		runOnUiThread(new Runnable() {
			@Override
			public void run() {
				try {
					showLoadingOnUiThread(serverAddr);
				} catch (Throwable t) {
					Log.w(TAG, "loadingShow failed", t);
				}
			}
		});
	}

	public void loadingStatus(final String text, final float percent) {
		runOnUiThread(new Runnable() {
			@Override
			public void run() {
				if (mLoadingOverlay == null) return;
				try {
					if (text != null && text.length() > 0) {
						if (mLoadingStatus != null &&
								!text.contentEquals(mLoadingStatus.getText()))
							mLoadingStatus.setText(text);
					}

					// build 106: only the bar reacts to percent now - the
					// number beside it was removed ("remove loading percent
					// only"); the file counter below still counts files
					float pct = percent;
					if (pct < 0f) pct = 0f;
					if (pct > 100f) pct = 100f;
					if (mLoadingBar != null) mLoadingBar.setPercent(pct);
				} catch (Throwable t) {
					Log.w(TAG, "loadingStatus failed", t);
				}
			}
		});
	}

	/** cancel sits beside whichever bar is last visible,
	 *  the pc dialog moves it between rows the same way */
	private void moveLoadingCancel(LinearLayout row) {
		if (mLoadingCancel == null || row == null) return;
		if (mLoadingCancel.getParent() == row) return;

		if (mLoadingCancel.getParent() instanceof ViewGroup)
			((ViewGroup) mLoadingCancel.getParent()).removeView(mLoadingCancel);

		LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.WRAP_CONTENT, dp(26));
		lp.leftMargin = dp(8);
		row.addView(mLoadingCancel, lp);
	}

	/** second bar of the loading dialog: engine calls it during
	 *  transfers with file name, fill 0..100 and info lines.
	 *  null file = hide the block. */
	public void loadingDownload(final String file, final float percent,
			final String status, final String footer) {
		runOnUiThread(new Runnable() {
			@Override
			public void run() {
				if (mLoadingOverlay == null) return;
				try {
					if (file == null || file.length() == 0) {
						if (mLoadingDlName != null) mLoadingDlName.setVisibility(View.GONE);
						if (mLoadingDlStatus != null) mLoadingDlStatus.setVisibility(View.GONE);
						if (mLoadingDlBar != null) mLoadingDlBar.setVisibility(View.GONE);
						if (mLoadingDlFooter != null) mLoadingDlFooter.setVisibility(View.GONE);
						if (mLoadingDlRow != null) mLoadingDlRow.setVisibility(View.GONE);
						moveLoadingCancel(mLoadingBarRow);
						return;
					}

					if (mLoadingDlName == null) return;

					mLoadingDlName.setVisibility(View.VISIBLE);
					// build 103: count every file this load pulls - the bare
					// number sits where the transfer percent used to, with no
					// % sign ("how many file like percent but don't add percent")
					if (!file.contentEquals(mLoadingDlName.getText())) {
						mLoadingDlName.setText(file);
						mLoadingDlFiles++;
						if (mLoadingDlCount != null) {
							mLoadingDlCount.setVisibility(View.VISIBLE);
							mLoadingDlCount.setText(Integer.toString(mLoadingDlFiles));
						}
					}

					if (mLoadingDlStatus != null) {
						if (status != null && status.length() > 0) {
							mLoadingDlStatus.setVisibility(View.VISIBLE);
							if (!status.contentEquals(mLoadingDlStatus.getText()))
								mLoadingDlStatus.setText(status);
						} else mLoadingDlStatus.setVisibility(View.GONE);
					}

					if (mLoadingDlBar != null) {
						mLoadingDlBar.setVisibility(View.VISIBLE);
						mLoadingDlBar.setPercent(percent);
					}

					// transfer running: cancel moves beside the transfer bar
					if (mLoadingDlRow != null) {
						mLoadingDlRow.setVisibility(View.VISIBLE);
						moveLoadingCancel(mLoadingDlRow);
					}

					if (mLoadingDlFooter != null) {
						if (footer != null && footer.length() > 0) {
							mLoadingDlFooter.setVisibility(View.VISIBLE);
							if (!footer.contentEquals(mLoadingDlFooter.getText()))
								mLoadingDlFooter.setText(footer);
						} else mLoadingDlFooter.setVisibility(View.GONE);
					}
				} catch (Throwable t) {
					Log.w(TAG, "loadingDownload failed", t);
				}
			}
		});
	}

	public void loadingHide() {
		runOnUiThread(new Runnable() {
			@Override
			public void run() {
				hideLoadingOverlay();
			}
		});
	}

	// Cancel tap: the window goes down right away (the engine is only
	// asked to disconnect afterwards, from ITS frame -
	// Host_ClientBegin). Static native, looked up by symbol in libxash.so
	private static native void nativeLoadingCancelled();

	// A7 round 100: scoreboard logo sources ---------------------------------
	//
	// The engine forwards every steamid it sees in userinfo once per
	// session (CL_AvatarUserInfo), the public profile photo is pulled and
	// saved as <gamedir>/media/avatars/av_<account>.png, and the client
	// scoreboard draws it on that player's row. "avatar_pick" runs the
	// system image chooser and saves <gamedir>/media/avatars/avatar_custom.png,
	// the first entry of my own row's logo chain.
	//
	// Everything below is called from the engine thread (avatarFetch from
	// userinfo parsing, avatarPick from the Options menu) and moves the
	// actual work off it; a fetch never blocks the frame.

	private static final int REQ_PICK_AVATAR = 4711;

	// one fetch per steamid per run, and retries are rate limited so a
	// filtered network cannot turn into a request storm
	private final java.util.Set<Long> mAvatarFetched =
		java.util.Collections.synchronizedSet( new java.util.HashSet<Long>() );

	// log once per id, keeps the console readable
	private final java.util.Set<Long> mAvatarFailLogged =
		java.util.Collections.synchronizedSet( new java.util.HashSet<Long>() );

	private static final Object AVATAR_RATE_LOCK = new Object();
	private static long sAvatarLastFetch;

	private static native void nativeConsolePrintf( String s );

	// A7 build 111: "a new avatar_custom.* pair just hit disk" - the engine
	// frees the cached texture + bumps @cl_avatargen on its next frame so
	// the Options preview / scoreboards update WITHOUT a rejoin
	private static native void nativeAvatarSaved();

	private static void consolePrintf( String s ) {
		try {
			nativeConsolePrintf( s );
		} catch ( Throwable t ) {
			Log.w( TAG, "consolePrintf failed", t );
		}
	}

	/** the game folder this activity runs (steamfriends uses the same default) */
	private String avatarGameDir() {
		try {
			Intent i = getIntent();
			String g = i != null ? i.getStringExtra( "gamedir" ) : null;
			if( g == null || g.length() == 0 )
				return "cstrike";
			return g;
		} catch ( Throwable t ) {
			return "cstrike";
		}
	}

	private File avatarDir() {
		try {
			File dir = new File( new File( resolveBaseDir(), avatarGameDir()), "media/avatars" );
			if( !dir.isDirectory() && !dir.mkdirs() && !dir.isDirectory())
				return null;
			return dir;
		} catch ( Throwable t ) {
			return null;
		}
	}

	public void avatarFetch( final long steamid64 ) {
		Long key = steamid64;
		if( mAvatarFetched.contains( key )) return;

		final long account = steamid64 - 76561197960265728L;
		if( account <= 0 || account > 0xFFFFFFFFL ) return;

		final File dir = avatarDir();
		if( dir == null ) {
			mAvatarFetched.add( key );
			return;
		}

		final File out = new File( dir, "av_" + account + ".png" );
		mAvatarFetched.add( key );

		Thread t = new Thread( new Runnable() {
			@Override public void run() {
				try {
					// a fresh copy from an earlier session is reused while it
					// still decodes; steam photos change, so anything older
					// than a day is pulled again, and broken leftovers are
					// not trusted either
					long stale = System.currentTimeMillis() - 24L * 60 * 60 * 1000;
					if( out.isFile() && out.length() > 0 && out.lastModified() > stale
							&& BitmapFactory.decodeFile( out.getAbsolutePath()) != null )
						return;

					// steamcommunity answers nothing on filtered networks, so
					// keep trying while the game runs instead of writing the
					// player off after one go
					for( int attempt = 0; attempt < 4; attempt++ ) {
						synchronized( AVATAR_RATE_LOCK ) {
							long now = System.currentTimeMillis();
							long wait = 300 - ( now - sAvatarLastFetch );
							if( wait > 0 ) Thread.sleep( wait );
							sAvatarLastFetch = System.currentTimeMillis();
						}

						String xml = httpGetString(
							"https://steamcommunity.com/profiles/" + steamid64 + "/?xml=1" );
						String url = xml == null ? null : extractXmlTag( xml, "avatarFull" );

						// steamcommunity is blocked on some filtered networks,
						// playerdb.co mirrors the same photo
						if( url == null || url.length() == 0 ) {
							String json = httpGetString(
								"https://playerdb.co/api/player/steam/" + steamid64 );
							url = json == null ? null : extractJsonString( json, "avatar" );
						}

						if( url != null && url.length() > 0 ) {
							byte[] img = httpGetBytes( url );
							if( img != null && img.length >= 64 ) {
								Bitmap bmp = BitmapFactory.decodeByteArray( img, 0, img.length );
								if( bmp != null ) {
									writeAvatarPair( dir, "av_" + account, bmp );
									bmp.recycle();
									consolePrintf( "Avatar: saved steam photo for account " + account );
									return;
								}
							}
						}

						Thread.sleep( 15000 );
					}
				} catch ( Throwable t ) {
					Log.w( TAG, "avatar fetch failed", t );
				}

				// not fetched this round: free the slot and try again in a
				// minute, the rate limiter keeps it gentle
				mAvatarFetched.remove( key );
				if( mAvatarFailLogged.add( key ))
					consolePrintf( "Avatar: no photo for account " + account +
						" yet (network blocked? retrying every minute)" );
				new android.os.Handler( android.os.Looper.getMainLooper()).postDelayed(
					new Runnable() {
						@Override public void run() {
							avatarFetch( steamid64 );
						}
					}, 60000 );
			}
		});
		t.setDaemon( true );
		t.start();
	}

	/** "avatar_pick" from Options -> Multiplayer: opens the system image
	 *  chooser; the picked picture is saved as avatar_custom.png in this
	 *  game's media/avatars and every other installed game gets a copy.
	 *  No picker in the menu itself - the scoreboard resolves it. */
	public void avatarPick() {
		runOnUiThread( new Runnable() {
			@Override public void run() {
				try {
					Intent pick = new Intent( Intent.ACTION_GET_CONTENT );
					pick.addCategory( Intent.CATEGORY_OPENABLE );
					pick.setType( "image/*" );
					startActivityForResult( Intent.createChooser( pick, "Choose logo image" ), REQ_PICK_AVATAR );
				} catch ( Throwable t ) {
					Log.w( TAG, "avatar picker start failed", t );
				}
			}
		});
	}

	@Override
	protected void onActivityResult( int requestCode, int resultCode, Intent data ) {
		try {
			super.onActivityResult( requestCode, resultCode, data );
		} catch ( Throwable t ) {
			Log.w( TAG, "onActivityResult", t );
		}

		if( requestCode != REQ_PICK_AVATAR || resultCode != RESULT_OK
				|| data == null || data.getData() == null )
			return;

		saveAvatarPick( data.getData(), null );
	}

	// build 101: "avatar_import <path>" from Options -> Import image...
	// The mainui file dialog (PC-style logos/*.bmp search inside the game
	// folders) hands the engine a path; no Android gallery at all.
	public void avatarImport( final String path ) {
		saveAvatarPick( null, path );
	}

	/** keep the gallery Uri path working while the in-app file dialog
	 *  (avatar_import) shares every other step with it */
	private void saveAvatarPick( final Uri uri, final String path ) {
		Thread t = new Thread( new Runnable() {
			@Override public void run() {
				try {
					File dir = avatarDir();
					if( dir == null ) {
						consolePrintf( "Avatar: no writable media/avatars folder for this game" );
						return;
					}

					// sample-bound decode so a 12mp photo can't blow the heap,
					// the scoreboard picture is tiny anyway
					BitmapFactory.Options bounds = new BitmapFactory.Options();
					bounds.inJustDecodeBounds = true;
					java.io.InputStream in = openAvatarStream( uri, path );
					BitmapFactory.decodeStream( in, null, bounds );
					if( in != null ) try { in.close(); } catch ( Throwable ignored ) {}

					int sample = 1;
					while( bounds.outWidth / sample > 256 || bounds.outHeight / sample > 256 )
						sample *= 2;

					BitmapFactory.Options opts = new BitmapFactory.Options();
					opts.inSampleSize = sample;
					in = openAvatarStream( uri, path );
					Bitmap bmp = BitmapFactory.decodeStream( in, null, opts );
					if( in != null ) try { in.close(); } catch ( Throwable ignored ) {}

					if( bmp == null ) {
						consolePrintf( "Avatar: could not decode the picked image"
								+ (( path != null ) ? ( " (" + path + ")" ) : "" ));
						return;
					}

					writeAvatarPair( dir, "avatar_custom", bmp );
					bmp.recycle();

					consolePrintf( "Avatar: saved avatar_custom.png into " + dir.getAbsolutePath());

					// every other installed game gets a copy too, so the
					// logo shows up if I switch games instead of re-picking
					propagateAvatarPick( dir );
				} catch ( Throwable t ) {
					Log.w( TAG, "avatar pick failed", t );
					consolePrintf( "Avatar: picking the image failed: " + t );
				}
			}
		});
		t.setDaemon( true );
		t.start();
	}

	/** gallery Uri (system picker) or plain file path (in-app dialog) */
	private java.io.InputStream openAvatarStream( Uri uri, String path ) throws Exception {
		if( uri != null )
			return getContentResolver().openInputStream( uri );
		return new java.io.FileInputStream( resolveImportFile( path ));
	}

	/** the dialog returns an FS-relative path; try it as absolute, as
	 *  gamedir-relative (logos/foo.bmp) and as base-dir relative */
	private File resolveImportFile( String p ) throws java.io.FileNotFoundException {
		if( p == null )
			throw new java.io.FileNotFoundException( "empty avatar import path" );

		String s = p.trim();
		File f = new File( s );
		if( f.isFile() )
			return f;

		f = new File( new File( resolveBaseDir(), avatarGameDir() ), s );
		if( f.isFile() )
			return f;

		f = new File( resolveBaseDir(), s );
		if( f.isFile() )
			return f;

		throw new java.io.FileNotFoundException( "avatar import file not found: " + p );
	}

	/** plain byte copy, the small file helper for the avatar mirrors */
	private static boolean copyFileTo( File src, File dst ) {
		try {
			FileInputStream in = new FileInputStream( src );
			java.io.FileOutputStream fos = new java.io.FileOutputStream( dst );
			byte[] buf = new byte[16384];
			int n;
			while(( n = in.read( buf )) > 0 )
				fos.write( buf, 0, n );
			fos.close();
			in.close();
			return true;
		} catch ( Throwable t ) {
			return false;
		}
	}

	/** square-fit a picture into a size x size uncompressed 32-bit tga,
	 *  top-down: the vgui loader reads tga only, the cs one prefers png,
	 *  so both formats live side by side */
	private static byte[] encodeTga( Bitmap src, int size ) {
		try {
			int w = src.getWidth(), h = src.getHeight();
			if( w <= 0 || h <= 0 || w > 4096 || h > 4096 ) return null;

			Bitmap made = null;
			Bitmap sq = src;
			if( w != h ) {
				int cw = Math.min( w, h );
				made = sq = Bitmap.createBitmap( src, ( w - cw ) / 2, ( h - cw ) / 2, cw, cw );
			}
			if( sq.getWidth() != size || sq.getHeight() != size ) {
				Bitmap scaled = Bitmap.createScaledBitmap( sq, size, size, true );
				if( made != null ) made.recycle();
				made = sq = scaled;
			}

			int[] px = new int[size * size];
			sq.getPixels( px, 0, size, 0, 0, size, size );
			if( made != null ) made.recycle();

			byte[] out = new byte[18 + size * size * 4];
			out[2] = 2;                             // uncompressed true-color
			out[12] = ( byte )( size & 0xFF );      // width, little endian
			out[13] = ( byte )(( size >> 8 ) & 0xFF );
			out[14] = ( byte )( size & 0xFF );      // height
			out[15] = ( byte )(( size >> 8 ) & 0xFF );
			out[16] = 32;                           // bits per pixel
			out[17] = ( byte )0x08;                 // 8 alpha bits

			int o = 18;
			for( int y = size - 1; y >= 0; y-- ) {
				for( int x = 0; x < size; x++ ) {
					int p = px[y * size + x];
					out[o++] = ( byte )( p & 0xFF );          // blue
					out[o++] = ( byte )(( p >> 8 ) & 0xFF );  // green
					out[o++] = ( byte )(( p >> 16 ) & 0xFF ); // red
					out[o++] = ( byte )(( p >> 24 ) & 0xFF ); // alpha
				}
			}
			return out;
		} catch ( Throwable t ) {
			return null;
		}
	}

	/** A7 build 111 - "support logo only right size": every picture saved
	 *  becomes ONE square size (center-crop, then scale, exactly like the
	 *  tga path always did). The scoreboard cell and the Options preview
	 *  are square, so a wide/tall import rendered stretched or overflowed
	 *  the avatar column ("sometime in score board size not looking
	 *  good"). Returns null when src is already the right size, meaning
	 *  "reuse src". */
	private static Bitmap makeAvatarSquare( Bitmap src, int size ) {
		try {
			int w = src.getWidth(), h = src.getHeight();
			if( w <= 0 || h <= 0 ) return null;

			Bitmap made = null;
			Bitmap sq = src;
			if( w != h ) {
				int cw = Math.min( w, h );
				made = sq = Bitmap.createBitmap( src, ( w - cw ) / 2, ( h - cw ) / 2, cw, cw );
			}
			if( sq.getWidth() != size || sq.getHeight() != size ) {
				Bitmap scaled = Bitmap.createScaledBitmap( sq, size, size, true );
				if( made != null ) made.recycle();
				made = sq = scaled;
			}
			return made;
		} catch ( Throwable t ) {
			return null;
		}
	}

	/** one avatar, two files: a square 128x128 png for the scoreboard's
	 *  engine loader, a 32x32 tga next to it for the vgui one. Raises the
	 *  engine's avatar-saved flag so the menu refreshes live (build 111). */
	private void writeAvatarPair( File dir, String stem, Bitmap bmp ) {
		try {
			Bitmap made = makeAvatarSquare( bmp, 128 );
			Bitmap out = ( made != null ) ? made : bmp;

			java.io.FileOutputStream fos = new java.io.FileOutputStream( new File( dir, stem + ".png" ));
			out.compress( Bitmap.CompressFormat.PNG, 100, fos );
			fos.close();

			byte[] tga = encodeTga( out, 32 );
			if( tga != null ) {
				fos = new java.io.FileOutputStream( new File( dir, stem + ".tga" ));
				fos.write( tga );
				fos.close();
			}

			if( made != null ) made.recycle();

			// tell the engine a fresh logo is on disk: it drops the cached
			// texture and bumps @cl_avatargen, so the Options preview and
			// both scoreboards reload it right away
			nativeAvatarSaved();
		} catch ( Throwable t ) {
			Log.w( TAG, "avatar write failed", t );
		}
	}

	/** copy the fresh avatar_custom pair into the other installed games'
	 *  media/avatars so the same picture shows there too */
	private void propagateAvatarPick( File dir ) {
		try {
			String game = avatarGameDir();
			File[] games = resolveBaseDir().listFiles();
			if( games == null ) return;

			for( File g : games ) {
				if( !g.isDirectory() || g.getName().equals( game )) continue;

				File dst = new File( g, "media/avatars" );
				if( !dst.isDirectory() && !dst.mkdirs() && !dst.isDirectory())
					continue;

				for( String ext : new String[] { "png", "tga" } ) {
					File src = new File( dir, "avatar_custom." + ext );
					if( src.isFile() && src.length() > 0 )
						copyFileTo( src, new File( dst, "avatar_custom." + ext ));
				}
			}
		} catch ( Throwable t ) {
			Log.w( TAG, "avatar propagate failed", t );
		}
	}

	private String httpGetString( String url ) {
		byte[] data = httpGetBytes( url );
		if( data == null ) return null;
		try {
			return new String( data, "UTF-8" );
		} catch ( Throwable t ) {
			return null;
		}
	}

	private byte[] httpGetBytes( String url ) {
		java.net.HttpURLConnection c = null;
		try {
			c = ( java.net.HttpURLConnection ) new java.net.URL( url ).openConnection();
			c.setConnectTimeout( 8000 );
			c.setReadTimeout( 10000 );
			c.setInstanceFollowRedirects( true );
			c.setRequestProperty( "User-Agent", "Mozilla/5.0 (Xash3D Android)" );

			java.io.InputStream in = c.getInputStream();
			java.io.ByteArrayOutputStream buf = new java.io.ByteArrayOutputStream();
			byte[] chunk = new byte[16384];
			int n;
			while(( n = in.read( chunk )) > 0 ) {
				buf.write( chunk, 0, n );
				if( buf.size() > 8 * 1024 * 1024 ) break;
			}
			in.close();
			return buf.toByteArray();
		} catch ( Throwable t ) {
			Log.w( TAG, "http get failed: " + url, t );
			return null;
		} finally {
			if( c != null ) try { c.disconnect(); } catch ( Throwable ignored ) {}
		}
	}

	/** first <tag>...</tag> value of a small xml page, CDATA stripped */
	private static String extractXmlTag( String xml, String tag ) {
		String open = "<" + tag + ">";
		String close = "</" + tag + ">";
		int a = xml.indexOf( open );
		int b = xml.indexOf( close, a >= 0 ? a : 0 );
		if( a < 0 || b < 0 ) return null;

		String v = xml.substring( a + open.length(), b ).trim();
		if( v.startsWith( "<![CDATA[" ) && v.endsWith( "]]>" ))
			v = v.substring( 9, v.length() - 3 ).trim();
		return v;
	}

	/** one string field from a small json reply, with the escapes steam
	 *  cdn urls use */
	private static String extractJsonString( String json, String field ) {
		try {
			String needle = "\"" + field + "\"";
			int i = json.indexOf( needle );
			if( i < 0 ) return null;
			i = json.indexOf( ':', i + needle.length() );
			if( i < 0 ) return null;
			i = json.indexOf( '"', i + 1 );
			if( i < 0 ) return null;
			StringBuilder b = new StringBuilder();
			for( int e = i + 1; e < json.length(); e++ ) {
				char c = json.charAt( e );
				if( c == '\\' && e + 1 < json.length() ) {
					char n2 = json.charAt( ++e );
					if( n2 == 'u' ) {
						b.appendCodePoint( Integer.parseInt( json.substring( e + 1, e + 5 ), 16 ));
						e += 4;
					} else if( n2 == '/' ) b.append( '/' );
					else b.append( n2 );
					continue;
				}
				if( c == '"' ) return b.toString();
				b.append( c );
			}
		} catch ( Throwable ignored ) {}
		return null;
	}

	/** A7 build 99: a classic GoldSource window edge - flat fill with a
	 *  3D bevel (raised = light top/left + dark bottom/right, sunken =
	 *  inverted). This is the cs16.css / GameUI dialog frame. */
	private static class BevelDrawable extends Drawable {
		private final int fill, lt, dk;
		private final boolean raised;
		private final float thick;

		BevelDrawable(int fill, int lt, int dk, boolean raised, float thick) {
			this.fill = fill;
			this.lt = lt;
			this.dk = dk;
			this.raised = raised;
			this.thick = thick;
		}

		@Override
		public void draw(Canvas c) {
			Rect b = getBounds();
			if (b.isEmpty()) return;
			Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
			p.setStyle(Paint.Style.FILL);

			p.setColor(fill);
			c.drawRect(b, p);

			// raised: light top/left, dark bottom/right. sunken: flipped.
			int light = raised ? lt : dk;
			int dark = raised ? dk : lt;
			float t = Math.max(1f, thick);

			p.setColor(light);
			c.drawRect(b.left, b.top, b.right, b.top + t, p);        // top
			c.drawRect(b.left, b.top, b.left + t, b.bottom, p);      // left
			p.setColor(dark);
			c.drawRect(b.left, b.bottom - t, b.right, b.bottom, p);  // bottom
			c.drawRect(b.right - t, b.top, b.right, b.bottom, p);    // right
		}

		@Override public void setAlpha(int a) { }
		@Override public void setColorFilter(ColorFilter cf) { }
		@Override public int getOpacity() { return PixelFormat.OPAQUE; }
	}

	/** raised olive dialog frame (cs16.css panel) */
	private Drawable makeLoadingPanelBackground() {
		return new BevelDrawable(LOADING_PANEL_BG, LOADING_BORDER_LT,
				LOADING_BORDER_DK, true, dp(2));
	}

	/** raised button; pressed inverts to a sunken accent-edged fill so a
	 *  tap reads like a real GameUI button */
	private Drawable makeLoadingButtonBackground() {
		Drawable normal = new BevelDrawable(LOADING_SECONDARY_BG, LOADING_BORDER_LT,
				LOADING_BORDER_DK, true, dp(1));
		Drawable pressed = new BevelDrawable(LOADING_ACCENT, LOADING_BORDER_DK,
				LOADING_BORDER_LT, false, dp(1));

		StateListDrawable sld = new StateListDrawable();
		sld.addState(new int[]{android.R.attr.state_pressed}, pressed);
		sld.addState(new int[]{-android.R.attr.state_pressed}, normal);
		return sld;
	}

	// A7 build 98: the load backdrop is the SAME Counter-Strike collage the
	// main menu uses - full screen, so a load is never a black frame. It is
	// composed once from <gamedir>/resource/BackgroundLayout.txt + the 15
	// 256x256 loading TGA tiles and cached for the process lifetime.
	private static Bitmap sLoadingArt;
	private static boolean sLoadingArtTried;

	/** decode a raw (type-2) 24-bit uncompressed TGA. Android's BitmapFactory
	 *  can't read TGA, so we do the tiny header + BGR->RGB + vertical flip
	 *  (descriptor bit 5 = 0 means the rows are stored bottom-up). */
	private static Bitmap decodeTga(File f) {
		byte[] all = null;
		try (InputStream in = new FileInputStream(f)) {
			java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream();
			byte[] buf = new byte[8192];
			int n;
			while ((n = in.read(buf)) > 0) bos.write(buf, 0, n);
			all = bos.toByteArray();
		} catch (IOException e) {
			return null;
		}
		if (all == null || all.length < 18) return null;

		int idLen = all[0] & 0xFF;
		int imgType = all[2] & 0xFF;
		int w = (all[12] & 0xFF) | ((all[13] & 0xFF) << 8);
		int h = (all[14] & 0xFF) | ((all[15] & 0xFF) << 8);
		int bpp = all[16] & 0xFF;
		int desc = all[17] & 0xFF;

		if (imgType != 2 || bpp != 24 || w <= 0 || h <= 0) return null;
		int off = 18 + idLen;
		if (all.length < off + w * h * 3) return null;

		boolean topDown = (desc & 0x20) != 0;
		int[] px = new int[w * h];
		for (int y = 0; y < h; y++) {
			int srcRow = topDown ? y : (h - 1 - y);
			int src = off + srcRow * w * 3;
			int dstRow = y * w;
			for (int x = 0; x < w; x++) {
				int b = all[src + x * 3] & 0xFF;
				int g = all[src + x * 3 + 1] & 0xFF;
				int r = all[src + x * 3 + 2] & 0xFF;
				px[dstRow + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
			}
		}
		return Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888);
	}

	/** compose the menu collage for the load backdrop; null on any failure
	 *  (the caller then falls back to a gradient, still never black). */
	private Bitmap loadLoadingArt() {
		if (sLoadingArtTried) return sLoadingArt;
		sLoadingArtTried = true;
		try {
			File base = resolveBaseDir();
			Intent gi = getIntent();
			String gamedir = gi != null ? gi.getStringExtra("gamedir") : null;
			if (gamedir == null || gamedir.isEmpty()) gamedir = "cstrike";

			File layoutFile = new File(base, gamedir + "/resource/BackgroundLayout.txt");
			if (!layoutFile.exists()) return null;

			List<String> lines = new java.util.ArrayList<>();
			try (InputStream in = new FileInputStream(layoutFile)) {
				java.io.BufferedReader br = new java.io.BufferedReader(
						new java.io.InputStreamReader(in, "US-ASCII"));
				String line;
				while ((line = br.readLine()) != null) lines.add(line);
			}
			int resW = 0, resH = 0;
			java.util.ArrayList<Object[]> tiles = new java.util.ArrayList<>(); // {path,x,y}
			for (String raw : lines) {
				String ln = raw.trim();
				if (ln.isEmpty() || ln.startsWith("//")) continue;
				String[] t = ln.split("\\s+");
				if (t.length >= 3 && t[0].equalsIgnoreCase("resolution")) {
					resW = Integer.parseInt(t[1]);
					resH = Integer.parseInt(t[2]);
				} else if (t.length >= 4 && t[0].toLowerCase().endsWith(".tga")) {
					int x = Integer.parseInt(t[2]);
					int y = Integer.parseInt(t[3]);
					tiles.add(new Object[]{t[0], x, y});
				}
			}
			if (resW <= 0 || resH <= 0 || tiles.isEmpty()) return null;

			Bitmap art = Bitmap.createBitmap(resW, resH, Bitmap.Config.ARGB_8888);
			android.graphics.Canvas c = new android.graphics.Canvas(art);
			c.drawColor(0xFF1B2430); // fill gaps so no transparent/black cells

			for (Object[] tl : tiles) {
				String rel = (String) tl[0];
				int x = (Integer) tl[1];
				int y = (Integer) tl[2];
				File tf = new File(base, gamedir + "/" + rel);
				Bitmap tile = decodeTga(tf);
				if (tile != null) {
					c.drawBitmap(tile, x, y, null);
					tile.recycle();
				}
			}
			sLoadingArt = art;
			Log.i(TAG, "loading backdrop composed " + resW + "x" + resH
					+ " from " + tiles.size() + " tiles");
		} catch (Throwable t) {
			Log.w(TAG, "loading backdrop compose failed", t);
		}
		return sLoadingArt;
	}

	/** the full-screen backdrop: the CS collage if it composed, else a dark
	 *  gradient - either way it fills the whole screen so a load never shows
	 *  a black frame. A light dim on top keeps the dialog readable. */
	private Drawable makeLoadingBackdrop() {
		Bitmap art = loadLoadingArt();
		if (art != null) {
			// ImageView CENTER_CROP (set by the caller) scales this to fill
			// the screen and crops the edges - the collage always fills the
			// whole frame, never letterboxed, never black.
			return new BitmapDrawable(getResources(), art);
		}
		GradientDrawable g = new GradientDrawable(
				android.graphics.drawable.GradientDrawable.Orientation.TL_BR,
				new int[]{0xFF2A3140, 0xFF14181F});
		return g;
	}

	private void showLoadingOnUiThread(String serverAddr) {
		mLoadingDlFiles = 0; // build 103: fresh file count per load
		if (mLoadingOverlay != null)
			hideLoadingOverlay();

		mLoadingServer = serverAddr != null ? serverAddr : "";

		FrameLayout overlay = new FrameLayout(this);

		// A7 build 98: full-screen CS collage backdrop (or gradient fallback)
		// so the whole load is the game art, never black. An ImageView in
		// centerCrop fills the screen regardless of aspect ratio.
		ImageView art = new ImageView(this);
		art.setImageDrawable(makeLoadingBackdrop());
		art.setScaleType(ImageView.ScaleType.CENTER_CROP);
		overlay.addView(art, new FrameLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

		// light dim over the art so the compact dialog stays readable
		View dim = new View(this);
		dim.setBackgroundColor(0x66000000);
		overlay.addView(dim, new FrameLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

		// classic centered dialog: olive box with the "Loading..." title,
		// status line, main bar and the transfer block in one frame
		LinearLayout dialog = new LinearLayout(this);
		dialog.setOrientation(LinearLayout.VERTICAL);
		dialog.setBackground(makeLoadingPanelBackground());
		int dpad = dp(12);
		dialog.setPadding(dpad, dpad, dpad, dpad);

		int dlgW = Math.min(dp(340),
				getResources().getDisplayMetrics().widthPixels - dp(32));
		FrameLayout.LayoutParams dialogLp = new FrameLayout.LayoutParams(
				dlgW, ViewGroup.LayoutParams.WRAP_CONTENT);
		dialogLp.gravity = Gravity.CENTER;
		overlay.addView(dialog, dialogLp);

		// title row: the game logo next to the classic caption
		LinearLayout titleRow = new LinearLayout(this);
		titleRow.setGravity(Gravity.CENTER_VERTICAL);

		ImageView logo = new ImageView(this);
		logo.setImageResource(R.drawable.cs_logo);
		logo.setScaleType(ImageView.ScaleType.FIT_CENTER);
		int logoSize = dp(18);
		LinearLayout.LayoutParams logoLp = new LinearLayout.LayoutParams(logoSize, logoSize);
		logoLp.rightMargin = dp(8);
		titleRow.addView(logo, logoLp);

		TextView title = new TextView(this);
		title.setText("Loading...");
		title.setTextColor(LOADING_TITLE);
		title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
		title.setTypeface(Typeface.DEFAULT_BOLD);
		titleRow.addView(title, new LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

		dialog.addView(titleRow, new LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

		// A7 build 99: gold caption rule under the title - the classic
		// GameUI window caption divider (cs16.css accent)
		View captionRule = new View(this);
		captionRule.setBackgroundColor(LOADING_ACCENT);
		LinearLayout.LayoutParams ruleLp = new LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, dp(2));
		ruleLp.topMargin = dp(6);
		dialog.addView(captionRule, ruleLp);

		// status line: what the engine is doing right now
		TextView status = new TextView(this);
		status.setText(mLoadingServer.isEmpty() ? "Loading..."
				: "Connecting to " + mLoadingServer + "...");
		status.setTextColor(LOADING_TEXT);
		status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
		status.setSingleLine(false);
		LinearLayout.LayoutParams statusLp = new LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
		statusLp.topMargin = dp(8);
		dialog.addView(status, statusLp);
		mLoadingStatus = status;

		// build 106: just the bar - the percent number beside it was
		// removed on request ("remove loading percent only")
		LinearLayout barRow = new LinearLayout(this);
		barRow.setGravity(Gravity.CENTER_VERTICAL);
		barRow.setBaselineAligned(false);

		LoadingBar bar = new LoadingBar(this);
		// the bar now takes all the leftover width (its old neighbour, the
		// percent TextView, is gone)
		barRow.addView(bar, new LinearLayout.LayoutParams(0, dp(18), 1f));

		LinearLayout.LayoutParams barRowLp = new LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
		barRowLp.topMargin = dp(8);
		dialog.addView(barRow, barRowLp);
		mLoadingBar = bar;
		mLoadingBarRow = barRow;

		// cancel sits here while nothing transfers
		Button cancel = new Button(this);
		cancel.setText("Cancel");
		cancel.setAllCaps(false);
		cancel.setTextColor(LOADING_TITLE);
		cancel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
		cancel.setBackground(makeLoadingButtonBackground());
		cancel.setStateListAnimator(null);
		cancel.setElevation(0f);
		cancel.setMinHeight(0);
		cancel.setPadding(dp(10), 0, dp(10), 0);
		cancel.setOnClickListener(new View.OnClickListener() {
			@Override
			public void onClick(View v) {
				hideLoadingOverlay();
				nativeLoadingCancelled();
			}
		});
		LinearLayout.LayoutParams cancelLp = new LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.WRAP_CONTENT, dp(26));
		cancelLp.leftMargin = dp(8);
		barRow.addView(cancel, cancelLp);
		mLoadingCancel = cancel;

		// transfer block: file, status line, its own bar, byte counters.
		// hidden until the first server file is being fetched
		TextView dlName = new TextView(this);
		dlName.setTextColor(LOADING_TITLE);
		dlName.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
		dlName.setSingleLine(false);
		dlName.setVisibility(View.GONE);
		LinearLayout.LayoutParams dlNameLp = new LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
		dlNameLp.topMargin = dp(8);
		dialog.addView(dlName, dlNameLp);
		mLoadingDlName = dlName;

		TextView dlStatus = new TextView(this);
		dlStatus.setTextColor(LOADING_TEXT);
		dlStatus.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
		dlStatus.setSingleLine(false);
		dlStatus.setVisibility(View.GONE);
		LinearLayout.LayoutParams dlStatusLp = new LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
		dlStatusLp.topMargin = dp(2);
		dialog.addView(dlStatus, dlStatusLp);
		mLoadingDlStatus = dlStatus;

		LinearLayout dlRow = new LinearLayout(this);
		dlRow.setGravity(Gravity.CENTER_VERTICAL);
		dlRow.setVisibility(View.GONE);

		LoadingBar dlBar = new LoadingBar(this);
		dlRow.addView(dlBar, new LinearLayout.LayoutParams(
				0, dp(16), 1f));
		mLoadingDlBar = dlBar;

		// build 103: file counter at the end of the transfer bar -
		// a bare number (files taken this load), no % sign
		TextView dlCount = new TextView(this);
		dlCount.setTextColor(LOADING_ACCENT);
		dlCount.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
		dlCount.setTypeface(Typeface.DEFAULT_BOLD);
		dlCount.setGravity(Gravity.RIGHT);
		dlCount.setText("0");
		dlCount.setVisibility(View.GONE);
		LinearLayout.LayoutParams dlCountLp = new LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
		dlCountLp.leftMargin = dp(8);
		dlRow.addView(dlCount, dlCountLp);
		mLoadingDlCount = dlCount;

		LinearLayout.LayoutParams dlRowLp = new LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
		dlRowLp.topMargin = dp(8);
		dialog.addView(dlRow, dlRowLp);
		mLoadingDlRow = dlRow;

		TextView dlFooter = new TextView(this);
		dlFooter.setTextColor(LOADING_TEXT);
		dlFooter.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
		dlFooter.setVisibility(View.GONE);
		LinearLayout.LayoutParams dlFooterLp = new LinearLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
		dlFooterLp.topMargin = dp(4);
		dialog.addView(dlFooter, dlFooterLp);
		mLoadingDlFooter = dlFooter;

		mLoadingOverlay = overlay;
		addContentView(overlay, new ViewGroup.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
	}

	private void hideLoadingOverlay() {
		if (mLoadingOverlay == null) return;

		FrameLayout overlay = mLoadingOverlay;
		mLoadingOverlay = null;
		mLoadingBar = null;
		mLoadingStatus = null;
		mLoadingDlCount = null;
		mLoadingDlName = null;
		mLoadingDlStatus = null;
		mLoadingDlBar = null;
		mLoadingDlFooter = null;
		mLoadingBarRow = null;
		mLoadingDlRow = null;
		mLoadingCancel = null;

		try {
			ViewGroup parent = (ViewGroup) overlay.getParent();
			if (parent != null) parent.removeView(overlay);
		} catch (Throwable t) {
			Log.w(TAG, "loading overlay remove failed", t);
		}
	}

	// A7 Steam friends: called from the engine's "steamfriends" console
	// command (engine/platform/android/android.c) through JNI. Opens the
	// friends WebView on top of the running game; closing it resumes the
	// session exactly where it was.
	public void openSteamFriends() {
		runOnUiThread(new Runnable() {
			@Override
			public void run() {
				try {
					Intent i = new Intent(XashActivity.this, SteamFriendsActivity.class);
					i.putExtra("basedir", resolveBaseDir().getAbsolutePath());
					String gamedir = getIntent() != null ? getIntent().getStringExtra("gamedir") : null;
					i.putExtra("gamedir", gamedir != null ? gamedir : "cstrike");
					startActivity(i);
				} catch (Throwable t) {
					Log.w(TAG, "steamfriends screen failed to start", t);
				}
			}
		});
	}

	@Override
	public String getCallingPackage() {
		if (mPackageName != null) {
			return mPackageName;
		}

		return super.getCallingPackage();
	}

	private AssetManager getAssets(boolean isEngine) {
		AssetManager am = null;

		if (isEngine) {
			am = getAssets();
		} else {
			try {
				am = getPackageManager().getResourcesForApplication(getCallingPackage()).getAssets();
			} catch (Exception e) {
				Log.e(TAG, "Unable to load mod assets!");
				e.printStackTrace();
			}
		}

		return am;
	}

	private String[] getAssetsList(boolean isEngine, String path) {
		AssetManager am = getAssets(isEngine);

		// never let a NullPointerException reach the JNI caller
		if (am == null)
			return new String[]{};

		try {
			String[] list = am.list(path);
			return list != null ? list : new String[]{};
		} catch (Exception e) {
			e.printStackTrace();
		}

		return new String[]{};
	}

	@Override
	public boolean dispatchKeyEvent(KeyEvent event) {
		if (SDLActivity.mBrokenLibraries) {
			return false;
		}

		int keyCode = event.getKeyCode();
		if (!mUseVolumeKeys) {
			if (keyCode == KeyEvent.KEYCODE_VOLUME_DOWN || keyCode == KeyEvent.KEYCODE_VOLUME_UP || keyCode == KeyEvent.KEYCODE_CAMERA || keyCode == KeyEvent.KEYCODE_ZOOM_IN || keyCode == KeyEvent.KEYCODE_ZOOM_OUT) {
				return false;
			}
		}

		return getWindow().superDispatchKeyEvent(event);
	}

	private static void appendStringExtra(StringBuilder sb, Intent intent, String key) {
		String value = intent.getStringExtra(key);
		if (value != null)
			sb.append("  ").append(key).append(" = ").append(value).append('\n');
	}

	// record intent info, so that it could be consumed later for crash reporting
	private void recordLaunchInfo() {
		// do not overwrite current launch info with pending crash log, shouldn't happen but might
		File pendingCrash = new File(getFilesDir(), "crashes/" + CrashReports.STACKTRACE_NAME);
		if (pendingCrash.exists() && pendingCrash.length() > 0)
			return;

		// write Android version, fingerprint, supported abis, etc
		CrashReports.writeSystemInfo(this);

		// now create intent info and pass it to crash reporting
		Intent intent = getIntent();
		if (intent == null)
			return;
		StringBuilder sb = new StringBuilder();
		sb.append("Action: ").append(intent.getAction()).append('\n');
		sb.append("Data: ").append(intent.getDataString()).append('\n');
		sb.append("Calling package: ").append(getCallingPackage()).append('\n');
		sb.append("Extras:\n");
		// only write intent extras that we care about
		appendStringExtra(sb, intent, "gamedir");
		appendStringExtra(sb, intent, "gamelibdir");
		appendStringExtra(sb, intent, "pakfile");
		appendStringExtra(sb, intent, "basedir");
		appendStringExtra(sb, intent, "package");
		appendStringExtra(sb, intent, "argv");
		sb.append("  usevolume = ").append(intent.getBooleanExtra("usevolume", false)).append('\n');
		String[] env = intent.getStringArrayExtra("env");
		if (env != null)
			sb.append("  env = ").append(Arrays.toString(env)).append('\n');
		CrashReports.writeIntentInfo(this, sb.toString());
	}

	// TODO: REMOVE LATER, temporary launchers support?
	@Override
	protected String[] getArguments() {
		File crashDir = new File(getFilesDir(), "crashes");
		crashDir.mkdirs();
		nativeSetenv("XASH3D_CRASH_DIR", crashDir.getAbsolutePath());

		recordLaunchInfo();

		String gamedir = getIntent().getStringExtra("gamedir");
		if (gamedir == null) gamedir = "valve";
		nativeSetenv("XASH3D_GAME", gamedir);

		String gamelibdir = getIntent().getStringExtra("gamelibdir");
		if (gamelibdir == null) {
			// v7: fall back to our own nativeLibraryDir so metamod can find
			// bundled server libraries (e.g. libhl_android_*.so for Half-Life)
			// when running games without an external game app.
			gamelibdir = getApplicationInfo().nativeLibraryDir;
		}
		if (gamelibdir != null) nativeSetenv("XASH3D_GAMELIBDIR", gamelibdir);

		String rodir = System.getenv("XASH3D_RODIR");
		if (rodir == null) {
			// FIXME: we are using rodir as a supplier for downloaded game libraries
			rodir = getFilesDir().getAbsolutePath() + "/gamelibs";
			nativeSetenv("XASH3D_RODIR", rodir);
		}
		Log.i(TAG, "XASH3D_RODIR = " + rodir);

		String pakfile = getIntent().getStringExtra("pakfile");
		if (pakfile != null) nativeSetenv("XASH3D_EXTRAS_PAK2", pakfile);

		String basedir = getIntent().getStringExtra("basedir");
		if (basedir != null) {
			nativeSetenv("XASH3D_BASEDIR", basedir);
		} else {
			String rootPath = Environment.getExternalStorageDirectory().getAbsolutePath() + "/xash";
			nativeSetenv("XASH3D_BASEDIR", rootPath);
		}

		mUseVolumeKeys = getIntent().getBooleanExtra("usevolume", false);
		mPackageName = getIntent().getStringExtra("package");

		String[] env = getIntent().getStringArrayExtra("env");
		if (env != null) {
			for (int i = 0; i < env.length; i += 2)
				nativeSetenv(env[i], env[i + 1]);
		}

		String argv = getIntent().getStringExtra("argv");
		if (argv == null) argv = "-console -log";

		return argv.split(" ");
	}
}
