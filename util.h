//
// General utility functions
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

#define NTH_BIT(x, n) (((x) >> (n)) & 1)

// Returns the next power of two greater than or equal to 'n'. Meant to be used
// at compile time (there are better ways to compute it at runtime).
//
// Assumes the argument is unsigned and non-zero, that 'int' is at least 32
// bits, and that the result fits in 32 bits.
#define GE_POW_2(n)                                              \
  (((n)-1       | (n)-1 >>  1 | (n)-1 >>  2 | (n)-1 >>  3 |      \
    (n)-1 >>  4 | (n)-1 >>  5 | (n)-1 >>  6 | (n)-1 >>  7 |      \
    (n)-1 >>  8 | (n)-1 >>  9 | (n)-1 >> 10 | (n)-1 >> 11 |      \
    (n)-1 >> 12 | (n)-1 >> 13 | (n)-1 >> 14 | (n)-1 >> 15 |      \
    (n)-1 >> 16 | (n)-1 >> 17 | (n)-1 >> 18 | (n)-1 >> 19 |      \
    (n)-1 >> 20 | (n)-1 >> 21 | (n)-1 >> 22 | (n)-1 >> 23 |      \
    (n)-1 >> 24 | (n)-1 >> 25 | (n)-1 >> 26 | (n)-1 >> 27 |      \
    (n)-1 >> 28 | (n)-1 >> 29 | (n)-1 >> 30 | (n)-1 >> 31 ) + 1)

// Verifies that the argument is an array and returns the number of elements in
// it
template<typename T, unsigned N>
char (&array_len_helper(T (&)[N]))[N];
#define ARRAY_LEN(arr) sizeof(array_len_helper(arr))

//
// File functions
//

// Returns the contents of file 'filename'. Buffer freed by caller.
uint8_t *get_file_buffer(char const *filename, size_t &size_out);

//
// Memory functions
//

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
