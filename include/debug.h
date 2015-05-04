void init_debug();

#ifdef OPTIMIZING
#  define UNREACHABLE __builtin_unreachable();
#else
#  define UNREACHABLE                              \
     fail("reached \"unreachable\" code at %s:%u", \
           __FILE__, (unsigned)__LINE__);
#endif
