// Local profiles: separate projects and progress for each person on one computer, with no account and
// nothing online. A family shares one copy of the game. On disk:
//
//   <data>/estado.json              which profile was last used
//   <data>/idioma.json              { "idioma": "en" }, the game's language
//   <data>/perfis/p1/perfil.json    { "name": "Jogador 1" }
//   <data>/perfis/p1/projetos/      that profile's projects
//   <data>/perfis/p1/autosave.bancada
//
// Folders use generated ids, never names, so a name can hold anything a person types.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bancada::io {

struct Profile {
    std::string id;
    std::string name;
};

class ProfileStore {
public:
    // Reads what's there and makes sure at least one profile exists.
    explicit ProfileStore(std::filesystem::path dataDirectory);

    const std::vector<Profile>& profiles() const { return profiles_; }
    const Profile& current() const { return profiles_[currentIndex_]; }

    // Both return false, changing nothing, if the disk refuses or the name is empty.
    bool create(std::string_view name);
    bool select(const std::string& id);

    std::filesystem::path projectsFolder() const;
    std::filesystem::path autosavePath() const;

    // The lessons: each one's board as the player left it, and which ones they finished.
    //   <data>/perfis/p1/aulas/<id>.bancada
    //   <data>/perfis/p1/aulas.json      { "concluidas": ["acender-led", ...] }
    std::filesystem::path lessonPath(std::string_view lesson) const;
    std::vector<std::string> finishedLessons() const;
    bool finishLesson(std::string_view lesson);  // false only if the disk refuses
    // Forgets every finished lesson and every lesson board, for this profile only. False if the disk refuses.
    bool resetLessons();

    // The computer the player writes their own programs on, kept apart from everything else.
    //   <data>/perfis/p1/programa.bancada
    std::filesystem::path programPath() const;
    std::filesystem::path program32Path() const;  // the same, for the 32-bit computer

    // The game's language, for every profile: its code, "pt" or "en", or empty if none was chosen.
    std::string language() const;
    bool setLanguage(std::string_view code);  // false only if the disk refuses

private:
    void load();
    bool write(const std::string& id, std::string_view name);
    void rememberCurrent();
    std::filesystem::path folderOf(const std::string& id) const;

    std::filesystem::path data_;
    std::vector<Profile> profiles_;
    std::size_t currentIndex_ = 0;
};

}  // namespace bancada::io
