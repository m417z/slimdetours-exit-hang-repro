/*
 * Forced include (cl /FI) which picks phnt as the NDK for the vendored
 * SlimDetours copy, so that it builds without the KNSoft.NDK NuGet package.
 * SlimDetours.NDK.inl documents this option: include another NDK before
 * SlimDetours. The two macros are undefined because SlimDetours.NDK.inl
 * defines its own when KNSoft.NDK isn't used.
 */

#pragma once

#include <phnt_windows.h>
#include <phnt.h>

#undef NtCurrentProcessId
#undef NtCurrentThreadId

/*
 * SlimDetours uses RtlIsEcCode on x64 to detect ARM64EC code, and KNSoft.NDK
 * declares it for every architecture. phnt declares it for ARM64EC only.
 */
#if defined(_M_X64) && !defined(_M_ARM64EC)
NTSYSAPI
BOOLEAN
NTAPI
RtlIsEcCode(
    _In_ ULONG64 CodePointer
    );
#endif
