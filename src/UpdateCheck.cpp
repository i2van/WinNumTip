#include "stdafx.h"
#include "UpdateCheck.h"

namespace {

// What the check asks for: github.com answers /releases/latest with a plain redirect to the
// newest release's tag page --
//   Location: https://github.com/i2van/WinNumTip/releases/tag/v2.1.0
// -- so the published version can be read straight out of one response header. That keeps
// the whole check to a single HEAD request with no JSON to parse and, unlike
// api.github.com, no rate limit shared with everything else on the user's address.
constexpr LPCTSTR kHost      = TEXT("github.com");
constexpr LPCTSTR kPath      = TEXT("/i2van/WinNumTip/releases/latest");
constexpr LPCTSTR kMethod    = TEXT("HEAD");
constexpr LPCTSTR kUserAgent = TEXT("WinNumTip/") APP_VERSION;

// Bound on every stage of the round trip (resolve, connect, send, receive), so a
// black-holed connection cannot keep the worker thread -- and the WinHTTP handles it owns --
// alive for minutes after the dialog that asked for the check has closed.
constexpr int kTimeoutMs = 5000;

// Versions are compared component by component (so 2.10 beats 2.9), over at most the four
// dot-separated components of the Windows file version Directory.Build.props stamps in.
constexpr int kVersionParts = 4;
// Ceiling for a single component, which also caps the accumulator: absurd input (a tag with
// a 20-digit number in it) saturates here instead of overflowing.
constexpr UINT kVersionPartMax = 99999;

// "255.255.255.255" and a terminator fit with room to spare; the Location header holds an
// absolute URL, so it gets the roomier buffer.
constexpr int kVersionMax  = 32;
constexpr int kLocationMax = 256;

// Cross-thread state. Each is a naturally aligned word, so a load or a store is a single
// atomic instruction (no torn values) on every supported architecture -- the same reasoning
// the keyboard hook's shared flag documents -- and that is all this needs: no Interlocked
// read-modify-write is used, both because each variable has a single writer at any moment
// (see Start and CheckThread) and because the Interlocked intrinsics are out-of-line CRT
// calls on ARM64, which this /NODEFAULTLIB build cannot link.
volatile BOOL g_running;    // TRUE while a request is in flight, so only one ever is
volatile BOOL g_available;  // TRUE when a completed check has found a newer release
volatile HWND g_notify;     // window posted WM_UPDATECHECK when a check finishes, or null

// Read the leading run of digits at 'p' as a number, then step past it and past one
// following '.', so repeated calls walk "2.11.0" as 2, 11, 0 -- and keep returning 0 once
// the string is exhausted, which is what makes "2.1" and "2.1.0" compare equal. Anything
// that is not a digit ends the number, and a component with more digits than
// kVersionPartMax allows saturates rather than wrapping.
UINT NextVersionPart(LPCTSTR& p) {
    UINT value = 0;
    while (*p >= TEXT('0') && *p <= TEXT('9')) {
        value = value >= kVersionPartMax
                    ? kVersionPartMax
                    : value * 10 + static_cast<UINT>(*p - TEXT('0'));
        ++p;
    }

    if (*p == TEXT('.')) ++p;

    return value;
}

// True when the published version 'latest' is newer than the one this binary was stamped
// with (APP_VERSION). The first differing component decides; equal versions -- and, by the
// same test, a published version older than a locally built one -- are not an update.
[[nodiscard]] bool IsNewerThanThisBuild(LPCTSTR latest) {
    LPCTSTR published = latest;
    LPCTSTR current   = APP_VERSION;

    for (int part = 0; part < kVersionParts; ++part) {
        const UINT publishedPart = NextVersionPart(published);
        const UINT currentPart   = NextVersionPart(current);
        if (publishedPart != currentPart) return publishedPart > currentPart;
    }

    return false;
}

// Copy the version out of the redirect target ".../releases/tag/v2.1.0" -> "2.1.0": what
// follows the last '/', with an optional leading 'v' dropped. Accepted only when what is
// left is a non-empty run of digits and dots that fits 'version', so a redirect to anything
// else (a sign-in page, an error page, a tag that is not a version) is rejected here rather
// than compared as if it were a version.
template <int N>
[[nodiscard]] bool VersionFromLocation(LPCTSTR location, _Out_writes_z_(N) TCHAR (&version)[N]) {
    version[0] = TEXT('\0');

    LPCTSTR tag = location;
    for (LPCTSTR p = location; *p; ++p)
        if (*p == TEXT('/')) tag = p + 1;

    if (*tag == TEXT('v') || *tag == TEXT('V')) ++tag;
    if (!*tag || lstrlen(tag) >= N) return false;

    for (LPCTSTR p = tag; *p; ++p)
        if ((*p < TEXT('0') || *p > TEXT('9')) && *p != TEXT('.')) return false;

    WinAPI::String::Copy(version, tag);

    return true;
}

// The blocking half of the check, run on the worker thread: fetch the latest release's
// version into 'version'. False on any failure -- offline, proxy refusal, an unexpected
// response -- which the caller reports as "no update" rather than bothering the user.
template <int N>
[[nodiscard]] bool QueryLatestVersion(_Out_writes_z_(N) TCHAR (&version)[N]) {
    version[0] = TEXT('\0');

    bool queried = false;

    // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY: go through whatever proxy the system is
    // configured with, auto-detection (WPAD) included, so the check reaches github.com on a
    // corporate network the same way the browser the "Update" link opens does.
    //
    // The three handles below are asserted, not merely tested: opening a session, naming the
    // server and building the request are local setup -- nothing is sent before
    // WinHttpSendRequest -- so a failure there is a programming error rather than the
    // offline/blocked case the calls further down are written to tolerate quietly.
    const HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    ASSERT(session);
    if (session) {
        VERIFY(WinHttpSetTimeouts(session, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs));

        const HINTERNET connection = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
        ASSERT(connection);
        if (connection) {
            // HEAD, not GET: the answer is in the response headers, so the release page's
            // body is never transferred.
            const HINTERNET request = WinHttpOpenRequest(connection, kMethod, kPath, nullptr,
                                                         WINHTTP_NO_REFERER,
                                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                         WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH);
            ASSERT(request);
            if (request) {
                // Do not follow the redirect: the redirect IS the answer (its Location names
                // the newest release's tag), and stopping here also means the request ends at
                // one exchange instead of fetching the release page too.
                DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
                VERIFY(WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY,
                                        &redirects, sizeof(redirects)));

                if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                    WinHttpReceiveResponse(request, nullptr)) {
                    // Absent (a non-redirect answer) or longer than the buffer: the query
                    // fails and the check reports no update.
                    TCHAR location[kLocationMax];
                    DWORD size = sizeof(location);
                    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                                            WINHTTP_HEADER_NAME_BY_INDEX, location, &size,
                                            WINHTTP_NO_HEADER_INDEX))
                        queried = VersionFromLocation(location, version);
                }

                VERIFY(WinHttpCloseHandle(request));
            }

            VERIFY(WinHttpCloseHandle(connection));
        }

        VERIFY(WinHttpCloseHandle(session));
    }

    return queried;
}

// Worker thread: one request, then publish the result and wake the window waiting for it.
DWORD WINAPI CheckThread(LPVOID /*param*/) {
    TCHAR version[kVersionMax];
    const bool available = QueryLatestVersion(version) && IsNewerThanThisBuild(version);

    // Publish the result and release the slot -- so the next About open starts a fresh check
    // -- before waking the dialog. MemoryBarrier keeps both stores from being seen after the
    // notification on a weakly ordered architecture, so the WM_UPDATECHECK handler's
    // IsAvailable() cannot still observe the previous result.
    g_available = available ? TRUE : FALSE;
    g_running   = FALSE;
    MemoryBarrier();

    // Not VERIFYed: the target window may legitimately have been destroyed just after it was
    // read (About closed while the request was finishing), and a failed post is exactly the
    // wanted outcome then -- the result stays cached for the next time About is opened.
    const HWND notify = g_notify;
    if (notify) PostMessage(notify, WM_UPDATECHECK, 0, 0);

    return 0;
}

} // namespace

namespace UpdateCheck {

void Start(HWND notify) {
    // Point the check -- the one already running, or the one started below -- at this window.
    // Start and Stop are the only writers of g_notify and both run on the UI thread (About's
    // WM_INITDIALOG / WM_DESTROY), so they cannot race each other; the worker only reads it.
    g_notify = notify;

    // An update already found stays found: the caller is showing the "Update" link for it
    // right now, and a newer release still being newer is all a repeat check could confirm.
    // So the first successful find is the last request this session makes -- re-opening
    // About after that costs nothing and needs no network.
    if (g_available) return;

    // One request at a time: a check already in flight reports to 'notify' by itself. Only
    // this thread ever raises the flag, and only after the worker it belongs to has lowered
    // it, so claiming the slot needs no atomic read-modify-write.
    if (g_running) return;
    g_running = TRUE;

    // Nothing waits on the worker (it reports by posting WM_UPDATECHECK), so its handle is
    // closed immediately -- the thread runs to completion on its own and the system reclaims
    // it. A creation failure just releases the slot again and leaves the link hidden.
    const HANDLE thread = CreateThread(nullptr, 0, CheckThread, nullptr, 0, nullptr);
    ASSERT(thread);
    if (thread) VERIFY(CloseHandle(thread));
    else        g_running = FALSE;
}

void Stop(HWND notify) {
    // Clear the target only while it is still 'notify': a newer About dialog may already have
    // taken it over, and that one must keep receiving the result.
    if (g_notify == notify) g_notify = nullptr;
}

bool IsAvailable() {
    return !!g_available;
}

} // namespace UpdateCheck
