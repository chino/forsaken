#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/timeb.h>
#include "main.h"
#include "file.h"
#include "util.h"
#include "string.h"
#include "title.h"

extern bool Debug;
bool DebugLog = false;

#ifdef WIN32
#include <windows.h>	// for various things
#include <ctype.h>		// for toupper
#else
#include <unistd.h>
#include <sys/wait.h>
#endif
#include <SDL.h>


char * convert_char( char from, char to, char* in )
{
	char * pch;
	if(!in)
		return in;
	pch = strchr( in, from );
	while( pch != NULL )
	{ 
		*pch = to;
		pch = strchr( pch+1, from );
	}
	return in;
}

char* convert_path( char* _str )
{
#ifdef WIN32
	return _str;
#else
	static char temp[500];
	char * str = temp;
	strncpy( temp, _str, sizeof(temp) );
	while (*str)
	{
		if (*str == '\\')
			*str = '/';
		else
			*str = (char) tolower(*str);
		str++;
	}
	return temp;
#endif
}

void strtoupper(char *str)
{
	while (*str)
	{
		*str = (char) toupper(*str); 
		str++; 
	}
}

void GetFilename( u_int8_t * Src, u_int8_t * Dest )
{
	u_int8_t	*	Char_Ptr;

	Char_Ptr = ( Src + strlen( Src ) ) -1;

	while( Char_Ptr != Src && *Char_Ptr != '\\' && *Char_Ptr != ':' ) Char_Ptr--;

	if( Char_Ptr == Src )
	{
		strcpy( Dest, Src );
		return;
	}

	if( Char_Ptr != ( Src + strlen( Src ) - 1 ) )
	{
		strcpy( Dest, ( Char_Ptr + 1 ) );
	}
	else
	{
		*Dest = 0;
	}
}

void Get_Ext( u_int8_t * Src, u_int8_t * Dest )
{
	u_int8_t	*	Char_Ptr;

	Char_Ptr = ( Src + strlen( Src ) ) -1;

	while( Char_Ptr != Src && *Char_Ptr != '\\' && *Char_Ptr != ':' && *Char_Ptr != '.' ) Char_Ptr--;

	if( *Char_Ptr == '.' )
	{
		Char_Ptr++;
		while( *Char_Ptr ) *Dest++ = *Char_Ptr++;
		*Dest = 0;
	}
	else
	{
		*Dest = 0;
	}
}

void Change_Ext( const char * Src, char * Dest, const char * Ext )
{
	u_int8_t	*	Char_Ptr;

	int length = strlen( Src );

	if ( ! length )
	{
		strcpy( Dest, Ext ); // set the extension
		Msg("Change_Ext called with Src empty!");
		return;
	}

	Char_Ptr = ( (u_int8_t*)Src + length ) - 1;

	while( Char_Ptr != (u_int8_t*)Src && *Char_Ptr != '\\' && *Char_Ptr != ':' && *Char_Ptr != '.' ) Char_Ptr--;

	if( *Char_Ptr == '.' )
	{
		while( (u_int8_t*)Src != Char_Ptr )
			*Dest++ = *Src++;
		strcpy( Dest, Ext );
	}
	else
	{
		strcpy( Dest, Src );
		Dest += strlen( Src );
		strcpy( Dest, Ext );
	}
}

void DebugPrintf( const char * format, ... ) // timestamp prefix
{
	static char buf[0x4000];
	char *buf2;
	int buf_length;
	va_list args;

	if(!Debug)
		return;

	struct timeb now;
	ftime(&now);
	sprintf( buf, "%ld.%.3d ",
		now.time, now.millitm);

	buf2 = strchr(buf,0);
	buf_length = sizeof(buf)-strlen(buf);

	va_start( args, format );
	vsnprintf( buf2, buf_length, format, args );
	va_end( args );

	buf[sizeof(buf)-1]=0; // terminate safely with null byte

	DebugPuts(buf);
}

void DebugPrintf_( const char * format, ... ) // no timestamp prefix
{
	static char buf[0x4000];
	va_list args;

	if(!Debug)
		return;

	va_start( args, format );
	vsnprintf( buf, sizeof(buf), format, args );
	va_end( args );

	buf[sizeof(buf)-1]=0; // terminate safely with null byte

	DebugPuts(buf);
}

void DebugPuts( char * buf )
{
	static FILE * logfile_fp;

	if(!Debug)
		return;

#ifdef WIN32
	OutputDebugString( buf );
#else
	fputs( buf, stderr );
#endif

	if( !DebugLog )
		return;

	if(!logfile_fp)
	{
		time_t now;
		struct tm *ts;
		char buf[80];
		now = time(NULL);
		ts = localtime(&now);
		strftime(buf,sizeof(buf),"Logs\\%m-%d-%y %H.%M.%S.txt",ts);
		logfile_fp = file_open( buf, "w" );
		if(logfile_fp)
		{
			strftime(buf,sizeof(buf),"time zone = %z\n",ts);
			fputs( buf, logfile_fp );
		}
	}

	if( logfile_fp )
	{
		fputs( buf, logfile_fp );
		fflush( logfile_fp );
	}
}

// prints a message only if it wasn't the last one to be printed...
// this way you can debug game state without getting a message every single loop
// for instance in the title screen if you print "in title screen" it will only print once
// instead of every single loop....
void DebugState( const char * str )
{
	static const char * last;
	if ( !Debug ) 
		return;
	if ( last && strcmp( str, last ) == 0 )
		return;
	last = str;
	DebugPrintf( str );
}

//
// Creates a message box and returns ok/cancel press
//

// temporarily jumps to desktop mode
//#include "render.h"
//extern render_info_t render_info;
extern void MenuGoFullScreen( MENUITEM *Item );
extern void input_grab( bool clip );
static char last_msg[ 1024 ];

int Msg( const char * msg, ... )
{
//	bool was_fullscreen = render_info.fullscreen;

	char txt[ 1024 ];
	va_list args;
	int res = 1; // return an ok

	va_start( args, msg );
	vsnprintf( txt, sizeof(txt), msg, args);
	va_end( args );

	txt[sizeof(txt)-1] = 0;

	// remembered for ShowFatalError()
	strncpy( last_msg, txt, sizeof(last_msg)-1 );

#if 0 //def WIN32

    if (render_info.fullscreen)
	{
		// switch to window mode
		// other wise pop up will get stuck behind main window
		MenuGoFullScreen(NULL);
		// push main window to background so popup shows
        SetWindowPos(GetActiveWindow(), HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
	}

	
	// release mouse so they can interact with message box
	input_grab( false );

    res = MessageBox( GetActiveWindow(), txt, "Forsaken", MB_OKCANCEL | MB_ICONEXCLAMATION );

    if (was_fullscreen)
	{
		// switch back to fullscreen
		MenuGoFullScreen(NULL);
        SetWindowPos(GetActiveWindow(), HWND_TOPMOST, 0, 0, 0, 0,  SWP_NOSIZE | SWP_NOMOVE);
		input_grab( true ); // don't do this in window mode just let them click back on the window
	}

	DebugPrintf( txt );

#else // ! WIN32

	DebugPrintf("Msg: %s\n",txt);

#endif
	// IDCANCEL	Cancel button was selected.
	// IDOK	OK button was selected.
	return res;
}

//
// Tells the user why the game is quitting on an error instead of
// silently dropping back to the desktop.
//

#if !SDL_VERSION_ATLEAST(2,0,0) && !defined(WIN32)
// sdl1 has no message box, try the usual desktop helpers
static bool run_dialog( const char * prog, const char * a1, const char * a2, const char * a3 )
{
	int status;
	pid_t pid = fork();
	if( pid < 0 )
		return false;
	if( pid == 0 )
	{
		execlp( prog, prog, a1, a2, a3, (char*) NULL );
		_exit( 127 );
	}
	if( waitpid( pid, &status, 0 ) < 0 )
		return false;
	return WIFEXITED( status ) && WEXITSTATUS( status ) != 127;
}
#endif

void ShowFatalError( void )
{
	char txt[ 1280 ];

	snprintf( txt, sizeof(txt),
		"ProjectX had to quit because of an error:\n\n%s\n\n"
		"Run with -log to write details to the logs folder.",
		last_msg[0] ? last_msg : "unknown error" );
	txt[sizeof(txt)-1] = 0;

	fprintf( stderr, "%s\n", txt );
	DebugPrintf( "ShowFatalError: %s\n", last_msg );

#if SDL_VERSION_ATLEAST(2,0,0)
	SDL_ShowSimpleMessageBox( SDL_MESSAGEBOX_ERROR, "ProjectX", txt, NULL );
#elif defined(WIN32)
	MessageBoxA( NULL, txt, "ProjectX", MB_OK | MB_ICONERROR );
#else
	if( getenv( "DISPLAY" ) )
	{
		char arg[ sizeof(txt) + 8 ];
		snprintf( arg, sizeof(arg), "--text=%s", txt );
		if( !run_dialog( "zenity", "--error", "--no-markup", arg ) )
			run_dialog( "xmessage", "-center", txt, NULL );
	}
#endif
}
