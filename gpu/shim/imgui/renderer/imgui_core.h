// stardust: no ImGui overlay in the port; hooks used by the renderer are no-ops, but for
// MustKeepDrawing: while Display Settings or Cheats is open the game is frozen (no flips), so the present
// thread redraws the last frame under the panel.
#pragma once
#include "video_core/renderer_vulkan/vk_common.h"
namespace Frontend { class WindowSDL; }
namespace Vulkan { class Instance; }
namespace StardustOverlay { bool PausesGame(); }
namespace ImGui::Core {
inline void Initialize(const Vulkan::Instance&, const Frontend::WindowSDL&, u32, vk::Format) {}
inline void OnSurfaceFormatChange(vk::Format) {}
inline void Shutdown(vk::Device) {}
inline bool MustKeepDrawing() { return StardustOverlay::PausesGame(); }
struct TextureManager { static void Submit() {} };
} // namespace ImGui::Core
