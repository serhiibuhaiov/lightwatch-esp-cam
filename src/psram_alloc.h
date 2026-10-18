#pragma once
//
// PSRAM allocators. PsramAllocator, for ArduinoJson, puts a document in PSRAM when the board has it, and falls back to
// the normal heap when it has not (or PSRAM is full). For the big documents (scenarios up to ~60 KB, /runs.json, the
// device list and its state, /api/status, the history of the readings, the config) and the Tuya session buffers; the
// small ones stay as they are.
// Never used for anything a task stack, an interrupt or DMA touches. PsramStdAllocator does the same for std::
// containers whose blocks are under 4 KB and so would stay in the internal RAM.
//
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include <cstdlib>
#include <new>

// True when the board has PSRAM (an AI-Thinker ESP32-CAM has 4 MB): the limits that need memory for big documents depend on it.
inline bool psramAvailable() {
  static const bool has = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > (1u << 20);
  return has;
}

// malloc() from PSRAM when the board has it, from the normal heap otherwise (or when PSRAM is full); free() releases it.
inline void* psramMalloc(size_t size) {
  void* p = psramAvailable() ? heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
  return p ? p : malloc(size);
}

class PsramAllocator : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t size) override { return psramMalloc(size); }
  void deallocate(void* p) override { free(p); }  // free() releases a block from any heap_caps region
  void* reallocate(void* p, size_t size) override {
    void* q = heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, size);  // on failure heap_caps_realloc leaves `p` as it was
  }
  static ArduinoJson::Allocator* instance() {
    static PsramAllocator a;
    return &a;
  }
};

// A std:: container allocator over psramMalloc() (normal heap without PSRAM): for data that would otherwise take the
// 8-bit internal RAM the network and the SD card driver need (blocks under 4 KB stay there), like the history of the
// readings. Out of memory: std::bad_alloc (abort without exceptions), as with the standard allocator.
template <class T>
struct PsramStdAllocator {
  using value_type = T;
  PsramStdAllocator() noexcept = default;
  template <class U>
  PsramStdAllocator(const PsramStdAllocator<U>&) noexcept {}
  T* allocate(size_t n) {
    void* p = psramMalloc(n ? n * sizeof(T) : 1);
    if (!p) {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
      throw std::bad_alloc();
#else
      abort();
#endif
    }
    return static_cast<T*>(p);
  }
  void deallocate(T* p, size_t) noexcept { free(p); }
  template <class U>
  bool operator==(const PsramStdAllocator<U>&) const noexcept { return true; }
  template <class U>
  bool operator!=(const PsramStdAllocator<U>&) const noexcept { return false; }
};
