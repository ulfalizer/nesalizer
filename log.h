#define LOG_INSTRUCTIONS
//#define LOG_PPU_REG_ACCESS
//#define LOG_PPU_MEM_ACCESS
//#define LOG_CONTROLLER_ACCESS
//#define LOG_MAPPER_OPERATIONS
//#define LOG_SPRITE_ZERO_STATUS

#ifdef LOG_PPU_REG_ACCESS
#    define LOG_PPU_REG(...) printf(__VA_ARGS__)
#else
#    define LOG_PPU_REG(...)
#endif

#ifdef LOG_PPU_MEM_ACCESS
#    define LOG_PPU_MEM(...) printf(__VA_ARGS__)
#else
#    define LOG_PPU_MEM(...)
#endif

#ifdef LOG_CONTROLLER_ACCESS
#    define LOG_CONTROLLER(...) printf(__VA_ARGS__)
#else
#    define LOG_CONTROLLER(...)
#endif

#ifdef LOG_MAPPER_OPERATIONS
#    define LOG_MAPPER(...) printf(__VA_ARGS__)
#else
#    define LOG_MAPPER(...)
#endif

#ifdef LOG_SPRITE_ZERO_STATUS
#    define LOG_SPRITE_ZERO(...) printf(__VA_ARGS__)
#else
#    define LOG_SPRITE_ZERO(...)
#endif
