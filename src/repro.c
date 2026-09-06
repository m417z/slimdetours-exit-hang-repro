/*
 * Reproduces a deadlock between process exit and the SlimDetours thread freeze.
 *
 * SlimDetoursTransactionBegin suspends every other thread in the process. When
 * it suspends the thread which is exiting the process, the process never dies:
 * RtlExitUserProcess is blocked in NtTerminateProcess waiting for the other
 * threads to run down, and the transaction thread, which is one of those
 * threads, never gets far enough to resume it.
 *
 * Without arguments the program is the parent: it spawns itself in a loop and
 * waits for each child. A child which doesn't exit within CHILD_TIMEOUT_MS has
 * reproduced the hang, and the parent writes a full memory dump of it.
 *
 * With "child <n>" the program is a child: it starts a hook transaction loop
 * plus a set of idle threads, then exits the process from its main thread at a
 * randomized point in the middle of that loop.
 */

#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SlimDetours.h"

#define IDLE_THREAD_COUNT 24
#define WARMUP_TRANSACTIONS 8
#define MAX_EXIT_DELAY_US 2000
#define CHILD_TIMEOUT_MS 5000
#define MAX_CHILDREN 100000

static HANDLE g_idleEvent;
static volatile LONG g_transactionCount;
static BOOL(WINAPI* g_pfnBeep)(DWORD, DWORD);

static BOOL WINAPI BeepDetour(DWORD dwFreq, DWORD dwDuration)
{
    return g_pfnBeep(dwFreq, dwDuration);
}

// Gives the freeze something to suspend, and gives process termination more
// threads to run down, which widens the window the race needs.
static DWORD WINAPI IdleThread(LPVOID param)
{
    UNREFERENCED_PARAMETER(param);

    WaitForSingleObject(g_idleEvent, INFINITE);
    return 0;
}

static DWORD WINAPI TransactionThread(LPVOID param)
{
    UNREFERENCED_PARAMETER(param);

    for (;;)
    {
        if (SUCCEEDED(SlimDetoursTransactionBegin()))
        {
            SlimDetoursAttach((PVOID*)&g_pfnBeep, (PVOID)BeepDetour);
            SlimDetoursTransactionCommit();
        }

        if (SUCCEEDED(SlimDetoursTransactionBegin()))
        {
            SlimDetoursDetach((PVOID*)&g_pfnBeep, (PVOID)BeepDetour);
            SlimDetoursTransactionCommit();
        }

        InterlockedIncrement(&g_transactionCount);
    }
}

static void SpinMicroseconds(DWORD microseconds)
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER now;
    LONGLONG ticks;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    ticks = (frequency.QuadPart * microseconds) / 1000000;
    do
    {
        QueryPerformanceCounter(&now);
    } while (now.QuadPart - start.QuadPart < ticks);
}

static int RunChild(unsigned int seed)
{
    HANDLE thread;
    int i;

    srand(seed ^ GetTickCount());

    g_pfnBeep = (BOOL(WINAPI*)(DWORD, DWORD))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Beep");
    if (!g_pfnBeep)
    {
        return 1;
    }

    g_idleEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_idleEvent)
    {
        return 1;
    }

    for (i = 0; i < IDLE_THREAD_COUNT; i++)
    {
        thread = CreateThread(NULL, 0, IdleThread, NULL, 0, NULL);
        if (!thread)
        {
            return 1;
        }
        CloseHandle(thread);
    }

    thread = CreateThread(NULL, 0, TransactionThread, NULL, 0, NULL);
    if (!thread)
    {
        return 1;
    }
    CloseHandle(thread);

    while (g_transactionCount < WARMUP_TRANSACTIONS)
    {
        Sleep(0);
    }

    // Land somewhere random in the transaction loop.
    SpinMicroseconds((DWORD)(rand() % MAX_EXIT_DELAY_US));

    ExitProcess(0);
}

static void WriteHangDump(HANDLE process, DWORD processId)
{
    WCHAR path[MAX_PATH];
    HANDLE file;
    MINIDUMP_TYPE type = MiniDumpWithFullMemory | MiniDumpWithHandleData |
                         MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo |
                         MiniDumpWithUnloadedModules;

    swprintf(path, ARRAYSIZE(path), L"hang-%lu.dmp", processId);

    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        wprintf(L"Failed to create %ls: %lu\n", path, GetLastError());
        return;
    }

    if (MiniDumpWriteDump(process, processId, file, type, NULL, NULL, NULL))
    {
        wprintf(L"Dump written to %ls\n", path);
    }
    else
    {
        wprintf(L"MiniDumpWriteDump failed: %lu\n", GetLastError());
    }

    CloseHandle(file);
}

// A hung child can't be killed and keeps its image file locked, which would
// stop this program from being rebuilt. Children run from a throwaway copy, so
// that only the copy is left locked.
static BOOL CopyChildImage(_Out_writes_(childPathCount) WCHAR* childPath, size_t childPathCount)
{
    WCHAR exePath[MAX_PATH];
    WCHAR tempPath[MAX_PATH];

    if (!GetModuleFileNameW(NULL, exePath, ARRAYSIZE(exePath)) ||
        !GetTempPathW(ARRAYSIZE(tempPath), tempPath))
    {
        return FALSE;
    }

    swprintf(childPath, childPathCount, L"%lsslimdetours-repro-%lu.exe", tempPath, GetCurrentProcessId());
    return CopyFileW(exePath, childPath, FALSE);
}

static int RunParent(void)
{
    WCHAR childPath[MAX_PATH];
    WCHAR commandLine[MAX_PATH + 64];
    STARTUPINFOW startupInfo;
    PROCESS_INFORMATION processInfo;
    int iteration;

    if (!CopyChildImage(childPath, ARRAYSIZE(childPath)))
    {
        wprintf(L"Failed to copy the child image: %lu\n", GetLastError());
        return 1;
    }

    wprintf(L"Spawning children until one hangs, %u ms timeout each.\n", CHILD_TIMEOUT_MS);

    for (iteration = 1; iteration <= MAX_CHILDREN; iteration++)
    {
        swprintf(commandLine, ARRAYSIZE(commandLine), L"\"%ls\" child %d", childPath, iteration);

        ZeroMemory(&startupInfo, sizeof(startupInfo));
        startupInfo.cb = sizeof(startupInfo);

        if (!CreateProcessW(childPath, commandLine, NULL, NULL, FALSE, 0, NULL, NULL,
                            &startupInfo, &processInfo))
        {
            wprintf(L"CreateProcess failed: %lu\n", GetLastError());
            return 1;
        }

        CloseHandle(processInfo.hThread);

        if (WaitForSingleObject(processInfo.hProcess, CHILD_TIMEOUT_MS) == WAIT_TIMEOUT)
        {
            wprintf(L"\nChild %d (pid %lu) hung.\n", iteration, processInfo.dwProcessId);
            WriteHangDump(processInfo.hProcess, processInfo.dwProcessId);
            wprintf(L"Pid %lu is stuck in process termination and can't be killed,\n"
                    L"it stays as a terminating process until the machine reboots.\n",
                    processInfo.dwProcessId);
            wprintf(L"Its image copy, %ls, stays locked as well.\n", childPath);
            CloseHandle(processInfo.hProcess);
            return 0;
        }

        CloseHandle(processInfo.hProcess);

        if (iteration % 25 == 0)
        {
            wprintf(L".");
            fflush(stdout);
        }
    }

    DeleteFileW(childPath);

    wprintf(L"\nNo hang after %d children.\n", MAX_CHILDREN);
    return 1;
}

int __cdecl main(int argc, char** argv)
{
    if (argc >= 2 && strcmp(argv[1], "child") == 0)
    {
        return RunChild(argc >= 3 ? (unsigned int)atoi(argv[2]) : 0);
    }

    return RunParent();
}
