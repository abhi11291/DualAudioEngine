#include <windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <avrt.h>
#include <endpointvolume.h>
#include <atomic>
#include <vector>
#include <stdexcept>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")

// --- 1. RING BUFFER ---
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

    bool Push(const BYTE* data, size_t size) {
        if (size > GetAvailableWrite()) return false;
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
        return true;
    }

    bool Pop(BYTE* dest, size_t size) {
        if (size > GetAvailableRead()) {
            std::memset(dest, 0, size); // Output silence on underrun
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
        HANDLE hTask = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);
        
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

        // Force Auto-Convert to handle Bluetooth mismatches natively
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
    AudioRingBuffer* pBufferA = nullptr;
    AudioRingBuffer* pBufferB = nullptr;
    WAVEFORMATEX* pMixFormat = nullptr;

    static DWORD WINAPI ThreadProc(LPVOID lpParam) {
        auto* engine = static_cast<AudioCaptureEngine*>(lpParam);
        DWORD taskIndex = 0;
        HANDLE hTask = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);
        
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
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                        engine->pBufferA->Push(pData, bytesToWrite);
                        engine->pBufferB->Push(pData, bytesToWrite);
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
    HRESULT Initialize(AudioRingBuffer* bufA, AudioRingBuffer* bufB, WAVEFORMATEX** outFormat) {
        pBufferA = bufA; pBufferB = bufB;
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
    AudioRenderEngine renderA;
    AudioRenderEngine renderB;
    AudioRingBuffer* bufferA;
    AudioRingBuffer* bufferB;

public:
    AudioMasterEngine() {
        bufferA = new AudioRingBuffer(65536); // 64KB power-of-two buffer
        bufferB = new AudioRingBuffer(65536);
    }
    ~AudioMasterEngine() { delete bufferA; delete bufferB; }
    
    bool Initialize(const wchar_t* deviceIdA, const wchar_t* deviceIdB) {
        IMMDeviceEnumerator* pEnum = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pEnum)))) return false;

        IMMDevice* pDeviceA = nullptr;
        IMMDevice* pDeviceB = nullptr;
        pEnum->GetDevice(deviceIdA, &pDeviceA);
        pEnum->GetDevice(deviceIdB, &pDeviceB);
        
        if (!pDeviceA || !pDeviceB) return false;

        WAVEFORMATEX* captureFormat = nullptr;
        capture.Initialize(bufferA, bufferB, &captureFormat);
        
        renderA.Initialize(pDeviceA, bufferA, captureFormat);
        renderB.Initialize(pDeviceB, bufferB, captureFormat);

        pDeviceA->Release(); pDeviceB->Release(); pEnum->Release();
        return true;
    }

    void Start() { renderA.Start(); renderB.Start(); capture.Start(); }
    void Stop() { capture.Stop(); renderA.Stop(); renderB.Stop(); }
};

// --- 5. EXPORT API ---
AudioMasterEngine* g_pEngine = nullptr;

extern "C" {
    __declspec(dllexport) bool InitializeDualAudioOutput(const wchar_t* deviceIdA, const wchar_t* deviceIdB) {
        if (g_pEngine != nullptr) return false; 
        if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return false;

        g_pEngine = new AudioMasterEngine();
        bool success = g_pEngine->Initialize(deviceIdA, deviceIdB);
        if (!success) { delete g_pEngine; g_pEngine = nullptr; CoUninitialize(); }
        return success;
    }
    __declspec(dllexport) void StartAudioRouting() { if (g_pEngine) g_pEngine->Start(); }
    __declspec(dllexport) void StopAudioRouting() { if (g_pEngine) g_pEngine->Stop(); }
    __declspec(dllexport) void ShutdownAudioEngine() {
        if (g_pEngine) { delete g_pEngine; g_pEngine = nullptr; CoUninitialize(); }
    }
}
