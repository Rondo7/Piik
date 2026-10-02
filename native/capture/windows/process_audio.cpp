#define NOMINMAX
#include <windows.h>

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <fcntl.h>
#include <io.h>
#include <mmdeviceapi.h>
#include <wrl.h>
#include <wrl/implements.h>

#include "capture_target.h"
#include "process_audio.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

namespace piik::capture {
namespace {
class ActivationHandler final : public RuntimeClass<
    RuntimeClassFlags<ClassicCom>, FtmBase, IActivateAudioInterfaceCompletionHandler> {
 public:
  explicit ActivationHandler(HANDLE completed) : completed_(completed) {}

  STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
    ComPtr<IUnknown> activated;
    HRESULT activationResult = E_UNEXPECTED;
    result_ = operation->GetActivateResult(&activationResult, &activated);
    if (SUCCEEDED(result_)) result_ = activationResult;
    if (SUCCEEDED(result_)) result_ = activated.As(&client_);
    SetEvent(completed_);
    return S_OK;
  }

  HRESULT Result() const { return result_; }
  ComPtr<IAudioClient> Client() const { return client_; }

 private:
  HANDLE completed_ = nullptr;
  HRESULT result_ = E_UNEXPECTED;
  ComPtr<IAudioClient> client_;
};

HRESULT ActivateProcessLoopback(DWORD pid, HANDLE completed,
                                ComPtr<IAudioClient>* client,
                                PROCESS_LOOPBACK_MODE mode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE) {
  AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
  parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  parameters.ProcessLoopbackParams.TargetProcessId = pid;
  parameters.ProcessLoopbackParams.ProcessLoopbackMode = mode;
  PROPVARIANT variant{};
  variant.vt = VT_BLOB;
  variant.blob.cbSize = sizeof(parameters);
  variant.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);

  auto handler = Make<ActivationHandler>(completed);
  if (!handler) return E_OUTOFMEMORY;
  ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
  HRESULT result = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                                __uuidof(IAudioClient), &variant,
                                                handler.Get(), &operation);
  if (FAILED(result)) return result;
  if (WaitForSingleObject(completed, 10'000) != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
  result = handler->Result();
  if (SUCCEEDED(result)) *client = handler->Client();
  return result;
}

HRESULT ActivateEndpoint(ComPtr<IAudioClient>* client, bool microphone = false, const std::wstring& device_id = L"") {
  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                    CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  ComPtr<IMMDevice> device;
  if (SUCCEEDED(result)) {
    result = device_id.empty()
        ? enumerator->GetDefaultAudioEndpoint(microphone ? eCapture : eRender, microphone ? eCommunications : eConsole, &device)
        : enumerator->GetDevice(device_id.c_str(), &device);
    if (SUCCEEDED(result) && microphone) {
      ComPtr<IMMEndpoint> endpoint;
      EDataFlow flow = eAll;
      result = device.As(&endpoint);
      if (SUCCEEDED(result)) result = endpoint->GetDataFlow(&flow);
      if (SUCCEEDED(result) && flow != eCapture) result = E_INVALIDARG;
    }
  }
  if (SUCCEEDED(result)) {
    result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(client->GetAddressOf()));
  }
  return result;
}

HRESULT CaptureAudioFrames(ComPtr<IAudioClient> client, HANDLE process, bool loopback,
                             HANDLE stop_event, const StopProbe& stop_probe,
                             const ReadyWriter& ready_writer,
                             const PCMWriter& writer) {
  if (stop_event == nullptr || !stop_probe || !ready_writer || !writer) {
    return HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
  }
  HANDLE sampleReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (sampleReady == nullptr) {
    const HRESULT result = HRESULT_FROM_WIN32(GetLastError());
    return result;
  }
  HRESULT result = S_OK;
  ComPtr<IAudioCaptureClient> capture;
  WAVEFORMATEX format{};
  format.wFormatTag = WAVE_FORMAT_PCM;
  format.nChannels = kAudioChannels;
  format.nSamplesPerSec = kAudioSampleRate;
  format.wBitsPerSample = kAudioBytesPerSample * 8;
  format.nBlockAlign = kAudioChannels * kAudioBytesPerSample;
  format.nAvgBytesPerSec = kAudioSampleRate * format.nBlockAlign;
  if (SUCCEEDED(result)) {
    result = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0) | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        0, 0, &format, nullptr);
  }
  if (SUCCEEDED(result)) result = client->GetService(IID_PPV_ARGS(&capture));
  if (SUCCEEDED(result)) result = client->SetEventHandle(sampleReady);
  if (SUCCEEDED(result)) result = client->Start();
  if (SUCCEEDED(result)) result = ready_writer();
  if (SUCCEEDED(result)) {
    std::array<BYTE, kAudioBytesPerChunk> silence{};
    result = writer(0, silence.data(), static_cast<DWORD>(silence.size()));
  }

  std::vector<BYTE> pending;
  size_t consumed = 0;
  UINT64 nextTimestamp = 0;
  const HANDLE process_waits[] = {process, sampleReady, stop_event};
  const HANDLE system_waits[] = {sampleReady, stop_event};
  while (SUCCEEDED(result)) {
    if (stop_probe()) {
      result = S_OK;
      break;
    }
    const DWORD wait = process != nullptr
                           ? WaitForMultipleObjects(3, process_waits, FALSE,
                                                    1'000)
                           : WaitForMultipleObjects(2, system_waits, FALSE,
                                                    1'000);
    if (process != nullptr && wait == WAIT_OBJECT_0) {
      result = HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
      break;
    }
    if (wait == WAIT_TIMEOUT) continue;
    const DWORD sample_index = process != nullptr ? WAIT_OBJECT_0 + 1
                                                  : WAIT_OBJECT_0;
    const DWORD stop_index = process != nullptr ? WAIT_OBJECT_0 + 2
                                                : WAIT_OBJECT_0 + 1;
    if (wait == stop_index) {
      result = S_OK;
      break;
    }
    if (wait != sample_index) {
      result = HRESULT_FROM_WIN32(GetLastError());
      break;
    }
    UINT32 frames = 0;
    while (SUCCEEDED(result = capture->GetNextPacketSize(&frames)) && frames > 0) {
      BYTE* data = nullptr;
      DWORD flags = 0;
      UINT64 devicePosition = 0;
      UINT64 qpcPosition = 0;
      result = capture->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcPosition);
      if (FAILED(result)) break;
      const size_t bytes = static_cast<size_t>(frames) * format.nBlockAlign;
      if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0) {
        capture->ReleaseBuffer(frames);
        continue;
      }
      if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
        pending.clear();
        consumed = 0;
      }
      if (pending.size() == consumed) {
        pending.clear();
        consumed = 0;
        nextTimestamp = qpcPosition;
      }
      if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
        pending.insert(pending.end(), bytes, 0);
      } else {
        pending.insert(pending.end(), data, data + bytes);
      }
      capture->ReleaseBuffer(frames);
      while (pending.size() - consumed >= kAudioBytesPerChunk) {
        result = writer(nextTimestamp, pending.data() + consumed,
                        kAudioBytesPerChunk);
        if (FAILED(result)) break;
        consumed += kAudioBytesPerChunk;
        nextTimestamp += kAudioChunkDuration100ns;
      }
      if (FAILED(result)) break;
      if (consumed > kAudioBytesPerChunk * 4) {
        pending.erase(pending.begin(), pending.begin() + static_cast<ptrdiff_t>(consumed));
        consumed = 0;
      }
    }
  }
  if (client) client->Stop();
  CloseHandle(sampleReady);
  return result;
}

}  // namespace

bool ProcessAudioAvailable() {
  HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (completed == nullptr) return false;
  ComPtr<IAudioClient> client;
  const HRESULT result =
      ActivateProcessLoopback(GetCurrentProcessId(), completed, &client);
  CloseHandle(completed);
  return SUCCEEDED(result) && client != nullptr;
}

bool SystemAudioAvailable() {
  ComPtr<IAudioClient> client;
  return SUCCEEDED(ActivateEndpoint(&client)) && client != nullptr;
}

HRESULT CaptureProcessAudio(DWORD pid, UINT64 expectedCreationTime,
                            HANDLE stop_event, const StopProbe& stop_probe,
                            const ReadyWriter& ready_writer,
                            const PCMWriter& writer) {
  HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com_result)) return com_result;
  HRESULT result = ValidateProcessTarget(pid, expectedCreationTime);
  HANDLE process = nullptr;
  HANDLE completed = nullptr;
  ComPtr<IAudioClient> client;
  if (SUCCEEDED(result)) {
    process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) result = HRESULT_FROM_WIN32(GetLastError());
  }
  if (SUCCEEDED(result)) {
    completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (completed == nullptr) result = HRESULT_FROM_WIN32(GetLastError());
  }
  if (SUCCEEDED(result)) {
    result = ActivateProcessLoopback(pid, completed, &client);
  }
  if (SUCCEEDED(result)) {
    result = CaptureAudioFrames(client, process, true, stop_event, stop_probe,
                                  ready_writer, writer);
  }
  if (completed != nullptr) CloseHandle(completed);
  if (process != nullptr) CloseHandle(process);
  client.Reset();
  CoUninitialize();
  return result;
}

HRESULT CaptureSystemAudio(HANDLE stop_event, const StopProbe& stop_probe,
                           const ReadyWriter& ready_writer,
                           const PCMWriter& writer) {
  HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com_result)) return com_result;
  ComPtr<IAudioClient> client;
  HRESULT result = ActivateEndpoint(&client);
  if (SUCCEEDED(result)) {
    result = CaptureAudioFrames(client, nullptr, true, stop_event, stop_probe,
                                  ready_writer, writer);
  }
  client.Reset();
  CoUninitialize();
  return result;
}

HRESULT CaptureSystemAudioWithExclusions(
    const std::vector<DWORD>& excluded_pids,
    HANDLE stop_event, const StopProbe& stop_probe,
    const ReadyWriter& ready_writer,
    const PCMWriter& writer) {
  if (excluded_pids.empty()) {
    return CaptureSystemAudio(stop_event, stop_probe, ready_writer, writer);
  }
  HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com_result)) return com_result;

  if (excluded_pids.size() == 1) {
    HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ComPtr<IAudioClient> client;
    HRESULT result = completed ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    if (SUCCEEDED(result)) {
      result = ActivateProcessLoopback(excluded_pids[0], completed, &client,
                                       PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE);
    }
    if (SUCCEEDED(result)) {
      result = CaptureAudioFrames(client, nullptr, true, stop_event, stop_probe,
                                  ready_writer, writer);
    }
    if (completed != nullptr) CloseHandle(completed);
    client.Reset();
    CoUninitialize();
    return result;
  }

  // Multiple exclusions: find active audio sessions not in excluded_pids
  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                    CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  ComPtr<IMMDevice> device;
  if (SUCCEEDED(result)) {
    result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  }
  ComPtr<IAudioSessionManager2> sessionManager;
  if (SUCCEEDED(result)) {
    result = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(sessionManager.GetAddressOf()));
  }
  ComPtr<IAudioSessionEnumerator> sessionEnumerator;
  if (SUCCEEDED(result)) {
    result = sessionManager->GetSessionEnumerator(&sessionEnumerator);
  }
  int sessionCount = 0;
  if (SUCCEEDED(result)) {
    result = sessionEnumerator->GetCount(&sessionCount);
  }

  std::vector<DWORD> included_pids;
  if (SUCCEEDED(result)) {
    for (int i = 0; i < sessionCount; ++i) {
      ComPtr<IAudioSessionControl> control;
      if (FAILED(sessionEnumerator->GetSession(i, &control))) continue;
      ComPtr<IAudioSessionControl2> control2;
      if (FAILED(control.As(&control2))) continue;
      DWORD pid = 0;
      if (FAILED(control2->GetProcessId(&pid)) || pid == 0 || pid == GetCurrentProcessId()) continue;
      if (std::find(excluded_pids.begin(), excluded_pids.end(), pid) != excluded_pids.end()) continue;
      if (std::find(included_pids.begin(), included_pids.end(), pid) == included_pids.end()) {
        included_pids.push_back(pid);
      }
    }
  }

  if (included_pids.empty()) {
    ready_writer();
    std::array<BYTE, kAudioBytesPerChunk> silence{};
    UINT64 nextTimestamp = 0;
    while (!stop_probe()) {
      if (WaitForSingleObject(stop_event, 20) != WAIT_TIMEOUT) break;
      result = writer(nextTimestamp, silence.data(), static_cast<DWORD>(silence.size()));
      if (FAILED(result)) break;
      nextTimestamp += kAudioChunkDuration100ns;
    }
    CoUninitialize();
    return result;
  }

  if (included_pids.size() == 1) {
    HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ComPtr<IAudioClient> client;
    result = completed ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    if (SUCCEEDED(result)) {
      result = ActivateProcessLoopback(included_pids[0], completed, &client,
                                       PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE);
    }
    if (SUCCEEDED(result)) {
      result = CaptureAudioFrames(client, nullptr, true, stop_event, stop_probe,
                                  ready_writer, writer);
    }
    if (completed != nullptr) CloseHandle(completed);
    client.Reset();
    CoUninitialize();
    return result;
  }

  struct StreamClient {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE sampleReady = nullptr;
    std::vector<BYTE> pending;
    size_t consumed = 0;
  };
  std::vector<StreamClient> streams;
  WAVEFORMATEX format{};
  format.wFormatTag = WAVE_FORMAT_PCM;
  format.nChannels = kAudioChannels;
  format.nSamplesPerSec = kAudioSampleRate;
  format.wBitsPerSample = kAudioBytesPerSample * 8;
  format.nBlockAlign = kAudioChannels * kAudioBytesPerSample;
  format.nAvgBytesPerSec = kAudioSampleRate * format.nBlockAlign;

  for (DWORD pid : included_pids) {
    HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!completed) continue;
    ComPtr<IAudioClient> client;
    HRESULT hr = ActivateProcessLoopback(pid, completed, &client,
                                         PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE);
    CloseHandle(completed);
    if (FAILED(hr) || !client) continue;

    HANDLE sampleReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!sampleReady) continue;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        0, 0, &format, nullptr);
    ComPtr<IAudioCaptureClient> capture;
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(sampleReady);
    if (SUCCEEDED(hr)) hr = client->Start();
    if (SUCCEEDED(hr)) {
      StreamClient sc;
      sc.client = client;
      sc.capture = capture;
      sc.sampleReady = sampleReady;
      streams.push_back(std::move(sc));
    } else {
      CloseHandle(sampleReady);
    }
  }

  if (streams.empty()) {
    ready_writer();
    std::array<BYTE, kAudioBytesPerChunk> silence{};
    UINT64 nextTimestamp = 0;
    while (!stop_probe()) {
      if (WaitForSingleObject(stop_event, 20) != WAIT_TIMEOUT) break;
      result = writer(nextTimestamp, silence.data(), static_cast<DWORD>(silence.size()));
      if (FAILED(result)) break;
      nextTimestamp += kAudioChunkDuration100ns;
    }
    CoUninitialize();
    return result;
  }

  ready_writer();
  std::array<BYTE, kAudioBytesPerChunk> silence{};
  result = writer(0, silence.data(), static_cast<DWORD>(silence.size()));

  UINT64 nextTimestamp = 0;
  std::vector<HANDLE> wait_handles;
  for (const auto& s : streams) {
    wait_handles.push_back(s.sampleReady);
  }
  wait_handles.push_back(stop_event);

  while (SUCCEEDED(result)) {
    if (stop_probe()) {
      result = S_OK;
      break;
    }
    const DWORD wait = WaitForMultipleObjects(
        static_cast<DWORD>(wait_handles.size()), wait_handles.data(), FALSE, 20);
    if (wait == WAIT_OBJECT_0 + wait_handles.size() - 1) {
      result = S_OK;
      break;
    }

    for (auto& s : streams) {
      UINT32 frames = 0;
      while (SUCCEEDED(s.capture->GetNextPacketSize(&frames)) && frames > 0) {
        BYTE* data = nullptr;
        DWORD flags = 0;
        UINT64 devicePosition = 0;
        UINT64 qpcPosition = 0;
        HRESULT hr = s.capture->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcPosition);
        if (FAILED(hr)) break;
        const size_t bytes = static_cast<size_t>(frames) * format.nBlockAlign;
        if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0) {
          s.capture->ReleaseBuffer(frames);
          continue;
        }
        if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
          s.pending.clear();
          s.consumed = 0;
        }
        if (s.pending.size() == s.consumed) {
          s.pending.clear();
          s.consumed = 0;
        }
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
          s.pending.insert(s.pending.end(), bytes, 0);
        } else {
          s.pending.insert(s.pending.end(), data, data + bytes);
        }
        s.capture->ReleaseBuffer(frames);
      }
    }

    bool has_chunk = false;
    for (const auto& s : streams) {
      if (s.pending.size() - s.consumed >= kAudioBytesPerChunk) {
        has_chunk = true;
        break;
      }
    }

    if (has_chunk) {
      std::array<int16_t, kAudioFramesPerChunk * kAudioChannels> mixed{};
      for (auto& s : streams) {
        if (s.pending.size() - s.consumed >= kAudioBytesPerChunk) {
          const auto* samples = reinterpret_cast<const int16_t*>(s.pending.data() + s.consumed);
          for (size_t i = 0; i < mixed.size(); ++i) {
            int32_t val = static_cast<int32_t>(mixed[i]) + static_cast<int32_t>(samples[i]);
            mixed[i] = static_cast<int16_t>(std::clamp(val, -32768, 32767));
          }
          s.consumed += kAudioBytesPerChunk;
          if (s.consumed > kAudioBytesPerChunk * 4) {
            s.pending.erase(s.pending.begin(), s.pending.begin() + static_cast<ptrdiff_t>(s.consumed));
            s.consumed = 0;
          }
        }
      }
      result = writer(nextTimestamp, reinterpret_cast<const BYTE*>(mixed.data()), kAudioBytesPerChunk);
      nextTimestamp += kAudioChunkDuration100ns;
    }
  }

  for (auto& s : streams) {
    if (s.client) s.client->Stop();
    if (s.sampleReady) CloseHandle(s.sampleReady);
  }
  CoUninitialize();
  return result;
}

HRESULT CaptureMicrophone(const std::wstring& device_id, HANDLE stop_event, const StopProbe& stop_probe,
                           const ReadyWriter& ready_writer, const PCMWriter& writer) {
  HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(result)) return result;
  ComPtr<IAudioClient> client;
  result = ActivateEndpoint(&client, true, device_id);
  if (SUCCEEDED(result)) {
    result = CaptureAudioFrames(client, nullptr, false, stop_event, stop_probe, ready_writer, writer);
  }
  client.Reset();
  CoUninitialize();
  return result;
}

}  // namespace piik::capture
