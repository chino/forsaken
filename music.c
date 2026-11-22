/*
 * music.c - plays the cd soundtrack from ripped tracks
 *
 * data/sound/music/trackNN.{ogg,flac,wav} stands in for cd audio track NN.
 * The title screen plays CD_TITLE_TRACK, levels play LevelTrack[] (oct2.c).
 * A small thread keeps an OpenAL source fed so level loads don't starve it.
 */

#ifdef MUSIC

#include "main.h"
#include "util.h"
#include "render.h"
#include "title.h"
#include "oct2.h"
#include "music.h"

#include <SDL.h>
#include <SDL_thread.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

// the thread can't use xmem's tracking allocator
#undef realloc
#undef free

#ifdef MUSIC_OGG
#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>
#endif

#ifdef MUSIC_FLAC
#include <FLAC/stream_decoder.h>
#endif

#define CD_TITLE_TRACK 7

#define MUSIC_BUFFERS 6
#define MUSIC_FRAMES  8192
#define MUSIC_FADE_MS 1000

extern BYTE MyGameStatus;
extern int16_t LevelNum;
extern render_info_t render_info;
extern MENU MENU_NEW_InGameSound;

bool music_disabled = false;

static SDL_mutex * lock;
static SDL_Thread * thread;

// DebugPrintf isn't thread safe, so the thread queues its log lines
static char log_queue[2048];
static size_t log_len;

static void mlog( const char * format, ... )
{
	char line[256];
	size_t n;
	va_list args;

	va_start( args, format );
	vsnprintf( line, sizeof(line), format, args );
	va_end( args );

	n = strlen( line );
	SDL_LockMutex( lock );
	if( log_len + n + 2 < sizeof(log_queue) )
	{
		memcpy( log_queue + log_len, line, n );
		log_len += n;
		log_queue[log_len++] = '\n';
		log_queue[log_len] = 0;
	}
	SDL_UnlockMutex( lock );
}

static void flush_log( void )
{
	char copy[sizeof(log_queue)];
	char * line, * next;

	SDL_LockMutex( lock );
	memcpy( copy, log_queue, log_len + 1 );
	log_len = 0;
	log_queue[0] = 0;
	SDL_UnlockMutex( lock );

	for( line = copy; ( next = strchr( line, '\n' ) ); line = next )
	{
		*next++ = 0;
		DebugPrintf( "music: %s\n", line );
	}
}

//
// decoders, all give interleaved signed 16 bit mono or stereo
//

enum { DEC_NONE, DEC_WAV, DEC_OGG, DEC_FLAC };

typedef struct {
	int type;
	int channels;
	long rate;
	FILE * fp;
	int wav_bytes;
	long wav_start, wav_size, wav_pos;
#ifdef MUSIC_OGG
	OggVorbis_File ogg;
#endif
#ifdef MUSIC_FLAC
	FLAC__StreamDecoder * flac;
	int16_t * flac_pcm;
	unsigned flac_cap, flac_frames, flac_pos;
	bool flac_bad;
#endif
} decoder_t;

static unsigned le16( const unsigned char * b ) { return b[0] | ( b[1] << 8 ); }
static unsigned long le32( const unsigned char * b )
{
	return b[0] | ( b[1] << 8 ) | ( b[2] << 16 ) | ( (unsigned long) b[3] << 24 );
}

static bool wav_open( decoder_t * d )
{
	unsigned char hdr[12], chunk[8], fmt[40];
	unsigned long size, skip;
	unsigned tag = 0, bits = 0;

	if( fread( hdr, 1, 12, d->fp ) != 12 || memcmp( hdr + 8, "WAVE", 4 ) )
		return false;
	while( fread( chunk, 1, 8, d->fp ) == 8 )
	{
		size = le32( chunk + 4 );
		skip = size + ( size & 1 );
		if( !memcmp( chunk, "fmt ", 4 ) )
		{
			size_t n = size < sizeof(fmt) ? size : sizeof(fmt);
			if( n < 16 || fread( fmt, 1, n, d->fp ) != n )
				return false;
			tag = le16( fmt );
			d->channels = le16( fmt + 2 );
			d->rate = le32( fmt + 4 );
			bits = le16( fmt + 14 );
			if( tag == 0xFFFE && n >= 26 )
				tag = le16( fmt + 24 );
			skip -= n;
		}
		else if( !memcmp( chunk, "data", 4 ) )
		{
			d->wav_bytes = bits / 8;
			d->wav_start = ftell( d->fp );
			d->wav_size = size > 0x7fffffff ? 0x7fffffff : (long) size;
			return tag == 1 && ( bits == 8 || bits == 16 || bits == 24 );
		}
		if( fseek( d->fp, (long) skip, SEEK_CUR ) )
			return false;
	}
	return false;
}

static int wav_read( decoder_t * d, int16_t * out, int frames )
{
	static unsigned char raw[MUSIC_FRAMES * 2 * 3];
	long frame_bytes = (long) d->wav_bytes * d->channels;
	long want = (long) frames * frame_bytes;
	long left = d->wav_size - d->wav_pos;
	size_t got, i;

	if( want > left )
		want = left - left % frame_bytes;
	if( want <= 0 )
		return 0;
	got = fread( raw, 1, want, d->fp );
	got -= got % frame_bytes;
	d->wav_pos += got;
	for( i = 0; i < got / d->wav_bytes; i++ )
	{
		unsigned char * s = raw + i * d->wav_bytes;
		if( d->wav_bytes == 1 )
			out[i] = (int16_t)( ( s[0] - 128 ) << 8 );
		else
			out[i] = (int16_t) le16( s + d->wav_bytes - 2 );
	}
	return got / frame_bytes;
}

#ifdef MUSIC_OGG

static size_t ogg_read_cb( void * p, size_t s, size_t n, void * fp ) { return fread( p, s, n, fp ); }
static int ogg_seek_cb( void * fp, ogg_int64_t o, int w ) { return fseek( fp, (long) o, w ); }
static long ogg_tell_cb( void * fp ) { return ftell( fp ); }

static bool ogg_open( decoder_t * d )
{
	ov_callbacks cb = { ogg_read_cb, ogg_seek_cb, NULL, ogg_tell_cb };
	vorbis_info * vi;

	if( ov_open_callbacks( d->fp, &d->ogg, NULL, 0, cb ) )
		return false;
	d->type = DEC_OGG;
	if( !( vi = ov_info( &d->ogg, -1 ) ) )
		return false;
	d->channels = vi->channels;
	d->rate = vi->rate;
	return true;
}

static int ogg_read( decoder_t * d, int16_t * out, int frames )
{
	int bytes = frames * d->channels * 2, done = 0, section;
	while( done < bytes )
	{
		long n = ov_read( &d->ogg, (char *) out + done, bytes - done,
			SDL_BYTEORDER == SDL_BIG_ENDIAN, 2, 1, &section );
		if( n == OV_HOLE )
			continue;
		if( n <= 0 )
			break;
		done += n;
	}
	return done / ( d->channels * 2 );
}

#endif // MUSIC_OGG

#ifdef MUSIC_FLAC

static FLAC__StreamDecoderWriteStatus flac_write_cb( const FLAC__StreamDecoder * dec, const FLAC__Frame * frame, const FLAC__int32 * const buf[], void * user )
{
	decoder_t * d = user;
	unsigned i, c, n = frame->header.blocksize, bps = frame->header.bits_per_sample;
	(void) dec;

	if( (int) frame->header.channels != d->channels )
		d->flac_bad = true;
	if( n > d->flac_cap && !d->flac_bad )
	{
		int16_t * p = realloc( d->flac_pcm, n * d->channels * sizeof(int16_t) );
		if( p ) { d->flac_pcm = p; d->flac_cap = n; }
		else d->flac_bad = true;
	}
	if( d->flac_bad )
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	for( i = 0; i < n; i++ )
		for( c = 0; c < (unsigned) d->channels; c++ )
			d->flac_pcm[ i * d->channels + c ] = (int16_t)( bps >= 16 ? buf[c][i] >> ( bps - 16 ) : buf[c][i] << ( 16 - bps ) );
	d->flac_frames = n;
	d->flac_pos = 0;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void flac_meta_cb( const FLAC__StreamDecoder * dec, const FLAC__StreamMetadata * meta, void * user )
{
	decoder_t * d = user;
	(void) dec;
	if( meta->type == FLAC__METADATA_TYPE_STREAMINFO )
	{
		d->channels = meta->data.stream_info.channels;
		d->rate = meta->data.stream_info.sample_rate;
	}
}

static void flac_error_cb( const FLAC__StreamDecoder * dec, FLAC__StreamDecoderErrorStatus status, void * user )
{
	(void) dec; (void) status; (void) user;
}

static bool flac_open( decoder_t * d )
{
	FLAC__StreamDecoderInitStatus init;

	if( !( d->flac = FLAC__stream_decoder_new() ) )
		return false;
	d->type = DEC_FLAC;
	init = FLAC__stream_decoder_init_FILE( d->flac, d->fp, flac_write_cb, flac_meta_cb, flac_error_cb, d );
	d->fp = NULL; // the decoder closes it
	return init == FLAC__STREAM_DECODER_INIT_STATUS_OK &&
		FLAC__stream_decoder_process_until_end_of_metadata( d->flac );
}

static int flac_read( decoder_t * d, int16_t * out, int frames )
{
	int done = 0;
	while( done < frames && !d->flac_bad )
	{
		if( d->flac_pos < d->flac_frames )
		{
			unsigned n = d->flac_frames - d->flac_pos;
			if( n > (unsigned)( frames - done ) )
				n = frames - done;
			memcpy( out + done * d->channels, d->flac_pcm + d->flac_pos * d->channels, n * d->channels * sizeof(int16_t) );
			d->flac_pos += n;
			done += n;
		}
		else if( FLAC__stream_decoder_get_state( d->flac ) == FLAC__STREAM_DECODER_END_OF_STREAM ||
			!FLAC__stream_decoder_process_single( d->flac ) )
			break;
	}
	return done;
}

#endif // MUSIC_FLAC

static void dec_close( decoder_t * d )
{
#ifdef MUSIC_OGG
	if( d->type == DEC_OGG )
		ov_clear( &d->ogg );
#endif
#ifdef MUSIC_FLAC
	if( d->flac )
		FLAC__stream_decoder_delete( d->flac );
	free( d->flac_pcm );
#endif
	if( d->fp )
		fclose( d->fp );
	memset( d, 0, sizeof(*d) );
}

static bool dec_open( decoder_t * d, const char * path )
{
	char magic[4] = "";
	bool ok = false;

	memset( d, 0, sizeof(*d) );
	if( !( d->fp = fopen( path, "rb" ) ) )
		return false;
	if( fread( magic, 1, 4, d->fp ) != 4 )
		magic[0] = 0;
	rewind( d->fp );

	if( !memcmp( magic, "RIFF", 4 ) )
	{
		d->type = DEC_WAV;
		ok = wav_open( d );
	}
#ifdef MUSIC_OGG
	else if( !memcmp( magic, "OggS", 4 ) )
		ok = ogg_open( d );
#endif
#ifdef MUSIC_FLAC
	else if( !memcmp( magic, "fLaC", 4 ) || !memcmp( magic, "ID3", 3 ) )
		ok = flac_open( d );
#endif

	if( !ok || d->channels < 1 || d->channels > 2 || d->rate <= 0 )
	{
		mlog( "can't play %s", path );
		dec_close( d );
		return false;
	}
	return true;
}

static int dec_read( decoder_t * d, int16_t * out, int frames )
{
	switch( d->type )
	{
	case DEC_WAV: return wav_read( d, out, frames );
#ifdef MUSIC_OGG
	case DEC_OGG: return ogg_read( d, out, frames );
#endif
#ifdef MUSIC_FLAC
	case DEC_FLAC: return flac_read( d, out, frames );
#endif
	}
	return 0;
}

static bool dec_rewind( decoder_t * d )
{
	switch( d->type )
	{
	case DEC_WAV: d->wav_pos = 0; return !fseek( d->fp, d->wav_start, SEEK_SET );
#ifdef MUSIC_OGG
	case DEC_OGG: return !ov_raw_seek( &d->ogg, 0 );
#endif
#ifdef MUSIC_FLAC
	case DEC_FLAC: d->flac_frames = d->flac_pos = 0; return FLAC__stream_decoder_reset( d->flac );
#endif
	}
	return false;
}

//
// streaming, runs on the music thread
//

// written by the main thread under lock
static int want_track;
static float want_volume = 0.7f;
static bool want_paused, want_quit;

static ALuint source, buffers[MUSIC_BUFFERS];
static decoder_t dec;
static int track, next_track;
static float fade;
static Uint32 last_ticks;
static int16_t pcm[MUSIC_FRAMES * 2];

static void stop_track( void )
{
	alSourceStop( source );
	alSourcei( source, AL_BUFFER, 0 );
	alSourceRewind( source );
	dec_close( &dec );
	track = 0;
}

static void start_track( int n )
{
	static const char * ext[] = { "ogg", "flac", "wav" };
	char path[64];
	int i;

	for( i = 0; i < 3; i++ )
	{
		snprintf( path, sizeof(path), "data/sound/music/track%02d.%s", n, ext[i] );
		if( dec_open( &dec, path ) )
		{
			mlog( "playing %s", path );
			track = n;
			return;
		}
	}
	mlog( "no file for cd track %d", n );
}

// fills a buffer, looping the track at its end
static bool fill( ALuint b )
{
	int got = 0, n, rewinds = 0;

	while( got < MUSIC_FRAMES )
	{
		n = dec_read( &dec, pcm + got * dec.channels, MUSIC_FRAMES - got );
		if( n > 0 )
			got += n;
		else if( rewinds++ || !dec_rewind( &dec ) )
			break;
	}
	if( !got )
		return false;
	alBufferData( b, dec.channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16,
		pcm, got * dec.channels * sizeof(int16_t), dec.rate );
	alSourceQueueBuffers( source, 1, &b );
	return true;
}

static int music_thread( void * unused )
{
	(void) unused;
	last_ticks = SDL_GetTicks();
	for( ;; SDL_Delay( 10 ) )
	{
		ALint state, queued, processed;
		ALuint b;
		float volume, dt;
		bool paused;
		Uint32 now = SDL_GetTicks();

		SDL_LockMutex( lock );
		if( want_quit )
		{
			SDL_UnlockMutex( lock );
			break;
		}
		next_track = want_track;
		volume = want_volume;
		paused = want_paused;
		SDL_UnlockMutex( lock );

		dt = ( now - last_ticks ) / (float) MUSIC_FADE_MS;
		last_ticks = now;
		if( dt > 0.1f )
			dt = 0.1f;

		alGetSourcei( source, AL_SOURCE_STATE, &state );
		if( paused )
		{
			if( state == AL_PLAYING )
				alSourcePause( source );
			continue;
		}

		// fade out before changing track, fade in after
		if( next_track != track )
		{
			fade -= dt;
			if( !dec.type || fade <= 0.0f )
			{
				fade = 0.0f;
				stop_track();
				if( next_track )
					start_track( next_track );
				if( !track )
					track = next_track; // don't retry a missing file
			}
		}
		else if( fade < 1.0f )
			fade = fade + dt > 1.0f ? 1.0f : fade + dt;
		alSourcef( source, AL_GAIN, volume * volume * fade );

		if( !dec.type )
			continue;

		alGetSourcei( source, AL_BUFFERS_PROCESSED, &processed );
		while( processed-- > 0 )
		{
			alSourceUnqueueBuffers( source, 1, &b );
			if( !fill( b ) )
				break;
		}
		alGetSourcei( source, AL_BUFFERS_QUEUED, &queued );
		if( queued == 0 )
		{
			int i;
			for( i = 0; i < MUSIC_BUFFERS && fill( buffers[i] ); i++ )
				;
			if( i == 0 )
			{
				mlog( "cd track %d does not decode", track );
				dec_close( &dec );
			}
			alGetSourcei( source, AL_BUFFERS_QUEUED, &queued );
		}
		if( state != AL_PLAYING && queued > 0 )
			alSourcePlay( source );
	}
	stop_track();
	return 0;
}

//
// main thread
//

static bool tried;

static void music_init( void )
{
	if( music_disabled || !alcGetCurrentContext() )
		return;
	tried = true;

	alGetError();
	alGenSources( 1, &source );
	alGenBuffers( MUSIC_BUFFERS, buffers );
	if( alGetError() != AL_NO_ERROR )
	{
		DebugPrintf( "music: could not create openal source\n" );
		return;
	}
	alSourcei( source, AL_SOURCE_RELATIVE, AL_TRUE );
	alSourcef( source, AL_ROLLOFF_FACTOR, 0.0f );
	alSourcef( source, AL_GAIN, 0.0f );

	if( !( lock = SDL_CreateMutex() ) )
		return;
#if SDL_VERSION_ATLEAST(2,0,0)
	thread = SDL_CreateThread( music_thread, "music", NULL );
#else
	thread = SDL_CreateThread( music_thread, NULL );
#endif
	DebugPrintf( "music: %s\n", thread ? "ready" : "could not start thread" );
}

void music_shutdown( void )
{
	if( !thread )
		return;
	SDL_LockMutex( lock );
	want_quit = true;
	SDL_UnlockMutex( lock );
	SDL_WaitThread( thread, NULL );
	thread = NULL;
	flush_log();
	alDeleteSources( 1, &source );
	alDeleteBuffers( MUSIC_BUFFERS, buffers );
	SDL_DestroyMutex( lock );
	lock = NULL;
}

void music_set_volume( float volume )
{
	if( lock )
		SDL_LockMutex( lock );
	want_volume = volume;
	if( lock )
		SDL_UnlockMutex( lock );
}

void music_update( void )
{
	int t = -1;

	if( !tried )
		music_init();
	if( !thread )
		return;
	flush_log();

	// like the cd version: title track in the menus, the level's track in game
	switch( MyGameStatus )
	{
	case STATUS_Title:
	case STATUS_BetweenLevels:
	case STATUS_ViewingStats:
		t = CD_TITLE_TRACK;
		break;
	case STATUS_Normal:
	case STATUS_SinglePlayer:
		if( LevelNum >= 0 && LevelNum < MAXLEVELS )
			t = LevelTrack[ LevelNum ];
		break;
	}

	SDL_LockMutex( lock );
	if( t >= 0 )
		want_track = t;
	want_paused = render_info.minimized ||
		( MyGameStatus == STATUS_SinglePlayer && CurrentMenu && CurrentMenu != &MENU_NEW_InGameSound );
	SDL_UnlockMutex( lock );
}

#endif // MUSIC
