#pragma once

#if defined(NDEBUG)
#define assert(x)  /* ignore */
#else
extern int __assert_failed(const char *fn, int lineno);

#define assert(x)  ((x) || __assert_failed(__FILE__, __LINE__))
#endif
