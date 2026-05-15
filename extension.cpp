#include "extension.h"
#include "patch.h"

#ifdef _WIN32
#  include <windows.h>
#  include <psapi.h>
#  pragma comment(lib, "psapi.lib")
#else
#  include <dlfcn.h>
#  include <sys/mman.h>
#  include <link.h>
#endif

#include "sm_platform.h"
#include "iplayerinfo.h"
#include "server_class.h"
#include "igamehelpers.h"
#include "isdktools.h"
#include "dt_send.h"
#include "eiface.h"
#include "edict.h"

#if defined(_WIN32)
#  define PLAT_WINDOWS
#elif defined(__x86_64__) || defined(__amd64__)
#  define PLAT_LINUX64
#else
#  define PLAT_LINUX32
#endif

#define MAX_EDICTS_N 2049
#define MAX_CLIENTS	65

// 15.05.2026 
// -------------------------- WARNING!!! MUST READ!!! -------------------------- 
// everything in this code is very very fragile, this is the price we pay for not changing the sdkhooks source itself and compiling it properly. lazy ass mfs
// with every new update there is a big chance that the whole code will break because of address and offset shifts.
// the current addresses are correct for the latest build of sourcemod 1.12. build 7230.
// this code is full of ASM code to make writing it simpler, yeah i just copied the assembly dump xd
// it is not being lazy, it is being efficient. this whole extension is just a mess anyway. just hope it never breaks.

// -------------------------- MAINTAINING FUNCTIONALITY --------------------------
// if anything breaks because of sourcemod updates:
// check lines 228+ and 328+ and look for the hardcoded addresses there. i kept the current ASM codes next to addresses for the reference.
// update them using a dump from tools like ida for the function SDKHooks::Hook_SetTransmit(CCheckTransmitInfo *, bool)
// take the first 16 bytes on linux
// then find the result value inside the same function below the Call(), usually the "cmp eax, 3" part (included).
// take 11 bytes for linux 64bit or 9 bytes for linux 32bit

// peace out ~Madness (null138)

TransmitThrottle g_TransmitThrottle;
SMEXT_LINK(&g_TransmitThrottle);

static CPatch g_PatchEntry;
static CPatch g_PatchResult;

struct CacheEntry
{
	float timestamp;
	bool  bSupercede;
};

static CacheEntry g_Cache[MAX_EDICTS_N][MAX_CLIENTS];

static int	 g_LastEntIdx		  = -1;
static int	 g_LastClientIdx	  = -1;
static bool	 g_ShouldCache		  = false;
static float g_flThrottleInterval = 0.4f;

static void OnThrottleIntervalChanged(IConVar *var, const char *pOldValue, float flOldValue)
{
	g_flThrottleInterval = static_cast<ConVar *>(var)->GetFloat();
}

ConVar g_ThrottleInterval("transmit_throttle_interval", "0.4", FCVAR_NOTIFY, "Throttle interval in seconds");

#ifdef PLAT_WINDOWS

typedef HMODULE ModuleHandle;

static ModuleHandle FindModule(const char *name)
{
	return GetModuleHandleA(name);
}

static void GetModuleRange(ModuleHandle h, uint8_t **base, size_t *size)
{
	MODULEINFO info;
	GetModuleInformation(GetCurrentProcess(), h, &info, sizeof(info));
	*base = (uint8_t *)info.lpBaseOfDll;
	*size = info.SizeOfImage;
}

#else

typedef void* ModuleHandle;

static ModuleHandle FindModule(const char *name)
{
	return dlopen(name, RTLD_NOLOAD | RTLD_NOW);
}

struct LinuxModuleRange { uintptr_t base; size_t size; };

static int dl_iter_cb(struct dl_phdr_info *info, size_t, void *data)
{
	auto *r = (LinuxModuleRange *)data;
	if (info->dlpi_addr != r->base)
		return 0;

	size_t end = 0;
	for (int i = 0; i < info->dlpi_phnum; i++)
	{
		const auto &ph = info->dlpi_phdr[i];
		if (ph.p_type == PT_LOAD)
		{
			size_t seg_end = ph.p_vaddr + ph.p_memsz;
			if (seg_end > end) end = seg_end;
		}
	}
	r->size = end;
	return 1;
}

static void GetModuleRange(ModuleHandle h, uint8_t **base, size_t *size)
{
	struct link_map *lm = nullptr;
	dlinfo(h, RTLD_DI_LINKMAP, &lm);

	LinuxModuleRange r;
	r.base = (uintptr_t)lm->l_addr;
	r.size = 0x800000;
	dl_iterate_phdr(dl_iter_cb, &r);

	*base = (uint8_t *)r.base;
	*size = r.size;
}

#endif

static void *ScanModule(ModuleHandle hModule, const uint8_t *sig, size_t sigLen)
{
	uint8_t *base;
	size_t	 size;
	GetModuleRange(hModule, &base, &size);

	for (size_t i = 0; i + sigLen < size; i++)
	{
		bool found = true;
		for (size_t j = 0; j < sigLen; j++)
		{
			if (sig[j] != 0x2A && base[i + j] != sig[j])
			{
				found = false;
				break;
			}
		}
		if (found) return base + i;
	}
	return nullptr;
}

class HookEntry
{
public:
	void Handler_Entry(CCheckTransmitInfo *pInfo, bool bAlways);
};

void HookEntry::Handler_Entry(CCheckTransmitInfo *pInfo, bool bAlways)
{
	CBaseEntity *pEnt = META_IFACEPTR(CBaseEntity);

	int entIdx	  = gamehelpers->EntityToBCompatRef(pEnt);
	int clientIdx = gamehelpers->IndexOfEdict(pInfo->m_pClientEnt);

	g_ShouldCache	= false;
	g_LastEntIdx	= -1;
	g_LastClientIdx = -1;

	if (entIdx > 0 && entIdx < MAX_EDICTS_N && clientIdx > 0 && clientIdx < MAX_CLIENTS)
	{
		CacheEntry &entry = g_Cache[entIdx][clientIdx];
		float now = Plat_FloatTime();

		if ((now - entry.timestamp) < g_flThrottleInterval)
		{
			if (entry.bSupercede)
				RETURN_META(MRES_SUPERCEDE);
			else
				RETURN_META(MRES_IGNORED);
		}

		g_LastEntIdx	= entIdx;
		g_LastClientIdx = clientIdx;
		g_ShouldCache	= true;
	}

#ifdef PLAT_WINDOWS
	{
		void *tramp = g_PatchEntry.GetTrampoline();
		typedef void (__thiscall *OrigFn)(CBaseEntity *, CCheckTransmitInfo *, bool);
		reinterpret_cast<OrigFn>(tramp)(pEnt, pInfo, bAlways);
	}
#else
	{
		void *tramp = g_PatchEntry.GetTrampoline();
		union
		{
			void *raw;
			void (HookEntry::*mfp)(CCheckTransmitInfo *, bool);
		} u;
		u.raw = tramp;
		(reinterpret_cast<HookEntry *>(pEnt)->*u.mfp)(pInfo, bAlways);
	}
#endif
}

static void Handler_Result(int result)
{
	if (!g_ShouldCache)
		return;
	if (g_LastEntIdx < 0 || g_LastClientIdx < 0)
		return;

	CacheEntry &entry = g_Cache[g_LastEntIdx][g_LastClientIdx];
	entry.bSupercede  = (result >= 3);
	entry.timestamp	  = Plat_FloatTime();
	g_ShouldCache	  = false;
}

static void *g_ResultPatchBuf		= nullptr;
static void *g_ResultTrampolineAddr = nullptr;

#ifdef PLAT_LINUX64
// ** x86-64
static bool BuildResultStub()
{
	const size_t kBufSz = 40;
	g_ResultPatchBuf = mmap(nullptr, kBufSz,
							PROT_READ | PROT_WRITE | PROT_EXEC,
							MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (g_ResultPatchBuf == MAP_FAILED) { g_ResultPatchBuf = nullptr; return false; }

	uint8_t *p = (uint8_t *)g_ResultPatchBuf;

	*p++ = 0x50;										   // push rax	(save + align)
	*p++ = 0x89; *p++ = 0xC7;							   // mov  edi, eax
	uint64_t fn = (uint64_t)(uintptr_t)Handler_Result;
	*p++ = 0x48; *p++ = 0xB8; memcpy(p, &fn, 8); p += 8;   // movabs rax, fn
	*p++ = 0xFF; *p++ = 0xD0;							   // call rax
	*p++ = 0x58;										   // pop  rax	(restore)
	uint64_t ptr = (uint64_t)(uintptr_t)&g_ResultTrampolineAddr;
	*p++ = 0x48; *p++ = 0xB8; memcpy(p, &ptr, 8); p += 8;  // movabs rax, &tramp
	*p++ = 0xFF; *p++ = 0x20;							   // jmp  [rax]

	return true;
}
static void FreeResultStub()
{
	if (g_ResultPatchBuf) { munmap(g_ResultPatchBuf, 40); g_ResultPatchBuf = nullptr; }
}

#else
// ** x86 (win and linux)
// push	 eax
// push	 eax
// call	 Handler_Result
// add	 esp, 4
// pop	 eax
// jmp	 dword ptr [g_ResultTrampolineAddr]
static bool BuildResultStub()
{
	const size_t kBufSz = 32;

#ifdef PLAT_WINDOWS
	g_ResultPatchBuf = VirtualAlloc(nullptr, kBufSz,
									MEM_COMMIT | MEM_RESERVE,
									PAGE_EXECUTE_READWRITE);
	if (!g_ResultPatchBuf) return false;
#else
	g_ResultPatchBuf = mmap(nullptr, kBufSz,
							PROT_READ | PROT_WRITE | PROT_EXEC,
							MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (g_ResultPatchBuf == MAP_FAILED) { g_ResultPatchBuf = nullptr; return false; }
#endif

	uint8_t *p = (uint8_t *)g_ResultPatchBuf;

	*p++ = 0x50;											  // push eax (save)
	*p++ = 0x50;											  // push eax (arg0)
	int32_t rel = (int32_t)((uint8_t *)Handler_Result - (p + 5));
	*p++ = 0xE8; memcpy(p, &rel, 4); p += 4;				  // call Handler_Result
	*p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;					  // add  esp, 4
	*p++ = 0x58;											  // pop  eax
	*p++ = 0xFF; *p++ = 0x25;								  // jmp  [abs]
	uint32_t abs = (uint32_t)(uintptr_t)&g_ResultTrampolineAddr;
	memcpy(p, &abs, 4); p += 4;

	return true;
}
static void FreeResultStub()
{
	if (!g_ResultPatchBuf) return;
#ifdef PLAT_WINDOWS
	VirtualFree(g_ResultPatchBuf, 0, MEM_RELEASE);
#else
	munmap(g_ResultPatchBuf, 32);
#endif
	g_ResultPatchBuf = nullptr;
}

#endif

bool TransmitThrottle::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	SM_GET_IFACE(GAMEHELPERS, gamehelpers);
	engine = (IVEngineServer *)g_SMAPI->GetEngineFactory(false)("VEngineServer021", nullptr);
	if (!engine) { snprintf(error, maxlength, "Could not get IVEngineServer"); return false; }
	sharesys->AddDependency(myself, "sdkhooks.ext", true, true);
	memset(g_Cache, 0, sizeof(g_Cache));
	g_ThrottleInterval.InstallChangeCallback(OnThrottleIntervalChanged);
	g_flThrottleInterval = g_ThrottleInterval.GetFloat();
	return true;
}

bool TransmitThrottle::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	ConVar_Register(0, this);
	return true;
}

void TransmitThrottle::SDK_OnAllLoaded()
{
#if defined(PLAT_WINDOWS)
	// ** win x86
	ModuleHandle hSDKHooks = FindModule("sdkhooks.ext.2.css.dll");

	const uint8_t sigEntry[] = {
		0x55,						  // push ebp
		0x8B, 0xEC,					  // mov  ebp, esp
		0x53,						  // push ebx
		0x8B, 0xD9,					  // mov  ebx, ecx
		0x8B, 0x0D,					  // mov  ecx, [g_SHPtr]
		0x2A, 0x2A, 0x2A, 0x2A,		  // wildcard
		0x56,						  // push esi
		0x57,						  // push edi
		0x8B, 0x01					  // mov  eax, [ecx]
	};
	const uint8_t sigResult[] = {
		0x8B, 0x0D,					  // mov  ecx, [g_SHPtr]
		0x2A, 0x2A, 0x2A, 0x2A,		  // wildcard
		0x83, 0xF8, 0x03			  // cmp  eax, 3
	};
	const int kEntryPatchLen  = 16;
	const int kResultPatchLen = 9;
	const int kOffsetMin	  = 40;
	const int kOffsetMax	  = 80;

#elif defined(PLAT_LINUX32)
	// ** linux x86
	ModuleHandle hSDKHooks = FindModule("sdkhooks.ext.2.css.so");

	const uint8_t sigEntry[] = {
		0x55,						   // push ebp
		0x89, 0xE5,					   // mov  ebp, esp
		0x53,						   // push ebx
		0x57,						   // push edi
		0x56,						   // push esi
		0x83, 0xEC, 0x1C,			   // sub  esp, 1Ch
		0xE8, 0x00, 0x00, 0x00, 0x00,  // call $+5
		0x5B,						   // pop  ebx
		0x81						   // add  ebx, <imm32>
	};
	const uint8_t sigResult[] = {
		0x83, 0xF8, 0x03,			   // cmp  eax, 3
		0x0F, 0x9D, 0xC2,			   // setnl dl
		0x8D, 0x04, 0x52			   // lea  eax, [edx+edx*2]
	};
	const int kEntryPatchLen  = 16;
	const int kResultPatchLen = 9;
	const int kOffsetMin	  = 90;
	const int kOffsetMax	  = 130;

#else
	// ** linux x86-64
	ModuleHandle hSDKHooks = FindModule("sdkhooks.ext.2.css.so");

	const uint8_t sigEntry[] = {
		0x55,						   // push rbp
		0x48, 0x89, 0xE5,			   // mov  rbp, rsp
		0x41, 0x57,					   // push r15
		0x41, 0x56,					   // push r14
		0x41, 0x54,					   // push r12
		0x53,						   // push rbx
		0x48, 0x89, 0xF3,			   // mov  rbx, rsi
		0x4C, 0x8D					   // lea  r15, [rip+...]
	};
	const uint8_t sigResult[] = {
		0x31, 0xD2,					   // xor  edx, edx
		0x83, 0xF8, 0x03,			   // cmp  eax, 3
		0x0F, 0x9D, 0xC2,			   // setnl dl
		0x8D, 0x34, 0x52			   // lea  esi, [rdx+rdx*2]
	};
	const int kEntryPatchLen  = 16;
	const int kResultPatchLen = 11;
	const int kOffsetMin	  = 80;
	const int kOffsetMax	  = 120;

#endif

	// i dont know why sometimes sdkhook loads late
	if (!hSDKHooks)
	{
		META_CONPRINTF("[TransmitThrottle] Could not find sdkhooks module.\n");
		return;
	}

	void *pFunc = ScanModule(hSDKHooks, sigEntry, sizeof(sigEntry));
	if (!pFunc)
	{
		META_CONPRINTF("[TransmitThrottle] Entry signature not found.\n");
		return;
	}

	uint8_t *pSearch = (uint8_t *)pFunc + kEntryPatchLen;
	void	*pResult = nullptr;
	for (int i = 0; i < 256; i++)
	{
		bool found = true;
		for (int j = 0; j < kResultPatchLen; j++)
		{
			if (sigResult[j] != 0x2A && pSearch[i + j] != sigResult[j])
			{
				found = false;
				break;
			}
		}
		if (found) { pResult = pSearch + i; break; }
	}

	if (!pResult)
	{
		META_CONPRINTF("[TransmitThrottle] Result signature not found.\n");
		return;
	}

	ptrdiff_t offset = (uint8_t *)pResult - (uint8_t *)pFunc;
	if (offset < kOffsetMin || offset > kOffsetMax)
	{
		META_CONPRINTF("[TransmitThrottle] Result sig at unexpected offset %d (expected %d-%d).\n",
					   (int)offset, kOffsetMin, kOffsetMax);
		return;
	}

	union
	{
		void (HookEntry::*mfp)(CCheckTransmitInfo *, bool);
		void *addr;
	} u;
	u.mfp = &HookEntry::Handler_Entry;

	if (!g_PatchEntry.Init(pFunc, u.addr, kEntryPatchLen))
	{
		META_CONPRINTF("[TransmitThrottle] ERROR: Failed to apply entry patch.\n");
		return;
	}

	if (!BuildResultStub())
	{
		META_CONPRINTF("[TransmitThrottle] ERROR: Failed to build result stub.\n");
		g_PatchEntry.Unpatch();
		return;
	}

	if (!g_PatchResult.Init(pResult, g_ResultPatchBuf, kResultPatchLen))
	{
		META_CONPRINTF("[TransmitThrottle] ERROR: Failed to apply result patch.\n");
		FreeResultStub();
		g_PatchEntry.Unpatch();
		return;
	}

	g_ResultTrampolineAddr = g_PatchResult.GetTrampoline();

	META_CONPRINTF("[TransmitThrottle] Patched calling to: interval=%.4f s.\n", g_flThrottleInterval);
}

void TransmitThrottle::SDK_OnUnload()
{
	g_ThrottleInterval.InstallChangeCallback(nullptr);
	g_PatchEntry.Unpatch();
	g_PatchResult.Unpatch();
	FreeResultStub();
	g_ResultTrampolineAddr = nullptr;
}
