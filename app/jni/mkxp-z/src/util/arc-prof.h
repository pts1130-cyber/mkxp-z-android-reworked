/*
** arc-prof.h  (ARC phone build)
**
** Tiny helpers for the log-only frame / sound / image profiler.
** Nothing in here changes how the game behaves.
*/

#ifndef ARC_PROF_H
#define ARC_PROF_H

#include <SDL_timer.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* ---- GL call counters (filled by wrappers installed in gl-fun.cpp) ---- */
struct ArcGlCount
{
	uint32_t draws;
	uint32_t fboBinds;
	uint32_t texImages;     /* glTexImage2D calls */
	uint32_t texSubImages;  /* glTexSubImage2D calls */
	uint32_t progSwitches;
	uint32_t clears;
	uint32_t texBinds;
	uint32_t readPixels;    /* glReadPixels calls (GPU -> CPU, stalls the GPU) */
	uint64_t texBytes;      /* approx. RGBA bytes uploaded */
	uint64_t texTicks;      /* CPU time spent inside texture upload calls */
	uint64_t readTicks;     /* CPU time spent inside glReadPixels */
};

inline ArcGlCount &arcGlCount()
{
	static ArcGlCount c;
	return c;
}

inline double arcTicksToMs(uint64_t ticks)
{
	static const double f = 1000.0 / (double) SDL_GetPerformanceFrequency();
	return (double) ticks * f;
}

/* ---- counters kept by other files (bitmap.cpp / filesystem.cpp) ---- */
extern uint64_t arcRwCalls;   /* filesystem.cpp: SDL_RWops read calls  */
extern uint64_t arcRwBytes;   /* filesystem.cpp: bytes read            */
extern uint32_t arcImgCount;  /* bitmap.cpp: images loaded from files  */
extern uint64_t arcImgTicks;  /* bitmap.cpp: time spent loading them   */

/* ---- per element-type CPU time while compositing (Sprite / Window ...) ---- */
struct ArcElemBin
{
	const char *name;
	double ms;
	uint32_t n;
};

struct ArcElemProf
{
	enum { MAX_BINS = 16 };
	ArcElemBin bins[MAX_BINS];
	int nbins;
	double childMs; /* time spent in nested elements of the element being drawn */

	void reset()
	{
		nbins = 0;
		childMs = 0;
	}

	void add(const char *nm, double ms)
	{
		for (int i = 0; i < nbins; ++i)
			if (bins[i].name == nm)
			{
				bins[i].ms += ms;
				++bins[i].n;
				return;
			}
		if (nbins < MAX_BINS)
		{
			bins[nbins].name = nm;
			bins[nbins].ms = ms;
			bins[nbins].n = 1;
			++nbins;
		}
	}
};

inline ArcElemProf &arcElemProf()
{
	static ArcElemProf p;
	return p;
}

/* typeid().name() looks like "6Sprite": skip the leading length digits */
inline const char *arcShortName(const char *n)
{
	while (*n >= '0' && *n <= '9')
		++n;
	return n;
}

/* wall clock "HH:MM:SS" (so the engine logs line up with the game's own logs) */
inline const char *arcClockStr(char *buf /* >= 16 bytes */)
{
	time_t t = time(0);
	struct tm tmv;
	localtime_r(&t, &tmv);
	snprintf(buf, 16, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
	return buf;
}

/* ---- log lines: _log/<name> ----
 * The first line of a run truncates the file, later lines are appended.
 * The file is opened and closed for every line, so a log the game cleans
 * up in between is simply started again instead of being written into a
 * deleted file. */
struct ArcLogFile
{
	bool started;
	int lines;
};

inline ArcLogFile &arcLogFile(int slot)
{
	static ArcLogFile files[4];
	return files[slot];
}

/* slot: 0 spare, 1 engine_snd.txt, 2 engine_gfx.txt, 3 spare */
inline void arcLogLine(int slot, const char *path, int maxLines, const char *fmt, ...)
{
	ArcLogFile &lf = arcLogFile(slot);

	if (lf.lines >= maxLines)
		return;

	FILE *f = fopen(path, lf.started ? "a" : "w");

	if (!f)
	{
		/* _log may not exist yet */
		mkdir("_log", 0777);
		f = fopen(path, lf.started ? "a" : "w");

		if (!f)
			return;
	}

	lf.started = true;

	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
	++lf.lines;
}

#endif /* ARC_PROF_H */
