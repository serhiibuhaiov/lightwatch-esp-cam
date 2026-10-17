#pragma once
//
// An ArduinoJson allocator that puts a document in PSRAM when the board has it, and falls back to the normal heap when
// it has not (or PSRAM is full). For the big documents (scenarios up to ~60 KB, /runs.json, the device list and its
// state, /api/status, the history of the readings, the config); the small ones stay as they are. Never used for
// anything a task stack, an interrupt or DMA touches.
//
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include <cstdlib>

// True when the board has PSRAM (an AI-Thinker ESP32-CAM has 4 MB): the limits that need memory for big documents depend on it.
inline bool psramAvailable() {
  static const bool has = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > (1u << 20);
  return has;
}

class PsramAllocator : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t size) override {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
  }
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
