#include "io/profiles.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <system_error>

#include "io/files.hpp"

namespace bancada::io {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr std::uint64_t kMaxSmallFile = 1 << 20;

std::span<const std::uint8_t> asBytes(const std::string& text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

json readJson(const fs::path& path) {
    const auto bytes = readWholeFile(path, kMaxSmallFile);
    if (!bytes) return json();
    return json::parse(bytes->begin(), bytes->end(), nullptr, false);
}

// The number in "p12", or 0 for anything else.
int profileNumber(const std::string& id) {
    if (id.size() < 2 || id.size() > 9 || id[0] != 'p') return 0;
    if (!std::all_of(id.begin() + 1, id.end(), [](char c) { return c >= '0' && c <= '9'; })) return 0;
    return std::stoi(id.substr(1));
}

}  // namespace

ProfileStore::ProfileStore(fs::path dataDirectory) : data_(std::move(dataDirectory)) {
    load();
    if (profiles_.empty()) {
        create("Jogador 1");
    }
    if (profiles_.empty()) profiles_.push_back({"p1", "Jogador 1"});  // a read-only disk still gets a game
}

fs::path ProfileStore::folderOf(const std::string& id) const { return data_ / "perfis" / id; }

fs::path ProfileStore::projectsFolder() const { return folderOf(current().id) / "projetos"; }

fs::path ProfileStore::autosavePath() const { return folderOf(current().id) / "autosave.bancada"; }

fs::path ProfileStore::lessonPath(std::string_view lesson) const { return folderOf(current().id) / "aulas" / pathFromUtf8(std::string(lesson) + ".bancada"); }

std::vector<std::string> ProfileStore::finishedLessons() const {
    std::vector<std::string> finished;
    const json file = readJson(folderOf(current().id) / "aulas.json");
    if (!file.is_object() || !file.contains("concluidas") || !file["concluidas"].is_array()) return finished;
    for (const json& id : file["concluidas"]) {
        if (id.is_string() && id.get<std::string>().size() <= 64) finished.push_back(id.get<std::string>());
    }
    return finished;
}

fs::path ProfileStore::programPath() const { return folderOf(current().id) / "programa.bancada"; }
fs::path ProfileStore::program32Path() const { return folderOf(current().id) / "programa32.bancada"; }

std::string ProfileStore::language() const {
    const json file = readJson(data_ / "idioma.json");
    if (!file.is_object() || !file.contains("idioma") || !file["idioma"].is_string()) return {};
    const std::string code = file["idioma"].get<std::string>();
    return code.size() <= 8 ? code : std::string();
}

bool ProfileStore::setLanguage(std::string_view code) {
    std::error_code error;
    fs::create_directories(data_, error);
    const std::string text = json{{"idioma", std::string(code)}}.dump();
    return writeFileAtomically(data_ / "idioma.json", asBytes(text));
}

bool ProfileStore::resetLessons() {
    std::error_code error;
    fs::remove(folderOf(current().id) / "aulas.json", error);
    if (error) return false;
    fs::remove_all(folderOf(current().id) / "aulas", error);
    return !error;
}

bool ProfileStore::finishLesson(std::string_view lesson) {
    std::vector<std::string> finished = finishedLessons();
    if (std::find(finished.begin(), finished.end(), lesson) != finished.end()) return true;
    finished.emplace_back(lesson);
    std::error_code error;
    fs::create_directories(folderOf(current().id), error);
    const std::string text = json{{"concluidas", finished}}.dump();
    return writeFileAtomically(folderOf(current().id) / "aulas.json", asBytes(text));
}

void ProfileStore::load() {
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator(data_ / "perfis", error)) {
        const std::string id = pathToUtf8(entry.path().filename());
        if (profileNumber(id) == 0) continue;
        const json file = readJson(entry.path() / "perfil.json");
        if (!file.is_object() || !file.contains("name") || !file["name"].is_string()) continue;
        profiles_.push_back({id, file["name"].get<std::string>()});
    }
    std::sort(profiles_.begin(), profiles_.end(),
              [](const Profile& a, const Profile& b) { return profileNumber(a.id) < profileNumber(b.id); });

    const json state = readJson(data_ / "estado.json");
    if (state.is_object() && state.contains("perfil") && state["perfil"].is_string()) {
        const std::string wanted = state["perfil"].get<std::string>();
        for (std::size_t i = 0; i < profiles_.size(); ++i) {
            if (profiles_[i].id == wanted) currentIndex_ = i;
        }
    }
}

bool ProfileStore::write(const std::string& id, std::string_view name) {
    std::error_code error;
    fs::create_directories(folderOf(id) / "projetos", error);
    if (error) return false;
    const std::string text = json{{"name", std::string(name)}}.dump();
    return writeFileAtomically(folderOf(id) / "perfil.json", asBytes(text));
}

void ProfileStore::rememberCurrent() {
    std::error_code error;
    fs::create_directories(data_, error);
    const std::string text = json{{"perfil", current().id}}.dump();
    writeFileAtomically(data_ / "estado.json", asBytes(text));
}

bool ProfileStore::create(std::string_view name) {
    if (name.empty()) return false;
    int highest = 0;
    for (const Profile& profile : profiles_) highest = std::max(highest, profileNumber(profile.id));
    const std::string id = "p" + std::to_string(highest + 1);
    if (!write(id, name)) return false;
    profiles_.push_back({id, std::string(name)});
    currentIndex_ = profiles_.size() - 1;
    rememberCurrent();
    return true;
}

bool ProfileStore::select(const std::string& id) {
    for (std::size_t i = 0; i < profiles_.size(); ++i) {
        if (profiles_[i].id != id) continue;
        currentIndex_ = i;
        rememberCurrent();
        return true;
    }
    return false;
}

}  // namespace bancada::io
