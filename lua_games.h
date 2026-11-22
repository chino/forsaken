#ifndef LUA_GAMES
#define LUA_GAMES

// the public game list used to be fetched over http by scripts/games.lua
static inline void games_update( void ) {}
static inline int games_length( void ) { return 0; }
static inline int games_index_at( char * name ) { (void) name; return 0; }
static inline const char * games_port_at( int index ) { (void) index; return ""; }
static inline const char * games_name_at( int index ) { (void) index; return ""; }
static inline const char * games_ip_at( int index ) { (void) index; return ""; }

#endif // LUA_GAMES