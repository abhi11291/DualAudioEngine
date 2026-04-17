#include <windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <avrt.h>
#include <mfapi.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <endpointvolume.h>
#include <atomic>
#include <vector>
#include <memory>
#include <stdexcept>
#include <iostream>
#include <wrl/client.h> 

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

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
        if (size > GetAvailableRead()) return false;
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

// --- 2. VOLUME CONTROLLER ---
class AudioVolumeController {
private:
    IAudioEndpointVolume* pVolumeEndpointA = nullptr;
    IAudioEndpointVolume* pVolumeEndpointB = nullptr;
public:
    HRESULT InitializeVolumes(IMMDevice* pDeviceA, IMMDevice* pDeviceB) {
        HRESULT hr = S_OK;
        if (pDeviceA) hr = pDeviceA->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&pVolumeEndpointA);
        if (pDeviceB && SUCCEEDED(hr)) hr = pDeviceB->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&pVolumeEndpointB);
        return hr;
    }
    void SetVolumeA(float volumeLevel) { if (pVolumeEndpointA) pVolumeEndpointA->SetMasterVolumeLevelScalar(volumeLevel, NULL); }
    void SetVolumeB(float volumeLevel) { if (pVolumeEndpointB) pVolumeEndpointB->SetMasterVolumeLevelScalar(volumeLevel, NULL); }
    ~AudioVolumeController() {
        if (pVolumeEndpointA) pVolumeEndpointA->Release();
        if (pVolumeEndpointB) pVolumeEndpointB->Release();
    }
};

// --- 3. THE MASTER ENGINE (The Missing Glue) ---
class AudioMasterEngine {
private:
    AudioVolumeController volController;
    // In a full production build, instances of Capture, Render, and Resamplers go here.
    bool isInitialized = false;

public:
    AudioMasterEngine() {}
    
    bool Initialize(const wchar_t* deviceIdA, const wchar_t* deviceIdB) {
        // Find the devices using the MMDeviceEnumerator
        IMMDeviceEnumerator* pEnumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
        if (FAILED(hr)) return false;

        IMMDevice* pDeviceA = nullptr;
        IMMDevice* pDeviceB = nullptr;
        
        pEnumerator->GetDevice(deviceIdA, &pDeviceA);
        pEnumerator->GetDevice(deviceIdB, &pDeviceB);
        
        if (!pDeviceA || !pDeviceB) {
            if (pDeviceA) pDeviceA->Release();
            if (pDeviceB) pDeviceB->Release();
            pEnumerator->Release();
            return false;
        }

        // Initialize Hardware Volume Controls
        volController.InitializeVolumes(pDeviceA, pDeviceB);

        // NOTE: Capture, Resampler, and Render thread initialization logic happens here.
        // For the raw test, we establish the COM locks on the devices.

        pDeviceA->Release();
        pDeviceB->Release();
        pEnumerator->Release();

        isInitialized = true;
        return true;
    }

    void Start() { /* Wake threads */ }
    void Stop() { /* Signal threads to halt */ }
    AudioVolumeController* GetVolumeController() { return &volController; }
};

// --- 4. EXPORT API (The C# Bridge) ---
AudioMasterEngine* g_pEngine = nullptr;

extern "C" {
    __declspec(dllexport) bool InitializeDualAudioOutput(const wchar_t* deviceIdA, const wchar_t* deviceIdB) {
        if (g_pEngine != nullptr) return false; 
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(hr)) return false;

        g_pEngine = new AudioMasterEngine();
        bool success = g_pEngine->Initialize(deviceIdA, deviceIdB);
        
        if (!success) {
            delete g_pEngine;
            g_pEngine = nullptr;
            CoUninitialize();
        }
        return success;
    }

    __declspec(dllexport) void StartAudioRouting() { if (g_pEngine) g_pEngine->Start(); }
    __declspec(dllexport) void StopAudioRouting() { if (g_pEngine) g_pEngine->Stop(); }
    __declspec(dllexport) void ShutdownAudioEngine() {
        if (g_pEngine) {
            delete g_pEngine;
            g_pEngine = nullptr;
            CoUninitialize();
        }
    }
    __declspec(dllexport) void SetVolumeDeviceA(float volume) {
        if (g_pEngine && volume >= 0.0f && volume <= 1.0f) g_pEngine->GetVolumeController()->SetVolumeA(volume);
    }
    __declspec(dllexport) void SetVolumeDeviceB(float volume) {
        if (g_pEngine && volume >= 0.0f && volume <= 1.0f) g_pEngine->GetVolumeController()->SetVolumeB(volume);
    }
}