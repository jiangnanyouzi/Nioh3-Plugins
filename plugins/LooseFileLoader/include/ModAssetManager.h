#pragma once

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
}  // namespace LooseFileLoader

