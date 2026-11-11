// ============================================================================
// DualSense BB Controller Audio Fix (Open-Source Clean-Room Reimplementation)
// Target: Death Stranding Director's Cut (PC - Steam / Epic, x64)
// Output: XAudio2_9.dll (drop next to ds.exe)
//
// Fixes the two audio architecture bugs in Death Stranding Director's Cut:
//   1. [Primary Bug - WASAPI Exclusive Lock]
//      PadAudioPlayback (DualSense HD Haptics, 4ch 48kHz, ch2/ch3) opens the
//      DualSense audio endpoint in AUDCLNT_SHAREMODE_EXCLUSIVE, locking out
//      Wwise's DS_Sub_Output stream (BB Voice, ch0/ch1, AUDCLNT_SHAREMODE_SHARED)
//      with AUDCLNT_E_DEVICE_IN_USE (0x8889000A).
//      -> Transparently wraps Exclusive-mode IAudioClient + IAudioRenderClient
//         into low-latency WASAPI Shared Mode (16-bit PCM -> 32-bit IEEE Float
//         4ch conversion), allowing Windows Audio Engine to mix BB Voice
//         (Front Left/Right -> Controller Speaker) and Native HD Haptics
//         (Rear Left/Right -> Voice-Coil Actuators) simultaneously!
//
//   2. [Secondary Bug - BB Output Device Swap / Inversion Bug]
//      In UpdateSubOutput (RVA 0x1DD4A00), when replacing "DS_Sub_Output",
//      ds.exe forgot `mov r8d, ebx`, passing the OLD device ID (`r8d`) instead
//      of the newly selected target device ID (`ebx`) to AkOutputSettings(),
//      requiring a double-toggle or leaving UI/audio inverted.
//      -> Pattern-scans the exact instruction sequence (verified against the
//         "DS_Sub_Output" string) and patches `mov r8d, ebx` in-place so
//         selecting the DualSense speaker in-game takes effect on the first try.
//
//   3. [DualSense HID Speaker Routing - Haute 4DS "Follow game" / wired USB]
//      ds.exe's libScePad sends AudioControl with OutputPathSelect = 0
//      (headphone only, speaker unrouted) and never enables speaker volume.
//      -> Hooks KERNEL32!WriteFile and patches outgoing Output Report 0x02 to
//         route the R channel to the internal speaker (X-X R), volume 0x64,
//         preamp +2, while respecting the game's own headset detection.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define INITGUID
#include <windows.h>
#include <unknwn.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

// ----------------------------------------------------------------------------
// Minimal WASAPI & MMDevice COM Declarations (avoids SDK header version issues)
// ----------------------------------------------------------------------------
typedef LONGLONG REFERENCE_TIME;

enum EDataFlow {
    eRender = 0,
    eCapture = 1,
    eAll = 2
};

enum ERole {
    eConsole = 0,
    eMultimedia = 1,
    eCommunications = 2
};

enum AUDCLNT_SHAREMODE {
    AUDCLNT_SHAREMODE_SHARED = 0,
    AUDCLNT_SHAREMODE_EXCLUSIVE = 1
};

#ifndef AUDCLNT_STREAMFLAGS_CROSSPROCESS
#define AUDCLNT_STREAMFLAGS_CROSSPROCESS        0x00010000
#define AUDCLNT_STREAMFLAGS_LOOPBACK            0x00020000
#define AUDCLNT_STREAMFLAGS_EVENTCALLBACK       0x00040000
#define AUDCLNT_STREAMFLAGS_NOPERSIST           0x00080000
#define AUDCLNT_STREAMFLAGS_RATEADJUST          0x00100000
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM      0x80000000
#endif

#ifndef AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY
#define AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY  0x1
#define AUDCLNT_BUFFERFLAGS_SILENT              0x2
#define AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR     0x4
#endif

// GUIDs
static const GUID CLSID_MMDeviceEnumerator_Local =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
// Note: ds.exe uses CLSID {BCDE0395-E52F-467C-8E3D-C4579291692E} (WinSDK MMDeviceEnumerator)
// AND standard {BCDE0395-E52F-467C-8E3D-C4579291433C}? Let's check IID_IMMDeviceEnumerator instead!
static const GUID IID_IMMDeviceEnumerator_Local =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID IID_IMMDeviceCollection_Local =
    {0x0BD7A1BE, 0x7A1A, 0x44DB, {0x83, 0x97, 0xCC, 0x53, 0x92, 0x38, 0x7B, 0x5E}};
static const GUID IID_IMMDevice_Local =
    {0xD666063F, 0x1587, 0x4E43, {0x81, 0xF1, 0xB9, 0x48, 0xE8, 0x07, 0x36, 0x3F}};
static const GUID IID_IAudioClient_Local =
    {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID IID_IAudioRenderClient_Local =
    {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const GUID SubFormat_PCM_Local =
    {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID SubFormat_IEEE_Float_Local =
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
// Private marker IID to avoid double-wrapping
static const GUID IID_IBBAudioFixProxyMarker =
    {0xBB50A0D1, 0x2026, 0x4D55, {0x9A, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01, 0x10}};

struct IPropertyStore;
struct IMMNotificationClient;

MIDL_INTERFACE("D666063F-1587-4E43-81F1-B948E807363F")
IMMDevice_Raw : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Activate(REFIID iid, DWORD dwClsCtx, void* pActivationParams, void** ppInterface) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD stgmAccess, IPropertyStore** ppProperties) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetId(LPWSTR* ppstrId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetState(DWORD* pdwState) = 0;
};

MIDL_INTERFACE("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E")
IMMDeviceCollection_Raw : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCount(UINT* pcDevices) = 0;
    virtual HRESULT STDMETHODCALLTYPE Item(UINT nDevice, IMMDevice_Raw** ppDevice) = 0;
};

MIDL_INTERFACE("A95664D2-9614-4F35-A746-DE8DB63617E6")
IMMDeviceEnumerator_Raw : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow dataFlow, DWORD dwStateMask, IMMDeviceCollection_Raw** ppDevices) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role, IMMDevice_Raw** ppEndpoint) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR pwstrId, IMMDevice_Raw** ppDevice) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient* pClient) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient* pClient) = 0;
};

MIDL_INTERFACE("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2")
IAudioClient_Raw : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE ShareMode, DWORD StreamFlags, REFERENCE_TIME hnsBufferDuration, REFERENCE_TIME hnsPeriodicity, const WAVEFORMATEX* pFormat, LPCGUID AudioSessionGuid) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* pNumBufferFrames) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* phnsLatency) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* pNumBufferFrames) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE ShareMode, const WAVEFORMATEX* pFormat, WAVEFORMATEX** ppClosestMatch) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** ppDeviceFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* phnsDefaultDevicePeriod, REFERENCE_TIME* phnsMinimumDevicePeriod) = 0;
    virtual HRESULT STDMETHODCALLTYPE Start(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE Reset(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE eventHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetService(REFIID riid, void** ppv) = 0;
};

MIDL_INTERFACE("F294ACFC-3146-4483-A7BF-ADDCA7C260E2")
IAudioRenderClient_Raw : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 NumFramesRequested, BYTE** ppData) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 NumFramesWritten, DWORD dwFlags) = 0;
};

static inline bool GuidEq(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

// ----------------------------------------------------------------------------
// Configuration & Diagnostic Logger
// ----------------------------------------------------------------------------
struct ModConfig {
    bool enableSharedModeHaptics = true;
    bool fixBBDeviceSwapBug      = true;
    bool enableLog               = true;
    float hapticsGain            = 1.0f;

    // DualSense HID speaker routing fix (Haute 4DS "Follow game" / wired USB)
    bool     fixSpeakerRouting     = true;
    uint8_t  speakerOutputPath     = 3;    // OutputPathSelect: 2 = L-L R, 3 = X-X R
    uint8_t  speakerVolume         = 0x64; // valid range 0x3D..0x64
    uint8_t  speakerPreampGain     = 2;    // AudioControl2 SpeakerCompPreGain 0..7
    bool     assumeNoHeadsetAtStart = true;
    uint32_t reassertIntervalMs    = 1000; // 0 = only patch reports the game itself sends
};

static ModConfig g_cfg;
static wchar_t   g_dllDir[MAX_PATH] = {0};
static wchar_t   g_logPath[MAX_PATH] = {0};
static CRITICAL_SECTION g_logCs;
static bool      g_logCsReady = false;

static void LogMsg(const char* fmt, ...) {
    if (!g_cfg.enableLog || !g_logCsReady || g_logPath[0] == L'\0') return;
    EnterCriticalSection(&g_logCs);
    HANDLE hFile = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char prefix[64];
        int plen = std::snprintf(prefix, sizeof(prefix), "[%02d:%02d:%02d.%03d] ",
                                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        char body[1024];
        va_list ap;
        va_start(ap, fmt);
        int blen = std::vsnprintf(body, sizeof(body) - 2, fmt, ap);
        va_end(ap);
        if (blen < 0) blen = 0;
        body[blen++] = '\r';
        body[blen++] = '\n';
        DWORD written = 0;
        if (plen > 0) WriteFile(hFile, prefix, (DWORD)plen, &written, nullptr);
        WriteFile(hFile, body, (DWORD)blen, &written, nullptr);
        CloseHandle(hFile);
    }
    LeaveCriticalSection(&g_logCs);
}

static void LoadConfig(HMODULE hSelf) {
    wchar_t selfPath[MAX_PATH] = {0};
    GetModuleFileNameW(hSelf, selfPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(selfPath, L'\\');
    if (lastSlash) {
        size_t dirLen = (size_t)(lastSlash - selfPath + 1);
        wcsncpy(g_dllDir, selfPath, dirLen);
        g_dllDir[dirLen] = L'\0';
    }

    wchar_t iniPath[MAX_PATH] = {0};
    std::swprintf(iniPath, MAX_PATH, L"%lsXAudio2_9_BBFix.ini", g_dllDir);
    std::swprintf(g_logPath, MAX_PATH, L"%lsXAudio2_9_BBFix.log", g_dllDir);

    g_cfg.enableSharedModeHaptics = GetPrivateProfileIntW(L"BBFix", L"EnableSharedModeHaptics", 1, iniPath) != 0;
    g_cfg.fixBBDeviceSwapBug      = GetPrivateProfileIntW(L"BBFix", L"FixBBDeviceSwapBug", 1, iniPath) != 0;
    g_cfg.enableLog               = GetPrivateProfileIntW(L"BBFix", L"EnableLog", 1, iniPath) != 0;

    wchar_t gainBuf[64] = {0};
    GetPrivateProfileStringW(L"BBFix", L"HapticsGain", L"1.0", gainBuf, 63, iniPath);
    float g = (float)_wtof(gainBuf);
    if (g >= 0.0f && g <= 4.0f) {
        g_cfg.hapticsGain = g;
    }

    g_cfg.fixSpeakerRouting = GetPrivateProfileIntW(L"BBFix", L"FixDualSenseSpeakerRouting", 1, iniPath) != 0;
    {
        UINT path = GetPrivateProfileIntW(L"BBFix", L"SpeakerOutputPath", 3, iniPath);
        g_cfg.speakerOutputPath = (path == 2) ? 2 : 3;
        UINT vol = GetPrivateProfileIntW(L"BBFix", L"SpeakerVolume", 100, iniPath);
        if (vol < 0x3D) vol = 0x3D;
        if (vol > 0x64) vol = 0x64;
        g_cfg.speakerVolume = (uint8_t)vol;
        UINT pre = GetPrivateProfileIntW(L"BBFix", L"SpeakerPreampGain", 2, iniPath);
        g_cfg.speakerPreampGain = (uint8_t)(pre > 7 ? 7 : pre);
        g_cfg.assumeNoHeadsetAtStart = GetPrivateProfileIntW(L"BBFix", L"AssumeNoHeadsetAtStart", 1, iniPath) != 0;
        g_cfg.reassertIntervalMs = GetPrivateProfileIntW(L"BBFix", L"SpeakerReassertIntervalMs", 1000, iniPath);
    }

    if (g_cfg.enableLog) {
        // Truncate log on fresh launch
        HANDLE hFile = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    }
}

// ----------------------------------------------------------------------------
// ProxyAudioClient & ProxyAudioRenderClient
// Converts PadAudioPlayback's Exclusive-mode 16-bit PCM stream into a
// low-latency WASAPI Shared-mode stream so Wwise BB audio can coexist!
// ----------------------------------------------------------------------------
class ProxyAudioClient;

class ProxyAudioRenderClient final : public IAudioRenderClient_Raw {
public:
    ProxyAudioRenderClient(ProxyAudioClient* parent, IAudioRenderClient_Raw* realRc);
    ~ProxyAudioRenderClient();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 NumFramesRequested, BYTE** ppData) override;
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 NumFramesWritten, DWORD dwFlags) override;

private:
    std::atomic<ULONG>      m_ref{1};
    ProxyAudioClient*       m_parent;
    IAudioRenderClient_Raw* m_real;
    std::vector<uint8_t>    m_staging;
};

class ProxyAudioClient final : public IAudioClient_Raw {
public:
    explicit ProxyAudioClient(IAudioClient_Raw* realClient)
        : m_real(realClient) {}

    ~ProxyAudioClient() {
        if (m_pMixFormat) {
            CoTaskMemFree(m_pMixFormat);
            m_pMixFormat = nullptr;
        }
        if (m_real) {
            m_real->Release();
            m_real = nullptr;
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (GuidEq(riid, IID_IBBAudioFixProxyMarker)) {
            *ppvObject = this;
            return S_OK;
        }
        if (GuidEq(riid, IID_IUnknown) || GuidEq(riid, IID_IAudioClient_Local)) {
            *ppvObject = static_cast<IAudioClient_Raw*>(this);
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppvObject);
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++m_ref;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE Initialize(
        AUDCLNT_SHAREMODE ShareMode,
        DWORD StreamFlags,
        REFERENCE_TIME hnsBufferDuration,
        REFERENCE_TIME hnsPeriodicity,
        const WAVEFORMATEX* pFormat,
        LPCGUID AudioSessionGuid) override
    {
        if (!g_cfg.enableSharedModeHaptics || ShareMode != AUDCLNT_SHAREMODE_EXCLUSIVE || !pFormat) {
            return m_real->Initialize(ShareMode, StreamFlags, hnsBufferDuration, hnsPeriodicity, pFormat, AudioSessionGuid);
        }

        // PadAudioPlayback requests AUDCLNT_SHAREMODE_EXCLUSIVE (16-bit PCM, 4ch 48kHz).
        // Convert transparently to AUDCLNT_SHAREMODE_SHARED!
        if (m_pMixFormat) {
            CoTaskMemFree(m_pMixFormat);
            m_pMixFormat = nullptr;
        }
        HRESULT hr = m_real->GetMixFormat(&m_pMixFormat);
        if (FAILED(hr) || !m_pMixFormat) {
            LogMsg("[WASAPI] GetMixFormat failed (hr=0x%08X), falling back to native Initialize.", (unsigned)hr);
            return m_real->Initialize(ShareMode, StreamFlags, hnsBufferDuration, hnsPeriodicity, pFormat, AudioSessionGuid);
        }

        REFERENCE_TIME hnsDefaultPeriod = 100000; // 10 ms fallback
        REFERENCE_TIME hnsMinPeriod     = 30000;
        m_real->GetDevicePeriod(&hnsDefaultPeriod, &hnsMinPeriod);
        if (hnsDefaultPeriod <= 0) hnsDefaultPeriod = 100000;

        m_appChannels      = pFormat->nChannels;
        m_appSampleRate    = pFormat->nSamplesPerSec;
        m_appBitsPerSample = pFormat->wBitsPerSample;
        m_appBytesPerFrame = pFormat->nBlockAlign ? pFormat->nBlockAlign : (pFormat->nChannels * (pFormat->wBitsPerSample / 8));

        m_mixChannels      = m_pMixFormat->nChannels;
        m_mixSampleRate    = m_pMixFormat->nSamplesPerSec;
        m_mixBitsPerSample = m_pMixFormat->wBitsPerSample;
        m_mixBytesPerFrame = m_pMixFormat->nBlockAlign;
        m_mixIsFloat       = false;

        if (m_pMixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            m_mixIsFloat = true;
        } else if (m_pMixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && m_pMixFormat->cbSize >= 22) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(m_pMixFormat);
            if (GuidEq(ext->SubFormat, SubFormat_IEEE_Float_Local)) {
                m_mixIsFloat = true;
            }
        }

        DWORD sharedFlags = StreamFlags & (AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST);

        // Build target format: if app sample rate differs from mix sample rate, ask WASAPI to resample cleanly.
        WAVEFORMATEXTENSIBLE customFloatFmt = {};
        const WAVEFORMATEX* initFmt = m_pMixFormat;
        if (m_appSampleRate != m_mixSampleRate && m_mixIsFloat) {
            customFloatFmt.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
            customFloatFmt.Format.nChannels       = m_mixChannels;
            customFloatFmt.Format.nSamplesPerSec  = m_appSampleRate;
            customFloatFmt.Format.wBitsPerSample  = 32;
            customFloatFmt.Format.nBlockAlign     = m_mixChannels * 4;
            customFloatFmt.Format.nAvgBytesPerSec = customFloatFmt.Format.nSamplesPerSec * customFloatFmt.Format.nBlockAlign;
            customFloatFmt.Format.cbSize          = 22;
            customFloatFmt.Samples.wValidBitsPerSample = 32;
            if (m_pMixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                customFloatFmt.dwChannelMask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(m_pMixFormat)->dwChannelMask;
            } else {
                customFloatFmt.dwChannelMask = (m_mixChannels == 4) ? 0x33 : 0x3;
            }
            customFloatFmt.SubFormat = SubFormat_IEEE_Float_Local;
            initFmt = &customFloatFmt.Format;
            m_mixBytesPerFrame = customFloatFmt.Format.nBlockAlign;
            sharedFlags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        }

        // 3x default period (~30 ms) ring buffer gives zero-underrun safety while keeping active queue capped at 1-2 periods (~10 ms).
        REFERENCE_TIME sharedBufDur = hnsDefaultPeriod * 3;
        if (sharedBufDur < 200000) sharedBufDur = 200000;

        hr = m_real->Initialize(AUDCLNT_SHAREMODE_SHARED, sharedFlags, sharedBufDur, 0, initFmt, AudioSessionGuid);
        if (FAILED(hr)) {
            LogMsg("[WASAPI] Shared Initialize failed (hr=0x%08X)! Retrying with AUTOCONVERTPCM on app format...", (unsigned)hr);
            hr = m_real->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                sharedFlags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                sharedBufDur, 0, pFormat, AudioSessionGuid);
            if (SUCCEEDED(hr)) {
                // Direct 16-bit PCM accepted by WASAPI
                m_mixChannels      = m_appChannels;
                m_mixBitsPerSample = m_appBitsPerSample;
                m_mixBytesPerFrame = m_appBytesPerFrame;
                m_mixIsFloat       = false;
            } else {
                LogMsg("[WASAPI] Fallback Shared Initialize also failed (hr=0x%08X).", (unsigned)hr);
                return hr;
            }
        }

        m_real->GetBufferSize(&m_realBufferFrames);
        m_periodFrames = (UINT32)((hnsDefaultPeriod * m_appSampleRate + 5000000ULL) / 10000000ULL);
        if (m_periodFrames == 0 || m_periodFrames > m_realBufferFrames / 2) {
            m_periodFrames = (m_realBufferFrames >= 2) ? (m_realBufferFrames / 2) : 480;
        }

        m_convertedToShared = true;
        LogMsg("[WASAPI] Converted Exclusive stream -> Shared Mode! App(%uch %uHz %ubit) -> Mix(%uch %uHz %s), period=%u frames, ring=%u frames, gain=%.2f",
               (unsigned)m_appChannels, (unsigned)m_appSampleRate, (unsigned)m_appBitsPerSample,
               (unsigned)m_mixChannels, (unsigned)m_mixSampleRate, m_mixIsFloat ? "float32" : "pcm",
               (unsigned)m_periodFrames, (unsigned)m_realBufferFrames, g_cfg.hapticsGain);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* pNumBufferFrames) override {
        if (!pNumBufferFrames) return E_POINTER;
        if (m_convertedToShared) {
            *pNumBufferFrames = m_periodFrames;
            return S_OK;
        }
        return m_real->GetBufferSize(pNumBufferFrames);
    }

    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* phnsLatency) override {
        return m_real->GetStreamLatency(phnsLatency);
    }

    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* pNumBufferFrames) override {
        return m_real->GetCurrentPadding(pNumBufferFrames);
    }

    HRESULT STDMETHODCALLTYPE IsFormatSupported(
        AUDCLNT_SHAREMODE ShareMode,
        const WAVEFORMATEX* pFormat,
        WAVEFORMATEX** ppClosestMatch) override
    {
        if (g_cfg.enableSharedModeHaptics && ShareMode == AUDCLNT_SHAREMODE_EXCLUSIVE && pFormat) {
            // Allow 16-bit PCM format checks to succeed even if the user disabled Exclusive Control in Windows mmsys.cpl
            if (pFormat->wBitsPerSample == 16 && (pFormat->nChannels == 2 || pFormat->nChannels == 4)) {
                return S_OK;
            }
        }
        return m_real->IsFormatSupported(ShareMode, pFormat, ppClosestMatch);
    }

    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** ppDeviceFormat) override {
        return m_real->GetMixFormat(ppDeviceFormat);
    }

    HRESULT STDMETHODCALLTYPE GetDevicePeriod(
        REFERENCE_TIME* phnsDefaultDevicePeriod,
        REFERENCE_TIME* phnsMinimumDevicePeriod) override
    {
        return m_real->GetDevicePeriod(phnsDefaultDevicePeriod, phnsMinimumDevicePeriod);
    }

    HRESULT STDMETHODCALLTYPE Start(void) override {
        return m_real->Start();
    }

    HRESULT STDMETHODCALLTYPE Stop(void) override {
        return m_real->Stop();
    }

    HRESULT STDMETHODCALLTYPE Reset(void) override {
        return m_real->Reset();
    }

    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE eventHandle) override {
        return m_real->SetEventHandle(eventHandle);
    }

    HRESULT STDMETHODCALLTYPE GetService(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        HRESULT hr = m_real->GetService(riid, ppv);
        if (SUCCEEDED(hr) && m_convertedToShared && GuidEq(riid, IID_IAudioRenderClient_Local) && *ppv) {
            auto* realRc = static_cast<IAudioRenderClient_Raw*>(*ppv);
            *ppv = static_cast<IAudioRenderClient_Raw*>(new ProxyAudioRenderClient(this, realRc));
        }
        return hr;
    }

    IAudioClient_Raw* m_real               = nullptr;
    WAVEFORMATEX*     m_pMixFormat         = nullptr;
    bool              m_convertedToShared  = false;
    UINT16            m_appChannels        = 4;
    UINT32            m_appSampleRate      = 48000;
    UINT16            m_appBitsPerSample   = 16;
    UINT16            m_appBytesPerFrame   = 8;
    UINT16            m_mixChannels        = 4;
    UINT32            m_mixSampleRate      = 48000;
    UINT16            m_mixBitsPerSample   = 32;
    UINT16            m_mixBytesPerFrame   = 16;
    bool              m_mixIsFloat         = true;
    UINT32            m_realBufferFrames   = 1440;
    UINT32            m_periodFrames       = 480;

private:
    std::atomic<ULONG> m_ref{1};
};

ProxyAudioRenderClient::ProxyAudioRenderClient(ProxyAudioClient* parent, IAudioRenderClient_Raw* realRc)
    : m_parent(parent), m_real(realRc)
{
    m_parent->AddRef();
    size_t initBytes = (size_t)(m_parent->m_periodFrames * 4) * m_parent->m_appBytesPerFrame;
    if (initBytes < 4096) initBytes = 4096;
    m_staging.resize(initBytes, 0);
}

ProxyAudioRenderClient::~ProxyAudioRenderClient() {
    if (m_real) {
        m_real->Release();
        m_real = nullptr;
    }
    if (m_parent) {
        m_parent->Release();
        m_parent = nullptr;
    }
}

HRESULT STDMETHODCALLTYPE ProxyAudioRenderClient::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (GuidEq(riid, IID_IUnknown) || GuidEq(riid, IID_IAudioRenderClient_Local)) {
        *ppvObject = static_cast<IAudioRenderClient_Raw*>(this);
        AddRef();
        return S_OK;
    }
    return m_real->QueryInterface(riid, ppvObject);
}

ULONG STDMETHODCALLTYPE ProxyAudioRenderClient::AddRef() {
    return ++m_ref;
}

ULONG STDMETHODCALLTYPE ProxyAudioRenderClient::Release() {
    ULONG r = --m_ref;
    if (r == 0) delete this;
    return r;
}

HRESULT STDMETHODCALLTYPE ProxyAudioRenderClient::GetBuffer(UINT32 NumFramesRequested, BYTE** ppData) {
    if (!ppData) return E_POINTER;
    size_t needed = (size_t)NumFramesRequested * m_parent->m_appBytesPerFrame;
    if (needed > m_staging.size()) {
        m_staging.resize(needed, 0);
    }
    if (needed > 0) {
        std::memset(m_staging.data(), 0, needed);
    }
    *ppData = m_staging.data();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyAudioRenderClient::ReleaseBuffer(UINT32 NumFramesWritten, DWORD dwFlags) {
    if (NumFramesWritten == 0) return S_OK;

    UINT32 padding = 0;
    if (FAILED(m_parent->m_real->GetCurrentPadding(&padding))) {
        return S_OK;
    }

    const UINT32 ringCap  = m_parent->m_realBufferFrames;
    const UINT32 avail    = (ringCap > padding) ? (ringCap - padding) : 0;
    // Cap queued audio at 2 periods (~20ms) so HD haptics always stay tightly synced with gameplay
    const UINT32 maxQueue = m_parent->m_periodFrames * 2 + (m_parent->m_periodFrames / 2);
    if (padding >= maxQueue || avail == 0) {
        return S_OK;
    }

    UINT32 toWrite = NumFramesWritten;
    if (toWrite > avail) toWrite = avail;
    UINT32 maxAdd = maxQueue - padding;
    if (toWrite > maxAdd) toWrite = maxAdd;
    if (toWrite == 0) return S_OK;

    BYTE* pDst = nullptr;
    HRESULT hr = m_real->GetBuffer(toWrite, &pDst);
    if (FAILED(hr) || !pDst) {
        return S_OK;
    }

    if (dwFlags & AUDCLNT_BUFFERFLAGS_SILENT) {
        std::memset(pDst, 0, (size_t)toWrite * m_parent->m_mixBytesPerFrame);
        m_real->ReleaseBuffer(toWrite, AUDCLNT_BUFFERFLAGS_SILENT);
        return S_OK;
    }

    const UINT32 frameSkip = NumFramesWritten - toWrite;
    const UINT16 appCh     = m_parent->m_appChannels;
    const UINT16 mixCh     = m_parent->m_mixChannels;
    const float  gain      = g_cfg.hapticsGain;

    if (m_parent->m_appBitsPerSample == 16 && m_parent->m_mixIsFloat) {
        const int16_t* src = reinterpret_cast<const int16_t*>(
            m_staging.data() + (size_t)frameSkip * m_parent->m_appBytesPerFrame);
        float* dst = reinterpret_cast<float*>(pDst);
        const float scale = (1.0f / 32768.0f) * gain;

        for (UINT32 f = 0; f < toWrite; ++f) {
            const int16_t* sf = src + (size_t)f * appCh;
            float*         df = dst + (size_t)f * mixCh;
            for (UINT16 c = 0; c < mixCh; ++c) {
                float v = 0.0f;
                if (c < appCh) {
                    v = (float)sf[c] * scale;
                    if (v > 1.0f) v = 1.0f;
                    else if (v < -1.0f) v = -1.0f;
                }
                df[c] = v;
            }
        }
    } else if (m_parent->m_appBytesPerFrame == m_parent->m_mixBytesPerFrame) {
        std::memcpy(pDst,
                    m_staging.data() + (size_t)frameSkip * m_parent->m_appBytesPerFrame,
                    (size_t)toWrite * m_parent->m_mixBytesPerFrame);
    } else {
        std::memset(pDst, 0, (size_t)toWrite * m_parent->m_mixBytesPerFrame);
    }

    m_real->ReleaseBuffer(toWrite, 0);
    return S_OK;
}

// ----------------------------------------------------------------------------
// ProxyMMDevice, ProxyMMDeviceCollection, ProxyMMDeviceEnumerator
// ----------------------------------------------------------------------------
class ProxyMMDevice final : public IMMDevice_Raw {
public:
    explicit ProxyMMDevice(IMMDevice_Raw* realDev) : m_real(realDev) {}
    ~ProxyMMDevice() { if (m_real) m_real->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (GuidEq(riid, IID_IBBAudioFixProxyMarker)) {
            *ppvObject = this;
            return S_OK;
        }
        if (GuidEq(riid, IID_IUnknown) || GuidEq(riid, IID_IMMDevice_Local)) {
            *ppvObject = static_cast<IMMDevice_Raw*>(this);
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppvObject);
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE Activate(REFIID iid, DWORD dwClsCtx, void* pActivationParams, void** ppInterface) override {
        HRESULT hr = m_real->Activate(iid, dwClsCtx, pActivationParams, ppInterface);
        if (SUCCEEDED(hr) && ppInterface && *ppInterface && GuidEq(iid, IID_IAudioClient_Local)) {
            auto* realAc = static_cast<IAudioClient_Raw*>(*ppInterface);
            void* existingMarker = nullptr;
            if (FAILED(realAc->QueryInterface(IID_IBBAudioFixProxyMarker, &existingMarker))) {
                *ppInterface = static_cast<IAudioClient_Raw*>(new ProxyAudioClient(realAc));
            }
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD stgmAccess, IPropertyStore** ppProperties) override {
        return m_real->OpenPropertyStore(stgmAccess, ppProperties);
    }
    HRESULT STDMETHODCALLTYPE GetId(LPWSTR* ppstrId) override {
        return m_real->GetId(ppstrId);
    }
    HRESULT STDMETHODCALLTYPE GetState(DWORD* pdwState) override {
        return m_real->GetState(pdwState);
    }

private:
    std::atomic<ULONG> m_ref{1};
    IMMDevice_Raw*     m_real;
};

static IMMDevice_Raw* WrapMMDevice(IMMDevice_Raw* dev) {
    if (!dev) return nullptr;
    void* marker = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_IBBAudioFixProxyMarker, &marker))) {
        return dev;
    }
    return new ProxyMMDevice(dev);
}

class ProxyMMDeviceCollection final : public IMMDeviceCollection_Raw {
public:
    explicit ProxyMMDeviceCollection(IMMDeviceCollection_Raw* realCol) : m_real(realCol) {}
    ~ProxyMMDeviceCollection() { if (m_real) m_real->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (GuidEq(riid, IID_IUnknown) || GuidEq(riid, IID_IMMDeviceCollection_Local)) {
            *ppvObject = static_cast<IMMDeviceCollection_Raw*>(this);
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppvObject);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE GetCount(UINT* pcDevices) override {
        return m_real->GetCount(pcDevices);
    }
    HRESULT STDMETHODCALLTYPE Item(UINT nDevice, IMMDevice_Raw** ppDevice) override {
        HRESULT hr = m_real->Item(nDevice, ppDevice);
        if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
            *ppDevice = WrapMMDevice(*ppDevice);
        }
        return hr;
    }

private:
    std::atomic<ULONG>       m_ref{1};
    IMMDeviceCollection_Raw* m_real;
};

class ProxyMMDeviceEnumerator final : public IMMDeviceEnumerator_Raw {
public:
    explicit ProxyMMDeviceEnumerator(IMMDeviceEnumerator_Raw* realEnum) : m_real(realEnum) {}
    ~ProxyMMDeviceEnumerator() { if (m_real) m_real->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (GuidEq(riid, IID_IBBAudioFixProxyMarker)) {
            *ppvObject = this;
            return S_OK;
        }
        if (GuidEq(riid, IID_IUnknown) || GuidEq(riid, IID_IMMDeviceEnumerator_Local)) {
            *ppvObject = static_cast<IMMDeviceEnumerator_Raw*>(this);
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppvObject);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow dataFlow, DWORD dwStateMask, IMMDeviceCollection_Raw** ppDevices) override {
        HRESULT hr = m_real->EnumAudioEndpoints(dataFlow, dwStateMask, ppDevices);
        if (SUCCEEDED(hr) && ppDevices && *ppDevices) {
            *ppDevices = new ProxyMMDeviceCollection(*ppDevices);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role, IMMDevice_Raw** ppEndpoint) override {
        HRESULT hr = m_real->GetDefaultAudioEndpoint(dataFlow, role, ppEndpoint);
        if (SUCCEEDED(hr) && ppEndpoint && *ppEndpoint) {
            *ppEndpoint = WrapMMDevice(*ppEndpoint);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR pwstrId, IMMDevice_Raw** ppDevice) override {
        HRESULT hr = m_real->GetDevice(pwstrId, ppDevice);
        if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
            *ppDevice = WrapMMDevice(*ppDevice);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient* pClient) override {
        return m_real->RegisterEndpointNotificationCallback(pClient);
    }

    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient* pClient) override {
        return m_real->UnregisterEndpointNotificationCallback(pClient);
    }

private:
    std::atomic<ULONG>       m_ref{1};
    IMMDeviceEnumerator_Raw* m_real;
};

// ----------------------------------------------------------------------------
// IAT Hook for ole32.dll!CoCreateInstance & ds.exe Binary Pattern Patcher
// ----------------------------------------------------------------------------
typedef HRESULT(WINAPI* PFN_CoCreateInstance)(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID* ppv);
static HRESULT WINAPI Hook_CoCreateInstance(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID* ppv) {
    HRESULT hr = ::CoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && GuidEq(riid, IID_IMMDeviceEnumerator_Local)) {
        auto* realEnum = static_cast<IMMDeviceEnumerator_Raw*>(*ppv);
        void* marker = nullptr;
        if (FAILED(realEnum->QueryInterface(IID_IBBAudioFixProxyMarker, &marker))) {
            *ppv = static_cast<IMMDeviceEnumerator_Raw*>(new ProxyMMDeviceEnumerator(realEnum));
            LogMsg("[Hook] Wrapped IMMDeviceEnumerator via CoCreateInstance.");
        }
    }
    return hr;
}

struct DelayImportDescriptorV2 {
    DWORD Attributes;
    DWORD DllNameRVA;
    DWORD ModuleHandleRVA;
    DWORD ImportAddressTableRVA;
    DWORD ImportNameTableRVA;
    DWORD BoundImportAddressTableRVA;
    DWORD UnloadInformationTableRVA;
    DWORD TimeDateStamp;
};

static bool PatchThunkList(uint8_t* base, PIMAGE_THUNK_DATA origThunk, PIMAGE_THUNK_DATA iatThunk, const char* targetFunc, void* newFunc) {
    for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++iatThunk) {
        if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) continue;
        auto* byName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(base + origThunk->u1.AddressOfData);
        if (std::strcmp(reinterpret_cast<const char*>(byName->Name), targetFunc) == 0) {
            DWORD oldProt = 0;
            if (VirtualProtect(&iatThunk->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &oldProt)) {
                iatThunk->u1.Function = reinterpret_cast<ULONG_PTR>(newFunc);
                VirtualProtect(&iatThunk->u1.Function, sizeof(ULONG_PTR), oldProt, &oldProt);
                return true;
            }
        }
    }
    return false;
}

static bool HookModuleIATAndDelayIAT(HMODULE hMod, const char* targetDll, const char* targetFunc, void* newFunc) {
    if (!hMod) return false;
    auto* base = reinterpret_cast<uint8_t*>(hMod);
    auto* dos  = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    bool hookedAny = false;

    // 1. Standard Import Directory (IAT)
    const auto& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress && impDir.Size) {
        auto* imp = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(base + impDir.VirtualAddress);
        for (; imp->Name != 0; ++imp) {
            const char* dllName = reinterpret_cast<const char*>(base + imp->Name);
            if (_stricmp(dllName, targetDll) != 0 || !imp->OriginalFirstThunk) continue;
            auto* origThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + imp->OriginalFirstThunk);
            auto* iatThunk  = reinterpret_cast<PIMAGE_THUNK_DATA>(base + imp->FirstThunk);
            if (PatchThunkList(base, origThunk, iatThunk, targetFunc, newFunc)) {
                hookedAny = true;
            }
        }
    }

    // 2. Delay-Load Import Directory (Delay-IAT — used by ds.exe for ole32.dll!)
    const auto& delayDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayDir.VirtualAddress && delayDir.Size) {
        auto* ddesc = reinterpret_cast<DelayImportDescriptorV2*>(base + delayDir.VirtualAddress);
        for (; ddesc->DllNameRVA != 0; ++ddesc) {
            // Attributes bit 0 == 1 means RVA-based (standard on x64 MSVC)
            if ((ddesc->Attributes & 1) == 0) continue;
            const char* dllName = reinterpret_cast<const char*>(base + ddesc->DllNameRVA);
            if (_stricmp(dllName, targetDll) != 0 || !ddesc->ImportNameTableRVA || !ddesc->ImportAddressTableRVA) continue;
            auto* origThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + ddesc->ImportNameTableRVA);
            auto* iatThunk  = reinterpret_cast<PIMAGE_THUNK_DATA>(base + ddesc->ImportAddressTableRVA);
            if (PatchThunkList(base, origThunk, iatThunk, targetFunc, newFunc)) {
                hookedAny = true;
            }
        }
    }

    return hookedAny;
}

// ----------------------------------------------------------------------------
// Fix #3: DualSense HID speaker routing (Haute 4DS "Follow game" / wired USB)
//
// ds.exe's built-in libScePad sends the USB Output Report 0x02 (48 bytes) via
// KERNEL32!WriteFile. When it thinks no headset is plugged in it only sets
//   AllowAudioControl (byte1 bit7) + AudioControl = 0x0C (echo/noise cancel)
// leaving OutputPathSelect (AudioControl bits 5..4) at 0 = "L-R X", i.e.
// headphone L/R only, internal speaker UNROUTED. It never enables speaker
// volume / preamp either. A real wired DualSense (or Haute 4DS in "Follow game"
// mode, which forwards the game's audio flags) therefore keeps the speaker
// silent, while "Always on" overrides the routing in adapter firmware.
//
// We patch the outgoing report in-place (same values Linux hid-playstation
// uses when no headphone is detected): OutputPathSelect = 3 (X-X R),
// speaker volume 0x64, SpeakerCompPreGain = 2, and clear SpeakerMute.
// The game's own headset decision (AudioControl 0x0C vs 0x00) is respected.
//
// Report layout (buf[0] = report ID 0x02, SetStateData starts at buf[1]):
//   buf[1]  flags0: bit5 AllowSpeakerVolume, bit7 AllowAudioControl
//   buf[2]  flags1: bit1 AllowAudioMute,     bit7 AllowAudioControl2
//   buf[6]  VolumeSpeaker
//   buf[8]  AudioControl: MicSelect(1:0) Echo(2) Noise(3) OutputPath(5:4) InputPath(7:6)
//   buf[10] MuteControl:  bit5 SpeakerMute
//   buf[38] AudioControl2: SpeakerCompPreGain(2:0)
// ----------------------------------------------------------------------------
struct HIDD_ATTRIBUTES_Local {
    ULONG  Size;
    USHORT VendorID;
    USHORT ProductID;
    USHORT VersionNumber;
};
typedef BOOLEAN(__stdcall* PFN_HidD_GetAttributes)(HANDLE, HIDD_ATTRIBUTES_Local*);

static std::atomic<PFN_HidD_GetAttributes> g_pfnHidGetAttributes{nullptr};
static std::atomic<bool>      g_hidLoadTried{false};
static std::atomic<HANDLE>    g_lastDsHandle{nullptr};
static std::atomic<ULONGLONG> g_lastDsCheckTick{0};
static std::atomic<int>       g_dsHeadsetState{-1};   // -1 unknown, 0 speaker, 1 headset
static std::atomic<ULONGLONG> g_lastSpeakerAssertTick{0};
static std::atomic<uint32_t>  g_speakerPatchCount{0};

static bool IsDualSenseHidHandle(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return false;
    ULONGLONG now = GetTickCount64();
    if (g_lastDsHandle.load() == h && now - g_lastDsCheckTick.load() < 2000) return true;

    PFN_HidD_GetAttributes fn = g_pfnHidGetAttributes.load();
    if (!fn) {
        if (g_hidLoadTried.exchange(true)) return false;
        HMODULE hHid = LoadLibraryW(L"hid.dll");
        if (hHid) fn = reinterpret_cast<PFN_HidD_GetAttributes>(GetProcAddress(hHid, "HidD_GetAttributes"));
        if (!fn) {
            LogMsg("[HID] Could not resolve hid.dll!HidD_GetAttributes; speaker routing fix disabled.");
            return false;
        }
        g_pfnHidGetAttributes.store(fn);
    }

    HIDD_ATTRIBUTES_Local attr = {};
    attr.Size = sizeof(attr);
    if (!fn(h, &attr)) return false;
    bool isDs = attr.VendorID == 0x054C && (attr.ProductID == 0x0CE6 || attr.ProductID == 0x0DF2);
    if (isDs) {
        if (g_lastDsHandle.exchange(h) != h) {
            LogMsg("[HID] DualSense output handle detected (VID=%04X PID=%04X) - speaker routing fix active.",
                   attr.VendorID, attr.ProductID);
        }
        g_lastDsCheckTick.store(now);
    }
    return isDs;
}

static void PatchDualSenseOutputReport(uint8_t* r) {
    const bool gameSetsAudio = (r[1] & 0x80) != 0;

    if (gameSetsAudio) {
        const uint8_t path = (r[8] >> 4) & 0x3;
        if (path != 0) return; // game routed the speaker itself - leave untouched
        // ds.exe: 0x0C when no headset (internal mic / speaker), 0x00 when headset plugged
        int newState = (r[8] & 0x0C) ? 0 : 1;
        int oldState = g_dsHeadsetState.exchange(newState);
        if (oldState != newState) {
            LogMsg("[HID] Game audio-control report: AudioControl=0x%02X -> %s.", r[8],
                   newState == 0 ? "no headset, routing to controller speaker" : "headset plugged, leaving headphone routing");
        }
    } else if (g_dsHeadsetState.load() == -1) {
        if (!g_cfg.assumeNoHeadsetAtStart) return;
        int expected = -1;
        g_dsHeadsetState.compare_exchange_strong(expected, 0);
    }

    if (g_dsHeadsetState.load() != 0) return;

    ULONGLONG now = GetTickCount64();
    if (!gameSetsAudio) {
        if (g_cfg.reassertIntervalMs == 0 && g_speakerPatchCount.load() != 0) return;
        if (g_speakerPatchCount.load() != 0 && now - g_lastSpeakerAssertTick.load() < g_cfg.reassertIntervalMs) return;
        r[8] = 0x0C; // same base value ds.exe uses for "no headset"
    }

    r[1] |= 0x80 | 0x20;                                          // AllowAudioControl + AllowSpeakerVolume
    r[8]  = (uint8_t)((r[8] & ~0x30) | (g_cfg.speakerOutputPath << 4));
    r[6]  = g_cfg.speakerVolume;
    r[2] |= 0x80;                                                 // AllowAudioControl2
    r[38] = (uint8_t)((r[38] & ~0x07) | (g_cfg.speakerPreampGain & 0x07));
    if (r[2] & 0x02) r[10] &= (uint8_t)~0x20;                     // game writes MuteControl: keep speaker unmuted

    g_lastSpeakerAssertTick.store(now);
    uint32_t n = ++g_speakerPatchCount;
    if (n == 1 || gameSetsAudio) {
        LogMsg("[HID] Patched DualSense report #%u: OutputPath=%u SpeakerVol=0x%02X PreGain=%u (%s).",
               n, (unsigned)g_cfg.speakerOutputPath, (unsigned)g_cfg.speakerVolume,
               (unsigned)g_cfg.speakerPreampGain, gameSetsAudio ? "game audio report" : "injected");
    }
}

static BOOL WINAPI Hook_WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
                                  LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped) {
    if (g_cfg.fixSpeakerRouting && lpBuffer &&
        nNumberOfBytesToWrite >= 48 && nNumberOfBytesToWrite <= 64 &&
        static_cast<const uint8_t*>(lpBuffer)[0] == 0x02 &&
        IsDualSenseHidHandle(hFile))
    {
        // ds.exe builds each report in its own scratch buffer right before this call
        PatchDualSenseOutputReport(const_cast<uint8_t*>(static_cast<const uint8_t*>(lpBuffer)));
    }
    return ::WriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped);
}

// Pattern scanner helper (-1 = wildcard byte)
static uint8_t* FindPattern(uint8_t* start, size_t size, const int* pat, size_t patLen) {
    if (size < patLen) return nullptr;
    size_t limit = size - patLen;
    for (size_t i = 0; i <= limit; ++i) {
        bool ok = true;
        for (size_t j = 0; j < patLen; ++j) {
            if (pat[j] >= 0 && start[i + j] != (uint8_t)pat[j]) {
                ok = false;
                break;
            }
        }
        if (ok) return start + i;
    }
    return nullptr;
}

typedef void(__fastcall* PFN_PadCloseStream)(void* pad);
typedef void(__fastcall* PFN_PadOpenAudioClient)(void* pad);

static void**                 g_ppPadAudioPlayback = nullptr;
static PFN_PadCloseStream     g_pfnPadCloseStream  = nullptr;
static PFN_PadOpenAudioClient g_pfnPadOpenClient   = nullptr;

extern "C" __declspec(dllexport) int BBAudioFix_ApplyPatchesToModule(HMODULE hExe) {
    if (!hExe) return 0;
    auto* base = reinterpret_cast<uint8_t*>(hExe);
    auto* dos  = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    int appliedCount = 0;

    // 1. IAT + Delay-IAT Hook ole32.dll!CoCreateInstance
    if (g_cfg.enableSharedModeHaptics) {
        if (HookModuleIATAndDelayIAT(hExe, "ole32.dll", "CoCreateInstance",
                                     reinterpret_cast<void*>(&Hook_CoCreateInstance))) {
            LogMsg("[Init] Hooked ole32.dll!CoCreateInstance (IAT / Delay-IAT) successfully.");
            appliedCount++;
        } else {
            LogMsg("[Init] Note: ole32.dll!CoCreateInstance not found in module IAT/Delay-IAT.");
        }
    }

    // 1b. IAT Hook KERNEL32!WriteFile (DualSense HID Output Report 0x02 speaker routing)
    if (g_cfg.fixSpeakerRouting) {
        bool hooked = HookModuleIATAndDelayIAT(hExe, "KERNEL32.dll", "WriteFile",
                                               reinterpret_cast<void*>(&Hook_WriteFile));
        hooked |= HookModuleIATAndDelayIAT(hExe, "api-ms-win-core-file-l1-1-0.dll", "WriteFile",
                                           reinterpret_cast<void*>(&Hook_WriteFile));
        if (hooked) {
            LogMsg("[Init] Hooked KERNEL32!WriteFile for DualSense speaker routing (path=%u vol=0x%02X pregain=%u).",
                   (unsigned)g_cfg.speakerOutputPath, (unsigned)g_cfg.speakerVolume, (unsigned)g_cfg.speakerPreampGain);
            appliedCount++;
        } else {
            LogMsg("[Init] Note: KERNEL32!WriteFile not found in module IAT; speaker routing fix inactive.");
        }
    }

    // Locate .text section
    auto* sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* textStart = nullptr;
    size_t   textSize  = 0;
    size_t   imageSize = nt->OptionalHeader.SizeOfImage;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".text", 5) == 0) {
            textStart = base + sec->VirtualAddress;
            textSize  = sec->Misc.VirtualSize;
            break;
        }
    }
    if (!textStart || !textSize) return appliedCount;

    // 2. Patch Bug #1: UpdateSubOutput (DS_Sub_Output ReplaceOutput missing `mov r8d, ebx`)
    // Original 11 bytes at 0x141dd4a00:
    //   45 33 C9                    xor r9d, r9d
    //   C7 44 24 20 00 00 00 00     mov dword ptr [rsp+0x20], 0
    // Followed immediately by:
    //   48 8D 15 <disp32>           lea rdx, [rip + disp32]  ; -> "DS_Sub_Output"
    if (g_cfg.fixBBDeviceSwapBug) {
        static const int patSubOut[] = {
            0x45, 0x33, 0xC9,
            0xC7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00,
            0x48, 0x8D, 0x15
        };
        uint8_t* hit = FindPattern(textStart, textSize, patSubOut, sizeof(patSubOut) / sizeof(patSubOut[0]));
        if (hit) {
            int32_t disp = *reinterpret_cast<const int32_t*>(hit + 14);
            const uint8_t* strPtr = hit + 18 + disp;
            if (strPtr >= base && strPtr + 14 <= base + imageSize &&
                std::memcmp(strPtr, "DS_Sub_Output", 14) == 0)
            {
                // Replacement 11 bytes:
                //   45 33 C9              xor r9d, r9d
                //   44 89 4C 24 20        mov dword ptr [rsp+0x20], r9d
                //   44 8B C3              mov r8d, ebx
                static const uint8_t patchBytes[11] = {
                    0x45, 0x33, 0xC9,
                    0x44, 0x89, 0x4C, 0x24, 0x20,
                    0x44, 0x8B, 0xC3
                };
                DWORD oldProt = 0;
                if (VirtualProtect(hit, sizeof(patchBytes), PAGE_EXECUTE_READWRITE, &oldProt)) {
                    std::memcpy(hit, patchBytes, sizeof(patchBytes));
                    VirtualProtect(hit, sizeof(patchBytes), oldProt, &oldProt);
                    FlushInstructionCache(GetCurrentProcess(), hit, sizeof(patchBytes));
                    LogMsg("[Patch] Fixed Bug #1 (DS_Sub_Output device swap inversion) at RVA 0x%llX!",
                           (unsigned long long)(hit - base));
                    appliedCount++;
                }
            } else {
                LogMsg("[Patch] Pattern matched at RVA 0x%llX, but string was not DS_Sub_Output.",
                       (unsigned long long)(hit - base));
            }
        } else {
            LogMsg("[Patch] DS_Sub_Output pattern not found (already patched or non-DS executable).");
        }
    }

    // 3. Locate PadAudioPlayback singleton & hot-reopen helpers (in case PadInputInit ran before XAudio2Create!)
    static const int patGetOrCreate[] = {
        0x48, 0x83, 0xEC, 0x38,
        0x48, 0x8B, 0x05, -1, -1, -1, -1,
        0x48, 0x85, 0xC0,
        0x0F, 0x85, -1, -1, -1, -1,
        0x48, 0x89, 0x74, 0x24, 0x48,
        0xB9, 0x00, 0x01, 0x00, 0x00
    };
    uint8_t* fnGetOrCreate = FindPattern(textStart, textSize, patGetOrCreate, sizeof(patGetOrCreate) / sizeof(patGetOrCreate[0]));
    if (fnGetOrCreate) {
        int32_t singletonDisp = *reinterpret_cast<const int32_t*>(fnGetOrCreate + 7);
        uint8_t* singletonAddr = fnGetOrCreate + 11 + singletonDisp;
        if (singletonAddr >= base && singletonAddr + 8 <= base + imageSize) {
            g_ppPadAudioPlayback = reinterpret_cast<void**>(singletonAddr);
            LogMsg("[Scan] Found g_pPadAudioPlayback singleton at RVA 0x%llX.",
                   (unsigned long long)(singletonAddr - base));
        }

        // Inside PadAudio_GetOrCreate, offset +0x196 is `call PadAudio_OpenAudioClient` (48 8B CF E8 <rel32>)
        for (size_t off = 0x150; off <= 0x1B0; ++off) {
            if (fnGetOrCreate[off] == 0x48 && fnGetOrCreate[off + 1] == 0x8B &&
                fnGetOrCreate[off + 2] == 0xCF && fnGetOrCreate[off + 3] == 0xE8)
            {
                int32_t rel = *reinterpret_cast<const int32_t*>(fnGetOrCreate + off + 4);
                uint8_t* openFn = fnGetOrCreate + off + 8 + rel;
                if (openFn >= textStart && openFn < textStart + textSize) {
                    g_pfnPadOpenClient = reinterpret_cast<PFN_PadOpenAudioClient>(openFn);
                    LogMsg("[Scan] Found PadAudioPlayback::OpenAudioClient at RVA 0x%llX.",
                           (unsigned long long)(openFn - base));
                }
                break;
            }
        }
    }

    static const int patCloseStream[] = {
        0x80, 0xB9, 0xF0, 0x00, 0x00, 0x00, 0x00,
        0xC6, 0x81, 0xF2, 0x00, 0x00, 0x00, 0x00,
        0x74, 0x1C,
        0xC6, 0x81, 0xD8, 0x00, 0x00, 0x00, 0x00,
        0xC6, 0x81, 0xF1, 0x00, 0x00, 0x00, 0x01
    };
    uint8_t* fnClose = FindPattern(textStart, textSize, patCloseStream, sizeof(patCloseStream) / sizeof(patCloseStream[0]));
    if (fnClose) {
        g_pfnPadCloseStream = reinterpret_cast<PFN_PadCloseStream>(fnClose);
        LogMsg("[Scan] Found PadAudioPlayback::CloseStream at RVA 0x%llX.",
               (unsigned long long)(fnClose - base));
    }

    return appliedCount;
}

static void CheckAndMigrateExistingPadAudio() {
    if (!g_cfg.enableSharedModeHaptics || !g_ppPadAudioPlayback) return;
    auto* pad = reinterpret_cast<uint8_t*>(*g_ppPadAudioPlayback);
    if (!pad) {
        LogMsg("[Migrate] g_pPadAudioPlayback is currently NULL (will be caught via CoCreateInstance IAT hook).");
        return;
    }

    auto** ppEnum = reinterpret_cast<IMMDeviceEnumerator_Raw**>(pad + 0x20);
    if (*ppEnum) {
        void* marker = nullptr;
        if (FAILED((*ppEnum)->QueryInterface(IID_IBBAudioFixProxyMarker, &marker))) {
            *ppEnum = new ProxyMMDeviceEnumerator(*ppEnum);
            LogMsg("[Migrate] Wrapped existing PadAudioPlayback->m_pEnumerator in ProxyMMDeviceEnumerator.");

            uint8_t bInitialized = *(volatile uint8_t*)(pad + 0xF0);
            if (bInitialized != 0 && g_pfnPadCloseStream && g_pfnPadOpenClient) {
                LogMsg("[Migrate] PadAudioPlayback already held Exclusive lock before XAudio2 loaded! Hot-reopening in Shared Mode...");
                g_pfnPadCloseStream(pad);
                g_pfnPadOpenClient(pad);
                LogMsg("[Migrate] Hot-reopen complete! New m_bInitialized=%u", (unsigned)(*(volatile uint8_t*)(pad + 0xF0)));
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Real System32\XAudio2_9.dll Forwarding
// ----------------------------------------------------------------------------
typedef HRESULT(__stdcall* PFN_XAudio2Create)(void** ppXAudio2, UINT32 Flags, UINT32 XAudio2Processor);
typedef HRESULT(__stdcall* PFN_CreateAudioReverb)(void** ppApo);
typedef HRESULT(__stdcall* PFN_CreateAudioVolumeMeter)(void** ppApo);
typedef HRESULT(__cdecl*   PFN_CreateFX)(REFCLSID clsid, void** pEffect, const void* pInitDat, UINT32 InitDataByteSize);
typedef void   (__cdecl*   PFN_X3DAudioCalculate)(const void* pInstance, const void* pListener, const void* pEmitter, UINT32 Flags, void* pDSPSettings);
typedef HRESULT(__cdecl*   PFN_X3DAudioInitialize)(UINT32 SpeakerChannelMask, float SpeedOfSound, void* Instance);
typedef HRESULT(__stdcall* PFN_CreateAudioReverbV2_8)(void** ppApo);
typedef HRESULT(__stdcall* PFN_XAudio2CreateV2_9)(void** ppXAudio2, UINT32 Flags, UINT32 XAudio2Processor);
typedef HRESULT(__stdcall* PFN_XAudio2CreateWithVersionInfo)(void** ppXAudio2, UINT32 Flags, UINT32 XAudio2Processor, DWORD ntddiVersion);
typedef HRESULT(__stdcall* PFN_XAudio2CreateWithSharedContexts)(void** ppXAudio2, UINT32 Flags, UINT32 XAudio2Processor, void* pSharedContexts);

static INIT_ONCE                          g_xaudioInitOnce = INIT_ONCE_STATIC_INIT;
static HMODULE                            g_hRealXAudio2   = nullptr;
static PFN_XAudio2Create                  g_fnXAudio2Create                  = nullptr;
static PFN_CreateAudioReverb              g_fnCreateAudioReverb              = nullptr;
static PFN_CreateAudioVolumeMeter         g_fnCreateAudioVolumeMeter         = nullptr;
static PFN_CreateFX                       g_fnCreateFX                       = nullptr;
static PFN_X3DAudioCalculate              g_fnX3DAudioCalculate              = nullptr;
static PFN_X3DAudioInitialize             g_fnX3DAudioInitialize             = nullptr;
static PFN_CreateAudioReverbV2_8          g_fnCreateAudioReverbV2_8          = nullptr;
static PFN_XAudio2CreateV2_9              g_fnXAudio2CreateV2_9              = nullptr;
static PFN_XAudio2CreateWithVersionInfo   g_fnXAudio2CreateWithVersionInfo   = nullptr;
static PFN_XAudio2CreateWithSharedContexts g_fnXAudio2CreateWithSharedContexts = nullptr;

static BOOL CALLBACK LoadRealXAudio2Callback(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t sysDir[MAX_PATH] = {0};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    wchar_t fullPath[MAX_PATH] = {0};
    std::swprintf(fullPath, MAX_PATH, L"%ls\\XAudio2_9.dll", sysDir);

    g_hRealXAudio2 = LoadLibraryW(fullPath);
    if (!g_hRealXAudio2) {
        LogMsg("[Error] Failed to load system XAudio2_9.dll from %ls (err=%lu)", fullPath, GetLastError());
        return FALSE;
    }

    g_fnXAudio2Create                  = reinterpret_cast<PFN_XAudio2Create>(GetProcAddress(g_hRealXAudio2, "XAudio2Create"));
    g_fnCreateAudioReverb              = reinterpret_cast<PFN_CreateAudioReverb>(GetProcAddress(g_hRealXAudio2, "CreateAudioReverb"));
    g_fnCreateAudioVolumeMeter         = reinterpret_cast<PFN_CreateAudioVolumeMeter>(GetProcAddress(g_hRealXAudio2, "CreateAudioVolumeMeter"));
    g_fnCreateFX                       = reinterpret_cast<PFN_CreateFX>(GetProcAddress(g_hRealXAudio2, "CreateFX"));
    g_fnX3DAudioCalculate              = reinterpret_cast<PFN_X3DAudioCalculate>(GetProcAddress(g_hRealXAudio2, "X3DAudioCalculate"));
    g_fnX3DAudioInitialize             = reinterpret_cast<PFN_X3DAudioInitialize>(GetProcAddress(g_hRealXAudio2, "X3DAudioInitialize"));
    g_fnCreateAudioReverbV2_8          = reinterpret_cast<PFN_CreateAudioReverbV2_8>(GetProcAddress(g_hRealXAudio2, "CreateAudioReverbV2_8"));
    g_fnXAudio2CreateV2_9              = reinterpret_cast<PFN_XAudio2CreateV2_9>(GetProcAddress(g_hRealXAudio2, "XAudio2CreateV2_9"));
    g_fnXAudio2CreateWithVersionInfo   = reinterpret_cast<PFN_XAudio2CreateWithVersionInfo>(GetProcAddress(g_hRealXAudio2, "XAudio2CreateWithVersionInfo"));
    g_fnXAudio2CreateWithSharedContexts = reinterpret_cast<PFN_XAudio2CreateWithSharedContexts>(GetProcAddress(g_hRealXAudio2, "XAudio2CreateWithSharedContexts"));

    LogMsg("[Init] Loaded system XAudio2_9.dll (%ls) successfully.", fullPath);
    CheckAndMigrateExistingPadAudio();
    return TRUE;
}

static inline bool EnsureRealXAudio2() {
    return InitOnceExecuteOnce(&g_xaudioInitOnce, LoadRealXAudio2Callback, nullptr, nullptr) != 0;
}

extern "C" {

HRESULT __stdcall Proxy_XAudio2Create(void** ppXAudio2, UINT32 Flags, UINT32 XAudio2Processor) {
    if (!EnsureRealXAudio2() || !g_fnXAudio2Create) return E_FAIL;
    return g_fnXAudio2Create(ppXAudio2, Flags, XAudio2Processor);
}

HRESULT __stdcall Proxy_CreateAudioReverb(void** ppApo) {
    if (!EnsureRealXAudio2() || !g_fnCreateAudioReverb) return E_FAIL;
    return g_fnCreateAudioReverb(ppApo);
}

HRESULT __stdcall Proxy_CreateAudioVolumeMeter(void** ppApo) {
    if (!EnsureRealXAudio2() || !g_fnCreateAudioVolumeMeter) return E_FAIL;
    return g_fnCreateAudioVolumeMeter(ppApo);
}

HRESULT __cdecl Proxy_CreateFX(REFCLSID clsid, void** pEffect, const void* pInitDat, UINT32 InitDataByteSize) {
    if (!EnsureRealXAudio2() || !g_fnCreateFX) return E_FAIL;
    return g_fnCreateFX(clsid, pEffect, pInitDat, InitDataByteSize);
}

void __cdecl Proxy_X3DAudioCalculate(const void* pInstance, const void* pListener, const void* pEmitter, UINT32 Flags, void* pDSPSettings) {
    if (EnsureRealXAudio2() && g_fnX3DAudioCalculate) {
        g_fnX3DAudioCalculate(pInstance, pListener, pEmitter, Flags, pDSPSettings);
    }
}

HRESULT __cdecl Proxy_X3DAudioInitialize(UINT32 SpeakerChannelMask, float SpeedOfSound, void* Instance) {
    if (!EnsureRealXAudio2() || !g_fnX3DAudioInitialize) return E_FAIL;
    return g_fnX3DAudioInitialize(SpeakerChannelMask, SpeedOfSound, Instance);
}

HRESULT __stdcall Proxy_CreateAudioReverbV2_8(void** ppApo) {
    if (!EnsureRealXAudio2() || !g_fnCreateAudioReverbV2_8) return E_FAIL;
    return g_fnCreateAudioReverbV2_8(ppApo);
}

HRESULT __stdcall Proxy_XAudio2CreateV2_9(void** ppXAudio2, UINT32 Flags, UINT32 XAudio2Processor) {
    if (!EnsureRealXAudio2() || !g_fnXAudio2CreateV2_9) return E_FAIL;
    return g_fnXAudio2CreateV2_9(ppXAudio2, Flags, XAudio2Processor);
}

HRESULT __stdcall Proxy_XAudio2CreateWithVersionInfo(void** ppXAudio2, UINT32 Flags, UINT32 XAudio2Processor, DWORD ntddiVersion) {
    if (!EnsureRealXAudio2() || !g_fnXAudio2CreateWithVersionInfo) return E_FAIL;
    return g_fnXAudio2CreateWithVersionInfo(ppXAudio2, Flags, XAudio2Processor, ntddiVersion);
}

HRESULT __stdcall Proxy_XAudio2CreateWithSharedContexts(void** ppXAudio2, UINT32 Flags, UINT32 XAudio2Processor, void* pSharedContexts) {
    if (!EnsureRealXAudio2() || !g_fnXAudio2CreateWithSharedContexts) return E_FAIL;
    return g_fnXAudio2CreateWithSharedContexts(ppXAudio2, Flags, XAudio2Processor, pSharedContexts);
}

} // extern "C"

// ----------------------------------------------------------------------------
// DllMain
// ----------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        InitializeCriticalSection(&g_logCs);
        g_logCsReady = true;
        LoadConfig(hModule);

        wchar_t exePath[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        LogMsg("=== DualSense BB Controller Audio Fix initialized ===");
        LogMsg("Host executable: %ls", exePath);

        HMODULE hMainExe = GetModuleHandleW(nullptr);
        BBAudioFix_ApplyPatchesToModule(hMainExe);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        LogMsg("=== DualSense BB Controller Audio Fix unloading ===");
    }
    return TRUE;
}
