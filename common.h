#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define __STDC_FORMAT_MACROS
#define __STDC_LIMIT_MACROS
#include <inttypes.h>
#include <new> // For std::nothrow
#include <unistd.h>

#include "debug.h"
#include "error.h"
#include "log.h"
#include "util.h"

// TODO: The C++ standard strictly puts identifiers from the <c*> headers in
// the std namespace. In practice they nearly always end up in the global
// namespace as well.
// using std::printf;
// using std::puts;
// using std::size_t;
// ...

extern char const *program_name; // argv[0]
