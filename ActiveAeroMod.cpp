#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <cctype>
#include <cstdlib>
#include <initializer_list>

static const DWORD64 kLapCheckRva = 0x23659A5;
static const BYTE kLapCheckContext[6] = { 0x3B, 0x8F, 0x70, 0x08, 0x00, 0x00 };
static const BYTE kOriginalLapBranch[2] = { 0x7C, 0x05 };
static const BYTE kNopBranch[2] = { 0x90, 0x90 };
static const BYTE kNoDrsLapBranch[2] = { 0xEB, 0x05 };

static const DWORD64 kGapCheckRva = 0x2365AB0;
static const DWORD64 kGapCheckContinueRva = 0x2365ABC;
static const DWORD64 kGapCheckRejectRva = kGapCheckContinueRva + 0x2E;
static const BYTE kOriginalGapInstructions[12] = {
    0x0F, 0x28, 0xC6, 0xF3, 0x0F, 0x5C, 0x03, 0x0F, 0x2F, 0xC7, 0x77, 0x2E
};
static const BYTE kPatchWindowJump[2] = { 0xEB, 0x0A };

static const BYTE kOvertakeCheckSignature[33] = {
    0x41, 0x0F, 0xB6, 0x86, 0x6D, 0x08, 0x00, 0x00,
    0xFE, 0xC8, 0xA8, 0xFD,
    0x41, 0x0F, 0xB6, 0x87, 0x6D, 0x08, 0x00, 0x00,
    0x0F, 0x5B, 0xC0,
    0x0F, 0x94, 0xC1,
    0xFE, 0xC8, 0xA8, 0xFD,
    0x0F, 0x94, 0xC0
};

static const DWORD kCarLapsOffset = 0x7E4;
static const DWORD kCarDrsLapOffset = 0x870;
static const DWORD kCarIndexOffset = 0x710;
static const DWORD kCarDistanceOffset = 0x194;
static const int kCarCount = 32;
static const int kDetectionQueueCapacity = 64;
static const float kDefaultAssistDistanceMeters = 2000.0f;
static const DWORD kSettingsPollIntervalMs = 100;
static const char kUnknownCircuitName[] = "__UNKNOWN__";


enum DrsMode : LONG {
    DrsModeOriginal = 0,
    DrsModeStraight = 1,
    DrsModeNone = 2
};

struct DetectionQueue {
    volatile LONG writeCount;
    LONG reserved;
    BYTE* carPointers[kDetectionQueueCapacity];
};

static DetectionQueue g_detectionQueue = {};
static volatile BYTE g_overtakeAssistByCar[kCarCount] = {};
static BYTE* g_assistedCarPointers[kCarCount] = {};
static float g_assistEndDistances[kCarCount] = {};
static float g_assistDurationsMeters[kCarCount] = {};
static volatile LONG g_overtakeAssistEnabled = 0;

static char g_currentCircuitName[64] = {};
static float g_assistDistanceMeters = kDefaultAssistDistanceMeters;
static bool g_hasCircuitDistanceOverride = false;

static wchar_t g_modDirectory[MAX_PATH] = {};
static BYTE* g_gameImageBase = nullptr;
static BYTE* g_straightModeGapTrampoline = nullptr;
static BYTE* g_originalDrsAssistGapTrampoline = nullptr;
static BYTE* g_noDrsGapTrampoline = nullptr;
static BYTE* g_overtakeAssistTrampoline = nullptr;
static BYTE g_straightModeGapPatch[12] = {};
static BYTE g_originalDrsAssistGapPatch[12] = {};
static BYTE g_noDrsGapPatch[12] = {};
static BYTE g_overtakeAssistPatch[33] = {};
static BYTE* g_overtakeCheckSite = nullptr;
static bool g_overtakeAssistPatchInstalled = false;

// Logging and guarded memory access
static void LogMessage(const char* fmt, ...) {
    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, _countof(path), _TRUNCATE, L"%s\\ActiveAeroMod.log", g_modDirectory);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") != 0 || !f) return;
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    std::fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u ", st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fputc('\n', f);
    std::fclose(f);
}

static bool IsMemoryReadable(const void* address, size_t size) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!address || !VirtualQuery(address, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t regionEnd = regionStart + mbi.RegionSize;
    if (start < regionStart || start > regionEnd || size > regionEnd - start) return false;
    const DWORD protection = mbi.Protect & 0xFF;
    return protection == PAGE_READONLY || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
           protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

static bool WriteExecutableMemory(BYTE* site, const BYTE* bytes, size_t size) {
    DWORD oldProtect = 0;
    if (!site || !bytes || !VirtualProtect(site, size, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    std::memcpy(site, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), site, size);
    DWORD ignored = 0;
    const bool restored = VirtualProtect(site, size, oldProtect, &ignored) != 0;
    return restored;
}

static bool MatchesEitherSequence(const BYTE* actual, const BYTE* first, const BYTE* second, size_t size) {
    return std::memcmp(actual, first, size) == 0 || std::memcmp(actual, second, size) == 0;
}

static bool FindUniqueExecutableSignature(const BYTE* pattern, size_t patternSize, BYTE** found, int* matches) {
    if (!g_gameImageBase || !pattern || !found || !matches) return false;
    *found = nullptr;
    *matches = 0;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_gameImageBase);
    if (!IsMemoryReadable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_gameImageBase + dos->e_lfanew);
    if (!IsMemoryReadable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return false;
    }
    const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const DWORD begin = section->VirtualAddress;
        DWORD extent = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
        if (begin >= imageSize) continue;
        if (extent > imageSize - begin) extent = imageSize - begin;
        if (extent < patternSize) continue;
        BYTE* start = g_gameImageBase + begin;
        for (DWORD off = 0; off <= extent - patternSize; ++off) {
            if (std::memcmp(start + off, pattern, patternSize) == 0) {
                ++(*matches);
                *found = start + off;
                if (*matches > 1) return false;
            }
        }
    }
    return *matches == 1;
}

static bool ValidateTargetGameBuild() {
    BYTE* lap = g_gameImageBase + kLapCheckRva;
    BYTE* gap = g_gameImageBase + kGapCheckRva;
    if (!IsMemoryReadable(lap - sizeof(kLapCheckContext), sizeof(kLapCheckContext) + 2) ||
        !IsMemoryReadable(gap, sizeof(kOriginalGapInstructions))) {
        LogMessage("DRS signature check failed: target addresses are unreadable; "
                   "no DRS changes made. image base=%p", g_gameImageBase);
        return false;
    }
    const bool lapContextOk =
        std::memcmp(lap - sizeof(kLapCheckContext), kLapCheckContext, sizeof(kLapCheckContext)) == 0;
    const bool lapBranchOk = std::memcmp(lap, kOriginalLapBranch, 2) == 0 || std::memcmp(lap, kNopBranch, 2) == 0;
    const bool gapOk = std::memcmp(gap, kOriginalGapInstructions, sizeof(kOriginalGapInstructions)) == 0;
    if (!lapContextOk || !lapBranchOk || !gapOk) {
        LogMessage("DRS signature mismatch; nothing changed. image base=%p", g_gameImageBase);
        LogMessage("lap context=%02X %02X %02X %02X %02X %02X; lap branch=%02X %02X",
            lap[-6], lap[-5], lap[-4], lap[-3], lap[-2], lap[-1], lap[0], lap[1]);
        LogMessage("gap bytes=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            gap[0], gap[1], gap[2], gap[3], gap[4], gap[5], gap[6], gap[7], gap[8], gap[9], gap[10], gap[11]);
        LogMessage("Expected Epic signatures at RVA 0x%llX and 0x%llX; "
                   "confirm Ghidra image base 0x140000000.", kLapCheckRva, kGapCheckRva);
        return false;
    }
    LogMessage("Epic DRS signatures validated (image base=%p, lap RVA=0x%llX, gap RVA=0x%llX).",
               g_gameImageBase, kLapCheckRva, kGapCheckRva);
    return true;
}

//code emission
static void EmitBytes(BYTE*& p, std::initializer_list<BYTE> bytes) {
    for (BYTE b : bytes) *p++ = b;
}
static void EmitUInt64(BYTE*& p, uint64_t value) {
    std::memcpy(p, &value, sizeof(value));
    p += sizeof(value);
}
static void EmitAbsoluteJump(BYTE*& p, const void* destination) {
    EmitBytes(p, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 }); // jmp qword ptr [rip]
    EmitUInt64(p, reinterpret_cast<uint64_t>(destination));
}
static void BuildAbsoluteJumpStub(BYTE* out, size_t outSize, const void* destination) {
    std::memset(out, 0x90, outSize);
    out[0] = 0x48; out[1] = 0xB8; // mov rax, imm64
    const uint64_t addr = reinterpret_cast<uint64_t>(destination);
    std::memcpy(out + 2, &addr, sizeof(addr));
    out[10] = 0xFF; out[11] = 0xE0; // jmp rax
}

static bool BuildGapCheckTrampoline(bool bypassLongGapLimit, bool rejectEveryGap,
                                    BYTE* patchBytes, BYTE** trampolineOut) {
    if (!patchBytes || !trampolineOut) return false;
    *trampolineOut = static_cast<BYTE*>(VirtualAlloc(
        nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!*trampolineOut) return false;
    BYTE* p = *trampolineOut;

    EmitBytes(p, { 0x0F, 0x28, 0xC6 });
    EmitBytes(p, { 0xF3, 0x0F, 0x5C, 0x03 });
    EmitBytes(p, { 0x0F, 0x2F, 0xC7 });

    if (rejectEveryGap) {
        EmitAbsoluteJump(p, g_gameImageBase + kGapCheckRejectRva);
    } else {
        EmitBytes(p, { 0x77, 0x00 });                  // ja gap-too-large path
        BYTE* jaDisplacement = p - 1;

        EmitBytes(p, { 0x51 });                        // push rcx
        EmitBytes(p, { 0x50 });                        // push rax
        EmitBytes(p, { 0x48, 0xB8 }); EmitUInt64(p, reinterpret_cast<uint64_t>(&g_detectionQueue));
        EmitBytes(p, { 0x8B, 0x08 });                  // mov ecx, [rax] (writeCount counter)
        EmitBytes(p, { 0x83, 0xE1, kDetectionQueueCapacity - 1 });        // and ecx, kDetectionQueueCapacity-1
        EmitBytes(p, { 0x48, 0x89, 0x7C, 0xC8, 0x08 });// mov [rax+rcx*8+8], rdi (car object)
        EmitBytes(p, { 0xF0, 0xFF, 0x00 });            // lock inc dword ptr [rax]
        EmitBytes(p, { 0x58 });                        // pop rax
        EmitBytes(p, { 0x59 });                        // pop rcx
        EmitAbsoluteJump(p, g_gameImageBase + kGapCheckContinueRva);

        BYTE* gapTooLarge = p;
        *jaDisplacement = static_cast<BYTE>(gapTooLarge - (jaDisplacement + 1));
        // Straight Mode bypasses the game's >1 s rejection; Original DRS mode
        // preserves it by jumping to the original conditional-branch target.
        EmitAbsoluteJump(p, g_gameImageBase + (bypassLongGapLimit ? kGapCheckContinueRva : kGapCheckRejectRva));
    }

    FlushInstructionCache(GetCurrentProcess(), *trampolineOut, static_cast<SIZE_T>(p - *trampolineOut));
    BuildAbsoluteJumpStub(patchBytes, 12, *trampolineOut);
    return true;
}

static bool BuildGapCheckTrampolines() {
    if (!BuildGapCheckTrampoline(true, false, g_straightModeGapPatch, &g_straightModeGapTrampoline)) {
        LogMessage("VirtualAlloc failed for Straight Mode gap cave.");
        return false;
    }
    if (!BuildGapCheckTrampoline(false, false, g_originalDrsAssistGapPatch, &g_originalDrsAssistGapTrampoline)) {
        LogMessage("VirtualAlloc failed for original-DRS assistance cave.");
        return false;
    }
    if (!BuildGapCheckTrampoline(false, true, g_noDrsGapPatch, &g_noDrsGapTrampoline)) {
        LogMessage("VirtualAlloc failed for No-DRS gap cave.");
        return false;
    }
    return true;
}

static bool BuildOvertakeAssistTrampoline() {
    int matches = 0;
    if (!FindUniqueExecutableSignature(kOvertakeCheckSignature, sizeof(kOvertakeCheckSignature),
                                       &g_overtakeCheckSite, &matches)) {
        LogMessage("Overtake assistance disabled: 33-byte signature found %d times "
                   "(expected exactly once). DRS patch remains available.", matches);
        g_overtakeCheckSite = nullptr;
        return false;
    }
    if (!IsMemoryReadable(g_overtakeCheckSite, sizeof(kOvertakeCheckSignature))) {
        LogMessage("Overtake probability assistance disabled: signature target is not readable.");
        g_overtakeCheckSite = nullptr;
        return false;
    }

    g_overtakeAssistTrampoline = static_cast<BYTE*>(VirtualAlloc(
        nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_overtakeAssistTrampoline) {
        LogMessage("Overtake probability assistance disabled: VirtualAlloc failed for cave.");
        g_overtakeCheckSite = nullptr;
        return false;
    }

    BYTE* p = g_overtakeAssistTrampoline;
    EmitBytes(p, { 0x0F, 0x5B, 0xC0 });                // cvtdq2ps xmm0, xmm0 (original instruction)
    EmitBytes(p, { 0x48, 0xB8 }); EmitUInt64(p, reinterpret_cast<uint64_t>(&g_overtakeAssistEnabled));
    EmitBytes(p, { 0x83, 0x38, 0x00 });                // cmp dword ptr [rax], 0
    EmitBytes(p, { 0x74, 0x00 });                      // je original branch
    BYTE* originalDisplacement = p - 1;

    // Optional overtake assistance active: replace the game's DRS asymmetry test with our per-attacker assist flag.
    EmitBytes(p, { 0x41, 0x0F, 0xB6, 0x8E, 0x10, 0x07, 0x00, 0x00 }); // movzx ecx, byte [r14+0x710]
    EmitBytes(p, { 0x83, 0xE1, kCarCount - 1 });            // and ecx, kCarCount-1
    EmitBytes(p, { 0x48, 0xB8 }); EmitUInt64(p, reinterpret_cast<uint64_t>(g_overtakeAssistByCar));
    EmitBytes(p, { 0x0F, 0xB6, 0x0C, 0x08 });          // movzx ecx, byte [rax+rcx]
    EmitBytes(p, { 0x31, 0xC0 });                      // xor eax, eax: al=0; defender DRS no longer blocks bonus
    EmitAbsoluteJump(p, g_overtakeCheckSite + sizeof(kOvertakeCheckSignature));

    BYTE* originalPath = p;
    *originalDisplacement = static_cast<BYTE>(originalPath - (originalDisplacement + 1));
    for (size_t i = 0; i < sizeof(kOvertakeCheckSignature); ++i) {
        if (i >= 20 && i < 23) continue;
        *p++ = kOvertakeCheckSignature[i];
    }
    EmitAbsoluteJump(p, g_overtakeCheckSite + sizeof(kOvertakeCheckSignature));
    FlushInstructionCache(GetCurrentProcess(), g_overtakeAssistTrampoline,
                          static_cast<SIZE_T>(p - g_overtakeAssistTrampoline));
    BuildAbsoluteJumpStub(g_overtakeAssistPatch, sizeof(g_overtakeAssistPatch), g_overtakeAssistTrampoline);

    DWORD64 rva = static_cast<DWORD64>(g_overtakeCheckSite - g_gameImageBase);
    LogMessage("Overtake signature found at RVA 0x%llX; per-car assistance is "
               "controlled by settings.ini.", rva);
    return true;
}

static bool ReplaceGapCheckPatch(const BYTE* bytes) {
    BYTE* site = g_gameImageBase + kGapCheckRva;
    if (!WriteExecutableMemory(site, kPatchWindowJump, 2)) return false;
    if (!WriteExecutableMemory(site + 2, bytes + 2, 10)) return false;
    return WriteExecutableMemory(site, bytes, 2);
}

static bool ApplyDrsMode(LONG drsMode, bool overtakeAssistRequested) {
    BYTE* lap = g_gameImageBase + kLapCheckRva;
    BYTE* gap = g_gameImageBase + kGapCheckRva;

    if (drsMode < DrsModeOriginal || drsMode > DrsModeNone) drsMode = DrsModeStraight;
    const bool overtakeAssistEnabled = overtakeAssistRequested &&
        g_overtakeAssistPatchInstalled && drsMode != DrsModeNone;
    const bool useOriginalDrsAssistTrampoline = drsMode == DrsModeOriginal && overtakeAssistEnabled;
    const BYTE* lapTarget = drsMode == DrsModeStraight ? kNopBranch :
        (drsMode == DrsModeNone ? kNoDrsLapBranch : kOriginalLapBranch);
    const BYTE* gapTarget = drsMode == DrsModeNone ? g_noDrsGapPatch :
        (drsMode == DrsModeStraight ? g_straightModeGapPatch :
            (useOriginalDrsAssistTrampoline ? g_originalDrsAssistGapPatch : kOriginalGapInstructions));

    if (!IsMemoryReadable(lap, 2) || !IsMemoryReadable(gap, sizeof(kOriginalGapInstructions))) {
        LogMessage("DRS transition aborted: target bytes are no longer readable.");
        return false;
    }
    const bool lapBytesKnown = MatchesEitherSequence(lap, kOriginalLapBranch, kNopBranch, 2) ||
        std::memcmp(lap, kNoDrsLapBranch, sizeof(kNoDrsLapBranch)) == 0;
    if (!lapBytesKnown) {
        LogMessage("DRS transition aborted: lap branch bytes changed after validation.");
        return false;
    }

    if (std::memcmp(gap, kOriginalGapInstructions, sizeof(kOriginalGapInstructions)) != 0 &&
        std::memcmp(gap, g_straightModeGapPatch, sizeof(g_straightModeGapPatch)) != 0 &&
        std::memcmp(gap, g_originalDrsAssistGapPatch, sizeof(g_originalDrsAssistGapPatch)) != 0 &&
        std::memcmp(gap, g_noDrsGapPatch, sizeof(g_noDrsGapPatch)) != 0) {
        LogMessage("DRS transition aborted: gap bytes no longer match original or this DLL's patches.");
        return false;
    }

    BYTE previousLap[2] = { lap[0], lap[1] };
    if (!WriteExecutableMemory(lap, lapTarget, 2)) {
        LogMessage("VirtualProtect failed at lap branch.");
        return false;
    }
    if (!ReplaceGapCheckPatch(gapTarget)) {
        WriteExecutableMemory(lap, previousLap, 2);
        LogMessage("Gap patch transition failed; lap branch rolled back where possible.");
        return false;
    }

    InterlockedExchange(&g_overtakeAssistEnabled, overtakeAssistEnabled ? 1 : 0);

    const char* modeName = drsMode == DrsModeOriginal ? "original DRS" :
        (drsMode == DrsModeStraight ? "Straight Mode" : "No DRS");
    LogMessage("DRS mode applied: %s; overtake_assist=%s%s.", modeName,
        overtakeAssistRequested ? "ON" : "OFF",
        (overtakeAssistRequested && !overtakeAssistEnabled)
            ? " (assist patch unavailable or mode has no eligible DRS detections)" : "");
    if (drsMode == DrsModeNone) {
        LogMessage("No DRS mode: lap and gap checks always take their original DRS-unavailable paths.");
    } else if (drsMode == DrsModeStraight) {
        LogMessage("Straight Mode: lap and gap restrictions bypassed; optional assistance "
                   "uses qualifying gap detections (%.0f m).",
            g_assistDistanceMeters);
    } else if (overtakeAssistEnabled) {
        LogMessage("Original DRS eligibility is preserved; qualifying gap detections "
                   "can receive probability assistance (%.0f m).",
            g_assistDistanceMeters);
    } else {
        LogMessage("Original DRS rules restored without probability assistance.");
    }
    return true;
}

static void ClearOvertakeAssistState() {
    for (int i = 0; i < kCarCount; ++i) {
        g_overtakeAssistByCar[i] = 0;
        g_assistedCarPointers[i] = nullptr;
        g_assistEndDistances[i] = 0.0f;
        g_assistDurationsMeters[i] = 0.0f;
    }
}

static void StartOvertakeAssistForCar(BYTE* car) {
    if (!car || !IsMemoryReadable(car + kCarIndexOffset, 1) || !IsMemoryReadable(car + kCarLapsOffset, 4) ||
        !IsMemoryReadable(car + kCarDrsLapOffset, 4) || !IsMemoryReadable(car + kCarDistanceOffset, 4)) return;
    const int lap = *reinterpret_cast<int*>(car + kCarLapsOffset);
    const int drsLap = *reinterpret_cast<int*>(car + kCarDrsLapOffset);
    if (lap < drsLap) return;
    const int index = car[kCarIndexOffset] & (kCarCount - 1);
    const float dist = *reinterpret_cast<float*>(car + kCarDistanceOffset);
    const bool sameDetection = g_overtakeAssistByCar[index] && g_assistedCarPointers[index] == car &&
        g_assistDurationsMeters[index] > 0.0f &&
        dist < g_assistEndDistances[index] - g_assistDurationsMeters[index] + 100.0f;
    if (!sameDetection) {
        g_assistDurationsMeters[index] = g_assistDistanceMeters;
        g_assistEndDistances[index] = dist + g_assistDurationsMeters[index];
        LogMessage("Overtake assist: circuit=%s, distance=%.0f m, car index %d, "
                   "lap %d, active until %.1f m (detection %.1f m).",
            g_currentCircuitName[0] ? g_currentCircuitName : kUnknownCircuitName, g_assistDurationsMeters[index], index, lap,
            g_assistEndDistances[index], dist);
    }
    g_assistedCarPointers[index] = car;
    g_overtakeAssistByCar[index] = 1;
}

static void ProcessDetectionQueue(LONG& readIndex) {
    const LONG writeCount = g_detectionQueue.writeCount;
    if (writeCount - readIndex > kDetectionQueueCapacity) readIndex = writeCount - kDetectionQueueCapacity;
    for (; readIndex < writeCount; ++readIndex) {
        BYTE* car = g_detectionQueue.carPointers[readIndex & (kDetectionQueueCapacity - 1)];
        StartOvertakeAssistForCar(car);
    }
}

static void ExpireOvertakeAssists() {
    for (int i = 0; i < kCarCount; ++i) {
        if (!g_overtakeAssistByCar[i]) continue;
        BYTE* car = g_assistedCarPointers[i];
        if (!car || !IsMemoryReadable(car + kCarDistanceOffset, 4)) {
            g_overtakeAssistByCar[i] = 0;
            g_assistedCarPointers[i] = nullptr;
            g_assistEndDistances[i] = 0.0f;
            g_assistDurationsMeters[i] = 0.0f;
            continue;
        }
        const float dist = *reinterpret_cast<float*>(car + kCarDistanceOffset);
        const float duration = g_assistDurationsMeters[i] > 0.0f
            ? g_assistDurationsMeters[i] : kDefaultAssistDistanceMeters;
        if (dist > g_assistEndDistances[i] || dist < g_assistEndDistances[i] - duration - 100.0f) {
            g_overtakeAssistByCar[i] = 0;
            g_assistedCarPointers[i] = nullptr;
            g_assistEndDistances[i] = 0.0f;
            g_assistDurationsMeters[i] = 0.0f;
        }
    }
}

static char* TrimWhitespace(char* value) {
    while (*value && std::isspace(static_cast<unsigned char>(*value))) ++value;
    char* end = value + std::strlen(value);
    while (end > value && std::isspace(static_cast<unsigned char>(end[-1]))) --end;
    *end = '\0';
    return value;
}

struct ModSettings {
    LONG drsMode;
    bool overtakeAssist;
};

static ModSettings ReadModSettings() {
    ModSettings settings = { DrsModeStraight, true };

    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, _countof(path), _TRUNCATE, L"%s\\settings.ini", g_modDirectory);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) {
        static bool missingLogged = false;
        if (!missingLogged) {
            LogMessage("settings.ini not found; using defaults DRSmode=straight and overtake_assist=ON.");
            missingLogged = true;
        }
        return settings;
    }

    char line[256] = {};
    static char lastInvalidMode[256] = {};
    static char lastInvalidAssist[256] = {};
    while (std::fgets(line, sizeof(line), f)) {
        char* value = TrimWhitespace(line);
        if (*value == '\0' || *value == '#' || *value == ';') continue;
        char* equals = std::strchr(value, '=');
        if (!equals) continue;
        *equals = '\0';
        char* key = TrimWhitespace(value);
        char* raw = TrimWhitespace(equals + 1);
        char* inlineComment = std::strpbrk(raw, "#;");
        if (inlineComment) *inlineComment = '\0';
        raw = TrimWhitespace(raw);

        if (_stricmp(key, "DRSmode") == 0) {
            if (_stricmp(raw, "drs") == 0 || _stricmp(raw, "original") == 0) {
                settings.drsMode = DrsModeOriginal;
            } else if (_stricmp(raw, "straight") == 0) {
                settings.drsMode = DrsModeStraight;
            } else if (_stricmp(raw, "none") == 0 || _stricmp(raw, "off") == 0) {
                settings.drsMode = DrsModeNone;
            } else {
                char invalid[256] = {};
                _snprintf_s(invalid, _countof(invalid), _TRUNCATE, "DRSmode=%s", raw);
                if (std::strncmp(lastInvalidMode, invalid, sizeof(lastInvalidMode) - 1) != 0) {
                    strncpy_s(lastInvalidMode, sizeof(lastInvalidMode), invalid, _TRUNCATE);
                    LogMessage("Invalid settings.ini value '%s'; DRSmode must be "
                               "drs, straight or none. Using straight.", raw);
                }
            }
        } else if (_stricmp(key, "overtake_assist") == 0) {
            if (_stricmp(raw, "on") == 0 || _stricmp(raw, "true") == 0 || std::strcmp(raw, "1") == 0) {
                settings.overtakeAssist = true;
            } else if (_stricmp(raw, "off") == 0 || _stricmp(raw, "false") == 0 || std::strcmp(raw, "0") == 0) {
                settings.overtakeAssist = false;
            } else {
                char invalid[256] = {};
                _snprintf_s(invalid, _countof(invalid), _TRUNCATE, "overtake_assist=%s", raw);
                if (std::strncmp(lastInvalidAssist, invalid, sizeof(lastInvalidAssist) - 1) != 0) {
                    strncpy_s(lastInvalidAssist, sizeof(lastInvalidAssist), invalid, _TRUNCATE);
                    LogMessage("Invalid settings.ini value '%s'; overtake_assist must be ON or OFF. Using ON.", raw);
                }
            }
        }
    }
    std::fclose(f);
    return settings;
}

static bool ParseDistanceMeters(const char* text, float* meters) {
    if (!text || !meters) return false;
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text) return false;
    while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end != '\0' || !(value > 0.0f) || value > 100000.0f) return false;
    *meters = value;
    return true;
}

static bool ReadCurrentCircuitName(char* out, size_t outSize) {
    if (!out || outSize < 2) return false;
    out[0] = '\0';
    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, _countof(path), _TRUNCATE, L"%s\\current_track.txt", g_modDirectory);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return false;
    char line[128] = {};
    const bool read = std::fgets(line, sizeof(line), f) != nullptr;
    std::fclose(f);
    if (!read) return false;
    char* value = TrimWhitespace(line);
    const size_t len = std::strlen(value);
    if (len == 0 || len >= outSize) return false;
    std::memcpy(out, value, len + 1);
    return true;
}

static float ReadCircuitAssistDistance(const char* circuit, bool* hasOverride) {
    float defaultMeters = kDefaultAssistDistanceMeters;
    float circuitMeters = 0.0f;
    if (hasOverride) *hasOverride = false;

    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, _countof(path), _TRUNCATE, L"%s\\overtake_assist.ini", g_modDirectory);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return defaultMeters;

    char line[256] = {};
    static char lastInvalidLine[256] = {};
    while (std::fgets(line, sizeof(line), f)) {
        char* value = TrimWhitespace(line);
        if (*value == '\0' || *value == '#' || *value == ';') continue;
        char* equals = std::strchr(value, '=');
        if (!equals) continue;
        *equals = '\0';
        char* key = TrimWhitespace(value);
        char* rawMeters = TrimWhitespace(equals + 1);
        float parsed = 0.0f;
        if (!ParseDistanceMeters(rawMeters, &parsed)) {
            if (std::strncmp(lastInvalidLine, value, sizeof(lastInvalidLine) - 1) != 0) {
                strncpy_s(lastInvalidLine, sizeof(lastInvalidLine), value, _TRUNCATE);
                LogMessage("Invalid overtake_assist.ini value for '%s'; expected metres in range (0, 100000].", key);
            }
            continue;
        }
        if (_stricmp(key, "default") == 0) {
            defaultMeters = parsed;
        } else if (circuit && *circuit && _stricmp(circuit, kUnknownCircuitName) != 0 && _stricmp(key, circuit) == 0) {
            circuitMeters = parsed;
        }
    }
    std::fclose(f);
    if (circuitMeters > 0.0f) {
        if (hasOverride) *hasOverride = true;
        return circuitMeters;
    }
    return defaultMeters;
}

static void RefreshCircuitSettings() {
    char detectedCircuit[64] = {};
    if (!ReadCurrentCircuitName(detectedCircuit, sizeof(detectedCircuit))) {
        strncpy_s(detectedCircuit, sizeof(detectedCircuit), g_currentCircuitName, _TRUNCATE);
    }
    const bool circuitChanged = _stricmp(detectedCircuit, g_currentCircuitName) != 0;
    if (circuitChanged) {
        strncpy_s(g_currentCircuitName, sizeof(g_currentCircuitName), detectedCircuit, _TRUNCATE);
        ClearOvertakeAssistState();
    }

    bool hasOverride = false;
    const float selected = ReadCircuitAssistDistance(g_currentCircuitName, &hasOverride);
    const bool settingChanged = selected != g_assistDistanceMeters || hasOverride != g_hasCircuitDistanceOverride;
    g_assistDistanceMeters = selected;
    g_hasCircuitDistanceOverride = hasOverride;

    if (circuitChanged) {
        if (g_currentCircuitName[0]) {
            const char* displayName =
                _stricmp(g_currentCircuitName, kUnknownCircuitName) == 0
                    ? "unknown/modded track" : g_currentCircuitName;
            LogMessage("Current circuit detected: '%s'.", displayName);
        } else {
            LogMessage("Current circuit not identified yet; using default assistance distance until detected.");
        }
    }
    if (circuitChanged || settingChanged) {
        LogMessage("Overtake probability assistance distance: %.0f m (%s).",
            g_assistDistanceMeters, hasOverride ? "circuit override" : "default fallback");
    }
}

// Background worker
static DWORD WINAPI ModWorkerThread(LPVOID) {
    g_gameImageBase = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    if (!g_gameImageBase) {
        LogMessage("GetModuleHandleW(nullptr) failed; no patch applied.");
        return 0;
    }
    if (!ValidateTargetGameBuild()) return 0;
    if (!BuildGapCheckTrampolines()) {
        LogMessage("Could not build all DRS mode caves; no patch applied.");
        return 0;
    }
    g_overtakeAssistPatchInstalled = BuildOvertakeAssistTrampoline();
    if (g_overtakeAssistPatchInstalled) {
        if (!WriteExecutableMemory(g_overtakeCheckSite, g_overtakeAssistPatch, sizeof(g_overtakeAssistPatch))) {
            LogMessage("Overtake probability patch could not be written; assistance disabled.");
            g_overtakeAssistPatchInstalled = false;
        } else {
            LogMessage("Overtake probability patch installed; no ERS or energy fields are modified.");
        }
    }

    RefreshCircuitSettings();
    ModSettings settings = ReadModSettings();
    LONG appliedMode = -1;
    bool appliedAssist = false;
    LONG detectionReadIndex = 0;
    unsigned settingsRefreshTicks = 0;

    for (;;) {
        if (++settingsRefreshTicks >= 10) {
            settingsRefreshTicks = 0;
            settings = ReadModSettings();
            RefreshCircuitSettings();
        }

        if (settings.drsMode != appliedMode || settings.overtakeAssist != appliedAssist) {
            if (!ApplyDrsMode(settings.drsMode, settings.overtakeAssist)) {
                LogMessage("DRS transition failed; worker stopping to avoid repeated writes.");
                return 0;
            }
            appliedMode = settings.drsMode;
            appliedAssist = settings.overtakeAssist;
            ClearOvertakeAssistState();
            detectionReadIndex = g_detectionQueue.writeCount;
        }

        if (appliedMode != DrsModeNone && appliedAssist && g_overtakeAssistPatchInstalled) {
            ProcessDetectionQueue(detectionReadIndex);
            ExpireOvertakeAssists();
        }
        Sleep(kSettingsPollIntervalMs);
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        GetModuleFileNameW(module, g_modDirectory, _countof(g_modDirectory));
        wchar_t* slash = wcsrchr(g_modDirectory, L'\\');
        if (slash) *slash = L'\0';
        HANDLE thread = CreateThread(nullptr, 0, ModWorkerThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
