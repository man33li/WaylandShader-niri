#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WsRuntime WsRuntime;
typedef struct WsParameter {
    const char* name;
    float value;
} WsParameter;

/* All calls for an instance are serialized. Creation may run on a worker.
 * display is the compositor's EGLDisplay. Each instance owns an unshared
 * desktop-GL context; calls restore the calling thread's EGL API/context/surfaces.
 * Empty preset creates a color-only runtime. Errors never cross the C ABI.
 */
WsRuntime* ws_create(void* display, const char* preset,
    const WsParameter* parameters, size_t parameter_count,
    char* error, size_t error_size);
void ws_destroy(WsRuntime* runtime);

/* Called with the compositor GLES context current. Image handles stay valid
 * until the next resize or destruction. Import independent GLES texture siblings;
 * Smithay owns those sibling names, the runtime owns desktop textures/EGLImages.
 * Input storage has a complete immutable mip chain, exported at level zero.
 */
bool ws_resize(WsRuntime* runtime, uint32_t width, uint32_t height,
    char* error, size_t error_size);
void* ws_input_image(const WsRuntime* runtime);
void* ws_output_image(const WsRuntime* runtime);

/* Called after the scene was GPU-blitted into the input sibling, with GLES
 * current. Inserts flushed EGL fences and server waits in both directions.
 * No readback, glFinish, or CPU fence waits. Output alpha is always opaque.
 */
bool ws_render(WsRuntime* runtime, uint64_t frame_number, float frames_per_second,
    uint32_t elapsed_milliseconds, bool clear_history, bool shader_enabled,
    float gamma, float saturation, char* error, size_t error_size);
bool ws_set_parameter(WsRuntime* runtime, const char* name, float value,
    char* error, size_t error_size);
/* Borrowed UTF-8 JSON parameter array using the existing controller schema. */
const char* ws_parameters_json(const WsRuntime* runtime);

#ifdef __cplusplus
}
#endif
