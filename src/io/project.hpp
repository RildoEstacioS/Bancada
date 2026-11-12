// A project on disk. What's saved is what the player made: parts, wires, joints, switch positions and
// where they were looking. Netlists and compiled code are never saved: they can always be rebuilt, and
// keeping them would mean keeping them valid forever.
#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "editor/board.hpp"
#include "editor/session.hpp"
#include "io/container.hpp"

namespace bancada::io {

inline constexpr std::uint32_t kFormatVersion = 10;

struct ProjectView {
    float x = -14;
    float y = -6;
    float cellPixels = 16;
};

struct Project {
    std::string name = "sem nome";
    editor::Board board;
    std::vector<editor::PartId> togglesOn;
    std::vector<std::pair<editor::PartId, std::uint64_t>> toggleValues;  // switches of more than one bit
    std::vector<editor::ChipData> chips;                                 // the project's other circuits
    ProjectView view;

    // Autosaves only. Never written to a normal file: paths hold a user's name, and files get shared.
    std::optional<std::string> origin;
    bool hasUnsavedChanges = false;
};

struct LoadedProject {
    std::optional<Project> project;
    std::optional<LoadError> error;
};

Bytes encodeProject(const Project& project);
LoadedProject decodeProject(std::span<const std::uint8_t> file);

bool saveProject(const std::filesystem::path& path, const Project& project);
LoadedProject loadProject(const std::filesystem::path& path);

// A short sentence for the player.
const char* describe(LoadError error);

// Upgrades a document one version at a time, so each step only ever deals with the version before it.
// steps[0] turns version 1 into 2, steps[1] turns 2 into 3, and so on. Steps stay here forever: they
// are the only way old files keep opening.
using MigrationStep = std::function<void(nlohmann::json&)>;
bool migrateDocument(nlohmann::json& document, std::uint32_t fromVersion, std::uint32_t toVersion,
                     std::span<const MigrationStep> steps);

}  // namespace bancada::io
