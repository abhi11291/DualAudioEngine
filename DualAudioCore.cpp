#include <windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <avrt.h>
#include <atomic>
#include <vector>
#include <stdexcept>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")

// --- 1. STRICT REAL-TIME OVERWRITE BUFFER ---
class AudioRingBuffer {
private:
    std::vector<BYTE> buffer;
    const size_t capacity;
    const size_t mask;
    alignas(64) std::atomic<size_t> writeIndex{ 0 };
    alignas(64) std::atomic<size_t> readIndex{ 0 };

    static bool IsPowerOfTwo(size_t x) { return (x != 0) && ((x & (x - 1)) == 0); }

public:
    AudioRingBuffer(size_t size) : capacity(size), mask(size - 1) {
        if (!IsPowerOfTwo(size)) throw std::invalid_argument("Capacity must be power of 2");
        buffer.resize(capacity, 0);
    }
    
    size_t GetAvailableRead() const { return writeIndex.load(std::memory_order_acquire) - readIndex.load(std::memory_order_acquire); }
    size_t GetAvailableWrite() const { return capacity - GetAvailableRead(); }

    void Push(const BYTE* data, size_t size) {
        // OVERWRITE LOGIC: If the buffer is too full (Target Headset is lagging),
        // we forcefully fast-forward the read pointer to drop the oldest audio.
        // This guarantees we NEVER suffer from "Ghost Playback" or unbounded latency.
        if (size > GetAvailableWrite()) {
            size_t currentRead = readIndex.load(std::memory_order_relaxed);
            readIndex.store(currentRead + size, std::memory_order_release);
        }

        size_t currentWrite = writeIndex.load(std::memory_order_relaxed);
        size_t offset = currentWrite & mask;
        if (offset + size > capacity) {
            size_t firstPart = capacity - offset;
            std::memcpy(buffer.data() + offset, data, firstPart);
            std::memcpy(buffer.data(), data + firstPart, size - firstPart);
        } else {
            std::memcpy(buffer.data() + offset, data, size);
        }
        writeIndex.store(currentWrite + size, std::memory_order_release);
    }

    bool Pop(BYTE* dest, size_t size) {
        if (size > GetAvailableRead()) {
            std::memset(dest, 0, size);
            return false;
        }
        size_t currentRead = readIndex.load(std::memory_order_relaxed);
        size_t offset = currentRead & mask;
        if (offset + size > capacity) {
            size_t firstPart = capacity - offset;
            std::memcpy(dest, buffer.data() + offset, firstPart);
            std::memcpy(dest + firstPart, buffer.data(), size - firstPart);
        } else {
            std::memcpy(dest, buffer.data() + offset, size);
        }
        readIndex.store(currentRead + size, std::memory_order_release);
        return true;
    }
};

// --- 2. RENDER ENGINE ---
class AudioRenderEngine {
private:
    IAudioClient* pAudioClient = nullptr;
    IAudioRenderClient* pRenderClient = nullptr;
    HANDLE hEvent = nullptr;
    HANDLE hThread = nullptr;
    bool isPlaying = false;
    AudioRingBuffer* pRingBuffer = nullptr;
    WAVEFORMATEX* pMixFormat = nullptr;
    UINT32 bufferFrameCount = 0;

    static DWORD WINAPI ThreadProc(LPVOID lpParam) {
        auto* engine = static_cast<AudioRenderEngine*>(lpParam);
        DWORD taskIndex = 0;
        HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        
        engine->pAudioClient->Start();
        while (engine->isPlaying) {
            if (WaitForSingleObject(engine->hEvent, 1000) != WAIT_OBJECT_0) continue;

            UINT32 numFramesPadding = 0;
            engine->pAudioClient->GetCurrentPadding(&numFramesPadding);
            UINT32 numFramesAvailable = engine->bufferFrameCount - numFramesPadding;
            if (numFramesAvailable == 0) continue;

            BYTE* pData = nullptr;
            if (SUCCEEDED(engine->pRenderClient->GetBuffer(numFramesAvailable, &pData))) {
                size_t bytesToRead = numFramesAvailable * engine->pMixFormat->nBlockAlign;
                DWORD flags = engine->pRingBuffer->Pop(pData, bytesToRead) ? 0 : AUDCLNT_BUFFERFLAGS_SILENT;
                engine->pRenderClient->ReleaseBuffer(numFramesAvailable, flags);
            }
        }
        engine->pAudioClient->Stop();
        if (hTask) AvRevertMmThreadCharacteristics(hTask);
        return 0;
    }

public:
    HRESULT Initialize(IMMDevice* pDevice, AudioRingBuffer* ringBuffer, WAVEFORMATEX* captureFormat) {
        pRingBuffer = ringBuffer;
        HRESULT hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient);
        if (FAILED(hr)) return hr;

        hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 
            0, 0, captureFormat, NULL);
        if (FAILED(hr)) return hr;

        pAudioClient->GetBufferSize(&bufferFrameCount);
        pMixFormat = captureFormat;
        hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        pAudioClient->SetEventHandle(hEvent);
        return pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRenderClient);
    }
    void Start() { isPlaying = true; hThread = CreateThread(NULL, 0, ThreadProc, this, 0, NULL); }
    void Stop() { isPlaying = false; SetEvent(hEvent); WaitForSingleObject(hThread, INFINITE); }
};

// --- 3. CAPTURE ENGINE ---
class AudioCaptureEngine {
private:
    IAudioClient* pAudioClient = nullptr;
    IAudioCaptureClient* pCaptureClient = nullptr;
    HANDLE hEvent = nullptr;
    HANDLE hThread = nullptr;
    bool isCapturing = false;
    AudioRingBuffer* pBuffer = nullptr;
    WAVEFORMATEX* pMixFormat = nullptr;

    static DWORD WINAPI ThreadProc(LPVOID lpParam) {
        auto* engine = static_cast<AudioCaptureEngine*>(lpParam);
        DWORD taskIndex = 0;
        HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        
        engine->pAudioClient->Start();
        while (engine->isCapturing) {
            if (WaitForSingleObject(engine->hEvent, 1000) != WAIT_OBJECT_0) continue;

            UINT32 packetLength = 0;
            engine->pCaptureClient->GetNextPacketSize(&packetLength);
            while (packetLength != 0) {
                BYTE* pData;
                UINT32 numFramesAvailable;
                DWORD flags;
                if (SUCCEEDED(engine->pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL))) {
                    size_t bytesToWrite = numFramesAvailable * engine->pMixFormat->nBlockAlign;
                    
                    // FLUSH LOGIC: If Windows says the media is paused, push instant zeroes to flush Headset B
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        std::vector<BYTE> silence(bytesToWrite, 0);
                        engine->pBuffer->Push(silence.data(), bytesToWrite);
                    } else {
                        engine->pBuffer->Push(pData, bytesToWrite);
                    }
                    engine->pCaptureClient->ReleaseBuffer(numFramesAvailable);
                }
                engine->pCaptureClient->GetNextPacketSize(&packetLength);
            }
        }
        engine->pAudioClient->Stop();
        if (hTask) AvRevertMmThreadCharacteristics(hTask);
        return 0;
    }

public:
    HRESULT Initialize(AudioRingBuffer* buf, WAVEFORMATEX** outFormat) {
        pBuffer = buf;
        IMMDeviceEnumerator* pEnum = nullptr;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&pEnum));
        IMMDevice* pDefaultDevice = nullptr;
        pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDefaultDevice);
        
        pDefaultDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient);
        pAudioClient->GetMixFormat(&pMixFormat);
        *outFormat = pMixFormat;

        pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, pMixFormat, NULL);
        hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        pAudioClient->SetEventHandle(hEvent);
        pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
        
        pDefaultDevice->Release();
        pEnum->Release();
        return S_OK;
    }
    void Start() { isCapturing = true; hThread = CreateThread(NULL, 0, ThreadProc, this, 0, NULL); }
    void Stop() { isCapturing = false; SetEvent(hEvent); WaitForSingleObject(hThread, INFINITE); }
};

// --- 4. THE MASTER ENGINE ---
class AudioMasterEngine {
private:
    AudioCaptureEngine capture;
    AudioRenderEngine renderTarget;
    AudioRingBuffer* buffer;

public:
    AudioMasterEngine() {
        // TIGHT BUFFER: 256 KB. Max capacity is ~680ms. Prevents massive audio backups.
        buffer = new AudioRingBuffer(262144); 
    }
    ~AudioMasterEngine() { delete buffer; }
    
    bool Initialize(const wchar_t* targetDeviceId, int delayMs) {
        IMMDeviceEnumerator* pEnum = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pEnum)))) return false;

        IMMDevice* pTargetDevice = nullptr;
        pEnum->GetDevice(targetDeviceId, &pTargetDevice);
        if (!pTargetDevice) return false;

        WAVEFORMATEX* captureFormat = nullptr;
        capture.Initialize(buffer, &captureFormat); 
        
        // Exact latency injection
        if (delayMs > 0 && captureFormat != nullptr) {
            size_t bytesPerSec = captureFormat->nSamplesPerSec * captureFormat->nBlockAlign;
            size_t delayBytes = (bytesPerSec * delayMs) / 1000;
            delayBytes -= (delayBytes % captureFormat->nBlockAlign);
            
            if (delayBytes <= buffer->GetAvailableWrite()) {
                std::vector<BYTE> silence(delayBytes, 0);
                buffer->Push(silence.data(), delayBytes);
            }
        }
        
        renderTarget.Initialize(pTargetDevice, buffer, captureFormat);

        pTargetDevice->Release(); pEnum->Release();
        return true;
    }

    void Start() { renderTarget.Start(); capture.Start(); }
    void Stop() { capture.Stop(); renderTarget.Stop(); }
};

// --- 5. EXPORT API ---
AudioMasterEngine* g_pEngine = nullptr;

extern "C" {
    __declspec(dllexport) bool InitializeDualAudioOutput(const wchar_t* targetDeviceId, int delayMs) {
        if (g_pEngine != nullptr) return false; 
        if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return false;

        g_pEngine = new AudioMasterEngine();
        bool success = g_pEngine->Initialize(targetDeviceId, delayMs);
        if (!success) { delete g_pEngine; g_pEngine = nullptr; CoUninitialize(); }
        return success;
    }
    __declspec(dllexport) void StartAudioRouting() { if (g_pEngine) g_pEngine->Start(); }
    __declspec(dllexport) void StopAudioRouting() { if (g_pEngine) g_pEngine->Stop(); }
    __declspec(dllexport) void ShutdownAudioEngine() {
        if (g_pEngine) { delete g_pEngine; g_pEngine = nullptr; CoUninitialize(); }
    }
}
