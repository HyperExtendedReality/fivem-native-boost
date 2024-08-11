/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include "scrEngine.h"
#include "CrossLibraryInterfaces.h"

#include <CrossBuildRuntime.h>
#include "Hooking.h"

#include <sysAllocator.h>

#include <MinHook.h>
#include <ICoreGameInit.h>

#include <unordered_set>
#include <array>
#include <vector>
#include <utility>


extern void PointerArgumentSafety();
extern bool storyMode;

#if __has_include("scrEngineStubs.h")
#include <scrEngineStubs.h>
#else
inline void HandlerFilter(void* handler)
{
}
#endif

fwEvent<> rage::scrEngine::OnScriptInit;
fwEvent<bool&> rage::scrEngine::CheckNativeScriptAllowed;

static rage::pgPtrCollection<GtaThread>* scrThreadCollection;
static uint32_t activeThreadTlsOffset;

uint32_t* scrThreadId;
static uint32_t* scrThreadCount;

rage::scriptHandlerMgr* g_scriptHandlerMgr;

static bool g_hasObfuscated;

// see https://github.com/ivanmeler/OpenVHook/blob/b5b4d84e76feb05a988e9d69b6b5c164458341cb/OpenVHook/Scripting/ScriptEngine.cpp#L22
#pragma pack(push, 1)
struct NativeRegistration_obf : public rage::sysUseAllocator
{
private:
	uint64_t nextRegistration1;
	uint64_t nextRegistration2;

public:
	rage::scrEngine::NativeHandler handlers[7];

private:
	uint32_t numEntries1;
	uint32_t numEntries2;
	uint32_t pad;
	uint64_t hashes[7 * 2];

public:
	inline NativeRegistration_obf* getNextRegistration()
	{
		uintptr_t result;
		auto v5 = reinterpret_cast<uintptr_t>(&nextRegistration1);
		auto v12 = 2i64;
		auto v13 = v5 ^ nextRegistration2;
		auto v14 = (char*)&result - v5;
		do
		{
			*(DWORD*)&v14[v5] = v13 ^ *(DWORD*)v5;
			v5 += 4i64;
			--v12;
		} while (v12);

		return reinterpret_cast<NativeRegistration_obf*>(result);
	}

	inline void setNextRegistration(NativeRegistration_obf* registration)
	{
		nextRegistration1 = ((uint64_t)&nextRegistration1 << 32) ^ ((uint32_t)&nextRegistration1 << 0) ^ (uint64_t)registration;
		nextRegistration2 = 0;
	}

	inline uint32_t getNumEntries()
	{
		return ((uintptr_t)&numEntries1) ^ numEntries1 ^ numEntries2;
	}

	inline void setNumEntries(uint32_t entries)
	{
		numEntries1 = (uint32_t)&numEntries1 ^ entries;
		numEntries2 = 0;
	}

	inline uint64_t getHash(uint32_t index)
	{
		auto naddr = 16 * index + reinterpret_cast<uintptr_t>(&nextRegistration1) + 0x54;
		auto v8 = 2i64;
		uint64_t nResult;
		auto v11 = (char*)&nResult - naddr;
		auto v10 = naddr ^ *(DWORD*)(naddr + 8);
		do
		{
			*(DWORD*)&v11[naddr] = v10 ^ *(DWORD*)(naddr);
			naddr += 4i64;
			--v8;
		} while (v8);

		return nResult;
	}

	inline void setHash(uint32_t index, uint64_t newHash)
	{
		auto hash = &hashes[index * 2];
		hash[0] = ((uint64_t)hash << 32) ^ ((uint32_t)hash << 0) ^ (uint64_t)newHash;
		hash[1] = 0;
	}
};
#pragma pack(pop)

template<typename TReg>
TReg** registrationTable;

static std::array<rage::scrEngine::NativeHandler, 65536> g_nativeHandlerCache;
static std::array<std::vector<std::pair<uint64_t, rage::scrEngine::NativeHandler>>, 256> g_nativeHandlers;

static std::unordered_set<GtaThread*> g_ownedThreads;

bool IsScriptInited();

struct NativeHash
{
public:
	explicit inline NativeHash(uint64_t hash)
	{
		m_hash = hash;
	}

	inline uint64_t GetHash() const
	{
		return m_hash;
	}

	inline bool operator==(const NativeHash& right) const
	{
		return m_hash == right.m_hash;
	}

	inline bool operator!=(const NativeHash& right) const
	{
		return !(*this == right);
	}

private:
	uint64_t m_hash;
};

namespace std
{
template<>
struct hash<NativeHash>
{
	inline size_t operator()(const NativeHash& hash) const
	{
		return hash.GetHash();
	}
};
}

namespace rage
{

// static std::unordered_map<uint64_t, scrEngine::NativeHandler> g_nativeHandlerCache;

pgPtrCollection<GtaThread>* scrEngine::GetThreadCollection()
{
	// return reinterpret_cast<pgPtrCollection<GtaThread>*>(0x1983310);
	return scrThreadCollection;
}

/*void scrEngine::SetInitHook(void(*hook)(void*))
{
	g_hooksDLL->SetHookCallback(StringHash("scrInit"), hook);
}*/

// static scrThread*& scrActiveThread = *(scrThread**)0x1849AE0;

scrThread* scrEngine::GetActiveThread()
{
	return *reinterpret_cast<scrThread**>(hook::get_tls() + activeThreadTlsOffset);
}

void scrEngine::SetActiveThread(scrThread* thread)
{
	*reinterpret_cast<scrThread**>(hook::get_tls() + activeThreadTlsOffset) = thread;
}

// static uint32_t& scrThreadId = *(uint32_t*)0x1849ADC;
// static uint32_t& scrThreadCount = *(uint32_t*)0x1849AF8;

static std::vector<std::function<void()>> g_onScriptInitQueue;

void scrEngine::CreateThread(GtaThread* thread)
{
	if (!IsScriptInited())
	{
		g_onScriptInitQueue.emplace_back([thread]()
		{
			CreateThread(thread);
		});
		return;
	}

	// get a free thread slot
	auto collection = GetThreadCollection();
	auto slot = std::find_if(collection->begin(), collection->end(),
	[](const auto& t)
	{
		return t == nullptr || t->GetContext()->ThreadId == 0;
	});

	if (slot == collection->end())
	{
		return;
	}

	auto context = thread->GetContext();
	context->ThreadId = (*scrThreadId)++;
	thread->SetScriptName(va("scr_%d", (*scrThreadCount) + 1));
	context->ScriptHash = (*scrThreadCount) + 1;
	(*scrThreadCount)++;

	*slot = thread;
	g_ownedThreads.insert(thread);
}

uint64_t MapNative(uint64_t inNative);
void ReviveNative(uint64_t inNative);

template<typename TReg>
bool RegisterNativeOverride(uint64_t hash, scrEngine::NativeHandler handler)
{
	TReg*& registration = registrationTable<TReg>[(hash & 0xFF)];

	uint64_t origHash = hash;

	// remove cached fastpath native
	g_nativeHandlerCache[NativeHash{ origHash }.GetHash() & 0xFFFF] = nullptr;

	hash = MapNative(hash);

	auto table = registrationTable<TReg>[hash & 0xFF];

	for (; table; table = table->getNextRegistration())
	{
		for (int i = 0; i < table->getNumEntries(); i++)
		{
			if (hash == table->getHash(i))
			{
				if (g_hasObfuscated)
				{
					handler = (scrEngine::NativeHandler)EncodePointer(handler);
				}

				table->handlers[i] = handler;

				return true;
			}
		}
	}

	return false;
}

template<typename TReg>
void RegisterNativeDo(uint64_t hash, scrEngine::NativeHandler handler)
{
	// re-implemented here as the game's own function is obfuscated
	TReg*& registration = registrationTable<TReg>[(hash & 0xFF)];

	// see if there's somehow an entry by this name already
	if (RegisterNativeOverride<TReg>(hash, handler))
	{
		return;
	}

	if (registration->getNumEntries() == 7)
	{
		TReg* newRegistration = new TReg();
		newRegistration->setNextRegistration(registration);
		newRegistration->setNumEntries(0);

		// should also set the entry in the registration table
		registration = newRegistration;
	}

	// add the entry to the list
	if (g_hasObfuscated)
	{
		handler = (scrEngine::NativeHandler)EncodePointer(handler);
	}

	uint32_t index = registration->getNumEntries();
	registration->setHash(index, hash);
	registration->handlers[index] = handler;

	registration->setNumEntries(index + 1);
}

void RegisterNative(uint64_t hash, scrEngine::NativeHandler handler)
{
	RegisterNativeDo<NativeRegistration_obf>(hash, handler);
}

void scrEngine::RegisterNativeHandler(const char* nativeName, NativeHandler handler)
{
	RegisterNativeHandler(HashString(nativeName), handler);
}

void scrEngine::RegisterNativeHandler(uint64_t nativeIdentifier, NativeHandler handler)
{
	uint8_t index = nativeIdentifier & 0xFF;
	g_nativeHandlers[index].emplace_back(nativeIdentifier, handler);
}

void scrEngine::ReviveNativeHandler(uint64_t nativeIdentifier, NativeHandler handler)
{
	RegisterNativeHandler(nativeIdentifier, handler);
	if (MapNative(nativeIdentifier) == nativeIdentifier)
	{
		// If two builds share the same maxVersion index, e.g., 2545 and 2612,
		// and a script command was removed, then the native may be found in the
		// mapping table instead of the unmapped table.
		ReviveNative(nativeIdentifier);
	}
}

static InitFunction initFunction([]()
{
	scrEngine::OnScriptInit.Connect([]()
	{
		auto doReg = []()
		{
			for (const auto& handlerList : g_nativeHandlers)
			{
				for (const auto& handler : handlerList)
				{
					RegisterNative(handler.first, handler.second);
				}
			}
			for (auto& handlerList : g_nativeHandlers)
			{
				handlerList.clear();
			}
		};

		doReg();
		PointerArgumentSafety();
		std::fill(g_nativeHandlerCache.begin(), g_nativeHandlerCache.end(), nullptr);
		doReg();

		for (auto& entry : g_onScriptInitQueue)
		{
			entry();
		}
		g_onScriptInitQueue.clear();
	},
	50000);
});

template<typename TReg>
scrEngine::NativeHandler GetNativeHandlerDo(uint64_t origHash, uint64_t hash)
{
	auto table = registrationTable<TReg>[hash & 0xFF];
	scrEngine::NativeHandler handler = nullptr;

	for (; table; table = table->getNextRegistration())
	{
		for (int i = 0; i < table->getNumEntries(); i++)
		{
			if (hash == table->getHash(i))
			{
				handler = (scrEngine::NativeHandler) /*DecodePointer(*/ table->handlers[i] /*)*/;
				HandlerFilter(&handler);

				if (handler)
				{
#include "BlockedNatives.h"
				}

				g_nativeHandlerCache[index] = handler;

				break;
			}
		}
	}

	return handler;
}

scrEngine::NativeHandler GetNativeHandlerWrap(uint64_t origHash, uint64_t hash)
{
	uint8_t index = hash & 0xFF;
	for (const auto& pair : g_nativeHandlers[index])
	{
		if (pair.first == hash)
		{
			auto handler = pair.second;
			HandlerFilter(&handler);
			return handler;
		}
	}
	return nullptr;
}

scrEngine::NativeHandler scrEngine::GetNativeHandler(uint64_t hash)
{
	uint16_t index = hash & 0xFFFF;
	if (auto cachedHandler = g_nativeHandlerCache[index])
	{
		return cachedHandler;
	}

	auto handler = GetNativeHandlerWrap(hash, MapNative(hash));
	g_nativeHandlerCache[index] = handler;
	return handler;
}
}

static int ReturnTrue()
{
	return true;
}

static int (*g_origReturnTrue)(void* a1, void* a2);

static int ReturnTrueFromScript(void* a1, void* a2)
{
	return !Instance<ICoreGameInit>::Get()->HasVariable("storyMode") || g_origReturnTrue(a1, a2);
}

static void (*g_CTheScripts__Shutdown)(void);

static void CTheScripts__Shutdown()
{
	g_CTheScripts__Shutdown();

	for (auto& thread : g_ownedThreads)
	{
		thread->Reset(thread->GetContext()->ScriptHash, nullptr, 0);
	}
}

static int (*g_origNoScript)(void*, int);

static int JustNoScript(GtaThread* thread, int a2)
{
	if (Instance<ICoreGameInit>::Get()->HasVariable("storyMode"))
	{
		return g_origNoScript(thread, a2);
	}

	if (g_ownedThreads.find(thread) != g_ownedThreads.end())
	{
		thread->Run(0);
	}

	return thread->GetContext()->State;
}

static void (*origStartupScript)();

static void StartupScriptWrap()
{
	for (auto& thread : g_ownedThreads)
	{
		if (!thread->GetContext()->ThreadId)
		{
			rage::scrEngine::CreateThread(thread);
		}
	}

	origStartupScript();
}

static InitFunction initFunction([]
{
	if (xbr::IsGameBuildOrGreater<2612>())
	{
		// IS_BIT_SET is missing in b2612+, re-adding for compatibility
		rage::scrEngine::OnScriptInit.Connect([]()
		{
			rage::scrEngine::ReviveNativeHandler(0xE2D0C323A1AE5D85 /* 2545 hash */, [](rage::scrNativeCallContext* ctx)
			{
				bool result = false;

				auto value = ctx->GetArgument<uint32_t>(0);
				auto offset = ctx->GetArgument<int>(1);

				if (offset < 32)
				{
					result = (value & (1 << offset)) != 0;
				}

				ctx->SetResult<int>(0, result);
			});
		});
	}
});

static HookFunction hookFunction([]()
{
	char* location = nullptr;

	if (xbr::IsGameBuildOrGreater<3258>())
	{
		location = hook::pattern("48 8B C8 EB ? 33 C9 48 8B 05").count(1).get(0).get<char>(10);
	}
	else if (xbr::IsGameBuildOrGreater<2545>())
	{
		location = hook::pattern("48 8B C8 EB 03 49 8B CD 48 8B 05").count(1).get(0).get<char>(11);
	}
	else
	{
		location = hook::pattern("48 8B C8 EB 03 48 8B CB 48 8B 05").count(1).get(0).get<char>(11);
	}

	scrThreadCollection = reinterpret_cast<decltype(scrThreadCollection)>(location + *(int32_t*)location + 4);

	activeThreadTlsOffset = *hook::pattern("48 8B 04 D0 4A 8B 14 00 48 8B 01 F3 44 0F 2C 42 20").count(1).get(0).get<uint32_t>(-4);

	{
		if (xbr::IsGameBuildOrGreater<3258>())
		{
			scrThreadId = hook::get_address<uint32_t*>(hook::get_pattern("8B 15 ? ? ? ? 48 8B 05 ? ? ? ? FF C2 89 15 ? ? ? ? 48 8B 0C F8", 2));
		}
		else if (xbr::IsGameBuildOrGreater<2612>())
		{
			scrThreadId = hook::get_address<uint32_t*>(hook::get_pattern("8B 15 ? ? ? ? 48 8B 05 ? ? ? ? FF C2 89 15 ? ? ? ? 48 8B 0C D8", 2));
		}
		else if (xbr::IsGameBuildOrGreater<2545>())
		{
			scrThreadId = hook::get_address<uint32_t*>(hook::get_pattern("8B 15 ? ? ? ? 48 8B 05 ? ? ? ? FF C2 89 15 ? ? ? ? E9", 2));
		}
		else if (xbr::IsGameBuildOrGreater<2372>())
		{
			scrThreadId = hook::get_address<uint32_t*>(hook::get_pattern("8B 15 ? ? ? ? 48 8B 05 ? ? ? ? FF C2 89", 2));
		}
		else
		{
			// memory layout dependent
			scrThreadId = hook::get_address<uint32_t*>(hook::get_pattern("33 FF 48 85 C0 74 08 48 8B C8 E8", -9)) - 2;
		}

		location = xbr::IsGameBuildOrGreater<2545>() ? hook::get_pattern<char>("FF 0D ? ? ? ? 48 8B D9 75", 2) : hook::get_pattern<char>("FF 0D ? ? ? ? 48 8B F9", 2);
		scrThreadCount = reinterpret_cast<decltype(scrThreadCount)>(location + *(int32_t*)location + 4);

		location = hook::pattern("76 32 48 8B 53 40").count(1).get(0).get<char>(9);
		registrationTable<NativeRegistration_obf> = reinterpret_cast<decltype(registrationTable<NativeRegistration_obf>)>(location + *(int32_t*)location + 4);
	}

	location = hook::pattern("74 17 48 8B C8 E8 ? ? ? ? 48 8D 0D").count(1).get(0).get<char>(13);

	g_scriptHandlerMgr = reinterpret_cast<decltype(g_scriptHandlerMgr)>(location + *(int32_t*)location + 4);

	// script re-init
	{
		auto location = hook::get_pattern("83 FB FF 0F 84 D6 00 00 00", -0x37);

		MH_Initialize();
		MH_CreateHook(location, StartupScriptWrap, (void**)&origStartupScript);
		MH_EnableHook(MH_ALL_HOOKS);
	}

	{
		MH_Initialize();

		// temp: kill stock scripts
		// NOTE: before removing make sure scrObfuscation in fivem-private can handle opcode 0x2C (NATIVE)
		// hook::jump(hook::pattern("48 83 EC 20 80 B9 ? 01 00 00 00 8B FA").count(1).get(0).get<void>(-0xB), JustNoScript);
		MH_CreateHook(hook::pattern("48 83 EC 20 80 B9 ? 01 00 00 00 8B FA").count(1).get(0).get<void>(-0xB), JustNoScript, (void**)&g_origNoScript);

		// make all CGameScriptId instances return 'true' in matching function (mainly used for 'is script allowed to use this object' checks)
		// hook::jump(hook::pattern("74 3C 48 8B 01 FF 50 10 84 C0").count(1).get(0).get<void>(-0x1A), ReturnTrue);
		if (xbr::IsGameBuildOrGreater<2802>())
		{
			MH_CreateHook(hook::pattern("74 41 48 8B 01 FF 50 10 84 C0").count(1).get(0).get<void>(-0x1A), ReturnTrueFromScript, (void**)&g_origReturnTrue);
		}
		else
		{
			MH_CreateHook(hook::pattern("74 3C 48 8B 01 FF 50 10 84 C0").count(1).get(0).get<void>(-0x1A), ReturnTrueFromScript, (void**)&g_origReturnTrue);
		}

		// replace `startup` initialization with resetting all owned threads
		// hook::jump(hook::get_pattern("48 63 18 83 FB FF 0F 84 D6", -0x34), ResetOwnedThreads);
		// MH_CreateHook(hook::get_pattern("48 63 18 83 FB FF 0F 84 D6", -0x34), ResetOwnedThreads, (void**)&g_origResetOwnedThreads);
		if (xbr::IsGameBuildOrGreater<2802>())
		{
			MH_CreateHook(hook::get_pattern("48 8B 01 FF 50 30 E8 ? ? ? ? E8", -0x61), CTheScripts__Shutdown, (void**)&g_CTheScripts__Shutdown);
		}
		else
		{
			MH_CreateHook(hook::get_pattern("48 8B 0D ? ? ? ? 33 D2 48 8B 01 FF 10", -0x58), CTheScripts__Shutdown, (void**)&g_CTheScripts__Shutdown);
		}

		MH_EnableHook(MH_ALL_HOOKS);
	}
});
