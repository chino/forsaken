/*
 * music.c - streamed soundtrack playback
 *
 * Forsaken played its soundtrack from redbook CD audio, which the port never
 * reimplemented.  This module plays the soundtrack from files instead:
 * Ogg Vorbis (libvorbisfile), FLAC (libFLAC) and PCM WAV (built in), each
 * decoder compiled in when its library is available (see the Makefile).
 *
 * Based on ideas from the music-support branch (Ogg streaming over OpenAL,
 * per-level track list under data/sound/music) and from Forsaken VR's
 * music.c / playlist.txt (GameOrDie007/Forsaken-VR: playlist keyed by level
 * short name, "*" fallback pool, explicit free buffer list, fades).
 *
 * Shape:
 *
 *   main thread    music_update() decides what should play from the game
 *                  state, resolves it to a list of files and posts that to
 *                  the shared state, together with volume and pause.
 *
 *   music thread   music_pump() owns the OpenAL source, the decoder and the
 *                  buffer queue: it fades between selections, decodes into
 *                  free buffers, loops single tracks and advances through
 *                  multi-track selections.
 *
 * If the thread cannot be created music_pump() runs from music_update()
 * instead.  Nothing in here may crash or block the game: every failure is
 * logged once and leaves music silent.
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
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

// The decoders run on the music thread: use libc's allocator directly, the
// xmem.h tracking wrappers keep unlocked global tables (and reject realloc(NULL)).
#undef malloc
#undef realloc
#undef free

#ifdef MUSIC_OGG
// we supply our own callbacks; the static ones in the header trip -Wunused
#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>
#endif

#ifdef MUSIC_FLAC
#include <FLAC/stream_decoder.h>
#endif

// game state we react to
extern BYTE MyGameStatus;
extern int16_t LevelNum;
extern render_info_t render_info;
extern MENU MENU_NEW_InGameSound;

bool music_disabled = false;

#define MUSIC_DIR        "data/sound/music/"
#define MUSIC_PLAYLIST   MUSIC_DIR "playlist.txt"

#define MUSIC_PATH_LEN   512
#define MUSIC_KEY_LEN    64
#define MUSIC_NAME_LEN   256   // file names in the playlist / folder
#define MUSIC_MAX_FILES  256   // audio files listed from MUSIC_DIR
#define MUSIC_MAX_ENTRIES 256  // playlist lines
#define MUSIC_MAX_SEL    16    // files in one selection (played in sequence)

// 6 x 8192 frames is ~1.1s at 44.1kHz; the thread tops it up every 10ms.
// Without a thread we can only refill once per frame, so keep more queued.
#define MUSIC_BUFFERS_THREAD 6
#define MUSIC_BUFFERS_MAX 12
#define MUSIC_BUFFER_FRAMES 8192
#define MUSIC_THREAD_SLEEP_MS 10

#define MUSIC_FADE_MS    1000

/*
 * ---------------------------------------------------------------------------
 *  logging (DebugPrintf uses a static buffer, so the thread queues lines)
 * ---------------------------------------------------------------------------
 */

static SDL_mutex * lock = NULL;
static SDL_Thread * thread = NULL;

static char log_queue[4096];
static size_t log_len = 0;
static bool log_queued = false; // true while the music thread may be logging

static void music_lock( void )   { if( lock ) SDL_LockMutex( lock ); }
static void music_unlock( void ) { if( lock ) SDL_UnlockMutex( lock ); }

static void mlog( const char * format, ... )
{
	char line[MUSIC_PATH_LEN + 128];
	size_t n;
	va_list args;

	va_start( args, format );
	vsnprintf( line, sizeof(line), format, args );
	va_end( args );
	line[sizeof(line)-1] = 0;

	if( !log_queued )
	{
		DebugPrintf( "music: %s\n", line );
		return;
	}

	n = strlen( line );
	music_lock();
	if( log_len + n + 2 < sizeof(log_queue) )
	{
		memcpy( log_queue + log_len, line, n );
		log_len += n;
		log_queue[log_len++] = '\n';
		log_queue[log_len] = 0;
	}
	music_unlock();
}

static void flush_log( void )
{
	char copy[sizeof(log_queue)];
	char * line, * next;

	music_lock();
	memcpy( copy, log_queue, log_len + 1 );
	log_len = 0;
	log_queue[0] = 0;
	music_unlock();

	for( line = copy; *line; line = next )
	{
		next = strchr( line, '\n' );
		if( !next )
			break;
		*next++ = 0;
		DebugPrintf( "music: %s\n", line );
	}
}

/*
 * ---------------------------------------------------------------------------
 *  decoders: all produce interleaved signed 16 bit, 1 or 2 channels
 * ---------------------------------------------------------------------------
 */

enum { DEC_NONE, DEC_WAV, DEC_OGG, DEC_FLAC };

typedef struct {
	int     type;
	int     channels;
	long    rate;
	FILE *  fp;
	char    path[MUSIC_PATH_LEN];

	// wav
	int     wav_bytes;      // bytes per sample: 1, 2 or 3
	long    wav_start;      // offset of the data chunk
	long    wav_size;       // size of the data chunk
	long    wav_pos;        // bytes consumed from the data chunk

#ifdef MUSIC_OGG
	OggVorbis_File ogg;
	int     ogg_section;
#endif

#ifdef MUSIC_FLAC
	FLAC__StreamDecoder * flac;
	int16_t * flac_pcm;     // one decoded block
	unsigned  flac_cap;     // capacity in frames
	unsigned  flac_frames;  // frames in the block
	unsigned  flac_pos;     // frames already handed out
	long      flac_length;  // file size for the length callback
	bool      flac_bad;
#endif
} decoder_t;

static uint32_t le16( const unsigned char * b ) { return b[0] | ( b[1] << 8 ); }
static uint32_t le32( const unsigned char * b )
{
	return b[0] | ( b[1] << 8 ) | ( b[2] << 16 ) | ( (uint32_t) b[3] << 24 );
}

// --- wav ------------------------------------------------------------------

static bool wav_open( decoder_t * d )
{
	unsigned char hdr[12], chunk[8], fmt[40];
	uint32_t size, tag = 0, bits = 0;
	bool have_fmt = false;

	if( fread( hdr, 1, 12, d->fp ) != 12 ||
		memcmp( hdr, "RIFF", 4 ) || memcmp( hdr + 8, "WAVE", 4 ) )
		return false;

	for(;;)
	{
		if( fread( chunk, 1, 8, d->fp ) != 8 )
			return false;
		size = le32( chunk + 4 );

		if( !memcmp( chunk, "fmt ", 4 ) )
		{
			size_t n = size < sizeof(fmt) ? size : sizeof(fmt);
			if( n < 16 || fread( fmt, 1, n, d->fp ) != n )
				return false;
			tag         = le16( fmt );
			d->channels = (int) le16( fmt + 2 );
			d->rate     = (long) le32( fmt + 4 );
			bits        = le16( fmt + 14 );
			if( tag == 0xFFFE && n >= 26 ) // WAVE_FORMAT_EXTENSIBLE
				tag = le16( fmt + 24 );
			if( fseek( d->fp, (long)( size - n ) + ( size & 1 ), SEEK_CUR ) )
				return false;
			have_fmt = true;
		}
		else if( !memcmp( chunk, "data", 4 ) )
		{
			if( !have_fmt )
				return false;
			d->wav_start = ftell( d->fp );
			d->wav_size  = (long) ( size > 0x7fffffff ? 0x7fffffff : size );
			d->wav_pos   = 0;
			break;
		}
		else if( fseek( d->fp, (long) size + ( size & 1 ), SEEK_CUR ) )
			return false;
	}

	if( tag != 1 || ( bits != 8 && bits != 16 && bits != 24 ) )
	{
		mlog( "%s: only 8/16/24 bit PCM wav is supported", d->path );
		return false;
	}
	d->wav_bytes = (int) bits / 8;
	return true;
}

static int wav_read( decoder_t * d, int16_t * out, int frames )
{
	static unsigned char raw[MUSIC_BUFFER_FRAMES * 2 * 3]; // only the pump uses it
	long frame_bytes = (long) d->wav_bytes * d->channels;
	long want = (long) frames * frame_bytes;
	long left = d->wav_size - d->wav_pos;
	size_t got, i, samples;

	if( want > (long) sizeof(raw) )
		want = (long) sizeof(raw) - ( sizeof(raw) % frame_bytes );
	if( want > left )
		want = left - ( left % frame_bytes );
	if( want <= 0 )
		return 0;

	got = fread( raw, 1, (size_t) want, d->fp );
	got -= got % frame_bytes;
	d->wav_pos += (long) got;
	samples = got / d->wav_bytes;

	for( i = 0; i < samples; i++ )
	{
		const unsigned char * s = raw + i * d->wav_bytes;
		switch( d->wav_bytes )
		{
		case 1:  out[i] = (int16_t)( ( s[0] - 128 ) << 8 ); break;
		case 2:  out[i] = (int16_t) le16( s ); break;
		default: out[i] = (int16_t) le16( s + 1 ); break; // top 16 of 24 bits
		}
	}
	return (int)( samples / d->channels );
}

static bool wav_rewind( decoder_t * d )
{
	d->wav_pos = 0;
	return fseek( d->fp, d->wav_start, SEEK_SET ) == 0;
}

// --- ogg vorbis -----------------------------------------------------------

#ifdef MUSIC_OGG

static size_t ogg_cb_read( void * ptr, size_t size, size_t n, void * fp )
{
	return fread( ptr, size, n, (FILE*) fp );
}

static int ogg_cb_seek( void * fp, ogg_int64_t offset, int whence )
{
	return fseek( (FILE*) fp, (long) offset, whence );
}

static long ogg_cb_tell( void * fp )
{
	return ftell( (FILE*) fp );
}

static bool ogg_open( decoder_t * d )
{
	// close is left to us so the FILE is always closed by music_close()
	ov_callbacks cb = { ogg_cb_read, ogg_cb_seek, NULL, ogg_cb_tell };
	vorbis_info * vi;

	if( ov_open_callbacks( d->fp, &d->ogg, NULL, 0, cb ) != 0 )
		return false;
	d->type = DEC_OGG; // from here on ov_clear() is needed

	vi = ov_info( &d->ogg, -1 );
	if( !vi )
		return false;
	d->channels    = vi->channels;
	d->rate        = vi->rate;
	d->ogg_section = -1;
	return true;
}

static int ogg_read( decoder_t * d, int16_t * out, int frames )
{
	const int big_endian = ( SDL_BYTEORDER == SDL_BIG_ENDIAN );
	int bytes = frames * d->channels * 2;
	int done = 0, holes = 0;

	while( done < bytes )
	{
		int section = 0;
		long n = ov_read( &d->ogg, (char*) out + done, bytes - done,
		                  big_endian, 2, 1, &section );
		if( n == OV_HOLE && ++holes < 8 )
			continue;
		if( n < 0 )
			return done ? done / ( d->channels * 2 ) : -1;
		if( n == 0 )
			break;
		if( section != d->ogg_section )
		{
			// chained streams may change layout; we can't follow that
			vorbis_info * vi = ov_info( &d->ogg, section );
			if( d->ogg_section >= 0 && ( !vi || vi->channels != d->channels || vi->rate != d->rate ) )
				break;
			d->ogg_section = section;
		}
		done += (int) n;
	}
	return done / ( d->channels * 2 );
}

static bool ogg_rewind( decoder_t * d )
{
	d->ogg_section = -1;
	return ov_raw_seek( &d->ogg, 0 ) == 0;
}

#endif // MUSIC_OGG

// --- flac -----------------------------------------------------------------

#ifdef MUSIC_FLAC

static FLAC__StreamDecoderReadStatus flac_cb_read( const FLAC__StreamDecoder * dec,
	FLAC__byte buffer[], size_t * bytes, void * user )
{
	decoder_t * d = user;
	(void) dec;
	*bytes = fread( buffer, 1, *bytes, d->fp );
	if( *bytes )
		return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
	return feof( d->fp ) ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM
	                     : FLAC__STREAM_DECODER_READ_STATUS_ABORT;
}

static FLAC__StreamDecoderSeekStatus flac_cb_seek( const FLAC__StreamDecoder * dec,
	FLAC__uint64 offset, void * user )
{
	decoder_t * d = user;
	(void) dec;
	return fseek( d->fp, (long) offset, SEEK_SET ) ? FLAC__STREAM_DECODER_SEEK_STATUS_ERROR
	                                               : FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

static FLAC__StreamDecoderTellStatus flac_cb_tell( const FLAC__StreamDecoder * dec,
	FLAC__uint64 * offset, void * user )
{
	decoder_t * d = user;
	long pos = ftell( d->fp );
	(void) dec;
	if( pos < 0 )
		return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
	*offset = (FLAC__uint64) pos;
	return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus flac_cb_length( const FLAC__StreamDecoder * dec,
	FLAC__uint64 * length, void * user )
{
	decoder_t * d = user;
	(void) dec;
	*length = (FLAC__uint64) d->flac_length;
	return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool flac_cb_eof( const FLAC__StreamDecoder * dec, void * user )
{
	decoder_t * d = user;
	(void) dec;
	return feof( d->fp ) ? true : false;
}

static FLAC__StreamDecoderWriteStatus flac_cb_write( const FLAC__StreamDecoder * dec,
	const FLAC__Frame * frame, const FLAC__int32 * const buffer[], void * user )
{
	decoder_t * d = user;
	unsigned i, c, n = frame->header.blocksize;
	unsigned bps = frame->header.bits_per_sample;
	(void) dec;

	if( (int) frame->header.channels != d->channels || bps < 4 || bps > 32 )
	{
		d->flac_bad = true;
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}
	if( n > d->flac_cap )
	{
		int16_t * p = realloc( d->flac_pcm, n * d->channels * sizeof(int16_t) );
		if( !p )
		{
			d->flac_bad = true;
			return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
		}
		d->flac_pcm = p;
		d->flac_cap = n;
	}
	for( i = 0; i < n; i++ )
		for( c = 0; c < (unsigned) d->channels; c++ )
		{
			FLAC__int32 s = buffer[c][i];
			d->flac_pcm[ i * d->channels + c ] = (int16_t)
				( bps >= 16 ? ( s >> ( bps - 16 ) ) : ( s << ( 16 - bps ) ) );
		}
	d->flac_frames = n;
	d->flac_pos = 0;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void flac_cb_metadata( const FLAC__StreamDecoder * dec,
	const FLAC__StreamMetadata * meta, void * user )
{
	decoder_t * d = user;
	(void) dec;
	if( meta->type == FLAC__METADATA_TYPE_STREAMINFO )
	{
		d->channels = (int) meta->data.stream_info.channels;
		d->rate     = (long) meta->data.stream_info.sample_rate;
	}
}

static void flac_cb_error( const FLAC__StreamDecoder * dec,
	FLAC__StreamDecoderErrorStatus status, void * user )
{
	// lost sync / bad crc are recoverable: the decoder resyncs by itself
	(void) dec; (void) status; (void) user;
}

static bool flac_open( decoder_t * d )
{
	if( fseek( d->fp, 0, SEEK_END ) )
		return false;
	d->flac_length = ftell( d->fp );
	rewind( d->fp );

	d->flac = FLAC__stream_decoder_new();
	if( !d->flac )
		return false;
	d->type = DEC_FLAC; // from here on music_close() must delete it

	if( FLAC__stream_decoder_init_stream( d->flac,
			flac_cb_read, flac_cb_seek, flac_cb_tell, flac_cb_length, flac_cb_eof,
			flac_cb_write, flac_cb_metadata, flac_cb_error, d )
		!= FLAC__STREAM_DECODER_INIT_STATUS_OK )
		return false;

	if( !FLAC__stream_decoder_process_until_end_of_metadata( d->flac ) )
		return false;

	return d->channels > 0 && d->rate > 0;
}

static int flac_read( decoder_t * d, int16_t * out, int frames )
{
	int done = 0, idle = 0;

	while( done < frames )
	{
		if( d->flac_pos < d->flac_frames )
		{
			unsigned n = d->flac_frames - d->flac_pos;
			if( n > (unsigned)( frames - done ) )
				n = (unsigned)( frames - done );
			memcpy( out + done * d->channels,
			        d->flac_pcm + d->flac_pos * d->channels,
			        n * d->channels * sizeof(int16_t) );
			d->flac_pos += n;
			done += (int) n;
			idle = 0;
			continue;
		}
		if( FLAC__stream_decoder_get_state( d->flac ) == FLAC__STREAM_DECODER_END_OF_STREAM )
			break;
		if( !FLAC__stream_decoder_process_single( d->flac ) || d->flac_bad || ++idle > 64 )
			return done ? done : -1;
	}
	return done;
}

static bool flac_rewind( decoder_t * d )
{
	d->flac_frames = d->flac_pos = 0;
	// reset() seeks back through our seek callback and re-reads metadata
	return FLAC__stream_decoder_reset( d->flac ) ? true : false;
}

#endif // MUSIC_FLAC

// --- generic --------------------------------------------------------------

static void music_close( decoder_t * d )
{
#ifdef MUSIC_OGG
	if( d->type == DEC_OGG )
		ov_clear( &d->ogg );
#endif
#ifdef MUSIC_FLAC
	if( d->flac )
	{
		FLAC__stream_decoder_finish( d->flac );
		FLAC__stream_decoder_delete( d->flac );
	}
	free( d->flac_pcm );
#endif
	if( d->fp )
		fclose( d->fp );
	memset( d, 0, sizeof(*d) );
}

static bool music_open( decoder_t * d, const char * path )
{
	unsigned char magic[4] = { 0, 0, 0, 0 };
	bool ok = false;

	memset( d, 0, sizeof(*d) );
	strncpy( d->path, path, sizeof(d->path) - 1 );

	d->fp = fopen( path, "rb" );
	if( !d->fp )
	{
		mlog( "cannot open %s", path );
		return false;
	}

	// pick the decoder by content, not by extension
	if( fread( magic, 1, 4, d->fp ) != 4 )
		magic[0] = 0;
	rewind( d->fp );

	if( !memcmp( magic, "RIFF", 4 ) )
	{
		d->type = DEC_WAV;
		ok = wav_open( d );
	}
	else if( !memcmp( magic, "OggS", 4 ) )
	{
#ifdef MUSIC_OGG
		ok = ogg_open( d );
#else
		mlog( "%s: built without Ogg Vorbis support", path );
#endif
	}
	else if( !memcmp( magic, "fLaC", 4 ) || !memcmp( magic, "ID3", 3 ) )
	{
#ifdef MUSIC_FLAC
		ok = flac_open( d );
#else
		mlog( "%s: built without FLAC support", path );
#endif
	}
	else
		mlog( "%s: unknown file format", path );

	if( ok && ( d->channels < 1 || d->channels > 2 || d->rate <= 0 ) )
	{
		mlog( "%s: unsupported layout (%d channels, %ld Hz)", path, d->channels, d->rate );
		ok = false;
	}
	if( !ok )
	{
		mlog( "%s: could not decode", path );
		music_close( d );
	}
	return ok;
}

// frames read, 0 at the end of the stream, -1 on error
static int music_read( decoder_t * d, int16_t * out, int frames )
{
	switch( d->type )
	{
	case DEC_WAV:  return wav_read( d, out, frames );
#ifdef MUSIC_OGG
	case DEC_OGG:  return ogg_read( d, out, frames );
#endif
#ifdef MUSIC_FLAC
	case DEC_FLAC: return flac_read( d, out, frames );
#endif
	}
	return -1;
}

static bool music_rewind( decoder_t * d )
{
	switch( d->type )
	{
	case DEC_WAV:  return wav_rewind( d );
#ifdef MUSIC_OGG
	case DEC_OGG:  return ogg_rewind( d );
#endif
#ifdef MUSIC_FLAC
	case DEC_FLAC: return flac_rewind( d );
#endif
	}
	return false;
}

/*
 * ---------------------------------------------------------------------------
 *  shared state (main thread writes, music thread reads)
 * ---------------------------------------------------------------------------
 */

static struct {
	char     files[MUSIC_MAX_SEL][MUSIC_PATH_LEN];
	int      count;
	unsigned serial;  // bumped for every new selection
	float    volume;
	bool     paused;
	bool     quit;
} shared = { .volume = 0.7f };

/*
 * ---------------------------------------------------------------------------
 *  streaming (music thread, or main thread when there is no thread)
 * ---------------------------------------------------------------------------
 */

static ALuint source = 0;
static ALuint buffers[MUSIC_BUFFERS_MAX];
static int    num_buffers = MUSIC_BUFFERS_THREAD;
static ALuint free_list[MUSIC_BUFFERS_MAX];
static int    free_count = 0;

static decoder_t dec;
static bool   dec_open = false;

static char   sel[MUSIC_MAX_SEL][MUSIC_PATH_LEN];
static bool   sel_bad[MUSIC_MAX_SEL];  // failed to open: don't retry (or re-log) every lap
static int    sel_count = 0;
static int    sel_index = 0;
static unsigned sel_serial = 0;
static bool   sel_pending = false;

static int    queued_channels = 0;  // format of what is on the source queue
static long   queued_rate = 0;
static bool   draining = false;     // next track has another format: let the queue empty first

static float  fade = 0.0f;
static float  last_gain = -1.0f;
static Uint32 last_ticks = 0;
static bool   was_paused = false;

static int16_t pcm[MUSIC_BUFFER_FRAMES * 2];

static void stop_and_flush( void )
{
	int i;
	alSourceStop( source );
	alSourcei( source, AL_BUFFER, 0 ); // detaches every queued buffer
	alSourceRewind( source );          // back to AL_INITIAL: next play is a fresh start
	for( i = 0; i < num_buffers; i++ )
		free_list[i] = buffers[i];
	free_count = num_buffers;
	queued_channels = 0;
	queued_rate = 0;
	draining = false;
}

static void close_track( void )
{
	if( dec_open )
		music_close( &dec );
	dec_open = false;
}

static const char * base_name( const char * path )
{
	const char * s = strrchr( path, '/' );
	return s ? s + 1 : path;
}

// opens sel[start], or the next one that works; false when none of them do
static bool open_from( int start )
{
	int i;
	close_track();
	for( i = 0; i < sel_count; i++ )
	{
		int idx = ( start + i ) % sel_count;
		if( sel_bad[idx] )
			continue;
		if( !music_open( &dec, sel[idx] ) )
			sel_bad[idx] = true;
		else
		{
			dec_open = true;
			sel_index = idx;
			mlog( "playing %s (%d ch, %ld Hz%s)", base_name( sel[idx] ), dec.channels, dec.rate,
				sel_count > 1 ? ", sequence" : ", looping" );
			return true;
		}
	}
	return false;
}

// end of the current track: loop it, or move on to the next one in the selection
static bool next_track( void )
{
	if( sel_count == 1 )
	{
		if( music_rewind( &dec ) )
		{
			mlog( "looping %s", base_name( sel[sel_index] ) );
			return true;
		}
		mlog( "could not rewind %s", base_name( sel[sel_index] ) );
		return false;
	}
	return open_from( sel_index + 1 );
}

// fills pcm[] with up to MUSIC_BUFFER_FRAMES frames of the current format
static int decode_block( void )
{
	int got = 0, empty = 0;
	int channels = dec.channels;
	long rate = dec.rate;

	while( got < MUSIC_BUFFER_FRAMES && dec_open )
	{
		int n = music_read( &dec, pcm + got * channels, MUSIC_BUFFER_FRAMES - got );
		if( n > 0 )
		{
			got += n;
			empty = 0;
			continue;
		}
		if( n < 0 )
			mlog( "decode error in %s", base_name( sel[sel_index] ) );

		// guard against files that open but never produce audio
		if( ++empty > sel_count + 1 || !next_track() )
		{
			mlog( "nothing playable left in the selection, stopping" );
			close_track();
			break;
		}
		if( dec.channels != channels || dec.rate != rate )
			break; // the rest goes in once the queue has drained
	}
	return got;
}

static void queue_buffers( void )
{
	ALint processed = 0, queued = 0;

	alGetSourcei( source, AL_BUFFERS_PROCESSED, &processed );
	while( processed-- > 0 && free_count < num_buffers )
	{
		ALuint b = 0;
		alSourceUnqueueBuffers( source, 1, &b );
		free_list[free_count++] = b;
	}

	alGetSourcei( source, AL_BUFFERS_QUEUED, &queued );
	if( draining )
	{
		if( queued > 0 )
			return;
		stop_and_flush();
	}

	while( free_count > 0 && dec_open )
	{
		ALuint b;
		int channels = dec.channels;
		long rate = dec.rate;
		int frames;

		if( queued_channels && ( queued_channels != channels || queued_rate != rate ) )
		{
			draining = true;
			return;
		}

		frames = decode_block();
		if( frames <= 0 )
			break;

		b = free_list[--free_count];
		alBufferData( b, channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16,
			pcm, (ALsizei)( frames * channels * sizeof(int16_t) ), (ALsizei) rate );
		alSourceQueueBuffers( source, 1, &b );
		queued_channels = channels;
		queued_rate = rate;
	}
}

static void music_pump( void )
{
	ALint state = 0, queued = 0;
	Uint32 now = SDL_GetTicks();
	float dt = ( now - last_ticks ) / (float) MUSIC_FADE_MS;
	float volume, gain, target;
	bool paused;

	last_ticks = now;
	if( dt < 0.0f ) dt = 0.0f;
	if( dt > 0.1f ) dt = 0.1f; // a long stall should not skip the fade

	music_lock();
	volume = shared.volume;
	paused = shared.paused;
	if( shared.serial != sel_serial )
	{
		memcpy( sel, shared.files, sizeof(sel) );
		sel_count = shared.count;
		sel_serial = shared.serial;
		memset( sel_bad, 0, sizeof(sel_bad) );
		sel_pending = true;
	}
	music_unlock();

	alGetSourcei( source, AL_SOURCE_STATE, &state );

	if( paused )
	{
		if( !was_paused && state == AL_PLAYING )
			alSourcePause( source );
		was_paused = true;
		return;
	}
	if( was_paused && state == AL_PAUSED )
		alSourcePlay( source );
	was_paused = false;

	// fade the old selection out before switching to the new one
	if( sel_pending && ( !dec_open || fade <= 0.0f ) )
	{
		sel_pending = false;
		stop_and_flush();
		close_track();
		fade = 0.0f;
		if( sel_count == 0 )
			mlog( "nothing to play here" );
		else
			open_from( 0 );
	}

	target = ( dec_open && !sel_pending ) ? 1.0f : 0.0f;
	if( fade < target ) { fade += dt; if( fade > target ) fade = target; }
	if( fade > target ) { fade -= dt; if( fade < target ) fade = target; }

	// perceptual-ish curve so the slider's lower half is still useful
	gain = volume * volume * fade;
	if( gain != last_gain )
	{
		alSourcef( source, AL_GAIN, gain );
		last_gain = gain;
	}

	queue_buffers();

	// first start, or the queue ran dry (stall) and openal stopped the source
	alGetSourcei( source, AL_SOURCE_STATE, &state );
	alGetSourcei( source, AL_BUFFERS_QUEUED, &queued );
	// (also while draining: the old format has to finish playing first)
	if( state != AL_PLAYING && queued > 0 )
	{
		if( state == AL_STOPPED )
			mlog( "stream underrun, restarting" );
		alSourcePlay( source );
	}
	else if( state != AL_PLAYING && queued == 0 && !dec_open && sel_count )
	{
		// everything failed or finished: forget it so we don't spin
		sel_count = 0;
	}
}

static int music_thread( void * unused )
{
	(void) unused;
	for(;;)
	{
		bool quit;
		music_lock();
		quit = shared.quit;
		music_unlock();
		if( quit )
			break;
		music_pump();
		SDL_Delay( MUSIC_THREAD_SLEEP_MS );
	}
	return 0;
}

/*
 * ---------------------------------------------------------------------------
 *  track lookup (main thread)
 * ---------------------------------------------------------------------------
 */

static char files[MUSIC_MAX_FILES][MUSIC_NAME_LEN]; // names in MUSIC_DIR
static int  num_files = 0;

typedef struct {
	char key[MUSIC_KEY_LEN];
	char file[MUSIC_NAME_LEN];
} entry_t;

static entry_t entries[MUSIC_MAX_ENTRIES];
static int     num_entries = 0;

static const char * exts[] = { ".ogg", ".oga", ".flac", ".fla", ".wav", NULL };

static bool has_audio_ext( const char * name )
{
	const char * dot = strrchr( name, '.' );
	int i;
	if( !dot )
		return false;
	for( i = 0; exts[i]; i++ )
		if( !strcasecmp( dot, exts[i] ) )
			return true;
	return false;
}

static int cmp_names( const void * a, const void * b )
{
	return strcasecmp( (const char*) a, (const char*) b );
}

static void scan_dir( void )
{
	DIR * dir = opendir( MUSIC_DIR );
	struct dirent * e;

	num_files = 0;
	if( !dir )
		return;
	while( ( e = readdir( dir ) ) && num_files < MUSIC_MAX_FILES )
	{
		if( e->d_name[0] == '.' || !has_audio_ext( e->d_name ) ||
			strlen( e->d_name ) >= MUSIC_NAME_LEN )
			continue;
		strcpy( files[num_files++], e->d_name );
	}
	closedir( dir );
	qsort( files, num_files, sizeof(files[0]), cmp_names );
}

static void trim( char * s )
{
	char * p = s;
	size_t n;
	while( *p == ' ' || *p == '\t' )
		p++;
	if( p != s )
		memmove( s, p, strlen( p ) + 1 );
	n = strlen( s );
	while( n && isspace( (unsigned char) s[n-1] ) )
		s[--n] = 0;
}

// "key   file name.ogg" - the file is the rest of the line, spaces allowed
static void read_playlist( void )
{
	char line[MUSIC_NAME_LEN + MUSIC_KEY_LEN];
	FILE * f = fopen( MUSIC_PLAYLIST, "r" );

	num_entries = 0;
	if( !f )
		return;
	while( fgets( line, sizeof(line), f ) )
	{
		char * sep;
		trim( line );
		if( !line[0] || line[0] == '#' || line[0] == ';' )
			continue;
		if( num_entries >= MUSIC_MAX_ENTRIES )
		{
			DebugPrintf( "music: playlist truncated at %d entries\n", num_entries );
			break;
		}
		for( sep = line; *sep && !isspace( (unsigned char) *sep ); sep++ )
			;
		if( !*sep )
		{
			DebugPrintf( "music: playlist line has no file: '%s'\n", line );
			continue;
		}
		*sep++ = 0;
		trim( sep );
		if( strlen( line ) >= MUSIC_KEY_LEN || strlen( sep ) >= MUSIC_NAME_LEN )
		{
			DebugPrintf( "music: playlist line too long: '%s'\n", line );
			continue;
		}
		strcpy( entries[num_entries].key,  line );
		strcpy( entries[num_entries].file, sep );
		num_entries++;
	}
	fclose( f );
}

// cd track number of a "track02.ogg" / "Track 2.flac" style name, else 0
static int track_number( const char * name )
{
	if( strncasecmp( name, "track", 5 ) )
		return 0;
	name += 5;
	while( *name == ' ' || *name == '_' || *name == '-' )
		name++;
	return isdigit( (unsigned char) *name ) ? atoi( name ) : 0;
}

/*
 * Finds a playlist file name in MUSIC_DIR: case-insensitively, with or
 * without extension, and a bare number N means cd track N (trackNN.*).
 * Names with a '/' are taken as relative paths below MUSIC_DIR.
 */
static bool find_file( const char * name, char * out )
{
	int i, j;
	char want[MUSIC_NAME_LEN + 8];
	struct stat st;

	if( strchr( name, '/' ) || strchr( name, '\\' ) )
	{
		snprintf( out, MUSIC_PATH_LEN, MUSIC_DIR "%.*s", MUSIC_NAME_LEN - 1, name );
		for( i = 0; out[i]; i++ )
			if( out[i] == '\\' )
				out[i] = '/';
		return stat( out, &st ) == 0;
	}

	if( name[0] && strspn( name, "0123456789" ) == strlen( name ) )
	{
		int n = atoi( name );
		for( i = 0; i < num_files; i++ )
			if( track_number( files[i] ) == n )
				break;
		if( i == num_files )
			return false;
		snprintf( out, MUSIC_PATH_LEN, MUSIC_DIR "%s", files[i] );
		return true;
	}

	for( j = -1; j == -1 || exts[j]; j++ )
	{
		snprintf( want, sizeof(want), "%.*s%s", MUSIC_NAME_LEN - 1, name, j < 0 ? "" : exts[j] );
		for( i = 0; i < num_files; i++ )
			if( !strcasecmp( files[i], want ) )
			{
				snprintf( out, MUSIC_PATH_LEN, MUSIC_DIR "%s", files[i] );
				return true;
			}
	}
	return false;
}

static int sort_by_track( const void * a, const void * b )
{
	return track_number( *(char * const *) a ) - track_number( *(char * const *) b );
}

/*
 * Resolves a key ("menu" or a level short name) to the files to play:
 *   1. playlist lines for the key (several lines play in sequence)
 *   2. a file named after the key, e.g. vol2.ogg / menu.flac
 *   3. the fallback pool: playlist "*" lines, else trackNN files in cd
 *      order, else every file in the folder - the menu gets the first,
 *      levels go round the rest by level number
 */
static int resolve( const char * key, int index, char out[][MUSIC_PATH_LEN] )
{
	char * pool[MUSIC_MAX_FILES];
	int i, n = 0, pool_n = 0;

	for( i = 0; i < num_entries && n < MUSIC_MAX_SEL; i++ )
		if( !strcasecmp( entries[i].key, key ) )
		{
			if( find_file( entries[i].file, out[n] ) )
				n++;
			else
				DebugPrintf( "music: playlist file for '%s' not found: %s\n", key, entries[i].file );
		}
	if( n )
		return n;

	if( find_file( key, out[0] ) )
		return 1;
	if( !strcasecmp( key, "menu" ) && find_file( "title", out[0] ) )
		return 1;

	for( i = 0; i < num_entries && pool_n < MUSIC_MAX_FILES; i++ )
		if( !strcmp( entries[i].key, "*" ) )
			pool[pool_n++] = entries[i].file;
	if( pool_n )
	{
		// pool entries are names, resolve the chosen one
		int pick = strcasecmp( key, "menu" ) ? index % pool_n : 0;
		for( i = 0; i < pool_n; i++ )
			if( find_file( pool[ ( pick + i ) % pool_n ], out[0] ) )
				return 1;
		return 0;
	}

	for( i = 0; i < num_files; i++ )
		if( track_number( files[i] ) > 0 )
			pool[pool_n++] = files[i];
	if( pool_n )
		qsort( pool, pool_n, sizeof(pool[0]), sort_by_track );
	else
		for( i = 0; i < num_files; i++ )
			pool[pool_n++] = files[i];
	if( !pool_n )
		return 0;

	if( !strcasecmp( key, "menu" ) || pool_n == 1 )
		i = 0;
	else
		i = 1 + index % ( pool_n - 1 );
	snprintf( out[0], MUSIC_PATH_LEN, MUSIC_DIR "%s", pool[i] );
	return 1;
}

/*
 * ---------------------------------------------------------------------------
 *  public api (main thread)
 * ---------------------------------------------------------------------------
 */

static bool ready = false;
static bool gave_up = false;
static bool logged_no_context = false;
static char current_key[MUSIC_KEY_LEN] = "";

static void music_init( void )
{
	ALenum error;
	int i;

	if( music_disabled )
	{
		DebugPrintf( "music: disabled by -nomusic\n" );
		gave_up = true;
		return;
	}

	// no context means -nosfx or no audio device: sound_init() never got far
	if( !alcGetCurrentContext() )
	{
		if( !logged_no_context )
			DebugPrintf( "music: no OpenAL context yet, music unavailable\n" );
		logged_no_context = true;
		return;
	}
	gave_up = true; // one attempt only from here on

	scan_dir();
	read_playlist();
	DebugPrintf( "music: %d audio files in %s, %d playlist entries, decoders: wav%s%s\n",
		num_files, MUSIC_DIR, num_entries,
#ifdef MUSIC_OGG
		" ogg",
#else
		"",
#endif
#ifdef MUSIC_FLAC
		" flac"
#else
		""
#endif
		);
	if( !num_files && !num_entries )
	{
		DebugPrintf( "music: no music files, see docs/music.md\n" );
		return;
	}

	alGetError();
	alGenSources( 1, &source );
	if( ( error = alGetError() ) != AL_NO_ERROR )
	{
		DebugPrintf( "music: alGenSources failed: %s\n", alGetString( error ) );
		source = 0;
		return;
	}

	lock = SDL_CreateMutex();
	num_buffers = lock ? MUSIC_BUFFERS_THREAD : MUSIC_BUFFERS_MAX;

	alGenBuffers( num_buffers, buffers );
	if( ( error = alGetError() ) != AL_NO_ERROR )
	{
		DebugPrintf( "music: alGenBuffers failed: %s\n", alGetString( error ) );
		alDeleteSources( 1, &source );
		source = 0;
		if( lock )
			SDL_DestroyMutex( lock );
		lock = NULL;
		return;
	}
	for( i = 0; i < num_buffers; i++ )
		free_list[i] = buffers[i];
	free_count = num_buffers;

	// never positioned in the world: always dead centre, no attenuation
	alSourcei ( source, AL_SOURCE_RELATIVE, AL_TRUE );
	alSource3f( source, AL_POSITION, 0.0f, 0.0f, 0.0f );
	alSource3f( source, AL_VELOCITY, 0.0f, 0.0f, 0.0f );
	alSourcef ( source, AL_ROLLOFF_FACTOR, 0.0f );
	alSourcei ( source, AL_LOOPING, AL_FALSE ); // looping is done by the decoder
	alSourcef ( source, AL_GAIN, 0.0f );

	last_ticks = SDL_GetTicks();
	ready = true;

	if( lock )
	{
		log_queued = true;
#if SDL_VERSION_ATLEAST(2,0,0)
		thread = SDL_CreateThread( music_thread, "music", NULL );
#else
		thread = SDL_CreateThread( music_thread, NULL );
#endif
		if( !thread )
			log_queued = false;
	}
	DebugPrintf( "music: ready, streaming %s\n",
		thread ? "from its own thread" : "from the main loop" );
}

void music_shutdown( void )
{
	if( !ready )
		return;
	ready = false;

	if( thread )
	{
		music_lock();
		shared.quit = true;
		music_unlock();
		SDL_WaitThread( thread, NULL );
		thread = NULL;
	}
	flush_log();
	log_queued = false;

	stop_and_flush();
	close_track();
	alDeleteSources( 1, &source );
	alDeleteBuffers( num_buffers, buffers );
	source = 0;

	if( lock )
		SDL_DestroyMutex( lock );
	lock = NULL;
	DebugPrintf( "music: shut down\n" );
}

void music_set_volume( float volume )
{
	if( volume < 0.0f ) volume = 0.0f;
	if( volume > 1.0f ) volume = 1.0f;
	music_lock();
	shared.volume = volume;
	music_unlock();
}

// key for the current game state, NULL while loading / in transition
static const char * wanted_key( int * index )
{
	*index = 0;
	switch( MyGameStatus )
	{
	case STATUS_Title:
	case STATUS_BetweenLevels:
	case STATUS_StartingMultiplayer:
	case STATUS_ViewingStats:
	case STATUS_WaitingToStartSinglePlayer:
	case STATUS_WaitingToStartMultiPlayerHost:
	case STATUS_WaitingToStartMultiPlayerClient:
	case STATUS_WaitingToStartTeamGame:
		return "menu";

	case STATUS_Normal:
	case STATUS_SinglePlayer:
	case STATUS_PlayingDemo:
		if( LevelNum < 0 || LevelNum >= MAXLEVELS || !ShortLevelNames[LevelNum][0] )
			return NULL;
		*index = LevelNum;
		return ShortLevelNames[LevelNum];
	}
	return NULL;
}

void music_update( void )
{
	const char * key;
	int index;
	bool paused;

	if( !ready )
	{
		if( !gave_up )
			music_init();
		if( !ready )
			return;
	}

	if( thread )
		flush_log();

	key = wanted_key( &index );
	if( key && strcasecmp( key, current_key ) )
	{
		char found[MUSIC_MAX_SEL][MUSIC_PATH_LEN];
		int n = resolve( key, index, found );

		snprintf( current_key, sizeof(current_key), "%s", key );
		DebugPrintf( "music: '%s' -> %s%s\n", key,
			n ? base_name( found[0] ) : "(nothing)", n > 1 ? " ..." : "" );

		music_lock();
		memcpy( shared.files, found, n * sizeof(found[0]) );
		shared.count = n;
		shared.serial++;
		music_unlock();
	}

	// the single player game freezes while a menu is up; keep playing in the
	// sound menu though, so the volume slider can be heard
	paused = render_info.minimized || render_info.bPaused ||
		( MyGameStatus == STATUS_SinglePlayer && CurrentMenu &&
		  CurrentMenu != &MENU_NEW_InGameSound );

	music_lock();
	shared.paused = paused;
	music_unlock();

	if( !thread )
		music_pump();
}

#endif // MUSIC
