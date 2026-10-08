#include "pch.h"
#include "../Public/CrashReporter.h"
#include <TlHelp32.h>
#include <winternl.h>
#include <cstdio>
#include <io.h>
#pragma comment(lib, "ntdll.lib")

void FreezeOtherThreads()
{
    auto thrHandle = GetCurrentThread();
    auto currentThr = GetThreadId(thrHandle);
    auto currentPrc = GetProcessIdOfThread(thrHandle);
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (h != INVALID_HANDLE_VALUE)
    {
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        if (Thread32First(h, &te))
        {
            do
            {
                if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID))
                {
                    if (te.th32ThreadID != currentThr && te.th32OwnerProcessID == currentPrc)
                    {
                        auto thr = OpenThread(THREAD_ALL_ACCESS, false, te.th32ThreadID);

                        if (thr != INVALID_HANDLE_VALUE && thr != nullptr)
                        {
                            SuspendThread(thr);
                            CloseHandle(thr);
                        }
                    }
                }
                te.dwSize = sizeof(te);
            }
            while (Thread32Next(h, &te));
        }
        CloseHandle(h);
    }
}

static thread_local int GGuardDepth = 0;

static volatile long GRecoveredNullCalls = 0;
static volatile long GGuardedFaults = 0;

static const char* volatile GBreadcrumb = "startup";

static HANDLE GLogHandle = INVALID_HANDLE_VALUE;
static HANDLE GConsoleHandle = INVALID_HANDLE_VALUE;
static DWORD64 GExeBase = 0, GExeEnd = 0;
static DWORD64 GDllBase = 0, GDllEnd = 0;

static volatile LONG GReportingThread = 0;

void FCrashReporter::EnterGuardedSection()
{
    ++GGuardDepth;
}

void FCrashReporter::LeaveGuardedSection()
{
    if (GGuardDepth > 0)
        --GGuardDepth;
}

int FCrashReporter::GetRecoveredNullCallCount()
{
    return (int)GRecoveredNullCalls;
}

void FCrashReporter::SetBreadcrumb(const char* Where)
{
    GBreadcrumb = Where ? Where : "(null)";
}

const char* FCrashReporter::GetBreadcrumb()
{
    return GBreadcrumb;
}

static bool ReadPointerSafely(DWORD64 Address, DWORD64& OutValue)
{
    if (Address < 0x10000 || (Address & 7) != 0)
        return false;

    MEMORY_BASIC_INFORMATION Info{};
    if (VirtualQuery((LPCVOID)Address, &Info, sizeof(Info)) != sizeof(Info))
        return false;

    if (Info.State != MEM_COMMIT)
        return false;

    if (Info.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;

    OutValue = *(DWORD64*)Address;
    return true;
}

struct FCrashText
{
    char* Buf;
    size_t Cap;
    size_t Len;

    void Str(const char* s)
    {
        while (s && *s && Len + 1 < Cap)
            Buf[Len++] = *s++;
    }

    void Hex(DWORD64 v)
    {
        char tmp[16];
        int n = 0;
        do
        {
            const int d = (int)(v & 0xF);
            tmp[n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
            v >>= 4;
        } while (v && n < 16);

        Str("0x");
        while (n > 0 && Len + 1 < Cap)
            Buf[Len++] = tmp[--n];
    }

    void Dec(long long v)
    {
        char tmp[24];
        int n = 0;
        const bool neg = v < 0;
        unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
        do
        {
            tmp[n++] = (char)('0' + (u % 10));
            u /= 10;
        } while (u && n < 22);

        if (neg && Len + 1 < Cap)
            Buf[Len++] = '-';
        while (n > 0 && Len + 1 < Cap)
            Buf[Len++] = tmp[--n];
    }

    
    
    void Addr(DWORD64 a)
    {
        Hex(a);

        if (a >= GExeBase && a < GExeEnd)
        {
            Str(" (FN+");
            Hex(a - GExeBase);
            Str(")");
        }
        else if (a >= GDllBase && a < GDllEnd)
        {
            Str(" (GS+");
            Hex(a - GDllBase);
            Str(")");
        }
        else if (a > 0x10000)
        {
            PVOID Base = nullptr;
            if (RtlPcToFileHeader((PVOID)a, &Base) && Base)
            {
                Str(" (module@");
                Hex((DWORD64)Base);
                Str("+");
                Hex(a - (DWORD64)Base);
                Str(")");
            }
        }
    }

    void WriteTo(HANDLE h) const
    {
        if (h == INVALID_HANDLE_VALUE || h == nullptr || Len == 0)
            return;

        DWORD Written = 0;
        WriteFile(h, Buf, (DWORD)Len, &Written, nullptr);
    }
};

static const char* ExceptionName(DWORD Code)
{
    switch (Code)
    {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW: return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION: return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    case 0xC0000374: return "STATUS_HEAP_CORRUPTION";
    case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN (fast-fail)";
    default: return "unknown";
    }
}

static void AppendAccessDetail(FCrashText& T, const EXCEPTION_RECORD* R)
{
    if ((R->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || R->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && R->NumberParameters >= 2)
    {
        switch (R->ExceptionInformation[0])
        {
        case 0: T.Str(" -- tried to READ "); break;
        case 1: T.Str(" -- tried to WRITE "); break;
        case 8: T.Str(" -- tried to EXECUTE "); break;
        default: T.Str(" -- touched "); break;
        }
        T.Hex((DWORD64)R->ExceptionInformation[1]);

        if ((DWORD64)R->ExceptionInformation[1] < 0x10000)
            T.Str(" (a null / near-null pointer)");
    }
}

static char GReportBuf[32768];
static CONTEXT GWalkContext;

static void WriteStackWalk(FCrashText& T, const CONTEXT* Start)
{
    GWalkContext = *Start;

    for (int Frame = 0; Frame < 48; Frame++)
    {
        const DWORD64 Pc = GWalkContext.Rip;
        if (Pc == 0)
            break;

        T.Str("    #");
        T.Dec(Frame);
        T.Str("  ");
        T.Addr(Pc);
        T.Str("\n");

        DWORD64 ImageBase = 0;
        PRUNTIME_FUNCTION Entry = RtlLookupFunctionEntry(Pc, &ImageBase, nullptr);

        if (!Entry)
        {
            
            DWORD64 Ret = 0;
            if (!ReadPointerSafely(GWalkContext.Rsp, Ret))
                break;
            GWalkContext.Rip = Ret;
            GWalkContext.Rsp += 8;
        }
        else
        {
            PVOID HandlerData = nullptr;
            DWORD64 EstablisherFrame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ImageBase, Pc, Entry, &GWalkContext, &HandlerData, &EstablisherFrame, nullptr);
        }
    }
}

static void WriteCrashReport(LPEXCEPTION_POINTERS EI)
{
    FCrashText T{ GReportBuf, sizeof(GReportBuf), 0 };
    const EXCEPTION_RECORD* R = EI->ExceptionRecord;
    const CONTEXT* C = EI->ContextRecord;

    T.Str("\n[CrashReporter] ================= UNHANDLED CRASH =================\n");
    T.Str("[CrashReporter] ");
    T.Str(ExceptionName(R->ExceptionCode));
    T.Str(" (code ");
    T.Hex(R->ExceptionCode);
    T.Str(")");
    AppendAccessDetail(T, R);
    T.Str("\n[CrashReporter] Crashed at  ");
    T.Addr(C->Rip);
    T.Str("\n[CrashReporter] Last step:  ");
    T.Str(GBreadcrumb);
    T.Str("\n[CrashReporter] Thread id:  ");
    T.Dec((long long)GetCurrentThreadId());
    T.Str("   (FN+ = inside Fortnite's own code, GS+ = inside this gameserver DLL)\n");

    T.Str("[CrashReporter] Registers:\n");
    T.Str("    RAX "); T.Addr(C->Rax); T.Str("\n");
    T.Str("    RBX "); T.Addr(C->Rbx); T.Str("\n");
    T.Str("    RCX "); T.Addr(C->Rcx); T.Str("\n");
    T.Str("    RDX "); T.Addr(C->Rdx); T.Str("\n");
    T.Str("    RSI "); T.Addr(C->Rsi); T.Str("\n");
    T.Str("    RDI "); T.Addr(C->Rdi); T.Str("\n");
    T.Str("    R8  "); T.Addr(C->R8);  T.Str("\n");
    T.Str("    R9  "); T.Addr(C->R9);  T.Str("\n");
    T.Str("    R10 "); T.Addr(C->R10); T.Str("\n");
    T.Str("    R11 "); T.Addr(C->R11); T.Str("\n");
    T.Str("    R12 "); T.Addr(C->R12); T.Str("\n");
    T.Str("    R13 "); T.Addr(C->R13); T.Str("\n");
    T.Str("    R14 "); T.Addr(C->R14); T.Str("\n");
    T.Str("    R15 "); T.Addr(C->R15); T.Str("\n");
    T.Str("    RBP "); T.Addr(C->Rbp); T.Str("\n");
    T.Str("    RSP "); T.Addr(C->Rsp); T.Str("\n");

    T.Str("[CrashReporter] Call stack (innermost first):\n");

    
    
    const size_t BeforeWalk = T.Len;
    __try
    {
        WriteStackWalk(T, C);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        T.Str("    (stack walk stopped -- the stack is damaged beyond this point)\n");
    }
    if (T.Len == BeforeWalk)
        T.Str("    (no frames could be read)\n");

    T.Str("[CrashReporter] ==================================================\n");
    T.Str("[CrashReporter] Send this whole log -- the 'Crashed at' and 'Last step' lines say exactly where it died.\n\n");

    T.WriteTo(GLogHandle);
    if (GLogHandle != INVALID_HANDLE_VALUE && GLogHandle != nullptr)
        FlushFileBuffers(GLogHandle);
    T.WriteTo(GConsoleHandle);
}

LONG WINAPI four_e_zero_h_UnhandledExceptionFilter(LPEXCEPTION_POINTERS ExceptionInfo)
{
    if ((ExceptionInfo->ExceptionRecord->ExceptionCode & 0x80000000) == 0 || (ExceptionInfo->ExceptionRecord->ExceptionCode & 0x30000000) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    
    if (GReportingThread != 0)
    {
        
        if ((DWORD)GReportingThread == GetCurrentThreadId())
            return EXCEPTION_CONTINUE_SEARCH;

        
        Sleep(INFINITE);
    }

    if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ExceptionInfo->ContextRecord->Rip == 0)
    {
        DWORD64 ReturnAddress = 0;

        if (GRecoveredNullCalls < 65536 && ReadPointerSafely(ExceptionInfo->ContextRecord->Rsp, ReturnAddress) && ReturnAddress > 0x10000)
        {
            const long Count = InterlockedIncrement(&GRecoveredNullCalls);

            if (Count <= 20 || (Count % 1000) == 0)
            {
                char Line[512];
                FCrashText T{ Line, sizeof(Line), 0 };
                T.Str("[CrashReporter] Stepped over a call to a function this build does not have (caller ");
                T.Addr(ReturnAddress);
                T.Str(", last step: ");
                T.Str(GBreadcrumb);
                T.Str("). The match carries on. [");
                T.Dec(Count);
                T.Str("]\n");
                T.WriteTo(GLogHandle);

                if (Count == 20)
                {
                    char Note[128];
                    FCrashText N{ Note, sizeof(Note), 0 };
                    N.Str("[CrashReporter] ...only every thousandth one gets printed from here on.\n");
                    N.WriteTo(GLogHandle);
                }
            }

            ExceptionInfo->ContextRecord->Rip = ReturnAddress;
            ExceptionInfo->ContextRecord->Rsp += 8;
            ExceptionInfo->ContextRecord->Rax = 0;

            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    
    
    if (GGuardDepth > 0)
    {
        const long Count = InterlockedIncrement(&GGuardedFaults);

        if (Count <= 25 || (Count % 200) == 0)
        {
            char Line[768];
            FCrashText T{ Line, sizeof(Line), 0 };
            T.Str("[CrashReporter] Guarded fault #");
            T.Dec(Count);
            T.Str(": ");
            T.Str(ExceptionName(ExceptionInfo->ExceptionRecord->ExceptionCode));
            AppendAccessDetail(T, ExceptionInfo->ExceptionRecord);
            T.Str(" | at ");
            T.Addr(ExceptionInfo->ContextRecord->Rip);
            T.Str(" | last step: ");
            T.Str(GBreadcrumb);
            T.Str(" -- contained, only that section was skipped.\n");
            T.WriteTo(GLogHandle);

            if (Count == 25)
            {
                char Note[128];
                FCrashText N{ Note, sizeof(Note), 0 };
                N.Str("[CrashReporter] ...guarded faults now printed every 200th.\n");
                N.WriteTo(GLogHandle);
            }
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    
    if (InterlockedCompareExchange(&GReportingThread, (LONG)GetCurrentThreadId(), 0) != 0)
        Sleep(INFINITE);

    
    
    __try
    {
        _fflush_nolock(stdout);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    __try
    {
        WriteCrashReport(ExceptionInfo);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        char Line[160];
        FCrashText T{ Line, sizeof(Line), 0 };
        T.Str("\n[CrashReporter] UNHANDLED CRASH -- and the report itself faulted part-way through. Last step: ");
        T.Str(GBreadcrumb);
        T.Str("\n");
        T.WriteTo(GLogHandle);
        if (GLogHandle != INVALID_HANDLE_VALUE && GLogHandle != nullptr)
            FlushFileBuffers(GLogHandle);
    }

    
    
    Sleep(2000);
    TerminateProcess(GetCurrentProcess(), ExceptionInfo->ExceptionRecord->ExceptionCode);

    return EXCEPTION_CONTINUE_EXECUTION;
}

static void CacheModuleRange(HMODULE Module, DWORD64& OutBase, DWORD64& OutEnd)
{
    OutBase = (DWORD64)Module;
    OutEnd = OutBase;

    if (!Module)
        return;

    auto Dos = (PIMAGE_DOS_HEADER)Module;
    if (Dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;

    auto Nt = (PIMAGE_NT_HEADERS)((BYTE*)Module + Dos->e_lfanew);
    if (Nt->Signature != IMAGE_NT_SIGNATURE)
        return;

    OutEnd = OutBase + Nt->OptionalHeader.SizeOfImage;
}

void FCrashReporter::Register()
{
    
    
    const int Fd = _fileno(stdout);
    if (Fd >= 0)
    {
        const intptr_t Os = _get_osfhandle(Fd);
        if (Os != -1 && Os != -2)
            GLogHandle = (HANDLE)Os;
    }

    GConsoleHandle = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    
    if (GConsoleHandle == GLogHandle)
        GConsoleHandle = INVALID_HANDLE_VALUE;

    CacheModuleRange(GetModuleHandleW(nullptr), GExeBase, GExeEnd);

    HMODULE Self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&four_e_zero_h_UnhandledExceptionFilter), &Self))
        CacheModuleRange(Self, GDllBase, GDllEnd);

    AddVectoredExceptionHandler(0, four_e_zero_h_UnhandledExceptionFilter);

    printf("[CrashReporter] Armed. Log handle %s, Fortnite at 0x%llX-0x%llX, gameserver DLL at 0x%llX-0x%llX.\n",
           (GLogHandle != INVALID_HANDLE_VALUE && GLogHandle != nullptr) ? "OK" : "MISSING (reports go to console only)",
           (unsigned long long)GExeBase, (unsigned long long)GExeEnd, (unsigned long long)GDllBase, (unsigned long long)GDllEnd);
    fflush(stdout);
}
