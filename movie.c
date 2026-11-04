/*
  Ogg Theora intro movie, played before the title when data/movies/intro.ogv
  is there. Any key, mouse or joystick button skips it.

  Ported from Forsaken VR (GameOrDie007/Forsaken-VR) source/movie.c,
  Copyright (C) 2026 Game Or Die, GPL version 2 or later.
*/

#ifdef MOVIE_THEORA

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ogg/ogg.h>
#include <theora/theoradec.h>
#include <vorbis/codec.h>
#include <AL/al.h>
#include <AL/alc.h>

#include "main.h"
#include "render.h"
#include "render_gl_shared.h"
#include "util.h"

extern RENDEROBJECT RenderBufs[4];
extern bool QuitRequested;

#define MOVIE_AL_BUFFERS 32
#define MOVIE_READ_CHUNK 8192
#define MOVIE_PCM_FRAMES 4096

typedef struct
{
	FILE *           f;
	ogg_sync_state   sync;

	bool             have_theora, have_vorbis;
	ogg_stream_state t_stream, v_stream;
	th_info          t_info;
	th_comment       t_comment;
	th_setup_info *  t_setup;
	th_dec_ctx *     t_dec;

	vorbis_info      v_info;
	vorbis_comment   v_comment;
	vorbis_dsp_state v_dsp;
	vorbis_block     v_block;
	bool             v_ready;

	ALuint           source;
	ALuint           albuf[ MOVIE_AL_BUFFERS ];
	ALuint           freebuf[ MOVIE_AL_BUFFERS ];
	int              free_count;
	ALenum           alfmt;
	ALint            al_queued, al_state;
	short            pcm[ MOVIE_PCM_FRAMES * 2 ];
	double           samples_done;

	GLuint           tex;
	int              w, h;
	unsigned char *  rgb;
} movie_t;

static int movie_read( movie_t * m )
{
	char * buf = ogg_sync_buffer( &m->sync, MOVIE_READ_CHUNK );
	size_t n;

	if ( !buf )
		return 0;
	n = fread( buf, 1, MOVIE_READ_CHUNK, m->f );
	ogg_sync_wrote( &m->sync, (long) n );
	return (int) n;
}

static void movie_page( movie_t * m, ogg_page * pg )
{
	if ( m->have_theora && ogg_page_serialno( pg ) == m->t_stream.serialno )
		ogg_stream_pagein( &m->t_stream, pg );
	else if ( m->have_vorbis && ogg_page_serialno( pg ) == m->v_stream.serialno )
		ogg_stream_pagein( &m->v_stream, pg );
}

#define CLAMP8( v ) ( (v) < 0 ? 0 : ( (v) > 255 ? 255 : (v) ) )

static void movie_yuv_to_rgb( movie_t * m, th_ycbcr_buffer yuv )
{
	int cx = ( m->t_info.pixel_fmt == TH_PF_444 ) ? 0 : 1;
	int cy = ( m->t_info.pixel_fmt == TH_PF_420 ) ? 1 : 0;
	int x, y;

	for ( y = 0; y < m->h; y++ )
	{
		unsigned char * dst = m->rgb + (size_t) y * m->w * 4;
		int sy = y + m->t_info.pic_y;
		const unsigned char * yr = yuv[0].data + (size_t) sy * yuv[0].stride;
		const unsigned char * ur = yuv[1].data + (size_t) ( sy >> cy ) * yuv[1].stride;
		const unsigned char * vr = yuv[2].data + (size_t) ( sy >> cy ) * yuv[2].stride;

		for ( x = 0; x < m->w; x++ )
		{
			int sx = x + m->t_info.pic_x;
			int Y = ( yr[sx] - 16 ) * 298;
			int U = ur[sx >> cx] - 128;
			int V = vr[sx >> cx] - 128;
			int r = ( Y + 409 * V + 128 ) >> 8;
			int g = ( Y - 100 * U - 208 * V + 128 ) >> 8;
			int b = ( Y + 516 * U + 128 ) >> 8;

			*dst++ = (unsigned char) CLAMP8( r );
			*dst++ = (unsigned char) CLAMP8( g );
			*dst++ = (unsigned char) CLAMP8( b );
			*dst++ = 255;
		}
	}
}

// letterboxed, through the same 2D path as the HUD
static void movie_draw( movie_t * m )
{
	RENDEROBJECT * ro = &RenderBufs[3];
	texture_t tex;
	TLVERTEX * v;
	WORD * idx;
	float sw = (float) render_info.ThisMode.w;
	float sh = (float) render_info.ThisMode.h;
	float src = (float) m->w / (float) m->h;
	float x0 = 0.0f, y0 = 0.0f, x1 = sw, y1 = sh;
	int i;

	if ( src > sw / sh )
	{
		y0 = ( sh - sw / src ) * 0.5f;
		y1 = sh - y0;
	}
	else
	{
		x0 = ( sw - sh * src ) * 0.5f;
		x1 = sw - x0;
	}

	if ( !FSLockPretransformedVertexBuffer( ro, &v ) )
		return;
	if ( !FSLockIndexBuffer( ro, &idx ) )
	{
		FSUnlockVertexBuffer( ro );
		return;
	}
	for ( i = 0; i < 4; i++ )
	{
		v[i].x = ( i == 1 || i == 2 ) ? x1 : x0;
		v[i].y = ( i >= 2 ) ? y1 : y0;
		v[i].z = 0.0f;
		v[i].rhw = 1.0f;
		v[i].color = 0xFFFFFFFF;
		v[i].tu = ( i == 1 || i == 2 ) ? 1.0f : 0.0f;
		v[i].tv = ( i >= 2 ) ? 1.0f : 0.0f;
	}
	idx[0] = 0; idx[1] = 1; idx[2] = 2;
	idx[3] = 0; idx[4] = 2; idx[5] = 3;
	FSUnlockVertexBuffer( ro );
	FSUnlockIndexBuffer( ro );

	tex.id = m->tex;
	ro->numTextureGroups = 1;
	ro->textureGroups[0].startVert = 0;
	ro->textureGroups[0].startIndex = 0;
	ro->textureGroups[0].numVerts = 4;
	ro->textureGroups[0].numTriangles = 2;
	ro->textureGroups[0].colourkey = false;
	ro->textureGroups[0].texture = (LPTEXTURE) &tex;

	FSClearBlack();
	disable_zbuff();
	draw_2d_object( ro );
	reset_zbuff();
	ro->numTextureGroups = 0;
}

// reclaim the buffers the device has played and queue more sound
static void movie_queue_audio( movie_t * m )
{
	ALint processed = 0;
	float ** pcm;
	int filled = 0;

	alGetSourcei( m->source, AL_BUFFERS_PROCESSED, &processed );
	while ( processed-- > 0 && m->free_count < MOVIE_AL_BUFFERS )
	{
		ALuint b = 0;
		ALint size = 0;

		alSourceUnqueueBuffers( m->source, 1, &b );
		alGetBufferi( b, AL_SIZE, &size );
		m->samples_done += (double) size / ( 2 * ( m->alfmt == AL_FORMAT_STEREO16 ? 2 : 1 ) );
		m->freebuf[ m->free_count++ ] = b;
	}

	while ( m->free_count > 0 )
	{
		int samples = vorbis_synthesis_pcmout( &m->v_dsp, &pcm );
		int chans = m->alfmt == AL_FORMAT_STEREO16 ? 2 : 1;
		int n, i, c;
		ALuint b;

		if ( samples <= 0 )
			break;
		n = samples > MOVIE_PCM_FRAMES ? MOVIE_PCM_FRAMES : samples;
		for ( i = 0; i < n; i++ )
			for ( c = 0; c < chans; c++ )
			{
				int s = (int) ( pcm[c][i] * 32767.0f );
				m->pcm[ i * chans + c ] = (short) ( s > 32767 ? 32767 : s < -32768 ? -32768 : s );
			}
		b = m->freebuf[ --m->free_count ];
		alBufferData( b, m->alfmt, m->pcm, n * chans * (int) sizeof( short ), m->v_info.rate );
		alSourceQueueBuffers( m->source, 1, &b );
		vorbis_synthesis_read( &m->v_dsp, n );
		filled++;
	}

	// playing a stopped source rewinds its queue, so only with fresh sound
	alGetSourcei( m->source, AL_BUFFERS_QUEUED, &m->al_queued );
	alGetSourcei( m->source, AL_SOURCE_STATE, &m->al_state );
	if ( filled && m->al_state != AL_PLAYING )
		alSourcePlay( m->source );
}

// one packet at a time: vorbis refuses blocks while its own buffer is full
static void movie_feed_audio( movie_t * m )
{
	ogg_packet pkt;

	if ( !m->v_ready )
		return;
	for (;;)
	{
		movie_queue_audio( m );
		if ( !m->free_count || ogg_stream_packetout( &m->v_stream, &pkt ) <= 0 )
			break;
		if ( vorbis_synthesis( &m->v_block, &pkt ) == 0 )
			vorbis_synthesis_blockin( &m->v_dsp, &m->v_block );
	}
}

// seconds of sound the device has played: the clock the pictures follow
static double movie_audio_clock( movie_t * m )
{
	ALint offset = 0;

	alGetSourcei( m->source, AL_SAMPLE_OFFSET, &offset );
	return ( m->samples_done + offset ) / m->v_info.rate;
}

static void movie_cleanup( movie_t * m )
{
	if ( m->source )
	{
		alSourceStop( m->source );
		alSourcei( m->source, AL_BUFFER, 0 );
		alDeleteSources( 1, &m->source );
		alDeleteBuffers( MOVIE_AL_BUFFERS, m->albuf );
	}
	if ( m->tex )
		glDeleteTextures( 1, &m->tex );
	free( m->rgb );
	if ( m->t_dec )
		th_decode_free( m->t_dec );
	th_setup_free( m->t_setup );
	th_comment_clear( &m->t_comment );
	th_info_clear( &m->t_info );
	if ( m->v_ready )
	{
		vorbis_block_clear( &m->v_block );
		vorbis_dsp_clear( &m->v_dsp );
	}
	vorbis_comment_clear( &m->v_comment );
	vorbis_info_clear( &m->v_info );
	if ( m->have_theora )
		ogg_stream_clear( &m->t_stream );
	if ( m->have_vorbis )
		ogg_stream_clear( &m->v_stream );
	ogg_sync_clear( &m->sync );
	fclose( m->f );
}

static bool movie_skipped( void )
{
	SDL_Event e;
	bool skip = false;

	while ( SDL_PollEvent( &e ) )
	{
		if ( e.type == SDL_QUIT )
			QuitRequested = true;
		if ( e.type == SDL_QUIT || e.type == SDL_KEYDOWN ||
		     e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_JOYBUTTONDOWN )
			skip = true;
	}
	return skip;
}

// read until both streams have their three headers
static bool movie_headers( movie_t * m )
{
	ogg_page pg;
	ogg_packet pkt;
	int t_headers = 0, v_headers = 0;

	while ( !feof( m->f ) && ( t_headers < 3 || ( m->have_vorbis && v_headers < 3 ) ) )
	{
		if ( !movie_read( m ) )
			break;
		while ( ogg_sync_pageout( &m->sync, &pg ) > 0 )
		{
			ogg_stream_state test;

			if ( !ogg_page_bos( &pg ) )
			{
				movie_page( m, &pg );
				continue;
			}
			ogg_stream_init( &test, ogg_page_serialno( &pg ) );
			ogg_stream_pagein( &test, &pg );
			if ( ogg_stream_packetpeek( &test, &pkt ) == 1 && !m->have_theora &&
			     th_decode_headerin( &m->t_info, &m->t_comment, &m->t_setup, &pkt ) > 0 )
			{
				m->t_stream = test;
				m->have_theora = true;
				ogg_stream_packetout( &m->t_stream, NULL );
				t_headers = 1;
			}
			else if ( ogg_stream_packetpeek( &test, &pkt ) == 1 && !m->have_vorbis &&
			          vorbis_synthesis_headerin( &m->v_info, &m->v_comment, &pkt ) == 0 )
			{
				m->v_stream = test;
				m->have_vorbis = true;
				ogg_stream_packetout( &m->v_stream, NULL );
				v_headers = 1;
			}
			else
				ogg_stream_clear( &test );
		}
		while ( m->have_theora && t_headers < 3 && ogg_stream_packetout( &m->t_stream, &pkt ) > 0 )
		{
			if ( th_decode_headerin( &m->t_info, &m->t_comment, &m->t_setup, &pkt ) <= 0 )
				return false;
			t_headers++;
		}
		while ( m->have_vorbis && v_headers < 3 && ogg_stream_packetout( &m->v_stream, &pkt ) > 0 )
		{
			if ( vorbis_synthesis_headerin( &m->v_info, &m->v_comment, &pkt ) != 0 )
				return false;
			v_headers++;
		}
	}
	return m->have_theora && t_headers == 3;
}

bool movie_play( const char * path )
{
	movie_t m;
	ogg_page pg;
	ogg_packet pkt;
	ogg_int64_t gp;
	bool have_frame = false, at_eof = false, audio_done = false;
	double frame_time = 0.0, clock, start, last_progress, audio_end_clock = 0.0, audio_end_wall = 0.0;
	int frames = 0, i, reads;

	memset( &m, 0, sizeof( m ) );
	m.f = fopen( convert_path( (char *) path ), "rb" );
	if ( !m.f )
		return false;

	ogg_sync_init( &m.sync );
	th_info_init( &m.t_info );
	th_comment_init( &m.t_comment );
	vorbis_info_init( &m.v_info );
	vorbis_comment_init( &m.v_comment );

	if ( !movie_headers( &m ) || !( m.t_dec = th_decode_alloc( &m.t_info, m.t_setup ) ) ||
	     !( m.rgb = (unsigned char *) malloc( (size_t) m.t_info.pic_width * m.t_info.pic_height * 4 ) ) )
	{
		DebugPrintf( "movie: cannot play %s\n", path );
		movie_cleanup( &m );
		return false;
	}
	m.w = m.t_info.pic_width;
	m.h = m.t_info.pic_height;
	DebugPrintf( "movie: %s %dx%d %.2f fps, sound %d\n", path, m.w, m.h,
		(double) m.t_info.fps_numerator / m.t_info.fps_denominator, (int) m.have_vorbis );

	if ( m.have_vorbis && alcGetCurrentContext() && vorbis_synthesis_init( &m.v_dsp, &m.v_info ) == 0 )
	{
		vorbis_block_init( &m.v_dsp, &m.v_block );
		m.v_ready = true;
		m.alfmt = m.v_info.channels >= 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
		alGenSources( 1, &m.source );
		alGenBuffers( MOVIE_AL_BUFFERS, m.albuf );
		alSourcei( m.source, AL_SOURCE_RELATIVE, AL_TRUE );
		for ( i = 0; i < MOVIE_AL_BUFFERS; i++ )
			m.freebuf[i] = m.albuf[i];
		m.free_count = MOVIE_AL_BUFFERS;
	}

	glGenTextures( 1, &m.tex );
	glBindTexture( GL_TEXTURE_2D, m.tex );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, m.w, m.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );

	start = last_progress = SDL_GetTicks() / 1000.0;

	// one frame is decoded and held until the clock reaches its time
	while ( !movie_skipped() )
	{
		for ( reads = 0; reads < 64; reads++ )
		{
			bool need_data = !have_frame || ( m.v_ready && m.free_count > 0 );

			if ( need_data && !at_eof )
			{
				if ( !movie_read( &m ) && feof( m.f ) )
					at_eof = true;
				while ( ogg_sync_pageout( &m.sync, &pg ) > 0 )
					movie_page( &m, &pg );
			}
			movie_feed_audio( &m );
			while ( !have_frame && ogg_stream_packetout( &m.t_stream, &pkt ) > 0 )
			{
				if ( th_decode_packetin( m.t_dec, &pkt, &gp ) == 0 )
				{
					frame_time = th_granule_time( m.t_dec, gp );
					have_frame = true;
				}
			}
			if ( !need_data || at_eof )
				break;
		}

		// the sound's clock stops with the sound: the wall clock takes over
		if ( at_eof && m.v_ready && !audio_done && ( !m.al_queued || m.al_state == AL_STOPPED ) )
		{
			audio_done = true;
			audio_end_clock = movie_audio_clock( &m );
			audio_end_wall = SDL_GetTicks() / 1000.0;
		}
		if ( at_eof && !have_frame && ( !m.v_ready || audio_done ) )
			break;

		// a broken file must not hang the game
		if ( SDL_GetTicks() / 1000.0 - last_progress > 5.0 )
			break;

		if ( audio_done )
			clock = audio_end_clock + SDL_GetTicks() / 1000.0 - audio_end_wall;
		else if ( m.v_ready )
			clock = movie_audio_clock( &m );
		else
			clock = SDL_GetTicks() / 1000.0 - start;

		if ( have_frame && frame_time <= clock )
		{
			th_ycbcr_buffer yuv;

			have_frame = false;
			if ( th_decode_ycbcr_out( m.t_dec, yuv ) == 0 )
			{
				movie_yuv_to_rgb( &m, yuv );
				glBindTexture( GL_TEXTURE_2D, m.tex );
				glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, m.w, m.h, GL_RGBA, GL_UNSIGNED_BYTE, m.rgb );
				movie_draw( &m );
				render_flip( &render_info );
				frames++;
				last_progress = SDL_GetTicks() / 1000.0;
			}
		}
		else
			SDL_Delay( 1 );
	}

	DebugPrintf( "movie: %d frames shown\n", frames );
	movie_cleanup( &m );
	return frames > 0;
}

#endif // MOVIE_THEORA
