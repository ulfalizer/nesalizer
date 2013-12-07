//#define LOG_MAPPER_OPERATIONS

#ifdef LOG_MAPPER_OPERATIONS
#    define LOG_MAPPER(...) printf(__VA_ARGS__)
#else
#    define LOG_MAPPER(...)
#endif
