#ifndef _DB_H
#define _DB_H 1

// Utility routines for working with decibels.

#include <math.h>

static inline double from_db(double db) { return pow(10.0, db / 20.0); }
static inline double to_db(double val) { return 20.0 * log10(val); }

#endif  // !defined(_DB_H)
