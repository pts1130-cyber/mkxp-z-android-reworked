/*
** soundemitter.cpp
**
** This file is part of mkxp.
**
** Copyright (C) 2014 Jonas Kulla <Nyocurio@gmail.com>
**
** mkxp is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 2 of the License, or
** (at your option) any later version.
**
** mkxp is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with mkxp.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "soundemitter.h"

#include "sharedstate.h"
#include "filesystem.h"
#include "exception.h"
#include "config.h"
#include "util.h"
#include "debugwriter.h"

#include <SDL_sound.h>

#include <stdlib.h>

#include "arc-prof.h"

/* ARC: 32 MB (was 10 MB). Decoded effects are 32 bit float, long ones (Ruin
 * Light, Z moves...) are several MB each and used to push each other out of
 * the cache, so they were decoded from disk again and again. */
#define SE_CACHE_MEM (32*1024*1024)

/* ARC: effect files up to this size are read into memory in one go */
#define ARC_SE_SLURP_MAX (24*1024*1024)

struct SoundBuffer
{
	/* Uniquely identifies this or equal buffer */
	std::string key;

	AL::Buffer::ID alBuffer;

	/* Link into the buffer cache priority list */
	IntruListLink<SoundBuffer> link;

	/* Buffer byte count */
	uint32_t bytes;

	/* Reference count */
	uint8_t refCount;

	SoundBuffer()
	    : link(this),
	      refCount(1)

	{
		alBuffer = AL::Buffer::gen();
	}

	static SoundBuffer *ref(SoundBuffer *buffer)
	{
		++buffer->refCount;

		return buffer;
	}

	static void deref(SoundBuffer *buffer)
	{
		if (--buffer->refCount == 0)
			delete buffer;
	}

private:
	~SoundBuffer()
	{
		AL::Buffer::del(alBuffer);
	}
};

/* Before: [a][b][c][d], After (index=1): [a][c][d][b] */
static void
arrayPushBack(std::vector<size_t> &array, size_t size, size_t index)
{
	size_t v = array[index];

	for (size_t t = index; t < size-1; ++t)
		array[t] = array[t+1];

	array[size-1] = v;
}

SoundEmitter::SoundEmitter(const Config &conf)
    : bufferBytes(0),
      srcCount(conf.SE.sourceCount),
      alSrcs(srcCount),
      atchBufs(srcCount),
      srcPrio(srcCount)
{
	for (size_t i = 0; i < srcCount; ++i)
	{
		alSrcs[i] = AL::Source::gen();
		atchBufs[i] = 0;
		srcPrio[i] = i;
	}
}

SoundEmitter::~SoundEmitter()
{
	for (size_t i = 0; i < srcCount; ++i)
	{
		AL::Source::stop(alSrcs[i]);
		AL::Source::del(alSrcs[i]);

		if (atchBufs[i])
			SoundBuffer::deref(atchBufs[i]);
	}

	BufferHash::const_iterator iter;
	for (iter = bufferHash.cbegin(); iter != bufferHash.cend(); ++iter)
		SoundBuffer::deref(iter->second);
}

void SoundEmitter::play(const std::string &filename,
                        int volume,
                        int pitch)
{
	float _volume = clamp<int>(volume, 0, 100) / 100.0f;
	float _pitch  = clamp<int>(pitch, 50, 150) / 100.0f;

	SoundBuffer *buffer = allocateBuffer(filename);

	if (!buffer)
		return;

	/* Try to find first free source */
	size_t i;
	for (i = 0; i < srcCount; ++i)
		if (AL::Source::getState(alSrcs[srcPrio[i]]) != AL_PLAYING)
			break;

	/* If we didn't find any, try to find the lowest priority source
	 * with the same buffer to overtake */
	if (i == srcCount)
		for (size_t j = 0; j < srcCount; ++j)
			if (atchBufs[srcPrio[j]] == buffer)
				i = j;

	/* If we didn't find any, overtake the one with lowest priority */
	if (i == srcCount)
		i = 0;

	size_t srcIndex = srcPrio[i];

	/* Only detach/reattach if it's actually a different buffer */
	bool switchBuffer = (atchBufs[srcIndex] != buffer);

	/* Push the used source to the back of the priority list */
	arrayPushBack(srcPrio, srcCount, i);

	AL::Source::ID src = alSrcs[srcIndex];
	AL::Source::stop(src);

	if (switchBuffer)
		AL::Source::detachBuffer(src);

	SoundBuffer *old = atchBufs[srcIndex];

	if (old)
		SoundBuffer::deref(old);

	atchBufs[srcIndex] = SoundBuffer::ref(buffer);

	if (switchBuffer)
		AL::Source::attachBuffer(src, buffer->alBuffer);

	AL::Source::setVolume(src, _volume * GLOBAL_VOLUME);
	AL::Source::setPitch(src, _pitch);

	AL::Source::play(src);
}

void SoundEmitter::stop()
{
	for (size_t i = 0; i < srcCount; i++)
		AL::Source::stop(alSrcs[i]);
}

/* ARC: close function of the in-memory ops used by SoundOpenHandler.
 * That ops struct lives on the stack of tryRead() and the file image is
 * freed by the handler itself, so closing must not free anything:
 * Sound_NewSample() (when no decoder takes the data) and Sound_FreeSample()
 * both close the ops they were handed. */
static int arcMemOpsClose(SDL_RWops *)
{
	return 0;
}

struct SoundOpenHandler : FileSystem::OpenHandler
{
	SoundBuffer *buffer;

	/* ARC: timing for the log (milliseconds) */
	double msRead;
	double msDecode;
	double msUpload;
	Sint64 fileBytes;
	bool slurped;
	int rate;
	int channels;

	SoundOpenHandler()
	    : buffer(0),
	      msRead(0), msDecode(0), msUpload(0),
	      fileBytes(-1), slurped(false), rate(0), channels(0)
	{}

	bool tryRead(SDL_RWops &ops, const char *ext)
	{
		/* ARC: the decoders (stb_vorbis, dr_mp3, wav...) pull the file out of
		 * PhysFS a few bytes at a time. On the phone's shared storage every
		 * single read call costs ~20 us, so one 100 KB effect used to freeze
		 * the game for seconds.  Read the whole file with ONE read and decode
		 * from memory instead. */
		uint64_t t0 = SDL_GetPerformanceCounter();

		SDL_RWops *src = &ops;
		SDL_RWops memOps;   /* in-memory ops, valid until tryRead() returns */
		void *mem = 0;

		Sint64 fsize = SDL_RWsize(&ops);
		fileBytes = fsize;

		if (fsize > 0 && fsize <= ARC_SE_SLURP_MAX)
		{
			mem = malloc((size_t) fsize);

			if (mem)
			{
				size_t got = 0;
				bool haveMemOps = false;

				while (got < (size_t) fsize)
				{
					size_t n = SDL_RWread(&ops, (char *) mem + got, 1,
					                      (size_t) fsize - got);
					if (n == 0)
						break;
					got += n;
				}

				if (got == (size_t) fsize)
				{
					SDL_RWops *tmp = SDL_RWFromConstMem(mem, (int) fsize);

					if (tmp)
					{
						/* plain copy: the read position lives in the struct,
						 * the data stays in mem. Our own close() never frees. */
						memOps = *tmp;
						SDL_FreeRW(tmp);
						memOps.close = arcMemOpsClose;
						haveMemOps = true;
					}
				}

				if (haveMemOps)
				{
					/* the PhysFS handle is not needed any more */
					SDL_RWclose(&ops);
					src = &memOps;
					slurped = true;
				}
				else
				{
					/* could not slurp: fall back to the old streaming path */
					free(mem);
					mem = 0;
					SDL_RWseek(&ops, 0, RW_SEEK_SET);
				}
			}
		}

		uint64_t t1 = SDL_GetPerformanceCounter();
		msRead = arcTicksToMs(t1 - t0);

		Sound_Sample *sample = Sound_NewSample(src, ext, 0, STREAM_BUF_SIZE);

		if (!sample)
		{
			/* Sound_NewSample() has already closed the ops it was given when no
			 * decoder accepted the data. Closing the PhysFS ops once more is
			 * harmless (the handle pointer is cleared on close, same as in the
			 * original code); the in-memory ops is ours and needs no closing. */
			SDL_RWclose(&ops);
			free(mem);
			return false;
		}

		/* Do all of the decoding in the handler so we don't have
		 * to keep the source ops around */
		uint32_t decBytes = Sound_DecodeAll(sample);
		uint8_t sampleSize = formatSampleSize(sample->actual.format);
		uint32_t sampleCount = decBytes / sampleSize;

		uint64_t t2 = SDL_GetPerformanceCounter();
		msDecode = arcTicksToMs(t2 - t1);

		buffer = new SoundBuffer;
		buffer->bytes = sampleSize * sampleCount;

		ALenum alFormat = chooseALFormat(sampleSize, sample->actual.channels);

		AL::Buffer::uploadData(buffer->alBuffer, alFormat, sample->buffer,
							   buffer->bytes, sample->actual.rate);

		rate = (int) sample->actual.rate;
		channels = (int) sample->actual.channels;

		/* closes src as well (the memory ops or the PhysFS ops) */
		Sound_FreeSample(sample);

		/* the decoder is gone, the file image is not needed any more */
		free(mem);

		msUpload = arcTicksToMs(SDL_GetPerformanceCounter() - t2);

		return true;
	}
};

SoundBuffer *SoundEmitter::allocateBuffer(const std::string &filename)
{
	SoundBuffer *buffer = bufferHash.value(filename, 0);

	if (buffer)
	{
		/* Buffer still in cashe.
		 * Move to front of priority list */
		buffers.remove(buffer->link);
		buffers.append(buffer->link);

		return buffer;
	}
	else
	{
		/* Buffer not in cache, needs to be loaded */
		uint64_t arcT0 = SDL_GetPerformanceCounter();

		SoundOpenHandler handler;
		shState->fileSystem().openRead(handler, filename.c_str());
		buffer = handler.buffer;

		/* ARC: log every effect that took 15 ms or more to load */
		{
			double total = arcTicksToMs(SDL_GetPerformanceCounter() - arcT0);

			if (total >= 15.0)
			{
				double rest = total - handler.msRead - handler.msDecode -
				              handler.msUpload;
				if (rest < 0)
					rest = 0;

				arcLogLine(1, "_log/engine_snd.txt", 400,
				           "[엔진 %.1f초] %.0fms 합계 | 파일열기+목록 %.0fms 읽기 %.0fms(%lld바이트%s) 풀기 %.0fms | 올리기 %.0fms | %dHz %dch | %s%s",
				           SDL_GetTicks() / 1000.0, total, rest,
				           handler.msRead, (long long) handler.fileBytes,
				           handler.slurped ? " 통째로" : " 조각조각",
				           handler.msDecode, handler.msUpload,
				           handler.rate, handler.channels,
				           filename.c_str(), buffer ? "" : "  (실패)");
			}
		}

		if (!buffer)
		{
			char buf[512];
			snprintf(buf, sizeof(buf), "Unable to decode sound: %s: %s",
			         filename.c_str(), Sound_GetError());
			Debug() << buf;

			return 0;
		}

		buffer->key = filename;
		uint32_t wouldBeBytes = bufferBytes + buffer->bytes;

		/* If memory limit is reached, delete lowest priority buffer
		 * until there is room or no buffers left */
		while (wouldBeBytes > SE_CACHE_MEM && !buffers.isEmpty())
		{
			SoundBuffer *last = buffers.tail();
			bufferHash.remove(last->key);
			buffers.remove(last->link);

			wouldBeBytes -= last->bytes;

			SoundBuffer::deref(last);
		}

		bufferHash.insert(filename, buffer);
		buffers.prepend(buffer->link);

		bufferBytes = wouldBeBytes;

		return buffer;
	}
}
