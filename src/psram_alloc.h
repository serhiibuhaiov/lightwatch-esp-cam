#pragma once
//
// An ArduinoJson allocator that puts a document in PSRAM when the board has it, and falls back to the normal heap when
// it has not (or PSRAM is full). For the big documents only (scenarios: up to ~40 KB parsed); the small ones stay as
// they are. Never used for anything a task stack, an interrupt or DMA touches.
//
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include <cstdlib>

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
