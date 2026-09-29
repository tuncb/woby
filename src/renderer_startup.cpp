#include "renderer_startup.h"
#include "graphics_helpers.h"
namespace woby {
void validateRendererStartup() { validateRendererCapabilities(*graphics::getCaps()); }
} // namespace woby
