#ifndef _TIMEBASE_H
#define _TIMEBASE_H 1

// Common timebase that allows us to represent one frame exactly in all the
// relevant frame rates:
//
//   Timebase:                1/120000
//   Frame at 50fps:       2400/120000
//   Frame at 60fps:       2000/120000
//   Frame at 59.94fps:    2002/120000
//   Frame at 23.976fps:   5005/120000
//
// If we also wanted to represent one sample at 48000 Hz, we'd need
// to go to 300000. Also supporting one sample at 44100 Hz would mean
// going to 44100000; probably a bit excessive.
#define TIMEBASE 120000

// Some muxes, like MP4 (or at least avformat's implementation of it),
// are not too fond of values above 2^31. At timebase 120000, that's only
// about five hours or so, so we define a coarser timebase that doesn't
// get 59.94 precisely (so there will be a marginal amount of pts jitter),
// but can do at least 50 and 60 precisely, and months of streaming.
#define COARSE_TIMEBASE 300

#endif  // !defined(_TIMEBASE_H)
