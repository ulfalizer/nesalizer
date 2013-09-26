void fail(char const *format, ...)
  __attribute__((format(printf, 1, 2), noreturn));

void fail_if(bool condition, char const *format, ...)
  __attribute__((format(printf, 2, 3)));

void errno_fail_if(bool condition, char const *format, ...)
  __attribute__((format(printf, 2, 3)));

void errno_val_fail_if(bool condition, int errno_val, char const *format, ...)
  __attribute__((format(printf, 3, 4)));
