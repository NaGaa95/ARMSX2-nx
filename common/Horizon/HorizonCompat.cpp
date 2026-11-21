// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <malloc.h>
#include <signal.h>
#include <sys/types.h>

// (libounce, the Switch 2's libnx, has its own: its allocator is mimalloc)
#ifndef __SWITCH2__
extern "C" int posix_memalign(void** memptr, size_t alignment, size_t size)
{
	void* const p = memalign(alignment, size);
	if (!p)
		return ENOMEM;
	*memptr = p;
	return 0;
}
#endif

// Horizon has no signals (HTTPDownloaderCurl masks them on its thread) and no file locks (GSCacheFile, Mesa).
extern "C" int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset)
{
	(void)how;
	(void)set;
	if (oldset)
		std::memset(oldset, 0, sizeof(*oldset));
	return 0;
}

extern "C" int flock(int fd, int operation)
{
	(void)fd;
	(void)operation;
	errno = ENOSYS;
	return -1;
}

extern "C" int lockf(int fd, int cmd, off_t len)
{
	(void)fd;
	(void)cmd;
	(void)len;
	return 0;
}
