/*
** soundemitter.h
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

#ifndef SOUNDEMITTER_H
#define SOUNDEMITTER_H

#include "intrulist.h"
#include "al-util.h"
#include "boost-hash.h"

#include <string>
#include <vector>
#include <deque>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>

struct SoundBuffer;
struct Config;
struct ArcDecodedSound;

struct SoundEmitter
{
	typedef BoostHash<std::string, SoundBuffer*> BufferHash;

	IntruList<SoundBuffer> buffers;
	BufferHash bufferHash;

	/* Byte count sum of all cached / playing buffers */
	uint32_t bufferBytes;

	const size_t srcCount;
	std::vector<AL::Source::ID> alSrcs;
	std::vector<SoundBuffer*> atchBufs;

	/* Indices of sources, sorted by priority (lowest first) */
	std::vector<size_t> srcPrio;

	SoundEmitter(const Config &conf);
	~SoundEmitter();

	void play(const std::string &filename,
	          int volume,
	          int pitch);

	void stop();

	/* ARC: decode an effect on a worker thread ahead of time, so the first
	 * play() does not block the game thread. Safe to call for names that
	 * are already cached or queued. */
	void preload(const std::string &filename);

private:
	SoundBuffer *allocateBuffer(const std::string &filename);
	void insertBuffer(SoundBuffer *buffer, const std::string &filename);
	void adoptDecoded();
	void workerMain();

	std::mutex arcLock;
	std::condition_variable arcCond;
	std::deque<std::string> arcQueue;
	std::unordered_set<std::string> arcPending;
	std::vector<std::shared_ptr<ArcDecodedSound> > arcDone;
	std::thread arcWorker;
	bool arcQuit;
};

#endif // SOUNDEMITTER_H
