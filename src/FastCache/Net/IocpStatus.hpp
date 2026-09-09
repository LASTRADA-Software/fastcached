// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(_WIN32)

    #include <FastCache/Async/IocpReactor.hpp>

    #include <winsock2.h>

namespace FastCache::Detail
{

/// Translate an overlapped completion's NTSTATUS into the Win32/WSA code the error
/// taxonomy is written in.
///
/// **One conversion, reached by every consumer, because the last one was written at a
/// single call site and the consumer that did not get it carries every read and write
/// this product does on Windows.** `IocpStatus`'s comment has the full account. The
/// mechanical half of the fix is that `IocpCompletion::dispatch` no longer hands over a
/// `DWORD`, so there is nothing to pass to a `WSAE*` table by accident; this function is
/// the only way to get one, and it takes the socket precisely because that is the
/// argument the reactor could not supply.
///
/// `WSAGetOverlappedResult` is the documented conversion: it reads the completed
/// operation's result off the OVERLAPPED and reports it through `WSAGetLastError()`, in
/// the same numbering `TranslateWsa` matches on. `fWait` is FALSE because the caller is
/// already inside the completion callback, so the operation is finished by construction.
///
/// **A closed socket is an ABORTED operation, and that is a judgement rather than a
/// lookup.** `IocpSocket::Close` and `~IocpListener` call `closesocket` and forget the
/// handle, which is what makes the pending operation complete at all -- and it also
/// makes `WSAGetOverlappedResult` unanswerable, since there is no socket left to ask.
/// Reporting `WSAENOTSOCK` there would name the diagnostic's own problem rather than the
/// operation's, so this answers `ERROR_OPERATION_ABORTED`, which is what actually
/// happened and what `EpollSocket::Close` reports for the same event. The cost is stated
/// rather than hidden: an operation that had genuinely failed some other way *and* was
/// then raced by a close is reported as cancelled. That is a caller already tearing the
/// socket down, and it is strictly better than the `SystemError` every one of these
/// answered before.
///
/// Measured while proving those two branches can fail independently: a `WSARecv`
/// aborted by `closesocket` reports `STATUS_LOCAL_DISCONNECT` (0xC0000241), NOT
/// `STATUS_CANCELLED` (0xC0000120), which is what `CancelIoEx` produces. So this branch
/// is not a shortcut around a value that would have translated correctly anyway --
/// there is no `WSAE*` row for either of them, and the two arrive by different routes.
///
/// @param socket The socket the operation was issued on, or `INVALID_SOCKET` if it has
///        since been closed.
/// @param completion The completion whose OVERLAPPED the kernel wrote into.
/// @param status What the reactor read out of that OVERLAPPED.
/// @return 0 when the operation succeeded, otherwise a Win32/WSA error code.
[[nodiscard]] inline DWORD WsaErrorOf(SOCKET socket, IocpCompletion& completion, IocpStatus status) noexcept
{
    if (!status.Failed())
        return 0;

    if (socket == INVALID_SOCKET)
        return static_cast<DWORD>(ERROR_OPERATION_ABORTED);

    DWORD transferred = 0;
    DWORD flags = 0;
    if (WSAGetOverlappedResult(
            socket, reinterpret_cast<LPWSAOVERLAPPED>(&completion.overlapped), &transferred, FALSE, &flags)
        == FALSE)
        return static_cast<DWORD>(WSAGetLastError());

    // **Winsock has just said the operation SUCCEEDED, so this answers 0.** The
    // documentation makes this rare rather than impossible: `Failed()` is `_status != 0`
    // and NOT every non-zero NTSTATUS is a failure -- the warning-severity ones
    // (`STATUS_BUFFER_OVERFLOW`, 0x80000005, and its neighbours) are successes carrying
    // a note. Answering `ERROR_OPERATION_ABORTED` here would turn one of those into a
    // `Cancelled` read and throw away the bytes it transferred, which is a wrong answer
    // rather than a vague one.
    //
    // Not the raw NTSTATUS either, which is what the single-site version this replaces
    // did: that puts the untranslated number back into the taxonomy through the one
    // path nobody tests.
    return 0;
}

} // namespace FastCache::Detail

#endif // _WIN32
