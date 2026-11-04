#ifndef LUA_GAMES
#define LUA_GAMES

#ifdef LUA
void games_update( void );
int games_length( void );
int games_index_at( char * name );
const char * games_port_at( int index );
const char * games_name_at( int index );
const char * games_ip_at( int index );
#else
// the public game list is only fetched by the lua scripts
static inline void games_update( void ) {}
static inline int games_length( void ) { return 0; }
static inline int games_index_at( char * name ) { (void) name; return 0; }
static inline const char * games_port_at( int index ) { (void) index; return ""; }
static inline const char * games_name_at( int index ) { (void) index; return ""; }
static inline const char * games_ip_at( int index ) { (void) index; return ""; }
#endif

#endif // LUA_GAMES