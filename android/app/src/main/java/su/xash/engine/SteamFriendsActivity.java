package su.xash.engine;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.util.Log;
import android.view.ViewGroup;
import android.webkit.CookieManager;
import android.webkit.ValueCallback;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.util.ArrayList;

/**
 * A7: Steam friends screen - the "connect Steam on Android, come back to
 * Counter-Strike" flow. A plain WebView on top of the running game:
 *
 *   Sign in   - steamcommunity.com/login (cookies persist in this app, so
 *               the session survives leaving the screen)
 *   Friends   - the account's friends page; the names are scraped out of it
 *               with JS and written to <gamedir>/friends/names.txt, which
 *               the menu and the scoreboard read to badge Steam friends
 *   Close     - finish(), the game resumes exactly where it was
 *
 * No Steam Web API key is used: everything comes from the logged-in
 * session already in the WebView.
 */
public class SteamFriendsActivity extends Activity {
	private static final String TAG = "SteamFriends";

	private static final String URL_LOGIN   = "https://steamcommunity.com/login/";
	private static final String URL_FRIENDS = "https://steamcommunity.com/my/friends/";

	// JS: first selector that produces nodes wins; also pulls the signed-in
	// steamid64 out of the page's embedded JSON, plus every "join this
	// server" link (steam://connect/IP:PORT) with the friend it belongs to.
	// A7 round 100: it also walks up from each persona node to the avatar
	// <img> of that friend and collects {name,src} pairs - the scoreboard
	// shows them in its logo column - and takes my own header photo.
	// Returns {"id":..,"self":..,"names":[..],"ents":[..],"servers":[..]}
	private static final String EXTRACT_JS =
		"(function(){" +
		"var out=[];var ents=[];" +
		"var sels=['#memberList .persona','.friend_block_v2 .persona'," +
		"'#memberContents .selectable_row .personaname','.selectable_row .persona'," +
		"'.friend_block .personaname','a.personaname'];" +
		"for(var i=0;i<sels.length;i++){" +
		" var n=document.querySelectorAll(sels[i]);" +
		" if(n.length>0){for(var j=0;j<n.length;j++){" +
		"  var t=(n[j].textContent||'').trim().split('\\n')[0];" +
		"  if(!t||out.indexOf(t)>=0)continue;" +
		"  out.push(t);" +
		"  var root=n[j];var img=null;" +
		"  for(var d=0;d<4&&root;d++){if(root.querySelector){img=root.querySelector('img');}if(img)break;root=root.parentElement;}" +
		"  var src='';if(img){src=img.getAttribute('src')||img.getAttribute('data-src')||'';}" +
		"  ents.push({name:t,src:src});" +
		"}break;}" +
		"}" +
		"var h=document.documentElement.innerHTML||'';" +
		"var m=h.match(/\"steamid\"\\s*:?\\s*\"?(7656\\d{13})\"?/);" +
		"if(!m)m=h.match(/profiles\\/(7656\\d{13})/);" +
		"var me=document.querySelector('img.avatar');var self='';" +
		"if(me){self=me.getAttribute('src')||me.getAttribute('data-src')||'';}" +
		"var res={id:m?m[1]:'',self:self,names:out,ents:ents,servers:[]};" +
		// [.] instead of an escaped dot keeps this string free of
		// backslashes: inside a character class the dot is literal, and
		// Java-side escaping of an ES5 regex is where mistakes hide
		"var re=new RegExp('steam://connect/[0-9]{1,3}[.][0-9]{1,3}[.][0-9]{1,3}[.][0-9]{1,3}:[0-9]+','g');" +
		"var mm;" +
		"while((mm=re.exec(h))!==null){" +
		" var addr=mm[0].substring(mm[0].lastIndexOf('/')+1);" +
		" var dup=false;" +
		" for(var k=0;k<res.servers.length;k++) if(res.servers[k].addr===addr) dup=true;" +
		" if(dup) continue;" +
		" var nm='';" +
		" var pre=h.substring(Math.max(0,mm.index-5000),mm.index);" +
		" var kk=pre.lastIndexOf('\"personaname\":\"');" +
		" if(kk>=0){var e=pre.indexOf('\"',kk+15); if(e>kk) nm=pre.substring(kk+15,e);}" +
		" res.servers.push({addr:addr,name:nm||addr});" +
		"}" +
		"return JSON.stringify(res);" +
		"})()";

	private WebView mWeb;
	private TextView mStatus;
	private File mFriendsDir;
	// <basedir>/<gamedir>: my own avatar goes to media/avatars under it
	private File mGameDir;
	private String mSteamId = "";

	@Override
	protected void onCreate( Bundle savedInstanceState )
	{
		super.onCreate( savedInstanceState );

		Intent intent = getIntent();
		String basedir = intent != null ? intent.getStringExtra( "basedir" ) : null;
		String gamedir = intent != null ? intent.getStringExtra( "gamedir" ) : null;

		if( basedir == null )
			basedir = new File( android.os.Environment.getExternalStorageDirectory(), "xash" ).getAbsolutePath();
		if( gamedir == null )
			gamedir = "cstrike";

		mGameDir = new File( basedir, gamedir );
		mFriendsDir = new File( mGameDir, "friends" );

		CookieManager cookies = CookieManager.getInstance();
		cookies.setAcceptCookie( true );

		buildLayout();

		mWeb.getSettings().setJavaScriptEnabled( true );
		mWeb.getSettings().setDomStorageEnabled( true );
		mWeb.getSettings().setUserAgentString( mWeb.getSettings().getUserAgentString() + " XashCS16Friends/1" );
		// the friends list is a desktop page: show it at a phone-ish zoom
		mWeb.getSettings().setLoadWithOverviewMode( true );
		mWeb.getSettings().setUseWideViewPort( true );
		mWeb.getSettings().setLayoutAlgorithm( WebSettings.LayoutAlgorithm.TEXT_AUTOSIZING );

		final Activity self = this;
		mWeb.setWebViewClient( new WebViewClient()
		{
			@Override
			public boolean shouldOverrideUrlLoading( WebView view, String url )
			{
				// keep everything inside the screen - an external link would
				// otherwise hand the session over to the system browser
				view.loadUrl( url );
				return true;
			}

			@Override
			public void onPageFinished( WebView view, String url )
			{
				extract( url );
			}
		});

		mSteamId = readAccountFile( "steamid" );
		setStatus( mSteamId.isEmpty() ? "Not signed in" : ( "Signed in: " + mSteamId ));
		mWeb.loadUrl( mSteamId.isEmpty() ? URL_LOGIN : URL_FRIENDS );
	}

	// ------------------------------------------------------------------
	// UI: a thin control bar over the full-height WebView
	// ------------------------------------------------------------------
	private void buildLayout()
	{
		LinearLayout root = new LinearLayout( this );
		root.setOrientation( LinearLayout.VERTICAL );
		root.setBackgroundColor( Color.BLACK );

		LinearLayout bar = new LinearLayout( this );
		bar.setOrientation( LinearLayout.HORIZONTAL );
		bar.setPadding( 8, 8, 8, 8 );

		mStatus = new TextView( this );
		mStatus.setTextColor( Color.LTGRAY );
		mStatus.setTextSize( 14 );
		mStatus.setPadding( 8, 12, 8, 0 );

		addButton( bar, "Back", v -> {
			if( mWeb.canGoBack()) mWeb.goBack();
		});
		addButton( bar, "Sign in", v -> mWeb.loadUrl( URL_LOGIN ));
		addButton( bar, "Friends", v -> mWeb.loadUrl( URL_FRIENDS ));
		addButton( bar, "Close", v -> finish());

		LinearLayout statusRow = new LinearLayout( this );
		statusRow.setOrientation( LinearLayout.HORIZONTAL );
		statusRow.addView( mStatus, new LinearLayout.LayoutParams(
			ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT ));

		bar.addView( statusRow, new LinearLayout.LayoutParams( 0,
			ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f ));

		mWeb = new WebView( this );

		root.addView( bar, new LinearLayout.LayoutParams(
			ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT ));
		root.addView( mWeb, new LinearLayout.LayoutParams(
			ViewGroup.LayoutParams.MATCH_PARENT, 0, 1.0f ));

		setContentView( root );
	}

	private void addButton( LinearLayout bar, String label, android.view.View.OnClickListener cb )
	{
		Button b = new Button( this );
		b.setText( label );
		b.setTextSize( 13 );
		b.setAllCaps( false );
		b.setOnClickListener( cb );
		bar.addView( b, new LinearLayout.LayoutParams(
			ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT ));
	}

	// ------------------------------------------------------------------
	// extraction + persistence
	// ------------------------------------------------------------------
	@SuppressLint("SetJavaScriptEnabled")
	private void extract( String url )
	{
		// only scrape the friends page; every other page still updates the id
		final boolean friendsPage = url != null && url.contains( "/friends" );

		mWeb.evaluateJavascript( EXTRACT_JS, new ValueCallback<String>()
		{
			@Override
			public void onReceiveValue( String value )
			{
				applyExtract( value, null, friendsPage );
			}
		});
	}

	private void applyExtract( String json, String fallbackUrl, boolean saveFriends )
	{
		if( json == null || "null".equals( json ) || json.isEmpty())
			return;

		try
		{
			JSONObject o = new JSONObject( json );

			String id = o.optString( "id", "" );
			if( !id.isEmpty() && !id.equals( mSteamId ))
			{
				mSteamId = id;
				writeAccountFile( "steamid", id );
				setStatus( "Signed in: " + id );
			}

			if( saveFriends )
			{
				ArrayList<String> names = new ArrayList<String>();
				for( int i = 0; i < o.getJSONArray( "names" ).length(); i++ )
				{
					String n = o.getJSONArray( "names" ).getString( i );
					if( n != null && !n.isEmpty() )
						names.add( n );
				}

				// joinable servers ("steam://connect/IP:PORT" links) are rare
				// on the page - they only exist for a friend who is in a
				// server that allows direct connects
				ArrayList<String[]> servers = new ArrayList<String[]>();
				org.json.JSONArray arr = o.optJSONArray( "servers" );
				if( arr != null )
				{
					for( int i = 0; i < arr.length(); i++ )
					{
						JSONObject s = arr.optJSONObject( i );
						if( s == null )
							continue;
						String addr = s.optString( "addr", "" );
						String nm = s.optString( "name", "" );
						if( addr.isEmpty())
							continue;
						servers.add( new String[]{ addr, nm });
					}
				}

				// a logged-out friends page (or another Steam redesign) yields
				// nothing - never overwrite a good list with an empty one
				if( names.isEmpty() && servers.isEmpty())
				{
					setStatus( "Signed in: " + ( mSteamId.isEmpty() ? "?" : mSteamId )
						+ "  -  0 friends read" );
				}
				else
				{
					if( !names.isEmpty())
						writeNames( names );
					if( !servers.isEmpty())
						writeServers( servers );
					setStatus( "Signed in: " + ( mSteamId.isEmpty() ? "?" : mSteamId )
						+ "  -  " + names.size() + " friends / "
						+ servers.size() + " servers saved" );
				}

				// A7 round 100: the scoreboard's logo column - each friend's
				// photo goes to friends/avatars/ with a name -> file map, and
				// my own photo to media/avatars/avatar_self.png (the first
				// entry of my row's chain that the server cannot supply)
				writeFriendAvatars( o.optJSONArray( "ents" ));
				fetchMyAvatar( o.optString( "self", "" ));
			}
		}
		catch( Throwable t )
		{
			// a Steam page re-design just changes the selectors - keep the
			// screen usable instead of dying on JSON
			setStatus( "Could not read the friends list (page layout changed)" );
		}
	}

	private String readAccountFile( String what )
	{
		try
		{
			File f = new File( mFriendsDir, what + ".txt" );
			if( !f.isFile()) return "";
			java.io.BufferedReader r = new java.io.BufferedReader(
				new java.io.InputStreamReader( new java.io.FileInputStream( f ), "UTF-8" ));
			String line = r.readLine();
			r.close();
			return line != null ? line.trim() : "";
		}
		catch( Throwable t )
		{
			return "";
		}
	}

	private void writeAccountFile( String what, String value )
	{
		try
		{
			mFriendsDir.mkdirs();
			OutputStreamWriter w = new OutputStreamWriter(
				new FileOutputStream( new File( mFriendsDir, what + ".txt" ), false ), "UTF-8" );
			w.write( value + "\n" );
			w.close();
		}
		catch( Throwable t )
		{
			// external storage missing - the list simply won't be shared
		}
	}

	// <gamedir>/friends/names.txt: one friendly name per line, UTF-8. The
	// scoreboard badges a row whose player name matches one of these lines.
	private void writeNames( ArrayList<String> names )
	{
		try
		{
			mFriendsDir.mkdirs();
			OutputStreamWriter w = new OutputStreamWriter(
				new FileOutputStream( new File( mFriendsDir, "names.txt" ), false ), "UTF-8" );
			for( int i = 0; i < names.size(); i++ )
				w.write( names.get( i ) + "\n" );
			w.close();
		}
		catch( Throwable t )
		{
		}
	}

	// <gamedir>/friends/servers.txt: "ip:port<TAB>server name" per line.
	// Find Servers -> Friends parses it (ServerBrowser::LoadFriendsTab) and
	// joins through the address on the first field.
	private void writeServers( ArrayList<String[]> servers )
	{
		try
		{
			mFriendsDir.mkdirs();
			OutputStreamWriter w = new OutputStreamWriter(
				new FileOutputStream( new File( mFriendsDir, "servers.txt" ), false ), "UTF-8" );
			for( int i = 0; i < servers.size(); i++ )
			{
				String addr = servers.get( i )[0];
				String nm = servers.get( i )[1];
				// never write a bare <TAB>: the name column has to show
				// something, the address is the safest fallback
				w.write( addr + "\t" + ( nm.isEmpty() ? addr : nm ) + "\n" );
			}
			w.close();
		}
		catch( Throwable t )
		{
		}
	}

	// ------------------------------------------------------------------
	// A7 round 100: avatar photos for the scoreboard's logo column
	// ------------------------------------------------------------------
	// Steam's avatar srcs are protocol-relative or full https URLs; both
	// come back as one plain https link (and a bare path is absolutised),
	// otherwise the HttpURLConnection would throw on "//host/..."
	private static String normalizeAvatarUrl( String url )
	{
		if( url == null || url.isEmpty())
			return "";

		url = url.trim();

		if( url.startsWith( "//" ))
			return "https:" + url;
		if( url.startsWith( "http://" ) || url.startsWith( "https://" ))
			return url;
		if( url.startsWith( "/" ))
			return "https://steamcommunity.com" + url;

		return "";
	}

	// centre square-crop, then scale: the row plate is small and a
	// non-square photo would be squashed in the scoreboard's logo column
	private static android.graphics.Bitmap squareCrop( android.graphics.Bitmap src, int size )
	{
		int w = src.getWidth(), h = src.getHeight();
		android.graphics.Bitmap made = src;

		if( w != h )
		{
			int c = Math.min( w, h );
			made = android.graphics.Bitmap.createBitmap( src, ( w - c ) / 2, ( h - c ) / 2, c, c );
		}

		if( made.getWidth() != size || made.getHeight() != size )
		{
			android.graphics.Bitmap scaled = android.graphics.Bitmap.createScaledBitmap(
				made, size, size, true );
			if( made != src )
				made.recycle();
			made = scaled;
		}

		return made;
	}

	private void writeTextFile( File f, String text )
	{
		try
		{
			FileOutputStream fos = new FileOutputStream( f, false );
			fos.write( text.getBytes( "UTF-8" ));
			fos.close();
		}
		catch( Throwable t )
		{
		}
	}

	private byte[] httpGet( String url )
	{
		java.net.HttpURLConnection c = null;
		try
		{
			c = ( java.net.HttpURLConnection ) new java.net.URL( url ).openConnection();
			c.setConnectTimeout( 8000 );
			c.setReadTimeout( 10000 );
			c.setInstanceFollowRedirects( true );
			c.setRequestProperty( "User-Agent", "Mozilla/5.0 (Xash3D Android)" );

			java.io.InputStream in = c.getInputStream();
			java.io.ByteArrayOutputStream buf = new java.io.ByteArrayOutputStream();
			byte[] chunk = new byte[16384];
			int n;
			while(( n = in.read( chunk )) > 0 )
			{
				buf.write( chunk, 0, n );
				if( buf.size() > 4 * 1024 * 1024 )
					break;
			}
			in.close();
			return buf.toByteArray();
		}
		catch( Throwable t )
		{
			return null;
		}
		finally
		{
			if( c != null )
				try { c.disconnect(); } catch( Throwable ignored ) {}
		}
	}

	/** first <tag>...</tag> value of a small xml page, CDATA stripped */
	private static String extractXmlTag( String xml, String tag )
	{
		String open = "<" + tag + ">";
		String close = "</" + tag + ">";
		int a = xml.indexOf( open );
		int b = xml.indexOf( close, a >= 0 ? a : 0 );

		if( a < 0 || b < 0 )
			return null;

		String v = xml.substring( a + open.length(), b ).trim();
		if( v.startsWith( "<![CDATA[" ) && v.endsWith( "]]>" ))
			v = v.substring( 9, v.length() - 3 ).trim();
		return v;
	}

	/** download every friend's photo into <gamedir>/friends/avatars/ and
	 *  write <gamedir>/friends/avatars.txt as "name<TAB>path" rows -
	 *  the scoreboard reads that map and draws the picture next to the
	 *  matching player's row (a name it doesn't know simply draws none).
	 *  Runs on a worker thread; an empty round never overwrites a good map. */
	private void writeFriendAvatars( final org.json.JSONArray ents )
	{
		if( ents == null || ents.length() == 0 )
			return;

		new Thread( new Runnable()
		{
			@Override
			public void run()
			{
				File dir = new File( mFriendsDir, "avatars" );

				if( !dir.isDirectory() && !dir.mkdirs())
					return;

				StringBuilder map = new StringBuilder();
				int ok = 0;

				try
				{
					for( int i = 0; i < ents.length(); i++ )
					{
						JSONObject e = ents.optJSONObject( i );
						if( e == null )
							continue;

						String name = e.optString( "name", "" ).replace( '\t', ' ' ).replace( '\n', ' ' );
						String url  = normalizeAvatarUrl( e.optString( "src", "" ));

						if( name.isEmpty() || url.isEmpty())
							continue;

						byte[] img = httpGet( url );
						if( img == null || img.length < 64 )
							continue;

						android.graphics.Bitmap bmp = android.graphics.BitmapFactory
							.decodeByteArray( img, 0, img.length );
						if( bmp == null )
							continue;

						android.graphics.Bitmap sq = squareCrop( bmp, 64 );
						bmp.recycle();

						String rel = "friends/avatars/fa_" + ok + ".png";
						FileOutputStream fos = new FileOutputStream( new File( dir, "fa_" + ok + ".png" ));
						sq.compress( android.graphics.Bitmap.CompressFormat.PNG, 90, fos );
						fos.close();
						sq.recycle();

						map.append( name ).append( '\t' ).append( rel ).append( '\n' );
						ok++;
					}
				}
				catch( Throwable t )
				{
					Log.w( TAG, "friend avatars failed", t );
				}

				if( ok > 0 )
					writeTextFile( new File( mFriendsDir, "avatars.txt" ), map.toString());
			}
		}).start();
	}

	/** my own photo - the first entry of my scoreboard row's chain the
	 *  server cannot supply (no *sid on casual xash servers) - saved as
	 *  <gamedir>/media/avatars/avatar_self.png */
	private void fetchMyAvatar( String selfUrl )
	{
		final String pageUrl = normalizeAvatarUrl( selfUrl );
		final String sid = mSteamId;

		new Thread( new Runnable()
		{
			@Override
			public void run()
			{
				try
				{
					String url = pageUrl;

					if( url.isEmpty() && !sid.isEmpty())
					{
						String xml = null;
						byte[] data = httpGet( "https://steamcommunity.com/profiles/" + sid + "/?xml=1" );
						if( data != null )
						{
							xml = new String( data, "UTF-8" );
							url = extractXmlTag( xml, "avatarFull" );
						}
					}

					if( url == null || url.isEmpty())
						return;

					byte[] img = httpGet( url );
					if( img == null || img.length < 64 )
						return;

					android.graphics.Bitmap bmp = android.graphics.BitmapFactory
						.decodeByteArray( img, 0, img.length );
					if( bmp == null )
						return;

					android.graphics.Bitmap sq = squareCrop( bmp, 64 );
					bmp.recycle();

					File dir = new File( mGameDir, "media/avatars" );
					if( !dir.isDirectory() && !dir.mkdirs())
						return;

					FileOutputStream fos = new FileOutputStream( new File( dir, "avatar_self.png" ));
					sq.compress( android.graphics.Bitmap.CompressFormat.PNG, 90, fos );
					fos.close();
					sq.recycle();

					Log.i( TAG, "welcome avatar saved" );
				}
				catch( Throwable t )
				{
					Log.w( TAG, "own avatar failed", t );
				}
			}
		}).start();
	}

	private void setStatus( String s )
	{
		mStatus.setText( s );
	}

	@Override
	protected void onPause()
	{
		super.onPause();
		if( mWeb != null )
			mWeb.onPause();
		CookieManager.getInstance().flush();
	}

	@Override
	protected void onResume()
	{
		super.onResume();
		if( mWeb != null )
			mWeb.onResume();
	}

	@Override
	protected void onDestroy()
	{
		if( mWeb != null )
		{
			mWeb.destroy();
			mWeb = null;
		}
		super.onDestroy();
	}
}