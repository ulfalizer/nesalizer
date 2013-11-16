void fail(char const *format, ...)
  __attribute__((format(printf, 1, 2), noreturn));

void errno_fail(int errno_val, char const *format, ...)
  __attribute__((format(printf, 2, 3)));

#define fail_if(condition, ...) \
    if (condition)              \
        fail(__VA_ARGS__);

#define errno_fail_if(condition, ...)   \
    if (condition)                      \
        errno_fail(errno, __VA_ARGS__);

#define errno_val_fail_if(condition, errno_val, ...) \
    if (condition)                                           \
        errno_fail(errno_val, __VA_ARGS__);
