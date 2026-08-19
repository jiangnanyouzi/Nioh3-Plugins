#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace LooseFileLoader {

class ModAssetManager {
public:
    void Build(const std::filesystem::path& gameRootDir);
    void Refresh();
    [[nodiscard]] std::optional<std::filesystem::path> Find(std::uint32_t fileHash) const;

private:
    // The resource hook can run while another game thread requests an asset.
    // Publish a complete replacement index under this lock rather than
    // mutating the map the hook is reading.
    mutable std::mutex mutex_{};
    std::filesystem::path gameRootDir_{};
    bool hasBuiltIndex_ = false;
    std::unordered_map<std::uint32_t, std::filesystem::path> overrides_{};
};

inline ModAssetManager g_modAssetManager;
// When false, Find() reports no overrides and the game reads its original
// archives again. Toggled at runtime via nioh3_loose_file_loader_toggle().
inline std::atomic_bool g_modsEnabled{true};
}  // namespace LooseFileLoader

