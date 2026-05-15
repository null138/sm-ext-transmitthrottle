// this is stolen code from many public projects. i cannot credit everyone because i do not remember where all of it came from
// scraped and built for what the extension needs

#include "patch.h"
#include <stdio.h>

CPatch::CPatch()
	: m_pTrampoline(nullptr), m_pFuncAddr(nullptr), m_PatchLen(0), m_bPatched(false)
{
	memset(m_OriginalBytes, 0, sizeof(m_OriginalBytes));
}

CPatch::~CPatch()
{
	Unpatch();
	if (m_pTrampoline)
	{
#ifdef _WIN32
		VirtualFree(m_pTrampoline, 0, MEM_RELEASE);
#else
		munmap(m_pTrampoline, TRAMPOLINE_SIZE);
#endif
		m_pTrampoline = nullptr;
	}
}

bool CPatch::SetPageWritable(void *addr, size_t len, bool writable)
{
#ifdef _WIN32
	DWORD oldProt;
	DWORD newProt = writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
	return VirtualProtect(addr, len, newProt, &oldProt) != 0;
#else
	uintptr_t pageStart = (uintptr_t)addr & ~(uintptr_t)(sysconf(_SC_PAGESIZE) - 1);
	size_t	  pageLen	= len + ((uintptr_t)addr - pageStart);
	int		  prot		= PROT_READ | PROT_EXEC | (writable ? PROT_WRITE : 0);
	return mprotect((void *)pageStart, pageLen, prot) == 0;
#endif
}

bool CPatch::Init(void *func, void *handler, size_t patchLen)
{
	if (!func || !handler)
		return false;

#ifdef _WIN64
	if (patchLen < 12)
		return false;
	const size_t jmpPatchSize = 12;
	const size_t jmpBackSize  = 12;
#else
	if (patchLen < 5)
		return false;
	const size_t jmpPatchSize = 5;
	const size_t jmpBackSize  = 5;
#endif

	if (patchLen + jmpBackSize > TRAMPOLINE_SIZE)
		return false;

	m_pFuncAddr = func;
	m_PatchLen	= patchLen;
	
	if (m_pTrampoline)
	{
#ifdef _WIN32
		VirtualFree(m_pTrampoline, 0, MEM_RELEASE);
#else
		munmap(m_pTrampoline, TRAMPOLINE_SIZE);
#endif
		m_pTrampoline = nullptr;
	}

#ifdef _WIN32
	m_pTrampoline = VirtualAlloc(nullptr, TRAMPOLINE_SIZE,
								 MEM_COMMIT | MEM_RESERVE,
								 PAGE_EXECUTE_READWRITE);
	if (!m_pTrampoline)
		return false;
#else
	m_pTrampoline = mmap(nullptr, TRAMPOLINE_SIZE,
						 PROT_READ | PROT_WRITE | PROT_EXEC,
						 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m_pTrampoline == MAP_FAILED)
	{
		m_pTrampoline = nullptr;
		return false;
	}
#endif

	uint8_t *tramp = (uint8_t *)m_pTrampoline;

	memcpy(m_OriginalBytes, func, patchLen);
	memcpy(tramp, func, patchLen);

	uint8_t *jmpBack = tramp + patchLen;

#ifdef _WIN64
	uintptr_t returnTarget = (uintptr_t)func + patchLen;
	jmpBack[0] = 0x48; jmpBack[1] = 0xB8;
	memcpy(&jmpBack[2], &returnTarget, 8);
	jmpBack[10] = 0xFF; jmpBack[11] = 0xE0;
#else
	uintptr_t trampolineJmpSrc = (uintptr_t)jmpBack + 5;
	uintptr_t returnTarget	   = (uintptr_t)func + patchLen;
	int32_t	  rel			   = (int32_t)(returnTarget - trampolineJmpSrc);
	jmpBack[0] = 0xE9;
	memcpy(&jmpBack[1], &rel, 4);
#endif

#ifdef __GNUC__
	__builtin___clear_cache((char *)tramp, (char *)tramp + TRAMPOLINE_SIZE);
#endif

	if (!SetPageWritable(func, patchLen, true))
		return false;

	uint8_t *dst = (uint8_t *)func;

#ifdef _WIN64
	uintptr_t handlerAddr = (uintptr_t)handler;
	dst[0] = 0x48; dst[1] = 0xB8;
	memcpy(&dst[2], &handlerAddr, 8);
	dst[10] = 0xFF; dst[11] = 0xE0;
#else
	uintptr_t jmpSrc = (uintptr_t)func + 5;
	int32_t	  jmpRel = (int32_t)((uintptr_t)handler - jmpSrc);
	dst[0] = 0xE9;
	memcpy(&dst[1], &jmpRel, 4);
#endif

	for (size_t i = jmpPatchSize; i < patchLen; i++)
		dst[i] = 0x90;

#ifdef __GNUC__
	__builtin___clear_cache((char *)func, (char *)func + patchLen);
#endif

	SetPageWritable(func, patchLen, false);
	m_bPatched = true;
	return true;
}

void CPatch::Unpatch()
{
	if (!m_bPatched || !m_pFuncAddr)
		return;

	SetPageWritable(m_pFuncAddr, m_PatchLen, true);
	memcpy(m_pFuncAddr, m_OriginalBytes, m_PatchLen);

#ifdef __GNUC__
	__builtin___clear_cache((char *)m_pFuncAddr,
							 (char *)m_pFuncAddr + m_PatchLen);
#endif

	SetPageWritable(m_pFuncAddr, m_PatchLen, false);
	m_bPatched = false;
}