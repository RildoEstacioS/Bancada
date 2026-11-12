#pragma once

#include <filesystem>

namespace bancada::app {

// Where projects and settings live. A folder named "dados" next to the program makes the game
// portable: everything stays in it. Without one, the system's usual per-user folder is used.
struct DataLocation {
    std::filesystem::path directory;
    bool portable = false;
};

DataLocation findDataLocation();

}  // namespace bancada::app
