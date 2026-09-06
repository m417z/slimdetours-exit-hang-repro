# SlimDetours process exit hang repro

Reproduces a process which never exits, because the SlimDetours thread freeze
suspended the thread that was exiting it.

A transaction which freezes threads suspends every other thread in the process
(`detour_thread_suspend` in `Thread.c`). If it suspends a thread which has
already entered `NtTerminateProcess` to exit the process, the process may hang:

* `RtlExitUserProcess` takes the loader lock, the PEB lock and the process heap
  locks, then calls `NtTerminateProcess(NULL, status)`. That call flags every
  other thread for termination, the transaction thread included, and waits for
  them to exit.
* The transaction thread suspends the exiting thread, and can then be
  terminated before it reaches `detour_thread_resume`.
* Nothing is left to resume the exiting thread. It stays suspended inside
  `NtTerminateProcess`, and the process is left with that one thread.

It is a race in both directions. The transaction thread may instead finish and
resume everyone before its own termination lands, and suspending the exiting
thread before it enters `NtTerminateProcess` is harmless for the same reason,
which is why this needs many attempts to hit.

A hung process reports `TerminateProcess` as succeeding but stays behind
anyway. Resuming its one thread makes it finish exiting immediately.

## Vendored SlimDetours

`vendor/SlimDetours` is an unmodified copy of `Source/KNSoft.SlimDetours` at
KNSoft.SlimDetours commit `7e1af892c40497927ca5edbe6d8687a925ed7e59`
(latest `origin/main`), which does not have a fix for this.

`vendor/phnt` is used as the NDK instead of the KNSoft.NDK NuGet package, which
SlimDetours supports out of the box: `SlimDetours.NDK.inl` picks up another NDK
when its header is included first. `src/phnt_compat.h` is force included
(`cl /FI`) to do that, and to declare `RtlIsEcCode`, which phnt declares for
ARM64EC only while SlimDetours also uses it on x64.

## Build

    build.bat [x64|x86]

Needs Visual Studio 2022 with the C++ desktop workload. The output is
`build\repro.exe`, with PDBs, so a dump of a hang resolves SlimDetours frames.

## Run

    build\repro.exe

The parent spawns itself as `repro.exe child <n>` in a loop and waits up to 5
seconds for each child. A child which doesn't exit in time has reproduced the
hang: the parent writes `hang-<pid>.dmp` next to itself, prints the pid and
stops. The hung process stays behind, so a debugger can be attached to it.

Because a hung child stays behind and holds its image file locked, children run
from a copy of the executable in `%TEMP%`. That keeps `build\repro.exe`
rebuildable and leaves the locked copy, one per reproduction, in `%TEMP%`
instead.

Each child:

* creates 24 idle threads, which lengthen each freeze, so that more of the
  transaction happens while the exiting thread is suspended
* starts one thread which attaches and detaches a hook on `kernel32!Beep` in a
  loop, freezing all other threads on every transaction
* exits the process from its main thread at a randomized point in that loop

It typically takes on the order of a hundred children, a minute or so.

## What a hang looks like

In the dump the parent wrote, the module is named after the image copy the
child ran from, and the exiting thread is the only one left:

    ntdll!NtTerminateProcess+0x14           <- suspended here
    ntdll!RtlExitUserProcess+0x47
    kernel32!ExitProcessImplementation+0xb
    ...!RunChild+0x115
    ...!main+0x160

The transaction thread has already exited, but a terminated thread's stack is
leaked rather than freed, so reading down from the top of one of the 1 MB stack
regions still shows where it was when it died:

    ntdll!NtSuspendThread                   <- the call which suspended the exiting thread
    ...!detour_thread_suspend+0xd1          (detour_suspend_next_thread inlined)
    ...!SlimDetoursTransactionBeginEx+0x5b
    ...!TransactionThread+0x50

The suspend target is the exiting thread: the thread handle and the
`THREAD_BASIC_INFORMATION` just queried for it are still in the frame.

The dump records the exiting thread's suspend count as 0, which is misleading.
Ask the live process instead, with `NtQueryInformationThread(ThreadSuspendCount)`,
and it answers 1. Resuming that thread ends the process on the spot.

## Fix

Hold the loader lock across the freeze. `RtlExitUserProcess` acquires it before
`NtTerminateProcess`, so while it is held no thread exiting that way can reach
that call, and whichever side gets there first makes the other one wait
somewhere harmless:

* Acquire it in `SlimDetoursTransactionBeginEx`, before `detour_thread_suspend`.
* Release it in `SlimDetoursTransactionCommit` and `SlimDetoursTransactionAbort`
  after `detour_thread_resume`, never before. Releasing first lets an exiting
  thread reach `NtTerminateProcess` while threads are still suspended, and it
  then waits for a suspended thread to exit, which it never will.
