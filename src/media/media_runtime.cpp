#include "media_runtime.h"
#include <mutex>
extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
}

bool initializeMediaRuntime() {
    static std::once_flag once;
    static bool ready = false;
    std::call_once(once, [] {
        avdevice_register_all();
        ready = avformat_network_init() >= 0;
    });
    return ready;
}
