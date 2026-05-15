#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#endif

// largest patch we ever write is 16 bytes. update might break this but fuck it. anyways
#define PATCH_SIZE		 16
// trampoline = original bytes (up to 16) + jmp back (12 on x64, 5 on x86) + slop shit
#define TRAMPOLINE_SIZE	 64

class CPatch
{
public:
	CPatch();
	~CPatch();

	bool  Init(void *func, void *handler, size_t patchLen = PATCH_SIZE);
	void  Unpatch();
	bool  IsPatched() const { return m_bPatched; }
	void *GetTrampoline() const { return m_pTrampoline; }

private:
	bool SetPageWritable(void *addr, size_t len, bool writable);

	void	*m_pTrampoline;
	uint8_t	 m_OriginalBytes[PATCH_SIZE + 2];
	void	*m_pFuncAddr;
	size_t	 m_PatchLen;
	bool	 m_bPatched;
};