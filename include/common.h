// General utility stuff and error handling

#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
// Bring in C99 type macros
#define __STDC_CONSTANT_MACROS
#define __STDC_FORMAT_MACROS
#define __STDC_LIMIT_MACROS
#include <inttypes.h>
#include <new> // For std::nothrow
#include <unistd.h>

// TODO: The C++ standard strictly puts identifiers from the <c*> headers in
// the std namespace. In practice they nearly always end up in the global
// namespace as well.
// using std::printf;
// using std::puts;
// using std::size_t;
// ...

extern char const *program_name; // argv[0]

//
// General utility functions and macros
//

bool is_pow_2_or_0(unsigned n);
uint8_t rev_byte(uint8_t n);

template<typename T>
T const &min(T const &x, T const &y) {
    return x < y ? x : y;
}

template<typename T>
T const &max(T const &x, T const &y) {
    return x > y ? x : y;
}

template<typename T>
void swap(T &x, T &y) {
    T const temp = x;
    x = y;
    y = temp;
}

// True if 'x' starts with the byte sequence in 'str' (excluding the
// terminating null)
#define MEM_EQ(x, str) !memcmp(x, str, sizeof (str) - 1)

#define NTH_BIT(x, n) (((x) >> (n)) & 1)

// Returns the next power of two greater than or equal to 'n'. Meant to be used
// at compile time (there are better ways to compute it at runtime).
//
// Assumes the argument is unsigned and non-zero, that 'int' is at least 32
// bits, and that the result fits in 32 bits.
#define GE_POW_2(n)                                                      \
  ((((n)-1)       | ((n)-1) >>  1 | ((n)-1) >>  2 | ((n)-1) >>  3 |      \
    ((n)-1) >>  4 | ((n)-1) >>  5 | ((n)-1) >>  6 | ((n)-1) >>  7 |      \
    ((n)-1) >>  8 | ((n)-1) >>  9 | ((n)-1) >> 10 | ((n)-1) >> 11 |      \
    ((n)-1) >> 12 | ((n)-1) >> 13 | ((n)-1) >> 14 | ((n)-1) >> 15 |      \
    ((n)-1) >> 16 | ((n)-1) >> 17 | ((n)-1) >> 18 | ((n)-1) >> 19 |      \
    ((n)-1) >> 20 | ((n)-1) >> 21 | ((n)-1) >> 22 | ((n)-1) >> 23 |      \
    ((n)-1) >> 24 | ((n)-1) >> 25 | ((n)-1) >> 26 | ((n)-1) >> 27 |      \
    ((n)-1) >> 28 | ((n)-1) >> 29 | ((n)-1) >> 30 | ((n)-1) >> 31 ) + 1)

// Verifies that the argument is an array and returns the number of elements in
// it
template<typename T, unsigned N>
char (&array_len_helper(T (&)[N]))[N];
#define ARRAY_LEN(arr) sizeof(array_len_helper(arr))

// Returns the contents of file 'filename'. Buffer freed by caller.
uint8_t *get_file_buffer(char const *filename, size_t &size_out);

// Initializes each element of an array to a given value. Verifies that the
// argument is an array.
template<typename T, size_t N>
void init_array(T (&arr)[N], T const&val) {
    for (size_t i = 0; i < N; ++i)
        arr[i] = val;
}

// Allocates an array of arbitrary type and initializes each element to a given
// value. Returns null on allocation errors. Freed by caller.
template<typename T>
T *alloc_array_init(size_t size, T const&val) {
    T *const res = new (std::nothrow) T[size];
    if (res)
        for (size_t i = 0; i < size; ++i)
            res[i] = val;
    return res;
}

// Frees a pointer and sets it to null, making null equivalent to not
// allocated, memory errors easier to debug, and the pointer safe to re-free
template<typename T>
void free_array_set_null(T *p) {
    delete [] p;
    p = 0;
}

#ifdef OPTIMIZING
#  define UNREACHABLE __builtin_unreachable();
#else
#  define UNREACHABLE                              \
     fail("reached \"unreachable\" code at %s:%u", \
           __FILE__, (unsigned)__LINE__);
#endif

// State serialization and deserialization helpers

// Saves a variable to or loads a variable from a buffer, incrementing the
// buffer pointer afterwards. If 'calculating_size' is true, the buffer pointer
// is incremented without saving or loading the value, which is used for buffer
// size calculations.
template<bool calculating_size, bool is_save, typename T>
void transfer(T &val, uint8_t *&bufp) {
    if (!calculating_size) {
        // Use memcpy to support arrays. Optimized well by GCC in other cases
        // too.
        if (is_save)
            memcpy(bufp, &val, sizeof(T));
        else
            memcpy(&val, bufp, sizeof(T));
    }
    bufp += sizeof(T);
}

// Ditto for saving a memory area referenced by a pointer
template<bool calculating_size, bool is_save, typename T>
void transfer_p(T *ptr, size_t len, uint8_t *&bufp) {
    if (!calculating_size) {
        if (is_save)
            memcpy(bufp, ptr, len);
        else
            memcpy(ptr, bufp, len);
    }
    bufp += len;
}

#define TRANSFER(x) transfer<calculating_size, is_save>(x, buf);
#define TRANSFER_P(x, len) transfer_p<calculating_size, is_save>(x, len, buf);

//
// Error reporting
//

// Prints a message to stderr and exits with EXIT_FAILURE
void fail(char const *format, ...)
  __attribute__((format(printf, 1, 2), noreturn));

// Prints a message along with errno to stderr and exits with EXIT_FAILURE
void errno_fail(int errno_val, char const *format, ...)
  __attribute__((format(printf, 2, 3), noreturn));

#define fail_if(condition, ...) \
    if (condition)              \
        fail(__VA_ARGS__);

#define errno_fail_if(condition, ...)   \
    if (condition)                      \
        errno_fail(errno, __VA_ARGS__);

#define errno_val_fail_if(condition, errno_val, ...) \
    if (condition)                                   \
        errno_fail(errno_val, __VA_ARGS__);

// Installs a handler that prints a backtrace for some fatal signals
void install_fatal_signal_handlers();
