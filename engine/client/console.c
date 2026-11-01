/*
console.c - developer console
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
#include "keydefs.h"
#include "protocol.h"		// get the protocol version
#include "con_nprint.h"
#include "qfont.h"
#include "wadfile.h"
#include "input.h"
#include "utflib.h"

static CVAR_DEFINE_AUTO( scr_conspeed, "600", FCVAR_ARCHIVE, "console moving speed" );
static CVAR_DEFINE_AUTO( con_notifytime, "3", FCVAR_ARCHIVE, "notify time to live" );
CVAR_DEFINE_AUTO( con_fontsize, "1", FCVAR_ARCHIVE, "console font number (0, 1 or 2)" );
static CVAR_DEFINE_AUTO( con_fontrender, "2", FCVAR_ARCHIVE, "console font render mode (0: additive, 1: holes, 2: trans)" );
static CVAR_DEFINE_AUTO( con_charset, "cp1251", FCVAR_ARCHIVE, "console font charset (only cp1251 supported now)" );
static CVAR_DEFINE_AUTO( con_fontscale, "1.0", FCVAR_ARCHIVE, "scale font texture" );
static CVAR_DEFINE_AUTO( con_fontnum, "-1", FCVAR_ARCHIVE, "console font number (0, 1 or 2), -1 for autoselect" );
static CVAR_DEFINE_AUTO( con_color, "240 180 24", FCVAR_ARCHIVE, "set a custom console color" );
static CVAR_DEFINE_AUTO( scr_drawversion, "1", FCVAR_ARCHIVE, "draw version in menu or screenshots, doesn't affect console" );
static CVAR_DEFINE_AUTO( con_oldfont, "0", 0, "use legacy font from gfx.wad, might be missing or broken" );
static CVAR_DEFINE_AUTO( con_showcompletion, "1", FCVAR_ARCHIVE, "perform simplified autocompletion while typing" );

static int g_codepage = 0;

static qboolean g_messagemode_privileged = true;

// A7 build 91: set while a developer (Con_DPrintf) line is being printed, so
// Con_AddLine can keep those lines out of the in-game notify - they used to
// pop up over the radar/HUD whenever dev mode was on ("Firing: ...")
extern qboolean con_debug_line;

// A7 build 91: floating console window (GoldSource-style: title bar, log
// view, scrollbar, input line + Submit) shown over the main menu and over
// the paused game. the legacy full-screen console is still used by -console
// and while loading.
static struct
{
	qboolean	visible;		// window is on screen
	qboolean	focused;		// keyboard goes to the input line
	qboolean	user_closed;	// user hid it: suppress auto-open until reset
	qboolean	dragging;		// dragging by the title bar
	qboolean	scrolling;		// dragging the log area to scroll it
	qboolean	clicked;		// a press inside the window was consumed
	int	x, y, w, h;		// window rect, render coords
	int	drag_x, drag_y;	// grab point inside the window
	int	scroll_y;		// cursor y where the log drag started
	int	backscroll;		// log scrollback, lines up from the bottom
	int	mouse_x, mouse_y;	// last known cursor position
	qboolean	mouse_in;		// cursor is over the window
} conwin;

// A7 build 92: the floating window draws with the crisp creditsfont (the
// same proportional font the HUD/message text uses) instead of the chunky
// console bitmap font; NULL while the legacy full-screen console draws
static cl_font_t	*conwin_font;

// A7 build 105: GoldSource-style dropdown under the input line - every
// matching command/cvar while the user types (user: "if i type something
// it show cvars"). refreshed on each edit, drawn + clickable by the window.
#define CONWIN_SUGGEST_ROWS	10	// rows before the "..." overflow row
static char	conwin_suggest[CONWIN_SUGGEST_ROWS][CON_SUGGEST_NAME];
static int	conwin_suggestTotal;	// matches, may exceed the stored rows

// defined below (they need the `con` state, which is declared further down)
static cl_font_t *Con_WindowFont( void );
static cl_font_t *Con_WinFont( void );

// defined in the window section, called on every edit of the input line
static void Con_WindowSuggestUpdate( void );

// defined further below in the drawing section; the window log view uses it
static int Con_DrawConsoleLine( const wrect_t *safe, int y, int lineno );

#define CON_TIMES		4	// notify lines
#define CON_MAX_TIMES	64	// notify max lines
#define COLOR_DEFAULT	'7'
#define CON_HISTORY		64
#define MAX_DBG_NOTIFY	128
#if XASH_LOW_MEMORY
#define CON_NUMFONTS	1		// do not load different font textures
#define CON_TEXTSIZE	32768	// max scrollback buffer characters in console (32 kb)
#define CON_MAXLINES	2048	// max scrollback buffer lines in console
#else
#define CON_NUMFONTS	3	// maxfonts
#define CON_TEXTSIZE	1048576	// max scrollback buffer characters in console (1 Mb)
#define CON_MAXLINES	16384	// max scrollback buffer lines in console
#endif
#define CON_LINES( i )	(con.lines[(con.lines_first + (i)) % con.maxlines])
#define CON_LINES_COUNT	con.lines_count
#define CON_LINES_LAST()	CON_LINES( CON_LINES_COUNT - 1 )

// console color typeing
rgba_t g_color_table[8] =
{
{   0,   0,   0, 255 },	// black
{ 255,   0,   0, 255 },	// red
{   0, 255,   0, 255 },	// green
{ 255, 255,   0, 255 },	// yellow
{   0,   0, 255, 255 },	// blue
{   0, 255, 255, 255 },	// cyan
{ 255,   0, 255, 255 },	// magenta
{ 240, 180,  24, 255 },	// default color (can be changed by user)
};

typedef struct
{
	string		szNotify;
	float		expire;
	rgba_t		color;
	int		key_dest;
} notify_t;

typedef struct con_lineinfo_s
{
	char		*start;
	size_t		length;
	double		addtime;		// notify stuff
} con_lineinfo_t;

typedef struct history_line_s
{
	string buffer;
	int    cursor;
	int    scroll;
} history_line_t;

typedef struct con_history_s
{
	history_line_t lines[CON_HISTORY];
	history_line_t backup;
	int     line; // the line being displayed from history buffer will be <= nextHistoryLine
	int     next; // the last line in the history buffer, not masked
} con_history_t;

typedef struct
{
	qboolean		initialized;

	// conbuffer
	char		*buffer;		// common buffer for all console lines
	int		bufsize;		// CON_TEXSIZE
	con_lineinfo_t	*lines;		// console lines
	int		maxlines;		// CON_MAXLINES

	int		lines_first;	// cyclic buffer
	int		lines_count;
	int		num_times;	// overlay lines count

	// console scroll
	int		backscroll;	// lines up from bottom to display
	int 		linewidth;	// characters across screen

	// console animation
	float		showlines;	// how many lines we should display
	float		vislines;		// in scanlines

	// console images
	int		background;	// console background

	// console fonts
	cl_font_t		chars[CON_NUMFONTS];// fonts.wad/font1.fnt
	cl_font_t		*curFont;

	// console input
	field_t		input;
	field_t		input_completion;

	// chatfiled
	field_t		chat;
	string		chat_cmd;		// can be overrieded by user

	// console history
	con_history_t	history;
	qboolean		historyLoaded;

	notify_t		notify[MAX_DBG_NOTIFY]; // for Con_NXPrintf
	qboolean		draw_notify;	// true if we have NXPrint message
} console_t;

static console_t		con;

static void Con_ClearField( field_t *edit );
static void Field_CharEvent( field_t *edit, int ch );
static void Con_InvalidateFonts( void );

static void Con_LoadHistory( con_history_t *self );
static void Con_SaveHistory( con_history_t *self );

/*
================
Con_BackgroundMapActive
================
*/
static qboolean Con_BackgroundMapActive( void )
{
	return sv_background.value != 0.0f || cl.background;
}

/*
================
Con_Clear_f
================
*/
static void Con_Clear_f( void )
{
	con.lines_count = 0;
	con.backscroll = 0; // go to end
}

/*
================
Con_SetColor
================
*/
static void Con_SetColor( void )
{
	if( !FBitSet( con_color.flags, FCVAR_CHANGED ))
		return;

	int r, g, b;
	int num = sscanf( con_color.string, "%i %i %i", &r, &g, &b );

	switch( num )
	{
	case 1:
		Con_DefaultColor( r, r, r, false );
		break;
	case 3:
		Con_DefaultColor( r, g, b, false );
		break;
	default:
		Cvar_DirectSet( &con_color, con_color.def_string );
		break;
	}

	ClearBits( con_color.flags, FCVAR_CHANGED );
}

/*
================
Con_ClearNotify
================
*/
void Con_ClearNotify( void )
{
	for( int i = 0; i < CON_LINES_COUNT; i++ )
		CON_LINES( i ).addtime = 0.0;
}

/*
================
Con_ClearTyping
================
*/
static void Con_ClearTyping( void )
{
	Con_ClearField( &con.input );
	con.input.widthInChars = con.linewidth;

	Cmd_AutoCompleteClear();
}

/*
================
Con_MessageMode_f
================
*/
static void Con_MessageMode_f( void )
{
	g_messagemode_privileged = Cmd_CurrentCommandIsPrivileged();

	if( Cmd_Argc() == 2 )
		Q_strncpy( con.chat_cmd, Cmd_Argv( 1 ), sizeof( con.chat_cmd ));
	else Q_strncpy( con.chat_cmd, "say", sizeof( con.chat_cmd ));

	Key_SetKeyDest( key_message );
}

/*
================
Con_MessageMode2_f
================
*/
static void Con_MessageMode2_f( void )
{
	g_messagemode_privileged = Cmd_CurrentCommandIsPrivileged();

	Q_strncpy( con.chat_cmd, "say_team", sizeof( con.chat_cmd ));
	Key_SetKeyDest( key_message );
}

/*
================
Con_ToggleConsole_f
================
*/
void Con_ToggleConsole_f( void )
{
	if( !host.allow_console || UI_CreditsActive( ))
		return; // disabled

	// A7 build 91: the legacy full-screen console (-console / desktop dev
	// work) keeps the old toggle behaviour untouched
	if( cls.key_dest == key_console )
	{
		SCR_EndLoadingPlaque();

		// show console only in game, from menu, or to close console
		if( cls.state != ca_active && cls.key_dest != key_menu && cls.key_dest != key_console )
			return;

		Con_ClearTyping();
		Con_ClearNotify();

		// closing console
		if( cls.state != ca_active || Con_BackgroundMapActive( ))
		{
			// not in game or in background mode - return to menu
			// UI_SetActiveMenu(true) reactivates menu without resetting history
			UI_SetActiveMenu( true );
		}
		else
		{
			// in game - return to game
			UI_SetActiveMenu( false );
		}
		return;
	}

	// A7 build 110: the floating window is RETIRED - the console is the
	// CLASSIC full-width top console again (user decision: "change console
	// fully" -> classic full-width top console sliding from the top). No
	// console while the loading plaque owns the screen (connect/download
	// keeps its loading screen).
	if( cls.disable_screen )
		return;

	if( conwin.visible )
		Con_WindowClose( false ); // safety: never leave an old window latched

	Con_ClearTyping();
	Con_ClearNotify();

	// Drop into the classic console: Con_RunConsole animates
	// con.showlines/con.vislines and Con_DrawConsole renders
	// Con_DrawSolidConsole whenever key_dest == key_console - the same
	// machinery -console/-dev work always used. Toggling again runs the
	// key_console branch at the top of this function and returns to
	// menu/game exactly as before.
	Key_SetKeyDest( key_console );
}

/*
================
Con_SetTimes_f
================
*/
static void Con_SetTimes_f( void )
{
	if( Cmd_Argc() != 2 )
	{
		Con_Printf( S_USAGE "contimes <n lines>\n" );
		return;
	}

	int newtimes = Q_atoi( Cmd_Argv( 1 ) );
	con.num_times = bound( CON_TIMES, newtimes, CON_MAX_TIMES );
}

/*
================
Con_FixTimes

Notifies the console code about the current time
(and shifts back times of other entries when the time
went backwards)
================
*/
static void Con_FixTimes( void )
{
	if( con.lines_count <= 0 ) return;

	double diff = cl.time - CON_LINES_LAST().addtime;
	if( diff >= 0.0 ) return; // nothing to fix

	for( int i = 0; i < con.lines_count; i++ )
		CON_LINES( i ).addtime += diff;
}

/*
================
Con_DeleteLine

Deletes the first line from the console history.
================
*/
static void Con_DeleteLine( void )
{
	if( con.lines_count == 0 )
		return;
	con.lines_count--;
	con.lines_first = (con.lines_first + 1) % con.maxlines;
}

/*
================
Con_DeleteLastLine

Deletes the last line from the console history.
================
*/
static void Con_DeleteLastLine( void )
{
	if( con.lines_count == 0 )
		return;
	con.lines_count--;
}

/*
================
Con_BytesLeft

Checks if there is space for a line of the given length, and if yes, returns a
pointer to the start of such a space, and NULL otherwise.
================
*/
static char *Con_BytesLeft( int length )
{
	if( length > con.bufsize )
		return NULL;

	if( con.lines_count == 0 )
	{
		return con.buffer;
	}
	else
	{
		char	*firstline_start = con.lines[con.lines_first].start;
		char	*lastline_onepastend = CON_LINES_LAST().start + CON_LINES_LAST().length;

		// the buffer is cyclic, so we first have two cases...
		if( firstline_start < lastline_onepastend ) // buffer is contiguous
		{
			// put at end?
			if( length <= con.buffer + con.bufsize - lastline_onepastend )
				return lastline_onepastend;
			// put at beginning?
			else if( length <= firstline_start - con.buffer )
				return con.buffer;

			return NULL;
		}
		else
		{
			// buffer has a contiguous hole
			if( length <= firstline_start - lastline_onepastend )
				return lastline_onepastend;

			return NULL;
		}
	}
}

/*
================
Con_AddLine

Appends a given string as a new line to the console.
================
*/
static void Con_AddLine( const char *line, int length, qboolean newline )
{
	char *putpos;

	if( !con.initialized || !con.buffer )
		return;

	Con_FixTimes();
	length++;	// reserve space for term

	ASSERT( length < CON_TEXTSIZE );

	while( !( putpos = Con_BytesLeft( length )) || con.lines_count >= con.maxlines )
		Con_DeleteLine();

	con_lineinfo_t *p;
	if( newline )
	{
		memcpy( putpos, line, length );
		putpos[length - 1] = '\0';
		con.lines_count++;

		p = &CON_LINES_LAST();
		p->start = putpos;
		p->length = length;
		// A7 build 91: developer lines get addtime 0, so Con_DrawNotify
		// never shows them over the HUD (they stay console-only)
		p->addtime = con_debug_line ? 0.0 : cl.time;
	}
	else
	{
		p = &CON_LINES_LAST();
		putpos = p->start + Q_strlen( p->start );
		memcpy( putpos, line, length - 1 );
		p->length = Q_strlen( p->start );
		putpos[p->length] = '\0';
		// A7 build 91: a dev line continuing an existing line must not
		// revive it for the notify either
		if( !con_debug_line )
			p->addtime = cl.time;
		p->length++;
	}
}

/*
================
Con_GetSafeArea

================
*/
static void Con_GetSafeArea( int lines, wrect_t *rc )
{
	rc->left = refState.width * host.window_insets[0];
	rc->right = refState.width - (int)( refState.width * host.window_insets[2] );
	rc->top = refState.height * host.window_insets[1];
	rc->bottom = Q_min( lines, refState.height - (int)( refState.height * host.window_insets[3] ));
}

/*
================
Con_CheckResize

If the line width has changed, reformat the buffer.
================
*/
static void Con_CheckResize( void )
{
	int	charWidth = 8;

	if( con.curFont && con.curFont->hFontTexture )
		charWidth = con.curFont->charWidths['O'] - 1;

	wrect_t safe;
	Con_GetSafeArea( refState.height, &safe );

	int width = (( safe.right - safe.left ) / charWidth ) - 2;
	if( !ref.initialized ) width = (640 / 5);

	if( width == con.linewidth )
		return;

	Con_ClearNotify();
	con.linewidth = width;
	con.backscroll = 0;

	con.input.widthInChars = con.linewidth;
}

/*
================
Con_PageUp
================
*/
void Con_PageUp( int lines )
{
	con.backscroll += abs( lines );
}

/*
================
Con_PageDown
================
*/
void Con_PageDown( int lines )
{
	con.backscroll -= abs( lines );
}

/*
================
Con_Top
================
*/
static void Con_Top( void )
{
	con.backscroll = CON_MAXLINES;
}

/*
================
Con_Bottom
================
*/
void Con_Bottom( void )
{
	con.backscroll = 0;
}

/*
================
Con_Visible
================
*/
int GAME_EXPORT Con_Visible( void )
{
	return (con.vislines > 0);
}

/*
================
Con_FixedFont
================
*/
qboolean Con_FixedFont( void )
{
	return CL_FixedFont( con.curFont );
}


/*
================
Con_LoadConsoleFont

INTERNAL RESOURCE
================
*/
static void Con_LoadConsoleFont( int fontNumber, cl_font_t *font )
{
	qboolean success = false;
	float scale = con_fontscale.value;

	if( font->valid )
		return; // already loaded

	if( con_oldfont.value )
	{
		success = Con_LoadVariableWidthFont( "gfx/conchars.fnt", font, scale, &con_fontrender, TF_FONT|TF_NEAREST );
	}
	else
	{
		string path;

		// A7-full-port: always prefer the bundled full-charset font
		// (font0/1/2_<charset>.fnt from the APK assets) instead of gating it
		// behind the fonts.wad CRC check. The game data ships a non-default
		// fonts.wad whose FONT* lumps render untidy through the wad load path
		// and lose glyphs such as '<', '=', '/'. The wad fonts and the bundled
		// .fnt files are byte-identical (cp1251), so nothing is lost; the wad
		// is only used when the charset .fnt file is missing.
		if( Q_snprintf( path, sizeof( path ),
			"font%i_%s.fnt", fontNumber, Cvar_VariableString( "con_charset" )) > 0 )
		{
			success = Con_LoadVariableWidthFont( path, font, scale, &con_fontrender, TF_FONT|TF_NEAREST );
		}

		if( !success )
		{
			Q_snprintf( path, sizeof( path ), "fonts/font%i", fontNumber );
			success = Con_LoadVariableWidthFont( path, font, scale, &con_fontrender, TF_FONT|TF_NEAREST );
		}
	}

	if( !success )
	{
		// quake fixed font as fallback
		// keep source to print directly into conback image
		if( !Con_LoadFixedWidthFont( "gfx/conchars", font, scale, &con_fontrender, TF_FONT|TF_NEAREST|TF_KEEP_SOURCE ))
			Con_DPrintf( S_ERROR "failed to load console font\n" );
	}
}

/*
================
Con_LoadConchars
================
*/
static void Con_LoadConchars( void )
{
	// load all the console fonts
	for( int i = 0; i < CON_NUMFONTS; i++ )
		Con_LoadConsoleFont( i, con.chars + i );

	// select properly fontsize
	int fontSize;
	if( con_fontnum.value >= 0 && con_fontnum.value <= CON_NUMFONTS - 1 )
		fontSize = con_fontnum.value;
	else if( refState.width <= 640 )
		fontSize = 0;
	else if( refState.width >= 1280 )
		fontSize = 2;
	else fontSize = 1;

	if( fontSize > CON_NUMFONTS - 1 )
		fontSize = CON_NUMFONTS - 1;

	// sets the current font
	con.curFont = &con.chars[fontSize];
}

/*
============================
Con_UtfProcessChar

Convert utf char to current font's single-byte encoding
============================
*/
int Con_UtfProcessCharForce( int in )
{
	// TODO: get rid of global state where possible
	static utfstate_t state = { 0 };

	// a utf-8 sequence can only continue with a continuation byte. anything
	// else means the previous lead byte was actually a classic single-byte
	// character: drop the stale state and render this byte as-is, so a char
	// like '<' after it is never swallowed or mangled
	if( state.len )
	{
		if( in < 0x80 || in > 0xBF )
		{
			memset( &state, 0, sizeof( state ));
			return in;
		}
	}
	else if(( in >= 0x80 && in <= 0xBF ) || in >= 0xF8 )
	{
		// stray continuation byte or invalid lead byte:
		// classic single-byte character, pass it through
		return in;
	}

	uint32_t ch = Q_DecodeUTF8( &state, in );

	if( !ch )
		return 0; // waiting for continuation bytes

	if( g_codepage == 1251 )
		return Q_UnicodeToCP1251( ch );
	if( g_codepage == 1252 )
		return Q_UnicodeToCP1252( ch );

	return '?'; // not implemented yet
}

int GAME_EXPORT Con_UtfProcessChar( int in )
{
	if( !cls.accept_utf8 ) // incoming character is not a UTF-8 sequence
		return in;

	// otherwise, decode it and convert to selected codepage
	return Con_UtfProcessCharForce( in );
}

// A7 build 103: the HUD TrueType atlas must rasterize the same single-byte
// codepage this function converts HUD strings into (cp1251 by default),
// otherwise every byte >= 127 lands in an empty glyph cell and does not
// show (user: "there's fonts missing which is points not showing in hud")
int Con_GetCodepage( void )
{
	return g_codepage;
}

/*
=================
Con_UtfMoveLeft

get position of previous printful char
=================
*/
int Con_UtfMoveLeft( char *str, int pos )
{
	utfstate_t state = { 0 };
	int k = 0;

	if( !cls.accept_utf8 ) // incoming character is not a UTF-8 sequence
		return pos - 1;

	if( pos == 1 )
		return 0;

	for( int i = 0; i < pos - 1; i++ )
	{
		if( Q_DecodeUTF8( &state, (byte)str[i] ))
			k = i + 1;
	}

	return k;
}

/*
=================
Con_UtfMoveRight

get next of previous printful char
=================
*/
int Con_UtfMoveRight( char *str, int pos, int length )
{
	utfstate_t state = { 0 };

	if( !cls.accept_utf8 ) // incoming character is not a UTF-8 sequence
		return pos + 1;

	for( int i = pos; i <= length; i++ )
	{
		if( Q_DecodeUTF8( &state, (byte)str[i] ))
			return i + 1;
	}

	return pos + 1;
}

static void Con_DrawCharToConback( int num, const byte *conchars, byte *dest )
{
	int row = num >> 4;
	int col = num & 15;
	const byte *source = conchars + (row << 10) + (col << 3);

	int drawline = 8;

	while( drawline-- )
	{
		for( int x = 0; x < 8; x++ )
			if( source[x] != 255 )
				dest[x] = 0x60 + source[x];
		source += 128;
		dest += 320;
	}

}

/*
====================
Con_GetFont

====================
*/
cl_font_t *Con_GetFont( int num )
{
	num = bound( 0, num, CON_NUMFONTS - 1 );
	return &con.chars[num];
}

/*
====================
Con_GetCurFont

====================
*/
cl_font_t *Con_GetCurFont( void )
{
	return con.curFont;
}

/*
====================
Con_DrawStringLen

compute string width and height in screen pixels
====================
*/
void GAME_EXPORT Con_DrawStringLen( const char *pText, int *length, int *height )
{
	// raw single-byte text, like the original engine
	CL_DrawStringLen( con.curFont, pText, length, height, 0 );
}

/*
====================
Con_DrawString

client version of routine
====================
*/
int Con_DrawString( int x, int y, const char *string, const rgba_t setColor )
{
	// raw single-byte text, like the original engine
	return CL_DrawString( x, y, string, setColor, con.curFont, 0 );
}

/*
================
Con_Init
================
*/
void Con_Init( void )
{
	if( host.type == HOST_DEDICATED )
		return; // dedicated server already have console

	// must be init before startup video subsystem
	Cvar_RegisterVariable( &scr_conspeed );
	Cvar_RegisterVariable( &con_notifytime );
	Cvar_RegisterVariable( &con_fontsize );
	Cvar_RegisterVariable( &con_charset );
	Cvar_RegisterVariable( &con_fontscale );
	Cvar_RegisterVariable( &con_fontrender );
	Cvar_RegisterVariable( &con_fontnum );
	Cvar_RegisterVariable( &con_color );
	Cvar_RegisterVariable( &scr_drawversion );
	Cvar_RegisterVariable( &con_oldfont );
	Cvar_RegisterVariable( &con_showcompletion );

	// init the console buffer
	con.bufsize = CON_TEXTSIZE;
	con.buffer = (char *)Z_Calloc( con.bufsize );
	con.maxlines = CON_MAXLINES;
	con.lines = (con_lineinfo_t *)Z_Calloc( con.maxlines * sizeof( *con.lines ));
	con.lines_first = con.lines_count = 0;
	con.num_times = CON_TIMES; // default as 4

	Con_CheckResize();

	Con_ClearField( &con.input );
	con.input.widthInChars = con.linewidth;

	Con_ClearField( &con.chat );
	con.chat.widthInChars = con.linewidth;

	Cmd_AddCommand( "toggleconsole", Con_ToggleConsole_f, "opens or closes the console" );
	Cmd_AddRestrictedCommand( "clear", Con_Clear_f, "clear console history" );
	Cmd_AddCommand( "messagemode", Con_MessageMode_f, "enable message mode \"say\"" );
	Cmd_AddCommand( "messagemode2", Con_MessageMode2_f, "enable message mode \"say_team\"" );
	Cmd_AddCommand( "contimes", Con_SetTimes_f, "change number of console overlay lines (4-64)" );
	con.initialized = true;

	Con_Printf( "Console initialized.\n" );
}

/*
================
Con_Shutdown
================
*/
void Con_Shutdown( void )
{
	con.initialized = false;

	if( con.buffer )
		Mem_Free( con.buffer );

	if( con.lines )
		Mem_Free( con.lines );

	con.buffer = NULL;
	con.lines = NULL;
	Con_SaveHistory( &con.history );
}

/*
================
Con_Print

Handles cursor positioning, line wrapping, etc
All console printing must go through this in order to be displayed
If no console is visible, the notify window will pop up.
================
*/
void Con_Print( const char *txt )
{
	static qboolean cr_pending = false;
	static qboolean colorstring = false;
	static char buf[MAX_PRINT_MSG];
	static int  lastlength = 0;
	static int  bufpos = 0;
	static int  charpos = 0;

	qboolean norefresh = false;
	int		c, mask = 0;

	// client not running
	if( !con.initialized || !con.buffer )
		return;

	if( txt[0] == 2 )
	{
		// go to colored text
		if( Con_FixedFont( ))
			mask = 128;
		txt++;
	}

	if( txt[0] == 3 )
	{
		norefresh = true;
		txt++;
	}

	for( ; *txt; txt++ )
	{
		if( cr_pending )
		{
			Con_DeleteLastLine();
			cr_pending = false;
		}
		c = *txt;

		switch( c )
		{
		case '\0':
			break;
		case '\r':
			if( txt[1] != '\n' )
			{
				Con_AddLine( buf, bufpos, true );
				lastlength = CON_LINES_LAST().length;
				cr_pending = true;
				bufpos = 0;
				charpos = 0;
			}
			break;
		case '\n':
			Con_AddLine( buf, bufpos, true );
			lastlength = CON_LINES_LAST().length;
			bufpos = 0;
			charpos = 0;
			break;
		default:
			buf[bufpos++] = c | mask;

			if( IsColorString( txt ))
			{
				// first color string character
				colorstring = true;
			}
			else if( colorstring )
			{
				// second color string character
				colorstring = false;
			}
			else
			{
				// not a color string, move char counter
				charpos++;
			}

			if(( bufpos >= sizeof( buf ) - 1 ) || charpos >= ( con.linewidth - 1 ))
			{
				Con_AddLine( buf, bufpos, true );
				lastlength = CON_LINES_LAST().length;
				bufpos = 0;
				charpos = 0;
			}
			break;
		}
	}

	if( norefresh ) return;

	// custom renderer cause problems while updates screen on-loading
	if( SV_Active() && cls.state < ca_active && !cl.video_prepped && !cls.disable_screen )
	{
		if( bufpos != 0 )
		{
			Con_AddLine( buf, bufpos, lastlength != 0 );
			lastlength = 0;
			bufpos = 0;
			charpos = 0;
		}

		// FIXME: disable updating screen, because when texture is bound any console print
		// can re-bound it to console font texture
#if 0
		if( !inupdate )
		{
			inupdate = true;
			SCR_UpdateScreen ();
			inupdate = false;
		}
#endif
	}
}

/*
================
Con_NXPrintfv

Draw a single debug line with specified height, color and time to live
================
*/
static void Con_NXPrintfv( keydest_t key_dest, const con_nprint_t *info, const char *fmt, va_list va )
{
	if( info->index < 0 || info->index >= ARRAYSIZE( con.notify ))
		return;

	Q_vsnprintf( con.notify[info->index].szNotify, sizeof( con.notify[info->index].szNotify ), fmt, va );

	// setup values
	con.notify[info->index].key_dest = key_dest;
	con.notify[info->index].expire = host.realtime + info->time_to_live;
	MakeRGBA( con.notify[info->index].color, (byte)(info->color[0] * 255), (byte)(info->color[1] * 255), (byte)(info->color[2] * 255), 255 );
	con.draw_notify = true;
}

/*
================
Con_NPrint

Draw a single debug line with specified height
================
*/
void GAME_EXPORT Con_NPrintf( int idx, const char *fmt, ... )
{
	va_list	args;
	con_nprint_t info =
	{
		.index = idx,
		.time_to_live = 4.0f,
		.color = { 1.0f, 1.0f, 1.0f },
	};

	va_start( args, fmt );
	Con_NXPrintfv( key_game, &info, fmt, args );
	va_end( args );
}

/*
================
Con_NXPrint

Draw a single debug line with specified height, color and time to live
================
*/
void GAME_EXPORT Con_NXPrintf( con_nprint_t *info, const char *fmt, ... )
{
	va_list	args;

	if( !info ) return;

	va_start( args, fmt );
	Con_NXPrintfv( key_game, info, fmt, args );
	va_end( args );
}

/*
================
UI_NPrint

Draw a single debug line with specified height (menu version)
================
*/
void GAME_EXPORT UI_NPrintf( int idx, const char *fmt, ... )
{
	va_list	args;
	con_nprint_t info =
	{
		.index = idx,
		.time_to_live = 4.0f,
		.color = { 1.0f, 1.0f, 1.0f },
	};

	va_start( args, fmt );
	Con_NXPrintfv( key_menu, &info, fmt, args );
	va_end( args );
}

/*
================
UI_NXPrint

Draw a single debug line with specified height, color and time to live (menu version)
================
*/
void GAME_EXPORT UI_NXPrintf( con_nprint_t *info, const char *fmt, ... )
{
	va_list	args;

	if( !info ) return;

	va_start( args, fmt );
	Con_NXPrintfv( key_menu, info, fmt, args );
	va_end( args );
}

/*
=============================================================================

EDIT FIELDS

=============================================================================
*/
/*
================
Con_ClearField
================
*/
static void Con_ClearField( field_t *edit )
{
	memset( edit->buffer, 0, sizeof( edit->buffer ));
	edit->cursor = 0;
	edit->scroll = 0;
}

/*
================
Field_Set
================
*/
MAYBE_UNUSED static void Field_Set( field_t *f, const char *string )
{
	f->scroll = 0;
	f->cursor = Q_strncpy( f->buffer, string, sizeof( f->buffer ));
}

/*
================
Field_Paste
================
*/
static void Field_Paste( field_t *edit )
{
	char *cbd = Sys_GetClipboardData();
	if( !cbd ) return;

	// send as if typed, so insert / overstrike works properly
	int pasteLen = Q_strlen( cbd );
	for( int i = 0; i < pasteLen; i++ )
		Field_CharEvent( edit, cbd[i] );
}

/*
=================
Field_GoTo

=================
*/
static void Field_GoTo( field_t *edit, int pos )
{
	edit->cursor = pos;
	edit->scroll = Q_max( 0, edit->cursor - edit->widthInChars );
}

/*
=================
Field_KeyDownEvent

Performs the basic line editing functions for the console,
in-game talk, and menu fields

Key events are used for non-printable characters, others are gotten from char events.
=================
*/
static void Field_KeyDownEvent( field_t *edit, int key )
{
	// shift-insert is paste
	if((( key == K_INS ) || ( key == K_KP_INS )) && Key_IsDown( K_SHIFT ))
	{
		Field_Paste( edit );
		return;
	}

	int len = Q_strlen( edit->buffer );

	if( key == K_DEL )
	{
		if( edit->cursor < len )
			memmove( edit->buffer + edit->cursor, edit->buffer + edit->cursor + 1, len - edit->cursor );
		return;
	}

	if( key == K_BACKSPACE || key == K_X_BUTTON )
	{
		if( edit->cursor > 0 )
		{
			int newcursor = Con_UtfMoveLeft( edit->buffer, edit->cursor );
			memmove( edit->buffer + newcursor, edit->buffer + edit->cursor, len - edit->cursor + 1 );
			edit->cursor = newcursor;
			if( edit->scroll ) edit->scroll--;
		}
		return;
	}

	if( key == K_RIGHTARROW || key == K_DPAD_RIGHT )
	{
		if( edit->cursor < len ) edit->cursor = Con_UtfMoveRight( edit->buffer, edit->cursor, edit->widthInChars );
		if( edit->cursor >= edit->scroll + edit->widthInChars && edit->cursor <= len )
			edit->scroll++;
		return;
	}

	if( key == K_LEFTARROW || key == K_DPAD_LEFT )
	{
		if( edit->cursor > 0 ) edit->cursor = Con_UtfMoveLeft( edit->buffer, edit->cursor );
		if( edit->cursor < edit->scroll ) edit->scroll--;
		return;
	}

	if( key == K_HOME || ( Q_tolower(key) == 'a' && Key_IsDown( K_CTRL )))
	{
		Field_GoTo( edit, 0 );
		return;
	}

	if( key == K_END || ( Q_tolower(key) == 'e' && Key_IsDown( K_CTRL )))
	{
		Field_GoTo( edit, len );
		return;
	}

	if( key == K_INS )
	{
		host.key_overstrike = !host.key_overstrike;
		return;
	}
}

/*
==================
Field_CharEvent
==================
*/
static void Field_CharEvent( field_t *edit, int ch )
{
	if( ch == 'v' - 'a' + 1 )
	{
		// ctrl-v is paste
		Field_Paste( edit );
		return;
	}

	if( ch == 'c' - 'a' + 1 )
	{
		// ctrl-c clears the field
		Con_ClearField( edit );
		return;
	}

	int len = Q_strlen( edit->buffer );

	if( ch == 'a' - 'a' + 1 )
	{
		// ctrl-a is home
		Field_GoTo( edit, 0 );
		return;
	}

	if( ch == 'e' - 'a' + 1 )
	{
		// ctrl-e is end
		Field_GoTo( edit, len );
		return;
	}

	// ignore any other non printable chars
	if( ch < 32 ) return;

	if( host.key_overstrike )
	{
		if ( edit->cursor == MAX_STRING - 1 ) return;
		edit->buffer[edit->cursor] = ch;
		edit->cursor++;
	}
	else
	{
		// insert mode
		if ( len == MAX_STRING - 1 ) return; // all full
		memmove( edit->buffer + edit->cursor + 1, edit->buffer + edit->cursor, len + 1 - edit->cursor );
		edit->buffer[edit->cursor] = ch;
		edit->cursor++;
	}

	if( edit->cursor >= edit->widthInChars ) edit->scroll++;
	if( edit->cursor == len + 1 ) edit->buffer[edit->cursor] = 0;
}

/*
==================
Field_DrawInputLine

A7 build 92: font is optional - the floating console window passes the
window font (creditsfont) so the input row matches the rest of the window
==================
*/
static int Field_DrawInputLine( int x, int y, const field_t *edit, byte alpha, qboolean cursor, cl_font_t *font )
{
	char str[MAX_SYSPATH];
	rgba_t colorDefault;
	const int prestep = bound( 0, edit->scroll, sizeof( edit->buffer ) - 1 );
	const int drawLen = bound( 0, edit->widthInChars, sizeof( str ));
	const int cursorCharPos = bound( 0, edit->cursor - prestep, sizeof( str ));

	if( !font ) font = con.curFont;
	if( !font ) return 0;

	memcpy( colorDefault, g_color_table[ColorIndex( COLOR_DEFAULT )], 3 * sizeof( colorDefault[0] ));
	colorDefault[3] = alpha;

	Q_strncpy( str, edit->buffer + prestep, drawLen );

	// draw it
	CL_DrawString( x, y, str, colorDefault, font, 0 );

	// calc cursor position
	str[cursorCharPos] = 0;
	int curPos;
	CL_DrawStringLen( font, str, &curPos, NULL, 0 );

	// draw the cursor
	if( !cursor || (int)( host.realtime * 4 ) & 1 )
		return curPos; // off blink

	CL_DrawCharacter( x + curPos, y, host.key_overstrike ? '|' : '_', colorDefault, font, 0 );

	return curPos;
}

/*
=============================================================================

CONSOLE HISTORY HANDLING

=============================================================================
*/
/*
===================
Con_HistoryFromField

===================
*/
static void Con_HistoryFromField( history_line_t *dst, const field_t *src )
{
	Q_strncpy( dst->buffer, src->buffer, sizeof( dst->buffer ));
	dst->cursor = src->cursor;
	dst->scroll = src->scroll;
}

/*
===================
Con_HistoryToField

===================
*/
static void Con_HistoryToField( field_t *dst, const history_line_t *src )
{
	Q_strncpy( dst->buffer, src->buffer, sizeof( dst->buffer ));
	dst->cursor = src->cursor;
	dst->scroll = src->scroll;
}

/*
===================
Con_HistoryUp

===================
*/
static void Con_HistoryUp( con_history_t *self, field_t *in )
{
	if( self->line == self->next )
		Con_HistoryFromField( &self->backup, in );
	else
		Con_HistoryFromField( &self->lines[self->line % CON_HISTORY], in );

	if(( self->next - self->line ) < CON_HISTORY )
		self->line = Q_max( 0, self->line - 1 );

	Con_HistoryToField( in, &self->lines[self->line % CON_HISTORY] );
}

/*
===================
Con_HistoryDown

===================
*/
static void Con_HistoryDown( con_history_t *self, field_t *in )
{
	Con_HistoryFromField( &self->lines[self->line % CON_HISTORY], in );

	self->line = Q_min( self->next, self->line + 1 );
	if( self->line == self->next )
		Con_HistoryToField( in, &self->backup );
	else
		Con_HistoryToField( in, &self->lines[self->line % CON_HISTORY] );
}

/*
===================
Con_HistoryAppend
===================
*/
static void Con_HistoryAppend( con_history_t *self, const field_t *from )
{
	int prevLine = Q_max( 0, self->line - 1 );
	const char *buf = from->buffer;

	// skip backslashes
	if( from->buffer[0] == '\\' || from->buffer[1] == '/' )
		buf++;

	// only if non-empty
	if( !from->buffer[0] )
		return;

	// skip empty commands
	if( Q_isspace( buf ))
		return;

	// if not copy (don't ignore backslashes)
	if( !Q_strcmp( from->buffer, self->lines[prevLine % CON_HISTORY].buffer ))
		return;

	Con_HistoryFromField( &self->lines[self->next % CON_HISTORY], from );
	self->line = ++self->next;
}

static void Con_LoadHistory( con_history_t *self )
{
	file_t *fd = FS_Open( "console_history.txt", "rb", true );

	if( !fd )
		return;

	while( !FS_Eof( fd ))
	{
		history_line_t *f = &self->lines[self->next % CON_HISTORY];

		FS_Gets( fd, f->buffer, sizeof( f->buffer ));
		f->cursor = Q_strlen( f->buffer );
		f->scroll = 0;

		// skip empty lines
		if( f->cursor == 0 )
			continue;

		// skip repeating lines
		if( self->next > 0 )
		{
			const history_line_t *prev = &self->lines[(self->next - 1) % CON_HISTORY];
			if( !Q_stricmp( prev->buffer, f->buffer ))
				continue;
		}

		self->next++;
	}

	FS_Close( fd );

	for( int i = self->next; i < CON_HISTORY; i++ )
	{
		history_line_t *f = &self->lines[i];

		memset( f, 0, sizeof( *f ));
	}

	self->line = self->next;
}

static void Con_SaveHistory( con_history_t *self )
{
	// do not save history if nothing was executed
	if( self->next == 0 )
		return;

	int historyStart = self->next - CON_HISTORY;
	if( historyStart < 0 )
		historyStart = 0;

	file_t *f = FS_Open( "console_history.txt", "wb", true );

	if( !f )
	{
		Con_Printf( S_ERROR "%s: can't open %s for write\n", __func__, "console_history.txt" );
		return;
	}

	for( int i = historyStart; i < self->next; i++ )
	{
		const char *s = self->lines[i % CON_HISTORY].buffer;

		// HACKHACK: don't save lines that have something that looks like a password
		if( Q_stristr( s, "password" ) || Q_stristr( s, "_pw" ))
			continue;

		FS_Printf( f, "%s\n", s );
	}

	FS_Close( f );
}


/*
=============================================================================

CONSOLE LINE EDITING

=============================================================================
*/
static void Con_InputCompletion( void )
{
	// A7 build 106: the floating window no longer runs its dropdown from
	// here - Con_WindowKey/Con_CharEvent call Con_WindowSuggestUpdate()
	// directly, so window typing never touches the ghost completion path
	// (the first typed character froze the game in build 105)

	if( !con_showcompletion.value )
		return;

	// keep a copy of console input
	con.input_completion = con.input;

	Con_CompleteCommand( &con.input_completion, false );
}

/*
====================
Key_Console

Handles history and console scrollback
====================
*/
void Key_Console( int key )
{
	// exit the console by pressing MINUS on NSwitch
	// or both Back(Select)/Start buttons for everyone else
	if( key == K_BACK_BUTTON || key == K_START_BUTTON || key == K_ESCAPE )
	{
		if( cls.state == ca_active && !cl.background )
		{
			UI_SetActiveMenu( false ); // we are in game, prevent menu from drawing
			Key_SetKeyDest( key_game );
		}
		else
			UI_SetActiveMenu( true );
		return;
	}

	// ctrl-L clears screen
	if( key == 'l' && Key_IsDown( K_CTRL ))
	{
		Cbuf_AddText( "clear\n" );
		return;
	}

	// enter or A finish the line
	if( key == K_ENTER || key == K_KP_ENTER || key == K_A_BUTTON )
	{
		// backslash text are commands, else chat
		if( con.input.buffer[0] == '\\' || con.input.buffer[0] == '/' )
			Cbuf_AddText( con.input.buffer + 1 ); // skip backslash
		else Cbuf_AddText( con.input.buffer ); // valid command
		Cbuf_AddText( "\n" );

		// echo to console
		Con_Printf( ">%s\n", con.input.buffer );

		// copy line to history buffer
		// just in case, remove all CR and LF characters pushing it to the history
		// not sure how they get even added in the first place
		COM_RemoveLineFeed( con.input.buffer, sizeof( con.input.buffer ));
		Con_HistoryAppend( &con.history, &con.input );

		Con_ClearField( &con.input );
		con.input.widthInChars = con.linewidth;
		Con_Bottom();
		Con_ClearField( &con.input_completion );

		if( cls.state == ca_disconnected )
		{
			// force an update, because the command may take some time
			SCR_UpdateScreen ();
		}
		return;
	}

	// command completion
	if( key == K_TAB || key == K_L2_BUTTON || key == K_LTRIGGER )
	{
		Con_CompleteCommand( &con.input, true );
		Con_Bottom();
		Con_ClearField( &con.input_completion );
		return;
	}

	// command history (ctrl-p ctrl-n for unix style)
	if(( key == K_MWHEELUP && Key_IsDown( K_SHIFT )) || ( key == K_UPARROW ) || (( Q_tolower(key) == 'p' ) && Key_IsDown( K_CTRL )))
	{
		Con_HistoryUp( &con.history, &con.input );
		Con_InputCompletion();
		return;
	}

	if(( key == K_MWHEELDOWN && Key_IsDown( K_SHIFT )) || ( key == K_DOWNARROW ) || (( Q_tolower(key) == 'n' ) && Key_IsDown( K_CTRL )))
	{
		Con_HistoryDown( &con.history, &con.input );
		Con_InputCompletion();
		return;
	}

	// console scrolling
	if( key == K_PGUP || key == K_DPAD_UP )
	{
		Con_PageUp( 1 );
		return;
	}

	if( key == K_PGDN || key == K_DPAD_DOWN )
	{
		Con_PageDown( 1 );
		return;
	}

	if( key == K_MWHEELUP )
	{
		if( Key_IsDown( K_CTRL ))
			Con_PageUp( 8 );
		else Con_PageUp( 2 );
		return;
	}

	if( key == K_MWHEELDOWN )
	{
		if( Key_IsDown( K_CTRL ))
			Con_PageDown( 8 );
		else Con_PageDown( 2 );
		return;
	}

	// ctrl-home = top of console
	if( key == K_HOME && Key_IsDown( K_CTRL ))
	{
		Con_Top();
		return;
	}

	// ctrl-end = bottom of console
	if( key == K_END && Key_IsDown( K_CTRL ))
	{
		Con_Bottom();
		return;
	}

	// enable the OSK with button press
	if( key == K_Y_BUTTON )
	{
		Key_EnableTextInput( true, true );
		return;
	}

	// pass to the normal editline routine
	Field_KeyDownEvent( &con.input, key );
	Con_InputCompletion();
}

/*
================
Key_Message

In game talk message
================
*/
void Key_Message( int key )
{
	char	buffer[MAX_SYSPATH];

	if( key == K_ESCAPE || key == K_BACK_BUTTON )
	{
		Key_SetKeyDest( key_game );
		Con_ClearField( &con.chat );
		return;
	}

	if( key == K_ENTER || key == K_KP_ENTER || key == K_A_BUTTON )
	{
		if( con.chat.buffer[0] && cls.state == ca_active )
		{
			Q_snprintf( buffer, sizeof( buffer ), "%s \"%s\"\n", con.chat_cmd, con.chat.buffer );

			if( g_messagemode_privileged )
				Cbuf_AddText( buffer );
			else Cbuf_AddFilteredText( buffer );
		}

		Key_SetKeyDest( key_game );
		Con_ClearField( &con.chat );
		return;
	}

	Field_KeyDownEvent( &con.chat, key );
}

/*
==============================================================================

A7 BUILD 91: FLOATING CONSOLE WINDOW

GoldSource-style floating window (title bar with a close box, log view with
a scrollbar, input line with Submit) drawn over the main menu and over the
paused game. key_dest stays key_menu, so the menu stays live behind it; the
focused flag routes the keyboard to the input line.

==============================================================================
*/
// A7 build 92: the font the window *is laid out with* - valid anywhere,
// not only while the window draws (hit-rects must match the rendered text)
static cl_font_t *Con_WindowFont( void )
{
	if( cls.creditsFont.valid )
		return &cls.creditsFont;
	return con.curFont;
}

// font actually used for the drawn text: the window font while the
// floating window renders, the console font for the legacy full console
static cl_font_t *Con_WinFont( void )
{
	if( conwin_font )
		return conwin_font;
	return con.curFont;
}

// A7 build 93: top-left column of the main menu - the window sits ABOVE
// the first menu row ("Random server"), like the reference:
//     Console   <- window
//     Random Server
// (user: "console menu that in main menu put in up of random server")
static void Con_WindowPlaceAtCorner( void )
{
	int	margin = Q_max( 8, refState.width / 48 );
	int	bottom;

	// A7 build 106: the user says the console is far too small for the
	// reference (geometry differs) - about 58% of the screen wide and
	// half of it tall
	conwin.w = refState.width * 58 / 100;
	conwin.h = refState.height * 50 / 100;

	// the menu list is laid out in a 1280x768 virtual space; the first row
	// (Random server) sits at virtual y=472 => 472/768 = 61.5% of height.
	// stop the window a little above that so it never covers the row
	bottom = refState.height * 58 / 100;

	conwin.x = margin;
	conwin.y = bottom - conwin.h;
}

static void Con_WindowLayout( void )
{
	if( conwin.w <= 0 ) conwin.w = refState.width >> 1;
	if( conwin.h <= 0 ) conwin.h = refState.height * 50 / 100;

	conwin.x = bound( 0, conwin.x, Q_max( 0, refState.width - conwin.w ));
	conwin.y = bound( 0, conwin.y, Q_max( 0, refState.height - conwin.h ));
}

static void Con_WindowClampScroll( void )
{
	conwin.backscroll = bound( 0, conwin.backscroll, Q_max( 0, CON_LINES_COUNT - 1 ));
}

static void Con_WindowTitleRect( int *x, int *y, int *w, int *h )
{
	cl_font_t	*font = Con_WindowFont();

	if( x ) *x = conwin.x;
	if( y ) *y = conwin.y;
	if( w ) *w = conwin.w;
	if( h ) *h = font ? font->charHeight + 8 : 24;
}

static void Con_WindowCloseRect( int *x, int *y, int *w, int *h )
{
	int	barH;

	Con_WindowTitleRect( NULL, y, NULL, &barH );
	*h = barH - 8;
	*w = *h;
	*x = conwin.x + conwin.w - *w - 4;
	*y += 4;
}

static void Con_WindowSubmitRect( int *x, int *y, int *w, int *h )
{
	cl_font_t	*font = Con_WindowFont();
	int	len = 0;
	int	pad = font ? font->charWidths[' '] : 8;

	if( font ) CL_DrawStringLen( font, "Submit", &len, NULL, 0 );
	*w = len + pad * 2;
	*h = ( font ? font->charHeight : 12 ) + 6;
	*x = conwin.x + conwin.w - *w - 5;
	*y = conwin.y + conwin.h - *h - 5;
}

// A7 build 105: rebuild the dropdown matches for what is currently typed
// A7 build 106: matches come from the flat name index (built once when the
// window opens), so this is a bounded array scan per keystroke
static void Con_WindowSuggestUpdate( void )
{
	static int	suggestLog = 0;

	if( !conwin.visible || !conwin.focused )
	{
		conwin_suggestTotal = 0;
		return;
	}

	conwin_suggestTotal = Con_GetCompletionMatches( con.input.buffer,
		conwin_suggest, CONWIN_SUGGEST_ROWS );

	// A7 build 106: a few markers only, so any remaining typing stall can
	// be attributed from the user's console screenshot
	if( suggestLog < 6 )
	{
		suggestLog++;
		Con_DPrintf( "[A7] suggest: \"%s\" -> %d matches (index %d)\n",
			con.input.buffer, conwin_suggestTotal, Con_SuggestBuildIndex( ));
	}
}

// A7 build 107: typing-freeze trace. the budget is zero all the time, so
// nothing is printed during normal play; the first printable character that
// reaches the window input line arms it, and every marker afterwards (char
// path, sdl text path, per-frame console draw) burns one line until it runs
// out. engine.log then shows the exact step where the stall happens (user:
// "if i type somethings nothings will work like freeze").
static int	a7TraceBudget = 0;

void Con_A7Trace( const char *msg )
{
	if( a7TraceBudget <= 0 )
		return;

	a7TraceBudget--;
	Con_DPrintf( "%s", msg );
}

// rows actually painted: the matched names plus a "..." row when more match
static int Con_WindowSuggestRows( void )
{
	if( conwin_suggestTotal <= 0 )
		return 0;

	return Q_min( conwin_suggestTotal, CONWIN_SUGGEST_ROWS ) +
		(( conwin_suggestTotal > CONWIN_SUGGEST_ROWS ) ? 1 : 0 );
}

// dropdown rect: hangs under the input row from the left edge, like the
// reference picture (its width stops before the Submit button)
static void Con_WindowSuggestRect( int *x, int *y, int *w, int *h )
{
	cl_font_t	*font = Con_WindowFont();
	int	pad = font ? font->charWidths[' '] : 8;
	int	charH = font ? font->charHeight : 12;
	int	subW = 0, rows = Con_WindowSuggestRows();

	Con_WindowSubmitRect( NULL, NULL, &subW, NULL );

	if( x ) *x = conwin.x + pad;
	if( y )
	{
		int	top = conwin.y + conwin.h + 4;
		int	hh = rows * ( charH + 4 ) + 4;

		// never hang past the bottom of the screen (draw and the click
		// test must agree, so the clamp lives here)
		if( top + hh > refState.height - 4 )
			top = Q_max( 4, refState.height - 4 - hh );
		*y = top;
	}
	if( w ) *w = Q_max( 80, conwin.w - pad * 2 - subW );
	if( h ) *h = rows * ( charH + 4 ) + 4;
}

// click on a dropdown row: put the name into the input line and keep typing
static void Con_WindowSuggestPick( int row )
{
	const char	*name = conwin_suggest[row];

	con.input.buffer[0] = 0;
	Q_strncpy( con.input.buffer, name, sizeof( con.input.buffer ));
	Q_strncat( con.input.buffer, " ", sizeof( con.input.buffer ));
	con.input.cursor = Q_strlen( con.input.buffer );
	con.input.scroll = 0;
	Con_ClearField( &con.input_completion );

	// the picked word is finished (trailing space) so the list closes
	Con_WindowSuggestUpdate();
}

// submit the current input line (the Submit button and ENTER both land here)
static void Con_WindowSubmit( void )
{
	// backslash text are commands, else chat
	if( con.input.buffer[0] == '\\' || con.input.buffer[0] == '/' )
		Cbuf_AddText( con.input.buffer + 1 ); // skip backslash
	else Cbuf_AddText( con.input.buffer ); // valid command
	Cbuf_AddText( "\n" );

	// echo to console
	Con_Printf( ">%s\n", con.input.buffer );

	COM_RemoveLineFeed( con.input.buffer, sizeof( con.input.buffer ));
	Con_HistoryAppend( &con.history, &con.input );

	Con_ClearField( &con.input );
	con.input.widthInChars = con.linewidth;
	Con_ClearField( &con.input_completion );
	conwin_suggestTotal = 0; // A7 build 105: dropdown dies with the line

	conwin.backscroll = 0; // follow the new output
}

void Con_WindowOpen( qboolean force )
{
	// A7 build 110: RETIRED - the classic full-width top console replaced
	// this (see Con_ToggleConsole_f). Kept as a no-op so
	// Con_ToggleConsole/Con_WindowAutoOpen (menu open/close hooks) compile
	// and stop latching the floating window on.
	(void)force;
}

void Con_WindowClose( qboolean userInitiated )
{
	if( !conwin.visible )
		return;

	conwin.visible = false;
	conwin.focused = false;
	conwin.dragging = false;
	conwin.scrolling = false;
	conwin.clicked = false;

	// remember the user's choice until the next connect
	if( userInitiated )
		conwin.user_closed = true;
}

// called by UI_SetActiveMenu: the window follows the menu
void Con_WindowAutoOpen( qboolean menuOpen )
{
	if( menuOpen )
	{
		if( !conwin.user_closed && !cls.disable_screen )
			Con_WindowOpen( true );
	}
	else Con_WindowClose( false );
}

// a fresh session forgets the user's close and hides the window
void Con_WindowReset( void )
{
	conwin.visible = false;
	conwin.focused = false;
	conwin.user_closed = false;
	conwin.dragging = false;
	conwin.scrolling = false;
	conwin.clicked = false;
	conwin.backscroll = 0;
}

qboolean Con_WindowVisible( void )
{
	return conwin.visible;
}

qboolean Con_WindowFocused( void )
{
	return conwin.visible && conwin.focused;
}

void Con_WindowSetMouse( int x, int y )
{
	if( conwin.dragging )
	{
		conwin.x = bound( 0, x - conwin.drag_x, Q_max( 0, refState.width - conwin.w ));
		conwin.y = bound( 0, y - conwin.drag_y, Q_max( 0, refState.height - conwin.h ));
	}

	// A7 build 92: touch drag-scroll of the log - dragging the sheet down
	// brings the older (higher) lines into the view, dragging up returns
	// to the newest output (wheel and PGUP/PGDN keep working too)
	if( conwin.scrolling )
	{
		cl_font_t	*font = Con_WindowFont();
		int		charH = font ? font->charHeight : 16;
		int		delta = y - conwin.scroll_y;

		if( charH > 0 && abs( delta ) >= charH )
		{
			conwin.backscroll += delta / charH;
			conwin.scroll_y += ( delta / charH ) * charH;
			Con_WindowClampScroll();
		}
	}

	conwin.mouse_x = x;
	conwin.mouse_y = y;
	conwin.mouse_in = conwin.visible && x >= conwin.x && x < conwin.x + conwin.w &&
		y >= conwin.y && y < conwin.y + conwin.h;
}

static qboolean Con_WindowInRect( int x, int y, int w, int h )
{
	return conwin.mouse_x >= x && conwin.mouse_x < x + w &&
		conwin.mouse_y >= y && conwin.mouse_y < y + h;
}

// A7 build 91: clicks, title-bar drag and wheel over the window are
// consumed here; everything else falls through to the menu behind it
qboolean Con_WindowMouseEvent( int key, qboolean down )
{
	int	x, y, w, h;

	// A7 build 110: CLASSIC TOP CONSOLE input row. A tap on the bottom
	// input band (same geometry Con_DrawInput uses: bottom minus two
	// character rows) raises the Android keyboard + focuses editing.
	// The retired floating window never reliably fired its own enable:
	// its input row overlapped the log-scroll band, the enable call never
	// ran once in a real session (device log: zero "en=1"), and typed
	// characters never reached the engine.
	if( !conwin.visible && cls.key_dest == key_console && down && key == K_MOUSE1 && con.curFont )
	{
		int	inputY = refState.height - ( con.curFont->charHeight * 2 );

		if( conwin.mouse_y >= inputY - 2 )
		{
			Con_DPrintf( "[A7] ev: classic input row tapped -> text input ON\n" );
			Key_EnableTextInput( true, true );
			return true;
		}
	}

	if( !conwin.visible || cls.key_dest != key_menu )
		return false;

	// wheel scrolls the log when the cursor is over the window
	if( key == K_MWHEELUP || key == K_MWHEELDOWN )
	{
		if( !conwin.mouse_in )
			return false;

		if( down )
		{
			conwin.backscroll += ( key == K_MWHEELUP ) ? 3 : -3;
			Con_WindowClampScroll();
		}
		return true;
	}

	if( key != K_MOUSE1 )
		return false;

	if( down )
	{
		int	titleH;

		// A7 build 105: a click on the completion dropdown - the rows hang
		// BELOW the window, outside its own hit rect, so test them first
		if( conwin.focused && conwin_suggestTotal > 0 && con.input.buffer[0] )
		{
			int	sx, sy, sw, sh;
			int	rowH, rowCount = Con_WindowSuggestRows();

			Con_WindowSuggestRect( &sx, &sy, &sw, &sh );
			rowH = ( Con_WindowFont() ? Con_WindowFont()->charHeight : 12 ) + 4;

			if( conwin.mouse_x >= sx && conwin.mouse_x < sx + sw &&
				conwin.mouse_y >= sy && conwin.mouse_y < sy + sh )
			{
				int	row = ( conwin.mouse_y - sy - 2 ) / rowH;

				conwin.clicked = true;
				conwin.focused = true;

				if( row >= 0 && row < rowCount && row < conwin_suggestTotal )
					Con_WindowSuggestPick( row );
				else Con_WindowSuggestUpdate(); // clicked the "..." row

				return true;
			}
		}

		if( !conwin.mouse_in )
		{
			// a click outside goes to the menu and drops our focus
			conwin.focused = false;
			conwin.clicked = false;
			return false;
		}

		conwin.clicked = true;
		conwin.focused = true;

		Con_WindowCloseRect( &x, &y, &w, &h );
		if( Con_WindowInRect( x, y, w, h ))
		{
			Con_WindowClose( true );
			return true;
		}

		Con_WindowSubmitRect( &x, &y, &w, &h );
		if( Con_WindowInRect( x, y, w, h ))
		{
			Con_WindowSubmit();
			return true;
		}

		// A7 build 92: a press anywhere in the log area starts a touch
		// drag-scroll (the user could not scroll the log at all on touch)
		Con_WindowTitleRect( NULL, NULL, NULL, &titleH );
		if( conwin.mouse_y >= conwin.y + titleH && conwin.mouse_y < y - 3 )
		{
			conwin.scrolling = true;
			conwin.scroll_y = conwin.mouse_y;
			return true;
		}

		// click on the input row: raise the on-screen keyboard on touch
		if( conwin.mouse_y >= y - 2 )
			Key_EnableTextInput( true, true );

		// grab by the title bar to drag the window
		Con_WindowTitleRect( &x, &y, &w, &h );
		if( conwin.mouse_y >= y && conwin.mouse_y < y + h )
		{
			conwin.dragging = true;
			conwin.drag_x = conwin.mouse_x - conwin.x;
			conwin.drag_y = conwin.mouse_y - conwin.y;
		}

		return true;
	}

	// release ends a consumed press (and any drag)
	if( conwin.clicked || conwin.dragging || conwin.scrolling )
	{
		conwin.dragging = false;
		conwin.scrolling = false;
		conwin.clicked = false;
		return true;
	}

	return false;
}

// A7 build 91: keyboard while the window is focused (ENTER submits, TAB
// completes, arrows walk the history, PGUP/PGDN scroll the log, everything
// else edits the input line)
void Con_WindowKey( int key )
{
	if( !conwin.visible )
		return;

	if( key == K_PGUP || key == K_DPAD_UP )
	{
		conwin.backscroll += 3;
		Con_WindowClampScroll();
		return;
	}

	if( key == K_PGDN || key == K_DPAD_DOWN )
	{
		conwin.backscroll -= 3;
		Con_WindowClampScroll();
		return;
	}

	if( key == K_HOME )
	{
		conwin.backscroll = Q_max( 0, CON_LINES_COUNT - 1 );
		return;
	}

	if( key == K_END )
	{
		conwin.backscroll = 0;
		return;
	}

	if( key == K_TAB || key == K_L2_BUTTON || key == K_LTRIGGER )
	{
		Con_CompleteCommand( &con.input, true );
		Con_ClearField( &con.input_completion );
		Con_WindowSuggestUpdate();
		return;
	}

	if(( key == K_UPARROW ) || (( Q_tolower( key ) == 'p' ) && Key_IsDown( K_CTRL )))
	{
		Con_HistoryUp( &con.history, &con.input );
		// A7 build 106: window edits refresh the dropdown only - never the
		// ghost completion (freeze on first typed character, build 105)
		Con_WindowSuggestUpdate();
		return;
	}

	if(( key == K_DOWNARROW ) || (( Q_tolower( key ) == 'n' ) && Key_IsDown( K_CTRL )))
	{
		Con_HistoryDown( &con.history, &con.input );
		Con_WindowSuggestUpdate();
		return;
	}

	if( key == K_ENTER || key == K_KP_ENTER || key == K_A_BUTTON )
	{
		Con_WindowSubmit();
		return;
	}

	// A7 build 108: with the platform text input inactive (host.textmode
	// false) a printable keydown arrives HERE instead of through
	// CL_CharEvent - the editline routine below only understands editing
	// keys (backspace/arrows/...), so typed letters were silently swallowed
	// ("type something, nothing works"). Feed them to the input line as
	// characters instead. Letters are 32..126 here: TAB(9), ENTER(13) and
	// ESC(27) are handled above, BACKSPACE is 127 and the arrows >= 128.
	if( key >= 32 && key < 127 && key != '`' && key != '~' )
	{
		Con_DPrintf( "[A7] ev: window key-char '%c'\n", (char)key );
		Field_CharEvent( &con.input, key );
		// A7 build 106: dropdown only (flat index scan), no ghost completion
		Con_WindowSuggestUpdate();
		return;
	}

	// pass to the normal editline routine
	Field_KeyDownEvent( &con.input, key );
	// A7 build 106: dropdown only (flat index scan), no ghost completion
	Con_WindowSuggestUpdate();
}

/*
================
Con_DrawConsoleWindow

Draws the floating console window (body, border, title bar with the close
box, log view with the scrollbar, input line and Submit button)
================
*/
static void Con_DrawConsoleWindow( void )
{
	wrect_t	safe;
	int	titleX, titleY, titleW, titleH;
	int	closeX, closeY, closeW, closeH;
	int	subX, subY, subW, subH;
	int	subLen = 0;
	int	charH, pad, lines, lineno, y, i;
	int	logTop, logBottom, closeGlyphW = 0;
	cl_font_t	*font = Con_WindowFont();

	if( !font )
		return;

	// A7 build 107: frame marker - if the log stops right after a "draw
	// enter" the stall is inside the window drawing, not in the keystroke
	Con_A7Trace( "[A7] cw: draw enter\n" );

	// log lines are drawn by Con_DrawConsoleLine - point it at our font
	conwin_font = font;

	Con_WindowLayout();
	Con_WindowClampScroll();

	charH = font->charHeight;
	pad = font->charWidths[' '];
	if( pad <= 0 ) pad = 8;

	Con_WindowTitleRect( &titleX, &titleY, &titleW, &titleH );
	Con_WindowCloseRect( &closeX, &closeY, &closeW, &closeH );
	Con_WindowSubmitRect( &subX, &subY, &subW, &subH );
	CL_DrawStringLen( font, "Submit", &subLen, NULL, 0 );
	CL_DrawCharacterLen( font, 'X', &closeGlyphW, NULL );

	// A7 build 106: SOLID light khaki - no see-through left at all (the
	// user: "panels are too dark / translucent, wrong green shade"). body,
	// log card, title and Submit are all stepped up one shade toward the
	// reference picture
	ref.dllFuncs.Color4ub( 255, 255, 255, 255 ); // don't grab the screenfade color
	ref.dllFuncs.FillRGBA( kRenderTransTexture, conwin.x, conwin.y, conwin.w, conwin.h, 108, 116, 84, 255 );

	// light outline around the whole sheet
	{
		byte	r = conwin.focused ? 156 : 146;
		byte	g = conwin.focused ? 162 : 151;
		byte	b = conwin.focused ? 136 : 126;

		ref.dllFuncs.FillRGBA( kRenderTransTexture, conwin.x, conwin.y, conwin.w, 1, r, g, b, 255 );
		ref.dllFuncs.FillRGBA( kRenderTransTexture, conwin.x, conwin.y, 1, conwin.h, r, g, b, 255 );
		ref.dllFuncs.FillRGBA( kRenderTransTexture, conwin.x, conwin.y + conwin.h - 1, conwin.w, 1, 62, 66, 50, 255 );
		ref.dllFuncs.FillRGBA( kRenderTransTexture, conwin.x + conwin.w - 1, conwin.y, 1, conwin.h, 62, 66, 50, 255 );
	}

	// title bar (lighter khaki than the body, separated by a dark line)
	ref.dllFuncs.FillRGBA( kRenderTransTexture, titleX, titleY, titleW, titleH, 122, 130, 100, 255 );
	ref.dllFuncs.FillRGBA( kRenderTransTexture, titleX, titleY + titleH - 1, titleW, 1, 62, 66, 50, 255 );

	// little wrench glyph at the left of the title, like the picture:
	// an open ring (spanner head) with a short handle under it
	{
		int	ix = titleX + pad;
		int	iy = titleY + ( titleH - 12 ) * 0.5f;
		byte	ir = 196, ig = 198, ib = 178;

		ref.dllFuncs.FillRGBA( kRenderTransTexture, ix, iy, 7, 2, ir, ig, ib, 255 );     // head top
		ref.dllFuncs.FillRGBA( kRenderTransTexture, ix, iy + 5, 7, 2, ir, ig, ib, 255 ); // head bottom
		ref.dllFuncs.FillRGBA( kRenderTransTexture, ix, iy, 2, 7, ir, ig, ib, 255 );     // head back
		ref.dllFuncs.FillRGBA( kRenderTransTexture, ix + 4, iy + 7, 3, 5, ir, ig, ib, 255 ); // handle
		CL_DrawString( titleX + pad + 12, titleY + ( titleH - charH ) * 0.5f, "Console", g_color_table[7], font, 0 );
	}

	// close box - neutral dark square, never red (picture has a plain X)
	ref.dllFuncs.FillRGBA( kRenderTransTexture, closeX, closeY, closeW, closeH, 66, 71, 54, 255 );
	ref.dllFuncs.FillRGBA( kRenderTransTexture, closeX, closeY, closeW, 1, 140, 145, 126, 255 );
	ref.dllFuncs.FillRGBA( kRenderTransTexture, closeX, closeY + closeH - 1, closeW, 1, 46, 50, 38, 255 );
	CL_DrawCharacter( closeX + ( closeW - closeGlyphW ) * 0.5f, closeY + ( closeH - charH ) * 0.5f,
		'X', g_color_table[7], font, 0 );

	// log view (newest line at the bottom, like GoldSource) on an inset card
	logTop = titleY + titleH + 3;
	logBottom = subY - 5;
	ref.dllFuncs.FillRGBA( kRenderTransTexture, conwin.x + 3, logTop, conwin.w - 6,
		Q_max( 0, logBottom - logTop ), 98, 106, 76, 255 );

	safe.left = conwin.x + pad;
	safe.right = conwin.x + conwin.w - pad - 10;
	safe.top = logTop + 3;
	safe.bottom = logBottom - 2;

	lines = ( safe.bottom - safe.top ) / charH;

	if( lines >= 1 )
	{
		lineno = CON_LINES_COUNT - 1 - conwin.backscroll;
		y = safe.bottom - charH;

		for( i = lines - 1; i >= 0 && lineno >= 0; i-- )
		{
			int	lh;

			// notify-only lines (leading '\1') are never in the log view
			while( lineno >= 0 && CON_LINES( lineno ).start && *CON_LINES( lineno ).start == '\1' )
				lineno--;

			if( lineno < 0 )
				break;

			lh = Con_DrawConsoleLine( &safe, y, lineno );
			if( lh <= 0 )
				lh = charH; // guard: never stall on a zero-height line

			y -= lh;
			lineno--;
		}

		// scrollbar: right column with square arrow buttons top and
		// bottom (A7 build 105 - the picture has the classic two-arrow
		// scrollbar, the bare track alone did not read as one)
		{
			int	trackX = conwin.x + conwin.w - pad - 8;
			int	trackW = 6;
			int	arrowH = 10;
			int	trackTop = safe.top + arrowH + 1;
			int	trackBot = safe.bottom - arrowH - 1;
			int	trackH = Q_max( 6, trackBot - trackTop );
			int	thumbH = Q_max( 8, trackH * lines / Q_max( 1, CON_LINES_COUNT ));
			int	thumbY;

			thumbH = Q_min( thumbH, trackH ); // short log: never outgrow the track
			thumbY = trackTop + ( trackH - thumbH ) * conwin.backscroll / Q_max( 1, CON_LINES_COUNT - lines );
			thumbY = bound( trackTop, thumbY, Q_max( trackTop, trackBot - thumbH ));
			byte	ar = conwin.focused ? 150 : 126;
			byte	ag = conwin.focused ? 155 : 130;
			byte	ab = conwin.focused ? 136 : 114;

			// arrow boxes
			ref.dllFuncs.FillRGBA( kRenderTransTexture, trackX, safe.top, trackW, arrowH, 66, 71, 54, 255 );
			ref.dllFuncs.FillRGBA( kRenderTransTexture, trackX, safe.bottom - arrowH, trackW, arrowH, 66, 71, 54, 255 );

			// up arrow (stacked bars) / down arrow
			ref.dllFuncs.FillRGBA( kRenderTransTexture, trackX + 2, safe.top + 2, 2, 1, ar, ag, ab, 255 );
			ref.dllFuncs.FillRGBA( kRenderTransTexture, trackX + 1, safe.top + 3, 4, 1, ar, ag, ab, 255 );
			ref.dllFuncs.FillRGBA( kRenderTransTexture, trackX + 0, safe.top + 4, 6, 1, ar, ag, ab, 255 );
			ref.dllFuncs.FillRGBA( kRenderTransTexture, trackX + 0, safe.bottom - arrowH + 5, 6, 1, ar, ag, ab, 255 );
			ref.dllFuncs.FillRGBA( kRenderTransTexture, trackX + 1, safe.bottom - arrowH + 6, 4, 1, ar, ag, ab, 255 );
			ref.dllFuncs.FillRGBA( kRenderTransTexture, trackX + 2, safe.bottom - arrowH + 7, 2, 1, ar, ag, ab, 255 );

			// track + thumb
			ref.dllFuncs.FillRGBA( kRenderTransTexture, trackX, trackTop, trackW, trackH, 62, 66, 50, 255 );
			ref.dllFuncs.FillRGBA( kRenderTransTexture, trackX, thumbY, trackW, thumbH,
				conwin.focused ? 146 : 112, conwin.focused ? 152 : 117, conwin.focused ? 132 : 97, 255 );
		}
	}

	// separator above the input row
	ref.dllFuncs.FillRGBA( kRenderTransTexture, conwin.x + 3, subY - 4, conwin.w - 6, 1, 62, 66, 50, 255 );

	// input line: text starts at the left edge of the row, like the
	// picture (no `]` prompt)
	Field_DrawInputLine( conwin.x + pad, subY + 4, &con.input, 255, conwin.focused, font );

	// Submit button - lighter than the panel so it reads as a button
	ref.dllFuncs.FillRGBA( kRenderTransTexture, subX, subY, subW, subH,
		conwin.focused ? 124 : 116, conwin.focused ? 132 : 123, conwin.focused ? 100 : 94, 255 );
	ref.dllFuncs.FillRGBA( kRenderTransTexture, subX, subY, subW, 1,
		conwin.focused ? 156 : 144, conwin.focused ? 162 : 150, conwin.focused ? 136 : 124, 255 );
	ref.dllFuncs.FillRGBA( kRenderTransTexture, subX, subY + subH - 1, subW, 1, 46, 50, 38, 255 );
	CL_DrawString( subX + ( subW - subLen ) * 0.5f, subY + ( subH - charH ) * 0.5f,
		"Submit", g_color_table[7], font, 0 );

	// A7 build 105: completion dropdown under the input row - every
	// command/cvar that starts with the typed word, "..." when there are
	// more (user: "change style of console... with this feature if i type
	// something it show cvars correct")
	Con_A7Trace( "[A7] cw: dropdown begin\n" );

	if( conwin.focused && conwin_suggestTotal > 0 && con.input.buffer[0] )
	{
		int	sx, sy, sw, sh;
		int	rowH = charH + 4;
		int	rowShown = Q_min( conwin_suggestTotal, CONWIN_SUGGEST_ROWS );
		int	rowCount = Con_WindowSuggestRows();
		int	hover = -1;

		Con_WindowSuggestRect( &sx, &sy, &sw, &sh );

		if( conwin.mouse_x >= sx && conwin.mouse_x < sx + sw )
		{
			for( i = 0; i < rowCount; i++ )
			{
				if( conwin.mouse_y >= sy + 2 + i * rowH && conwin.mouse_y < sy + 2 + ( i + 1 ) * rowH )
					hover = i;
			}
		}

		ref.dllFuncs.FillRGBA( kRenderTransTexture, sx, sy, sw, sh, 112, 120, 88, 255 );
		ref.dllFuncs.FillRGBA( kRenderTransTexture, sx, sy, sw, 1, 156, 162, 136, 255 );
		ref.dllFuncs.FillRGBA( kRenderTransTexture, sx, sy, 1, sh, 156, 162, 136, 255 );
		ref.dllFuncs.FillRGBA( kRenderTransTexture, sx + sw - 1, sy, 1, sh, 62, 66, 50, 255 );
		ref.dllFuncs.FillRGBA( kRenderTransTexture, sx, sy + sh - 1, sw, 1, 62, 66, 50, 255 );

		for( i = 0; i < rowShown; i++ )
		{
			int	ry = sy + 2 + i * rowH;

			if( hover == i )
				ref.dllFuncs.FillRGBA( kRenderTransTexture, sx + 1, ry, sw - 2, rowH, 142, 150, 120, 255 );

			CL_DrawString( sx + pad, ry, conwin_suggest[i], g_color_table[7], font, 0 );
		}

		if( conwin_suggestTotal > rowShown )
		{
			int	ry = sy + 2 + rowShown * rowH;

			if( hover == rowShown )
				ref.dllFuncs.FillRGBA( kRenderTransTexture, sx + 1, ry, sw - 2, rowH, 142, 150, 120, 255 );

			CL_DrawString( sx + pad, ry, "...", g_color_table[7], font, 0 );
		}

		Con_A7Trace( "[A7] cw: dropdown done\n" );
	}

	conwin_font = NULL;
}

/*
==============================================================================

DRAWING

==============================================================================
*/
/*
================
Con_DrawInput

The input line scrolls horizontally if typing goes beyond the right edge
================
*/
static void Con_DrawInput( const wrect_t *safe )
{
	// don't draw anything (always draw if not active)
	if( cls.key_dest != key_console || !con.curFont )
		return;

	int y = safe->bottom - ( con.curFont->charHeight * 2 );
	CL_DrawCharacter( safe->left + con.curFont->charWidths[' '], y, ']', g_color_table[7], con.curFont, 0 );
	int x = Field_DrawInputLine( safe->left + con.curFont->charWidths[' ']*2, y, &con.input, 255, true, NULL );

	// HACKHACK: avoid rendering issues when scroll != 0
	if( con_showcompletion.value && con.input.scroll == 0 )
	{
		int len = Q_strlen( con.input.buffer );

		if( FBitSet( con_showcompletion.flags, FCVAR_CHANGED ))
		{
			Con_InputCompletion();
			ClearBits( con_showcompletion.flags, FCVAR_CHANGED );
		}

		// Con_CompleteCommand destroys the buffer. Need to figure out how to make it append only
		if( Q_strlen(con.input_completion.buffer) > len && !Q_strncmp( con.input.buffer, con.input_completion.buffer, len ))
		{
			con.input_completion.scroll = len;
			Field_DrawInputLine( safe->left + con.curFont->charWidths[' ']*2 + x, y, &con.input_completion, 128, false, NULL );
		}
	}
}

/*
================
Con_DrawDebugLines

Custom debug messages
================
*/
static int Con_DrawDebugLines( void )
{
	notify_t *notify = con.notify;
	int	count = 0;
	int	y = 20;

	if( !con.curFont || !con.curFont->valid )
		return 0;

	int defaultX = refState.width / 4;
	int fontTall = con.curFont->charHeight + 1;

	for( int i = 0; i < ARRAYSIZE( con.notify ); i++, notify++ )
	{
		int x, len;

		if( host.realtime > notify->expire )
			continue;

		if( notify->key_dest != cls.key_dest )
			continue;

		Con_DrawStringLen( notify->szNotify, &len, NULL );
		x = refState.width - Q_max( defaultX, len ) - 10;

		if( y + fontTall > refState.height - 20 )
			return count;

		count++;
		y += fontTall;
		CL_DrawString( x, y, notify->szNotify, notify->color, con.curFont, FONT_DRAW_NOLF );
	}

	return count;
}

/*
================
Con_DrawDebug

Draws the debug messages (not passed to console history)
================
*/
void Con_DrawDebug( void )
{
	// A7 build 92: the "Downloading [N remaining]: ... %" line that used to
	// sit in the top-right corner is gone (user request: "don't show ...
	// text download number on top right") - download progress is already
	// shown by the loading bar at the bottom of the loading screen
	if( !host.allow_console || Con_BackgroundMapActive( ))
		return;

	// A7 build 93: the top-of-screen debug band (Con_NPrintf rows - the
	// developer/warning lines fed through HUD_PRINTNOTIFY) must not paint
	// over the radar while playing or over the loading screen
	// (user: "developer logs texts still showing in top of radar - HIDE
	// IT"). the lines still land in the console history as usual.
	if( cls.state != ca_disconnected )
		return;

	if( con.draw_notify && !Con_Visible( ))
	{
		if( Con_DrawDebugLines() == 0 )
			con.draw_notify = false;
	}
}

/*
================
Con_DrawNotify

Draws the last few lines of output transparently over the game top
================
*/
static void Con_DrawNotify( void )
{
	double	time = cl.time;
	int	x, y = 0;
	// A7 build 92: message styling - the crisp credits font (the same one
	// the HUD text messages use) instead of the chunky console bitmap font
	cl_font_t	*font = cls.creditsFont.valid ? &cls.creditsFont : con.curFont;

	if( !con.curFont || !font ) return;

	// A7 build 92: engine messages are console-only while actually playing
	// (user: "just hide it - only shown texts log in console"). developer
	// lines never reach the notify either: Con_AddLine stores them with
	// addtime 0, so they stay console-only ("Firing: ..." is gone for good)
	//
	// A7 build 95: and the whole top-left band stays OFF the loading
	// screen (user: "fix black screen when i start map with hiding text
	// log in top - hide it"). the load shows backdrop + centered dialog
	// only. every line still lands in the console history as usual
	if( cls.disable_screen )
		return;

	if( host.allow_console && !Con_BackgroundMapActive( ) && cls.state != ca_active )
	{
		// A7 build 92: the build-91 bottom anchor is reverted - the lines
		// are back at the TOP-LEFT like the original engine, and they read
		// like a normal CS 1.6 message line (amber default colour + HUD font)
		x = font->charWidths[' '];
		y = 6;

		for( int i = Q_max( 0, CON_LINES_COUNT - con.num_times ); i < CON_LINES_COUNT; i++ )
		{
			con_lineinfo_t	*l = &CON_LINES( i );

			if( l->addtime < ( time - con_notifytime.value ))
				continue;

			CL_DrawString( x, y, l->start, g_color_table[7], font, 0 );
			y += font->charHeight;
		}
	}

	if( cls.key_dest == key_message )
	{
		string	buf;
		int	len;

		x = font->charWidths[' '];
		y = refState.height / 4; // sane default if the client has no hook

		// update chatline position from client.dll
		if( clgame.dllFuncs.pfnChatInputPosition )
			clgame.dllFuncs.pfnChatInputPosition( &x, &y );

		Q_snprintf( buf, sizeof( buf ), "%s: ", con.chat_cmd );

		CL_DrawStringLen( font, buf, &len, NULL, 0 );
		CL_DrawString( x, y, buf, g_color_table[7], font, 0 );

		Field_DrawInputLine( x + len, y, &con.chat, 255, true, font );
	}

	ref.dllFuncs.Color4ub( 255, 255, 255, 255 );
}

/*
================
Con_DrawConsoleLine

Draws a line of the console; returns its height in lines.
If alpha is 0, the line is not drawn, but still wrapped and its height
returned.
================
*/
static int Con_DrawConsoleLine( const wrect_t *safe, int y, int lineno )
{
	con_lineinfo_t	*li = &CON_LINES( lineno );
	cl_font_t	*font = Con_WinFont();

	if( !li || !li->start || *li->start == '\1' )
		return 0;	// this string will be shown only at notify

	if( !font )
		return 0;

	if( y >= safe->top + font->charHeight )
	{
		float x = safe->left + font->charWidths[' '];

		CL_DrawString( x, y, li->start, g_color_table[7], font, 0 );
	}

	return font->charHeight;
}

/*
================
Con_LastVisibleLine

Calculates the last visible line index and how much to show
of it based on con.backscroll.
================
*/
static void Con_LastVisibleLine( int *lastline )
{
	int lines_seen = 0;

	con.backscroll = Q_max( 0, con.backscroll );
	*lastline = 0;

	// now count until we saw con_backscroll actual lines
	for( int i = CON_LINES_COUNT - 1; i >= 0; i-- )
	{
		// line is the last visible line?
		*lastline = i;

		if( lines_seen + 1 > con.backscroll && lines_seen <= con.backscroll )
			return;

		lines_seen += 1;
	}

	// if we get here, no line was on screen - scroll so that one line is visible then.
	con.backscroll = lines_seen - 1;
}

/*
================
Con_DrawConsole

Draws the console with the solid background
================
*/
static void Con_DrawSolidConsole( int lines )
{
	int	x, y;
	float	fraction;
	int	start;
	int	stringLen, charH;
	string	curbuild;
	byte	color[4];

	if( lines <= 0 ) return;

	// draw the background
	ref.dllFuncs.GL_SetRenderMode( kRenderNormal );
	ref.dllFuncs.Color4ub( 255, 255, 255, 255 ); // to prevent grab color from screenfade
	if( refState.width * 3 / 4 < refState.height && lines >= refState.height )
		ref.dllFuncs.R_DrawStretchPic( 0, lines - refState.height, refState.width, refState.height - refState.width * 3 / 4, 0, 0, 1, 1, R_GetBuiltinTexture( REF_BLACK_TEXTURE ));
	ref.dllFuncs.R_DrawStretchPic( 0, lines - refState.width * 3 / 4, refState.width, refState.width * 3 / 4, 0, 0, 1, 1, con.background );

	if( !con.curFont || !host.allow_console )
		return; // nothing to draw

	wrect_t safe;
	Con_GetSafeArea( lines, &safe );

	// draw current version
	memcpy( color, g_color_table[7], sizeof( color ));

	Q_snprintf( curbuild, MAX_STRING, XASH_ENGINE_NAME " %i/" XASH_VERSION " (%s-%s build %i)", PROTOCOL_VERSION, Q_buildos(), Q_buildarch(), Q_buildnum( ));

	Con_DrawStringLen( curbuild, &stringLen, &charH );

	start = safe.right - stringLen;
	fraction = lines / (float)refState.height;
	color[3] = Q_min( fraction * 2.0f, 1.0f ) * 255; // fadeout version number

#if XASH_MOBILE_PLATFORM
	// the top of the screen might be hidden by the on-screen keyboard panning
	Con_DrawString( start, safe.bottom - charH, curbuild, color );
#else
	Con_DrawString( start, safe.top, curbuild, color );
#endif

	// draw the text
	if( CON_LINES_COUNT > 0 )
	{
		int	ymax = safe.bottom - (con.curFont->charHeight * 2.0f);
		int	lastline;

		Con_LastVisibleLine( &lastline );
		y = ymax - con.curFont->charHeight;

		if( con.backscroll )
		{
			start = con.curFont->charWidths[' ']; // offset one space at left screen side

			// draw red arrows to show the buffer is backscrolled
			CL_SetFontColor( con.curFont, g_color_table[1] );
			for( x = 0; x < con.linewidth; x += 4 )
				CL_DrawCharacter( safe.left + ( x + 1 ) * start, y, '^', NULL, con.curFont, FONT_DRAW_NOCOLOR );
			y -= con.curFont->charHeight;
		}
		x = lastline;

		while( 1 )
		{
			y -= Con_DrawConsoleLine( &safe, y, x );

			// top of console buffer or console window
			if( x == 0 || y < safe.top + con.curFont->charHeight )
				break;
			x--;
		}
	}

	// draw the input prompt, user text, and cursor if desired
	Con_DrawInput( &safe );

	y = lines - ( con.curFont->charHeight * 1.2f );
	SCR_DrawFPS( Q_max( y, 4 )); // to avoid to hide fps counter

	ref.dllFuncs.Color4ub( 255, 255, 255, 255 );
}

/*
==================
Con_DrawConsole
==================
*/
void Con_DrawConsole( void )
{
	// build 90: the loading plaque owns the screen while cls.disable_screen
	// is latched — never unveil the console during a load (a user-toggled
	// console clears the latch first, so manual open/close still works)
	if( cls.disable_screen )
	{
		// A7 build 92: the connect lines are still shown on the plaque, but
		// as CS 1.6-style message lines (top-left, amber, credits font) -
		// they used to come out as raw white console text at the top
		if( cls.state != ca_disconnected )
			Con_DrawNotify();
		return;
	}

	// never draw console when changelevel in-progress
	if( cls.state != ca_disconnected && ( cls.changelevel || cls.changedemo ))
		return;

	// A7 build 91: floating console window over the menu / paused game.
	// it replaces the full-screen console there; the legacy console keeps
	// its behaviour everywhere else (loading, -console, dev work)
	if( conwin.visible )
	{
		if( cls.key_dest != key_menu )
			Con_WindowClose( false ); // safety: never over live gameplay
		else
		{
			Con_CheckResize();
			Con_DrawConsoleWindow();
			return;
		}
	}

	// check for console width changes from a vid mode change
	Con_CheckResize ();

	if( cls.state == ca_connecting || cls.state == ca_connected )
	{
		if( !cl_allow_levelshots.value && !cls.timedemo )
		{
			if( cls.key_dest != key_console && Con_BackgroundMapActive( ))
				con.vislines = con.showlines = 0;
			else con.vislines = con.showlines = refState.height;
		}
		else
		{
			con.showlines = 0;

			if( host_developer.value >= DEV_EXTENDED && !cls.timedemo )
				Con_DrawNotify(); // draw notify lines
		}
	}

	// if disconnected, render console full screen
	switch( cls.state )
	{
	case ca_disconnected:
		// A7 build 96: the legacy full screen is raised ONLY when the
		// user actually keyed the console. after `restart`/`disconnect`
		// the client just sits in key_game with no window - the old
		// force-open here unveiled the full Xash console every time
		// (round 96: "typing restart ... it show full console of xash,
		// i don't want it"). put the GameUI menu back instead
		if( cls.key_dest == key_console )
			Con_DrawSolidConsole( refState.height );
		else if( cls.key_dest != key_menu )
			UI_SetActiveMenu( true ); // runs after UI_UpdateMenu's pass
		break;
	case ca_connecting:
	case ca_connected:
	case ca_validate:
		// build 101: the connecting/changelevel states used to force the
		// full Xash console (with the engine version line) onto the screen
		// while a server changed maps. GoldSource shows the loading screen
		// there - only an actual user-keyed console (-console/-dev work)
		// may still draw in these states
		if( cls.key_dest == key_console )
			Con_DrawSolidConsole( con.vislines );
		break;
	case ca_active:
	case ca_cinematic:
		if( Con_BackgroundMapActive( ))
		{
			if( cls.key_dest == key_console )
				Con_DrawSolidConsole( refState.height );
		}
		else
		{
			if( con.vislines )
				Con_DrawSolidConsole( con.vislines );
			else if( cls.state == ca_active && ( cls.key_dest == key_game || cls.key_dest == key_message ) && !cls.timedemo )
				Con_DrawNotify(); // draw notify lines
		}
		break;
	}

	if( !Con_Visible( )) SCR_DrawFPS( 4 );
}

/*
==================
Con_DrawVersion

Used by menu
==================
*/
void Con_DrawVersion( void )
{
	// draws the current build
	byte	*color = g_color_table[7];
	string	curbuild;

	if( !scr_drawversion.value )
		return;

	// build 101: no Xash version stamp while a load or a server map
	// change runs - GoldSource only shows its loading screen there
	// ("when server changing map it show console xash version")
	if( cls.disable_screen || cls.changelevel || cls.changedemo )
		return;

	if( cls.state == ca_connecting || cls.state == ca_connected || cls.state == ca_validate )
		return;

	if( cls.key_dest == key_menu )
	{
		// A7: cs16.css-style menu has no engine version bar in the corner
		return;
	}
	else
	{
		qboolean draw_version;

		if( CL_IsDevOverviewMode() == 2 || net_graph.value )
			return;

		draw_version = cls.scrshot_action == scrshot_normal
			|| cls.scrshot_action == scrshot_snapshot
			|| host.force_draw_version_time > host.realtime;

		if( !draw_version )
			return;

		Q_snprintf( curbuild, sizeof( curbuild ),
			XASH_ENGINE_NAME " v%i/" XASH_VERSION " (%s-%s build %i)", PROTOCOL_VERSION, Q_buildos(), Q_buildarch(), Q_buildnum( ));
	}

	int stringLen, charH = 0;
	Con_DrawStringLen( curbuild, &stringLen, &charH );
	int start = refState.width - stringLen * 1.05f;
	int height = refState.height - charH * 1.05f;

	Con_DrawString( start, height, curbuild, color );
}

/*
==================
Con_DestHeight

==================
*/
static int Con_DestHeight( void )
{
	if( !host.allow_console || cls.key_dest != key_console )
		return 0; // none visible

	if( cls.state < ca_active || cl.first_frame )
		return refState.height;	// full screen

	return refState.height >> 1;	// half screen
}

/*
==================
Con_GetInputRect

Where the currently edited line is drawn, in render coordinates
==================
*/
qboolean Con_GetInputRect( int *x, int *y, int *w, int *h )
{
	if( !con.curFont )
		return false;

	if( cls.key_dest == key_console )
	{
		int lines = Con_DestHeight();

		if( lines <= 0 )
			return false;

		wrect_t safe;
		Con_GetSafeArea( lines, &safe );

		*x = safe.left + con.curFont->charWidths[' '];
		*y = safe.bottom - con.curFont->charHeight * 2;
		*w = safe.right - *x;
		*h = con.curFont->charHeight;
		return true;
	}

	if( cls.key_dest == key_message )
	{
		*x = con.curFont->charWidths[' '];
		*y = 0;

		if( clgame.dllFuncs.pfnChatInputPosition )
		{
			clgame.dllFuncs.pfnChatInputPosition( x, y );
			*h = con.curFont->charHeight;
		}
		else
		{
			// chat line is pushed down by the notify lines drawn above it
			*h = con.curFont->charHeight * ( con.num_times + 1 );
		}

		*w = refState.width - *x;
		return true;
	}

	return false;
}

/*
==================
Con_RunConsole

Scroll it up or down
==================
*/
void Con_RunConsole( void )
{
	Con_SetColor( );

	con.showlines = Con_DestHeight();

	float lines_per_frame = fabs( scr_conspeed.value ) * host.realframetime;

	if( con.showlines < con.vislines )
	{
		con.vislines -= lines_per_frame;
		if( con.showlines > con.vislines )
			con.vislines = con.showlines;
	}
	else if( con.showlines > con.vislines )
	{
		con.vislines += lines_per_frame;
		if( con.showlines < con.vislines )
			con.vislines = con.showlines;
	}

	if( FBitSet( con_charset.flags|con_fontscale.flags|con_fontnum.flags|cl_charset.flags|con_oldfont.flags, FCVAR_CHANGED ))
	{
		if( con_fontscale.value < 1.0f )
			Cvar_DirectSet( &con_fontscale, "1" );
		
		// update codepage parameters
		if( !Q_stricmp( con_charset.string, "cp1251" ))
		{
			g_codepage = 1251;
		}
		else if( !Q_stricmp( con_charset.string, "cp1252" ))
		{
			g_codepage = 1252;
		}
		else
		{
			g_codepage = 0;
		}

		cls.accept_utf8 = !Q_stricmp( cl_charset.string, "utf-8" );
		Con_InvalidateFonts();
		Con_LoadConchars();
		ClearBits( con_charset.flags,   FCVAR_CHANGED );
		ClearBits( con_fontnum.flags,   FCVAR_CHANGED );
		ClearBits( con_fontscale.flags, FCVAR_CHANGED );
		ClearBits( cl_charset.flags,    FCVAR_CHANGED );
		ClearBits( con_oldfont.flags,   FCVAR_CHANGED );
	}
}

/*
==============================================================================

CONSOLE INTERFACE

==============================================================================
*/
/*
================
Con_CharEvent

Console input
================
*/
void Con_CharEvent( int key )
{
	// distribute the key down event to the apropriate handler
	if( cls.key_dest == key_console )
	{
		// no tilda or backtick in console is supported
		if( key == '`' || key == '~' )
		{
			Con_ToggleConsole_f();
			return;
		}

		Field_CharEvent( &con.input, key );
		Con_InputCompletion();
	}
	else if( cls.key_dest == key_message )
	{
		Field_CharEvent( &con.chat, key );
	}
	else if( cls.key_dest == key_menu && Con_WindowFocused( ))
	{
		// A7 build 91: typing goes to the floating console window while it
		// is focused (the menu stays live behind it)
		if( key == '`' || key == '~' )
			return;

		// A7 build 107: arm the trace on the first character, then bracket
		// every stage of the keystroke
		a7TraceBudget = 28;
		Con_A7Trace( "[A7] chr: window branch in\n" );

		Field_CharEvent( &con.input, key );
		Con_A7Trace( "[A7] chr: field ok\n" );

		// A7 build 106: FIRST typed character froze the game here in build
		// 105 - only refresh the dropdown (flat name index, no engine list
		// walk, no ghost completion) so the keystroke can return instantly
		Con_WindowSuggestUpdate();
		Con_A7Trace( "[A7] chr: window branch out\n" );
	}
}

static int Con_LoadSimpleConback( const char *name, int flags )
{
	for( int i = 0; i < 5; i++ )
	{
		string path;

		switch( i )
		{
		case 0:
			Q_snprintf( path, sizeof( path ), "gfx/shell/%s.dds", name );
			break;
		case 1:
			Q_snprintf( path, sizeof( path ), "gfx/shell/%s.bmp", name );
			break;
		case 2:
			Q_snprintf( path, sizeof( path ), "gfx/shell/%s.tga", name );
			break;
		case 3:
			Q_snprintf( path, sizeof( path ), "cached/%s640", name );
			break;
		case 4:
			Q_snprintf( path, sizeof( path ), "cached/%s", name );
			break;
		}

		if( g_fsapi.FileExists( path, false ))
		{
			int gl_texturenum = ref.dllFuncs.GL_LoadTexture( path, NULL, 0, flags );

			if( gl_texturenum )
				return gl_texturenum;
		}
	}

	return 0;
}

/*
=========
Con_VidInit

INTERNAL RESOURCE
=========
*/
void Con_VidInit( void )
{
	const uint flags = TF_IMAGE|TF_ALLOW_NEAREST;

	if( !con.historyLoaded )
	{
		Con_LoadHistory( &con.history );
		con.historyLoaded = true;
	}

	if( Sys_CheckParm( "-oldfont" ))
		Cvar_DirectSet( &con_oldfont, "1" );

	Con_LoadConchars();
	Con_CheckResize();

#if XASH_LOW_MEMORY
	con.background = R_GetBuiltinTexture( REF_GRAY_TEXTURE );
#else
	// loading console image
	con.background = Con_LoadSimpleConback( host.allow_console ? "conback" : "loading", flags );

	if( !con.background ) // last chance - quake conback image
	{
		fs_offset_t length = 0;
		const byte *buf;

		// quake games always use fixed width fonts, hl games only use variable width
		// and we want to store buildnumber only in quake
		if( con.curFont && con.curFont->type == FONT_FIXED && ( buf = ref.dllFuncs.R_GetTextureOriginalBuffer( con.curFont->hFontTexture )) != NULL )
		{
			lmp_t	*cb = (lmp_t *)FS_LoadFile( "gfx/conback.lmp", &length, false );

			// another sanity test, quake background is always 320x200
			if( cb && cb->width == 320 && cb->height == 200 )
			{
				char ver[64];
				int len = Q_snprintf( ver, 64, "%i", Q_buildnum( )); // can store only buildnum
				byte *dest = (byte *)(cb + 1) + 320 * 186 + 320 - 11 - 8 * len;
				int y = len;
				for( int x = 0; x < y; x++ )
					Con_DrawCharToConback( ver[x], buf, dest + (x << 3));
				con.background = ref.dllFuncs.GL_LoadTexture( "#gfx/conback.lmp", (byte *)cb, length, TF_IMAGE );
			}
			if( cb ) Mem_Free( cb );
		}

		if( !con.background ) // trying the load unmodified conback
			con.background = ref.dllFuncs.GL_LoadTexture( "gfx/conback.lmp", NULL, 0, TF_IMAGE );
	}

	// missed console image will be replaced as gray background like X-Ray or Crysis
	if( con.background == R_GetBuiltinTexture( REF_DEFAULT_TEXTURE ) || con.background == 0 )
		con.background = R_GetBuiltinTexture( REF_GRAY_TEXTURE );
#endif
}

/*
=========
Con_InvalidateFonts

=========
*/
void Con_InvalidateFonts( void )
{
	for( int i = 0; i < ARRAYSIZE( con.chars ); i++ )
		CL_FreeFont( &con.chars[i] );
	con.curFont = NULL;
}

/*
=========
Con_FastClose

immediately close the console
=========
*/
void Con_FastClose( void )
{
	Con_ClearField( &con.input );
	Con_ClearNotify();
	con.showlines = 0;
	con.vislines = 0;
}

/*
=========
Con_DefaultColor

called from MainUI
=========
*/
void Con_DefaultColor( int r, int g, int b, qboolean gameui )
{
	r = bound( 0, r, 255 );
	g = bound( 0, g, 255 );
	b = bound( 0, b, 255 );

	// gameui wants to override console color... check if it's not default
	if( gameui && ( g_color_table[7][0] != r || g_color_table[7][1] != g || g_color_table[7][2] != b ))
	{
		// yes, different from default orange, disable con_color
		SetBits( con_color.flags, FCVAR_READ_ONLY );
		ClearBits( con_color.flags, FCVAR_CHANGED );
	}

	MakeRGBA( g_color_table[7], r, g, b, 255 );
}

#if XASH_ENGINE_TESTS
#include "tests.h"

static void Test_RunConHistory( void )
{
	con_history_t hist = { 0 };
	field_t input = { 0 };
	const char *strs1[] = { "map t0a0", "quit", "wtf", "wtf", "", "nyan" };
	const char *strs2[] = { "nyan", "wtf", "quit", "map t0a0" };
	const char *testbackup = "unfinished_edit";

	for( int i = 0; i < ARRAYSIZE( strs1 ); i++ )
	{
		Field_Set( &input, strs1[i] );
		Con_HistoryAppend( &hist, &input );
	}

	Field_Set( &input, testbackup );

	for( int i = 0; i < ARRAYSIZE( strs2 ); i++ )
	{
		Con_HistoryUp( &hist, &input );
		TASSERT_STR( input.buffer, strs2[i] );
	}

	// check for overrun
	Con_HistoryUp( &hist, &input );

	for( int i = ARRAYSIZE( strs2 ) - 1; i >= 0; i-- )
	{
		TASSERT_STR( input.buffer, strs2[i] );
		Con_HistoryDown( &hist, &input );
	}

	TASSERT_STR( input.buffer, testbackup );
}

void Test_RunCon( void )
{
	TRUN( Test_RunConHistory() );
}

#endif /* XASH_ENGINE_TESTS */
