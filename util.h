#ifndef UTIL_INCLUDED
#define UTIL_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

void strtoupper(char *str);

int Msg( const char * fmt, ... );
void ShowFatalError( void ); // pops up the last Msg() so the user knows why we quit

void DebugPrintf_( const char * format, ... ); // no prefix timestamp
void DebugPrintf( const char * format, ... );  // prefix timestamp
void DebugState( const char * str );           // only prints if str changed
void DebugPuts( char * str );                  // 

void Change_Ext( const char * Src, char * Dest, const char * Ext );
void Get_Ext( u_int8_t * Src, u_int8_t * Dest );
void GetFilename( u_int8_t * Src, u_int8_t * Dest );

char* convert_path( char* _str );
char * convert_char( char from, char to, char* in );

/*
 * Reading packed file data.
 *
 * The level and model files are loaded into a byte buffer and walked with
 * pointers of mixed types, so a float (or a struct holding floats) in them
 * frequently sits at an odd address.  Dereferencing such a pointer is
 * undefined behaviour: x86 does not care, but strict-alignment CPUs such as
 * 32bit ARM (VLDR/LDRD/LDM) raise SIGBUS.
 *
 * Go through memcpy instead.  The helpers take void pointers on
 * purpose so the compiler can not assume the natural alignment of the
 * original type; where unaligned loads are cheap this still compiles to a
 * plain load.
 */

static inline void copy_unaligned( void * dst, const void * src, size_t size )
{
	memcpy( dst, src, size );
}

// copy one (possibly misaligned) object pointed to by ptr into var
#define READ_UNALIGNED( var, ptr ) copy_unaligned( &(var), (ptr), sizeof(var) )
// and store var back to such an object
#define WRITE_UNALIGNED( ptr, var ) copy_unaligned( (ptr), &(var), sizeof(var) )

// return the float at *p and advance *p past it, like *(*p)++
static inline float read_float( float ** p )
{
	const void * src = *p;
	float f;
	memcpy( &f, src, sizeof( f ) );
	(*p)++;
	return f;
}

#ifdef __cplusplus
};
#endif

#endif
