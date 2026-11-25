// The window: translates SDL events into editor events, and keeps projects on disk. The rules of both
// live in src/editor and src/io.
//
//   --data DIR         use DIR for profiles and autosaves
//   --open FILE        open a project at start, instead of the autosave
//   --save FILE        save the project to FILE just before quitting
//   --frames N         quit after N frames
//   --screenshot FILE  save the last frame as a BMP
//   --script NAME      send real window events through the normal path: "xor", "perfis", "portas", "refino",
//                      "computador", "programa", "codigo", "velocidade", "atalhos", "computador16", "painel", "inicio", "led", "nand",
//                      "pisca", "dentro:N" (the Nth circuit of the example), "pasta:N" (the Nth library folder) or
//                      "raiox[:S[:I]]" (the X-ray, I instructions into the example, on step S), "aulas[:N[:end]]" (the list, on act N, maybe scrolled to the end), "aovivo:N" (inside, live)
//                      or "aula:N[:v][:r]" (lesson N, with Verificar pressed, or its first resistor's value open
//                      with one Backspace typed)
//   --dev              every act of the lessons open, for this run
//   --tamanho WxH      the window's size (1280x720 if not given)
//   --stress N         fill the board with N connected NANDs and report frame times
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/draw.hpp"
#include "app/paths.hpp"
#include "asm/assembler.hpp"
#include "asm/mips.hpp"
#include "editor/boxes.hpp"
#include "editor/computer16.hpp"
#include "editor/computer32.hpp"
#include "editor/computer32_view.hpp"
#include "editor/electric_examples.hpp"
#include "editor/gallery.hpp"
#include "editor/course.hpp"
#include "editor/lessons.hpp"
#include "electric/units.hpp"
#include "i18n/i18n.hpp"
#include "editor/session.hpp"
#include "editor/text_edit.hpp"
#include "io/files.hpp"
#include "io/profiles.hpp"
#include "io/project.hpp"

namespace {

using namespace bancada;
using app::Camera;
using app::Vec2;
namespace fs = std::filesystem;

constexpr Uint64 kAutosaveEveryMs = 30'000;
constexpr Uint64 kNoteMs = 4'000;
constexpr std::size_t kMaxNameBytes = 24;

enum class Prompt : std::uint8_t { None, Name, Value, Contents, NewChip, DevPassword, InsidePassword };

constexpr const char* kDevPassword = "1111";  // opens every act of the lessons, for this run

// A program being edited. It's only a draft until it's assembled into the ROM.
struct CodeEditing {
    editor::TextEdit text;
    std::string original;
    std::vector<assembly::Error> problems;
    std::vector<std::uint16_t> words;
    bool mips = false;                  // the 32-bit computer's ROM, in MIPS
    std::vector<std::uint32_t> words32;  // its program
    std::vector<std::uint32_t> data32;   // and its .data
    editor::PartId rom = 0;
    std::size_t capacity = 0;
    int scrollLine = 0;
    int scrollColumn = 0;
    bool selecting = false;
    std::string warning;
    bool help = false;  // the list of every instruction is open over the code
    float helpScroll = 0.0f;
    std::vector<int> lines;  // for each assembled word, its line
    int tab = 1;             // the right column's tab: task, commands or blocks
    int blocksFirst = 0;     // the Blocks tab: the first block shown
    int blocksFollowed = -1;  // the running block it last moved to, so it follows the program but not over a scroll
};

std::optional<std::uint64_t> parseHex(const std::string& text) {
    if (text.empty() || text.size() > 16) return std::nullopt;
    std::uint64_t value = 0;
    for (const char c : text) {
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        if (digit < 0) return std::nullopt;
        value = value * 16 + static_cast<std::uint64_t>(digit);
    }
    return value;
}

class App {
public:
    App(SDL_Window* window, SDL_Renderer* renderer, fs::path dataDirectory)
        : window_(window), renderer_(renderer), profiles_(std::move(dataDirectory)) {
        openEvent_ = SDL_RegisterEvents(4);
        saveEvent_ = openEvent_ + 1;
        codeOpenEvent_ = openEvent_ + 2;
        codeSaveEvent_ = openEvent_ + 3;
        chooseStartingLanguage();
        session_.watchClock([this] { watchPrints(); });
        syncPalette();
        resetView();
        updateSize();
    }

    editor::Session& session() { return session_; }
    Camera& camera() { return camera_; }
    const app::PaletteView& palette() const { return palette_; }
    int height() const { return height_; }
    int width() const { return width_; }
    bool quitting() const { return quitting_; }
    bool codeOpen() const { return code_.has_value(); }
    // The program editor is the view now: under the toolbar, which keeps working, over everything else.
    bool codeVisible() const {
        return code_.has_value() && codeShown_ && !homeOpen_ && !lessonsOpen_ && !galleryOpen_ && topic_ < 0 && insidePlan_ == nullptr;
    }
    int codeHeight() const { return height_ - static_cast<int>(app::kToolbarHeight); }
    std::size_t codeProblems() const { return code_ ? code_->problems.size() : 0; }

    // The size the screens are laid out in. A phone or a tablet has many pixels to a point: there everything is
    // drawn scaled by the screen's density, kept to at least 720 points of height, the size it was all drawn for.
    // Two fingers move the view and pinch it closer or farther, as on any phone: on the board and inside a part.
    // One finger is the mouse, which SDL makes of it; when a second one lands, what the first had started is let go.
    void touch(const SDL_TouchFingerEvent& finger) {
        const auto at = std::find_if(fingers_.begin(), fingers_.end(), [&](const auto& f) { return f.first == finger.fingerID; });
        if (finger.type == SDL_EVENT_FINGER_DOWN) {
            if (at == fingers_.end()) fingers_.push_back({finger.fingerID, {finger.x, finger.y}});
            touchScroll_ = 0.0f;
        } else if (finger.type == SDL_EVENT_FINGER_MOTION) {
            if (at != fingers_.end()) {
                if (fingers_.size() == 1) scrollWithFinger(finger.y - at->second.y);
                at->second = {finger.x, finger.y};
            }
        } else if (at != fingers_.end()) {
            fingers_.erase(at);
        }
        if (fingers_.size() != 2) {
            pinching_ = false;
            return;
        }
        const SDL_FPoint a = fingers_[0].second;
        const SDL_FPoint b = fingers_[1].second;
        const float centerX = (a.x + b.x) / 2.0f;
        const float centerY = (a.y + b.y) / 2.0f;
        const float distance = std::max(1.0f, std::hypot(a.x - b.x, a.y - b.y));
        if (!pinching_) {
            pinching_ = true;
            panning_ = false;
            if (session_.mode() != editor::Mode::Idle) session_.key(editor::Key::Escape);
        } else if (!peeks_.empty()) {
            const SDL_FRect area = app::peekBoardArea(width_, height_);
            peeks_.back().camera.pan(centerX - pinchX_, centerY - pinchY_);
            peeks_.back().camera.zoomAt(centerX - area.x, centerY - area.y, distance / pinchDistance_);
        } else if (insidePlan_ != nullptr) {
            insideView_.originX += centerX - pinchX_;
            insideView_.originY += centerY - pinchY_;
            zoomInside(centerX, centerY, distance / pinchDistance_);
        } else if (!modal() && view_ == app::kViewBoard) {
            camera_.pan(centerX - pinchX_, centerY - pinchY_);
            camera_.zoomAt(centerX, centerY, distance / pinchDistance_);
        }
        pinchX_ = centerX;
        pinchY_ = centerY;
        pinchDistance_ = distance;
    }

    // One finger up or down on a long screen scrolls it, the content following the finger. How many pixels make
    // one turn of the mouse wheel there decides how far each move goes; where the wheel moves in whole steps
    // (lines of code, rows of the table) the finger's moves add up to a step first.
    void scrollWithFinger(float dy) {
        float unit = 0.0f;
        bool whole = false;
        if (insidePlan_ != nullptr || bundle_ != 0 || !peeks_.empty()) return;
        if (codeVisible()) {
            unit = code_->help ? 60.0f : 24.0f;
            whole = !code_->help;
        } else if (lessonsOpen_) {
            unit = 60.0f;
        } else if (topic_ >= 0) {
            unit = 70.0f;
        } else if (!modal() && mouseX_ < app::kPanelWidth) {
            unit = 40.0f;
            // Scrolling the library is not picking a part up.
            if (draggingFromPalette_) {
                draggingFromPalette_ = false;
                session_.key(editor::Key::Escape);
            }
        } else if (!modal() && overRightPanel(mouseX_)) {
            unit = 54.0f;
            whole = true;
        }
        if (unit <= 0.0f) return;
        touchScroll_ += dy;
        const float turns = touchScroll_ / unit;
        if (!whole) {
            wheel(turns);
            touchScroll_ = 0.0f;
        } else if (std::abs(turns) >= 1.0f) {
            const float step = std::trunc(turns);
            wheel(step);
            touchScroll_ -= step * unit;
        }
    }

    void updateSize() {
        int w = 0;
        int h = 0;
        SDL_GetCurrentRenderOutputSize(renderer_, &w, &h);
#ifdef SDL_PLATFORM_ANDROID
        // Only where nothing of the system sits: not under the camera's notch or a bar that's still showing.
        SDL_Rect safe{0, 0, w, h};
        if (!SDL_GetWindowSafeArea(window_, &safe) || safe.w <= 0 || safe.h <= 0) safe = {0, 0, w, h};
        const float density = SDL_GetWindowDisplayScale(window_);
        const float scale = std::max(1.0f, std::min(density, static_cast<float>(std::min(safe.w, safe.h)) / 720.0f));
        SDL_SetRenderScale(renderer_, scale, scale);
        const SDL_Rect view{static_cast<int>(static_cast<float>(safe.x) / scale), static_cast<int>(static_cast<float>(safe.y) / scale),
                            static_cast<int>(static_cast<float>(safe.w) / scale), static_cast<int>(static_cast<float>(safe.h) / scale)};
        SDL_SetRenderViewport(renderer_, &view);
        w = view.w;
        h = view.h;
#endif
        width_ = w;
        height_ = h;
    }

    void handle(SDL_Event event) {
        if (event.type == openEvent_ || event.type == saveEvent_ || event.type == codeOpenEvent_ || event.type == codeSaveEvent_) {
            finishDialog(event);
            return;
        }
        SDL_ConvertEventToRenderCoordinates(renderer_, &event);
        switch (event.type) {
            case SDL_EVENT_QUIT:
                quitting_ = true;
                break;
            // A phone sends the game away without asking and may close it there: what's on the bench is saved first.
            case SDL_EVENT_WILL_ENTER_BACKGROUND:
            case SDL_EVENT_TERMINATING:
                autosave();
                break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
                updateSize();
                break;
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_MOTION:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED:
                touch(event.tfinger);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (pinching_ && event.motion.which == SDL_TOUCH_MOUSEID) break;  // the fingers move the view, not the cursor
                mouseMoved(event.motion.x, event.motion.y);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                mouseDown(event.button.button, event.button.x, event.button.y);
                if (event.button.button == SDL_BUTTON_LEFT && event.button.clicks == 2 && !peeks_.empty()) {
                    peekDeeper(event.button.x, event.button.y);
                } else if (event.button.button == SDL_BUTTON_LEFT && event.button.clicks == 2 && overBoard(mouseX_, mouseY_) && !modal()) {
                    session_.leftUp();
                    const editor::Part* part = session_.partUnderCursor();
                    if (part != nullptr && part->kind == editor::PartKind::Chip) {
                        session_.openChipUnderCursor();
                    } else if (part != nullptr && openPeek(*part)) {
                        // how the part is made, from the gallery
                    } else if (part != nullptr && !editor::isGate(part->kind) && app::canLookInside(*part)) {
                        lookInside(part->id);  // a big part the gallery doesn't build: where it sits in the whole
                    }
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                mouseUp(event.button.button);
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                wheel(event.wheel.y);
                break;
            case SDL_EVENT_KEY_DOWN:
                // Android's Back button does what Esc does: closes, cancels, goes back a level.
                keyDown(event.key.key == SDLK_AC_BACK ? SDLK_ESCAPE : event.key.key, event.key.mod);
                break;
            case SDL_EVENT_KEY_UP:
                typeOnKeyboard(event.key.key, event.key.mod, false);
                break;
            case SDL_EVENT_TEXT_INPUT:
                if (codeVisible()) {
                    if (!code_->text.insert(event.text.text)) code_->warning = kCodeTooBig;
                    codeEdited();
                    break;
                }
                if (palette_.searching) {
                    palette_.filter += event.text.text;
                    palette_.scroll = 0;
                    break;
                }
                if ((profilesOpen_ || prompt_ != Prompt::None) && typing_.size() + std::strlen(event.text.text) <= maxTyped()) {
                    typing_ += event.text.text;
                }
                break;
            case SDL_EVENT_DROP_FILE:
                if (event.drop.data != nullptr && !code_ && mayDiscard()) openFile(io::pathFromUtf8(event.drop.data));
                break;
            default:
                break;
        }
    }

    void fit() {
        const editor::Board& board = session_.board();
        if (board.parts().empty()) return;
        // Wires can go beyond the parts, and names stand to the right of the last column.
        const editor::Rect extent = board.extent();
        const int left = extent.left;
        const int top = extent.top;
        const int right = extent.right;
        const int bottom = extent.bottom;
        const float areaWidth = static_cast<float>(width_) - app::kPanelWidth - rightEdgeWidth() - 110.0f;
        const float areaHeight = static_cast<float>(height_) - boardTop() - app::kStatusHeight - (session_.currentCircuit() != 0 ? 80.0f : 40.0f);
        const float zoom = std::min(areaWidth / static_cast<float>(right - left), areaHeight / static_cast<float>(bottom - top));
        // A board with little on it isn't blown up to fill the window: it stays at a size to build on,
        // in the middle, with room around for what comes next.
        camera_.cellPixels = std::clamp(zoom, 0.25f, 18.0f);
        const float spareX = std::max(0.0f, (areaWidth - static_cast<float>(right - left) * camera_.cellPixels) / 2.0f);
        const float spareY = std::max(0.0f, (areaHeight - static_cast<float>(bottom - top) * camera_.cellPixels) / 2.0f);
        camera_.originX = static_cast<float>(left) - (app::kPanelWidth + 20.0f + spareX) / camera_.cellPixels;
        // Inside a chip, the way back sits over the board's top: room is left under it.
        const float margin = session_.currentCircuit() != 0 ? 60.0f : 20.0f;
        camera_.originY = static_cast<float>(top) - (boardTop() + margin + spareY) / camera_.cellPixels;
    }

    // Where the board starts on screen: under the toolbar, and under the lesson's strip during one.
    float boardTop() const {
        return app::kToolbarHeight + ((lesson_ >= 0 || example_ >= 0) && view_ == app::kViewBoard ? app::kLessonStripHeight : 0.0f);
    }

    std::vector<app::PaletteGroup> libraryGroups(const std::string& filter = {}) const {
        return app::paletteGroups(session_, filter, lesson_ >= 0 ? &lessonLimit_ : nullptr);
    }

    // --- Lessons -------------------------------------------------------------------------------------

    void showLessons() {
        homeOpen_ = false;
        topic_ = -1;
        lessonsOpen_ = true;
        finished_ = profiles_.finishedLessons();
        // The act to go on with: the last one open that still has something to do, or else the last one open.
        const int acts = static_cast<int>(editor::lessonActs().size());
        lessonAct_ = 0;
        for (int a = 0; a < acts; ++a) {
            if (!devUnlocked_ && !editor::actOpen(a, finished_)) break;
            lessonAct_ = a;
            if (editor::actDone(a, finished_) < static_cast<int>(editor::actLessons(a).size())) break;
        }
        lessonScroll_ = 0.0f;
    }

    app::LessonListView lessonListView() const {
        app::LessonListView view;
        view.act = lessonAct_;
        view.scroll = lessonScroll_;
        view.finished = &finished_;
        view.confirming = SDL_GetTicks() < resetConfirmUntil_;
        view.unlocked = devUnlocked_;
        view.denied = SDL_GetTicks() < devDeniedUntil_;
        return view;
    }

    bool finishedHere(const std::string& id) const { return std::find(finished_.begin(), finished_.end(), id) != finished_.end(); }

    // A topic's reading, over everything.
    void openReading(int topic) {
        if (topic < 0 || topic >= static_cast<int>(editor::topics().size())) return;
        autosave();
        lessonsOpen_ = false;
        homeOpen_ = false;
        topic_ = topic;
        readingScroll_ = 0.0f;
        readingPage_ = 0;
        finished_ = profiles_.finishedLessons();
    }
    void showReadingPage(int page) {
        if (topic_ < 0) return;
        readingPage_ = std::clamp(page, 0, app::readingPageCount(topic_) - 1);
        readingScroll_ = 0.0f;
    }
    // Answers every question of the reading right: for the screenshots.
    void answerReading() {
        if (topic_ < 0) return;
        const editor::Topic& topic = editor::topics()[static_cast<std::size_t>(topic_)];
        std::vector<int>& answers = readingAnswers_[topic_];
        answers.assign(topic.blocks.size(), -1);
        for (std::size_t b = 0; b < topic.blocks.size(); ++b) {
            if (topic.blocks[b].kind == editor::TheoryBlockKind::Question) answers[b] = editor::questionParts(topic.blocks[b].text).right;
        }
    }

    app::ReadingView readingView() const {
        app::ReadingView view;
        view.topic = topic_;
        view.scroll = readingScroll_;
        view.read = topic_ >= 0 && finishedHere(editor::readingId(editor::topics()[static_cast<std::size_t>(topic_)]));
        view.page = readingPage_;
        const auto answers = readingAnswers_.find(topic_);
        if (answers != readingAnswers_.end()) view.answers = &answers->second;
        return view;
    }

    // Done reading: it counts as read, and the first exercise not done yet opens.
    void startReadingExercises() {
        const editor::Topic& topic = editor::topics()[static_cast<std::size_t>(topic_)];
        profiles_.finishLesson(editor::readingId(topic));
        finished_ = profiles_.finishedLessons();
        topic_ = -1;
        int chosen = -1;
        for (const char* id : topic.exercises) {
            const int lesson = editor::lessonIndex(id);
            if (lesson >= 0 && chosen < 0 && !finishedHere(id)) chosen = lesson;
        }
        if (chosen < 0 && !topic.exercises.empty()) chosen = editor::lessonIndex(topic.exercises.front());
        if (chosen >= 0) {
            startLesson(chosen);
        } else {
            showLessons();
        }
    }

    // After a lesson: the next one, through its reading when it starts a topic not read yet, or the list
    // when it's in an act still closed.
    void goToLesson(int index) {
        if (index < 0 || index >= static_cast<int>(editor::lessons().size())) return;
        finished_ = profiles_.finishedLessons();
        if (!devUnlocked_ && !editor::actOpen(editor::lessons()[static_cast<std::size_t>(index)].act, finished_)) {
            autosave();
            showLessons();
            return;
        }
        const int topic = editor::topicOfLesson(index);
        if (topic >= 0 && topic != editor::topicOfLesson(lesson_) &&
            !finishedHere(editor::readingId(editor::topics()[static_cast<std::size_t>(topic)]))) {
            openReading(topic);
            return;
        }
        startLesson(index);
    }

    // Where Next goes from an exercise, in the course's order: the next exercise of the same reading, or else
    // the next reading, always, even one read before, since it's where what comes next is explained. -1
    // and a topic: that reading; an index: that exercise; both -1: the course is over.
    struct NextStep {
        int lesson = -1;
        int topic = -1;
    };

    NextStep nextStep() const {
        const int topic = editor::topicOfLesson(lesson_);
        if (topic < 0) return {lesson_ + 1 < static_cast<int>(editor::lessons().size()) ? lesson_ + 1 : -1, -1};
        const editor::Topic& here = editor::topics()[static_cast<std::size_t>(topic)];
        const std::string_view id = editor::lessons()[static_cast<std::size_t>(lesson_)].id;
        for (std::size_t i = 0; i + 1 < here.exercises.size(); ++i) {
            if (id == here.exercises[i]) return {editor::lessonIndex(here.exercises[i + 1]), -1};
        }
        if (topic + 1 < static_cast<int>(editor::topics().size())) return {-1, topic + 1};
        return {};
    }

    void goToNext() {
        const NextStep next = nextStep();
        if (next.lesson >= 0) {
            startLesson(next.lesson);
            return;
        }
        if (next.topic < 0) {
            showLessons();
            return;
        }
        finished_ = profiles_.finishedLessons();
        if (!devUnlocked_ && !editor::actOpen(editor::topics()[static_cast<std::size_t>(next.topic)].act, finished_)) {
            autosave();
            showLessons();
            return;
        }
        openReading(next.topic);
    }

    // Opens a lesson where the player left it, or from its first board. Whatever was open is saved first.
    void startLesson(int index) {
        if (index < 0 || index >= static_cast<int>(editor::lessons().size())) return;
        autosave();
        lessonsOpen_ = false;
        homeOpen_ = false;
        programMode_ = false;  // else the lesson's board would be saved over the player's program
        program32_ = false;
        lesson_ = index;
        const editor::Lesson& lesson = editor::lessons()[static_cast<std::size_t>(index)];
        lessonLimit_ = {};
        lessonLimit_.kinds = editor::lessonLibrary(lesson);
        for (const char* key : lesson.boxes) lessonLimit_.boxes.push_back(editor::boxFromKey(key));
        view_ = app::kViewBoard;
        palette_.folder = 0;
        palette_.filter.clear();
        palette_.scroll = 0;

        io::LoadedProject saved = io::loadProject(lessonFile(lesson));
        if (saved.project) {
            saved.project->hasUnsavedChanges = false;
            applyProject(std::move(*saved.project), std::nullopt);
        } else {
            io::Project project;
            project.board = lesson.start();
            if (lesson.chips != nullptr) project.chips = lesson.chips();
            applyProject(std::move(project), std::nullopt);
            fit();
        }
        projectName_ = lesson.title;
        session_.markSaved();
        resetQuiz();
        lessonRevision_ = UINT64_MAX;
        lessonProblemShown_ = false;
        lessonHint_ = -1;
        palette_.folder = lessonFolder(lesson);
        syncPalette();
        // A programming lesson is about the code: it opens on the editor, the task beside it.
        if (lesson.check == editor::LessonCheck::Program) openLessonProgram();
    }

    // The library opens on the folder of the lesson's first part: the one the lesson is about.
    int lessonFolder(const editor::Lesson& lesson) const {
        const std::vector<app::PaletteGroup> groups = libraryGroups();
        for (int g = 0; g < static_cast<int>(groups.size()); ++g) {
            for (const app::PaletteItem& item : groups[static_cast<std::size_t>(g)].items) {
                if (lesson.parts.empty() ? item.kind == editor::PartKind::Box : item.kind == lesson.parts.front()) return g;
            }
        }
        return 0;
    }

    // The first press only asks; a second one within a few seconds forgets every lesson of this player.
    void resetLessons() {
        if (SDL_GetTicks() >= resetConfirmUntil_) {
            resetConfirmUntil_ = SDL_GetTicks() + kNoteMs;
            return;
        }
        resetConfirmUntil_ = 0;
        leaveLesson();  // the open lesson's board would otherwise be saved again
        if (!profiles_.resetLessons()) showNote("Não deu para apagar o progresso.", true);
        finished_ = profiles_.finishedLessons();
        lessonResult_ = {};
    }

    // Where the player's board for a lesson is kept: under the lesson's version too, so that a lesson
    // that changed in a new version of the game starts fresh.
    fs::path lessonFile(const editor::Lesson& lesson) const {
        return profiles_.lessonPath(std::string(lesson.id) + "-" + editor::lessonVersion(lesson));
    }

    // Back to the player's own project, with the lesson kept as it is.
    // The same for the player's program computer.
    void leaveLesson() {
        if (lesson_ < 0 && !programMode_) return;
        autosave();
        lesson_ = -1;
        programMode_ = false;
        program32_ = false;
        restoreAutosave();
        syncPalette();
    }

    void restartLesson() {
        const editor::Lesson& lesson = editor::lessons()[static_cast<std::size_t>(lesson_)];
        io::Project project;
        project.board = lesson.start();
            if (lesson.chips != nullptr) project.chips = lesson.chips();
        project.name = lesson.title;
        applyProject(std::move(project), std::nullopt);
        fit();
        autosavedRevision_ = UINT64_MAX;  // the fresh board replaces the saved one
        resetQuiz();
        lessonProblemShown_ = false;
        lessonHint_ = -1;
        lessonRevision_ = UINT64_MAX;
    }

    // Checked after every change: a lesson finishes the moment it works. What's wrong only shows once
    // the player asks, with Verificar, so that it doesn't nag while they're still building.
    void checkLessonNow() {
        if (lesson_ < 0 || session_.currentCircuit() != 0 || session_.revision() == lessonRevision_) return;
        if (editor::lessons()[static_cast<std::size_t>(lesson_)].check == editor::LessonCheck::Answer) return;  // the questions decide
        lessonRevision_ = session_.revision();
        const editor::Lesson& lesson = editor::lessons()[static_cast<std::size_t>(lesson_)];
        const bool before = lessonResult_.passed;
        const editor::ChipBoards chips = session_.chipBoards();
        lessonResult_ = editor::checkLesson(lesson, session_.board(), &chips);
        if (lessonResult_.passed && !before) {
            profiles_.finishLesson(lesson.id);
            finished_ = profiles_.finishedLessons();
            lessonHint_ = -1;
        }
    }

    void pressLessonButton(app::LessonButton button) {
        const editor::Lesson& lesson = editor::lessons()[static_cast<std::size_t>(lesson_)];
        switch (button) {
            case app::LessonButton::Check:
                if (lesson.check == editor::LessonCheck::Answer) {
                    submitQuiz();
                    break;
                }
                lessonRevision_ = UINT64_MAX;
                checkLessonNow();
                lessonProblemShown_ = true;
                lessonHint_ = -1;
                break;
            case app::LessonButton::Hint:
                // A lesson of questions has a hint for each question: the one being asked.
                if (lesson.check == editor::LessonCheck::Answer && quizIndex_ < lesson.questions.size()) {
                    lessonHint_ = 0;
                    lessonProblemShown_ = false;
                    break;
                }
                lessonHint_ = (lessonHint_ + 1) % static_cast<int>(lesson.hints.size());
                lessonProblemShown_ = false;
                break;
            case app::LessonButton::Restart: restartLesson(); break;
            case app::LessonButton::List:
                autosave();
                showLessons();
                break;
            case app::LessonButton::Program: openLessonProgram(); break;
            case app::LessonButton::Reading: openReading(editor::topicOfLesson(lesson_)); break;
            case app::LessonButton::Next: goToNext(); break;
            case app::LessonButton::None: break;
        }
    }

    // --- A lesson of questions ----------------------------------------------------------------------------

    bool quizActive() const {
        return lesson_ >= 0 && editor::lessons()[static_cast<std::size_t>(lesson_)].check == editor::LessonCheck::Answer && view_ == app::kViewBoard &&
               !modal();
    }

    // A lesson just opened or started over: its questions from the first, and no result left from the lesson
    // before. A board's result comes back with the next check; a lesson of questions has no board to check,
    // so it shows as done only when the player finished it before.
    void resetQuiz() {
        quizIndex_ = 0;
        quizTyped_.clear();
        quizWrong_ = false;
        quizRight_ = false;
        quizBadDigit_ = false;
        lessonResult_ = {};
        if (lesson_ < 0) return;
        const editor::Lesson& lesson = editor::lessons()[static_cast<std::size_t>(lesson_)];
        if (lesson.check == editor::LessonCheck::Answer && finishedHere(lesson.id)) lessonResult_ = {true, lesson.success};
    }

    // Enter, or the button: checks the answer, or after a right one goes to the next question. The last one
    // right finishes the lesson.
    void submitQuiz() {
        const editor::Lesson& lesson = editor::lessons()[static_cast<std::size_t>(lesson_)];
        if (quizIndex_ >= lesson.questions.size()) return;
        if (quizRight_) {
            ++quizIndex_;
            lessonHint_ = -1;
            quizTyped_.clear();
            quizRight_ = false;
            quizWrong_ = false;
            return;
        }
        if (quizTyped_.empty()) return;
        if (editor::answers(lesson.questions[quizIndex_], quizTyped_)) {
            quizRight_ = true;
            quizWrong_ = false;
            if (quizIndex_ + 1 == lesson.questions.size() && !lessonResult_.passed) {
                lessonResult_ = {true, lesson.success};
                profiles_.finishLesson(lesson.id);
                finished_ = profiles_.finishedLessons();
                lessonHint_ = -1;
            }
        } else {
            quizWrong_ = true;
        }
    }

    bool quizKey(SDL_Keycode key, SDL_Keymod mod) {
        if ((mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) != 0) return false;
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            submitQuiz();
            return true;
        }
        if (key == SDLK_BACKSPACE) {
            if (!quizTyped_.empty() && !quizRight_) quizTyped_.pop_back();
            quizWrong_ = false;
            return true;
        }
        char c = 0;
        if (key >= SDLK_0 && key <= SDLK_9) c = static_cast<char>('0' + (key - SDLK_0));
        if (key >= SDLK_KP_1 && key <= SDLK_KP_9) c = static_cast<char>('1' + (key - SDLK_KP_1));
        if (key == SDLK_KP_0) c = '0';
        if (key >= SDLK_A && key <= SDLK_F) c = static_cast<char>('A' + (key - SDLK_A));
        if (key == SDLK_X) c = 'x';
        if (c == 0) return false;
        const editor::Lesson& lesson = editor::lessons()[static_cast<std::size_t>(lesson_)];
        if (quizIndex_ < lesson.questions.size() && c != 'x') {
            const int base = lesson.questions[quizIndex_].base;
            const int digit = c >= 'A' ? c - 'A' + 10 : c - '0';
            if (digit >= base && !quizRight_) {
                quizBadDigit_ = true;
                return true;
            }
        }
        quizBadDigit_ = false;
        if (quizWrong_) quizTyped_.clear();  // after a wrong answer, a new one starts
        if (!quizRight_ && quizTyped_.size() < 20) quizTyped_ += c;
        quizWrong_ = false;
        return true;
    }

    app::QuizView quizView() const {
        const editor::Lesson& lesson = editor::lessons()[static_cast<std::size_t>(lesson_)];
        app::QuizView view;
        view.count = static_cast<int>(lesson.questions.size());
        if (quizIndex_ < lesson.questions.size()) {
            view.question = &lesson.questions[quizIndex_];
            view.number = static_cast<int>(quizIndex_) + 1;
        }
        view.typed = quizTyped_;
        view.wrong = quizWrong_;
        view.right = quizRight_;
        view.badDigit = quizBadDigit_;
        return view;
    }

    app::LessonStrip lessonStrip() const {
        app::LessonStrip strip;
        strip.index = lesson_;
        strip.passed = lessonResult_.passed;
        const NextStep next = nextStep();
        strip.hasNext = next.lesson >= 0 || next.topic >= 0;
        const editor::Lesson& lesson = editor::lessons()[static_cast<std::size_t>(lesson_)];
        strip.program = lesson.check == editor::LessonCheck::Program;
        const std::vector<int> act = editor::actLessons(lesson.act);
        for (std::size_t i = 0; i < act.size(); ++i) {
            if (act[i] == lesson_) strip.position = static_cast<int>(i) + 1;
            const char* id = editor::lessons()[static_cast<std::size_t>(act[i])].id;
            strip.actDone.push_back(std::find(finished_.begin(), finished_.end(), id) != finished_.end());
        }
        strip.hasReading = editor::topicOfLesson(lesson_) >= 0;
        if (strip.passed) {
            strip.message = lessonResult_.message;
        } else if (lessonHint_ >= 0 && lesson.check == editor::LessonCheck::Answer && quizIndex_ < lesson.questions.size()) {
            strip.message = i18n::format("Dica: {}", {i18n::tr(lesson.questions[quizIndex_].hint)});
            strip.hint = true;
        } else if (lessonHint_ >= 0) {
            strip.message = i18n::format("Dica: {}", {i18n::tr(lesson.hints[static_cast<std::size_t>(lessonHint_)])});
            strip.hint = true;
        } else if (lessonProblemShown_) {
            strip.message = lessonResult_.message;
            strip.problem = true;
        }
        return strip;
    }

    SDL_FRect lessonStripArea() const { return {app::kPanelWidth, app::kToolbarHeight, boardRightEdge() - app::kPanelWidth, app::kLessonStripHeight}; }

    app::HomeInfo homeInfo() const {
        app::HomeInfo info;
        info.lessonsLine = lessonsLine();
        info.total = static_cast<int>(editor::lessons().size());
        const std::vector<std::string> finished = profiles_.finishedLessons();
        for (const std::string& id : finished) info.done += editor::lessonIndex(id) >= 0 ? 1 : 0;
        info.about = i18n::format("{} atos, {} aulas teóricas e {} exercícios, cada um verificado pelo próprio circuito.",
                                  {std::to_string(editor::lessonActs().size()), std::to_string(editor::topics().size()), std::to_string(info.total)});
        for (std::size_t act = 0; act < editor::lessonActs().size(); ++act) {
            const int count = static_cast<int>(editor::actLessons(static_cast<int>(act)).size());
            if (count > 0 && editor::actDone(static_cast<int>(act), finished) < count) {
                info.next = i18n::tr(editor::lessonActs()[act].title);
                break;
            }
        }
        return info;
    }

    std::string lessonsLine() const {
        std::size_t done = 0;
        for (const std::string& id : profiles_.finishedLessons()) done += editor::lessonIndex(id) >= 0 ? 1 : 0;
        if (done == 0) {
            return i18n::format("{} aulas e {} exercícios", {std::to_string(editor::topics().size()), std::to_string(editor::lessons().size())});
        }
        return i18n::format("{} de {} exercícios feitos", {std::to_string(done), std::to_string(editor::lessons().size())});
    }

    void draw(const char* dataNote) {
        app::drawBoard(renderer_, session_, camera_, width_, height_);
        app::InterfaceText text;
        text.dataNote = dataNote;
        text.profile = profiles_.current().name.c_str();
        text.project = projectName_.c_str();
        text.dirty = session_.dirty();
        if (SDL_GetTicks() < noteUntil_) {
            text.note = note_.c_str();
            text.noteIsError = noteIsError_;
        }
        text.showTable = tableVisible_ && !quizLesson() && !codeVisible();
        text.tableScroll = tableScroll_;
        text.showHelp = helpOpen_;
        text.mouseX = mouseX_;
        text.mouseY = mouseY_;
        text.searching = palette_.searching;
        const std::optional<editor::ComputerState> computer = editor::readComputer(session_.board(), session_.circuit(), session_.chipBoards());
        const std::optional<editor::Computer32State> computer32 =
            computer ? std::nullopt : editor::readComputer32(session_.board(), session_.circuit(), session_.chipBoards());
        computerHere_ = computer.has_value() || computer32.has_value();
        is32_ = computer32.has_value();
        if (!computerHere_) view_ = app::kViewBoard;
        text.computer = computer ? &*computer : nullptr;
        text.computer32 = computer32 ? &*computer32 : nullptr;
        text.console = &console_;
        text.view = view_;
        text.zoom = zoom_;
        text.programShown = codeVisible();
        if (view_ == app::kViewXray && computer) advanceXray(*computer, text);
        if (view_ == app::kViewXray && computer32) advanceXray(computer32->pc, computer32->halted, text);
        app::DebugView debug;
        if (computer) {
            followComputer(*computer);
            debug.breakpoints = &breakpoints_;
            debug.before = registersBefore_;
            debug.memoryBefore = memoryBefore_;
            debug.hasBefore = hasBefore_;
            debug.running = session_.running();
            text.debug = &debug;
        } else if (computer32) {
            followComputer32(*computer32);
            debug.breakpoints = &breakpoints_;
            debug.before32 = registers32Before_;
            debug.hasBefore = hasBefore_;
            debug.running = session_.running();
            text.debug = &debug;
        }
        reading_.clear();
        if (session_.mode() == editor::Mode::Idle && view_ == app::kViewBoard && !modal() && overBoard(mouseX_, mouseY_)) {
            const Vec2 at = camera_.toBoard(mouseX_, mouseY_);
            const editor::Part* part = session_.board().partAtCell({static_cast<int>(std::floor(at.x)), static_cast<int>(std::floor(at.y))});
            if (part != nullptr && editor::isElectric(part->kind)) {
                if (const std::optional<editor::PartReading> reading = session_.circuit().reading(part->id)) reading_ = app::describeReading(*part, *reading);
            }
        }
        text.reading = reading_.c_str();
        app::LessonStrip strip;
        app::QuizView quiz;
        if (lesson_ >= 0) {
            strip = lessonStrip();
            text.lesson = &strip;
            text.limit = &lessonLimit_;
            if (quizActive()) {
                quiz = quizView();
                text.quiz = &quiz;
            }
        }
        const app::ExampleStrip example{example_, exampleVersion_};
        if (example_ >= 0) text.example = &example;
        app::drawInterface(renderer_, session_, palette_, width_, height_, text);
        if (profilesOpen_) app::drawProfiles(renderer_, profileList(), width_, height_);
        if (prompt_ != Prompt::None) {
            const char* unit = "";
            if (prompt_ == Prompt::Value && electricValue()) unit = electric::unitSymbol(*editor::valueQuantity(session_.inspected()->part.kind));
            app::drawPrompt(renderer_, promptTitle(), typing_, width_, height_, unit);
        }
        if (codeVisible()) {
            followBlocks();
            const SDL_Rect below{0, static_cast<int>(app::kToolbarHeight), width_, codeHeight()};
            SDL_SetRenderViewport(renderer_, &below);
            app::drawCode(renderer_, codeView(), width_, codeHeight());
            SDL_SetRenderViewport(renderer_, nullptr);
        }
        if (homeOpen_) app::drawHome(renderer_, width_, height_, mouseX_, mouseY_, homeInfo(), homeComputers_);
        if (homeOpen_ && prompt_ == Prompt::InsidePassword) app::drawPrompt(renderer_, promptTitle(), std::string(typing_.size(), '*'), width_, height_);
        if (lessonsOpen_) app::drawLessonList(renderer_, width_, height_, mouseX_, mouseY_, lessonListView());
        if (lessonsOpen_ && prompt_ == Prompt::DevPassword) app::drawPrompt(renderer_, promptTitle(), std::string(typing_.size(), '*'), width_, height_);
        if (topic_ >= 0) app::drawReading(renderer_, width_, height_, mouseX_, mouseY_, readingView());
        if (galleryOpen_) app::drawGallery(renderer_, width_, height_, mouseX_, mouseY_, galleryGroup_);
        refreshInside();
        if (insidePlan_ != nullptr) app::drawInside(renderer_, width_, height_, mouseX_, mouseY_, insideView_);
        if (!peeks_.empty()) app::drawPeek(renderer_, width_, height_, mouseX_, mouseY_, peekView());
        if (bundle_ != 0 && insidePlan_ == nullptr) {
            app::BundleView view;
            view.part = session_.board().part(bundle_);
            if (view.part == nullptr) {
                bundle_ = 0;
            } else {
                for (unsigned pin = 0; pin <= view.part->inputs; ++pin) view.values.push_back(session_.circuit().pinValue(bundle_, pin));
                app::drawBundle(renderer_, width_, height_, mouseX_, mouseY_, view);
            }
        }
    }

    // "Por dentro": the computer drawn whole. Its plan is made the first time, and kept.
    void openInside(bool is32, app::InsideLevel level = app::InsideLevel::Gates) {
        std::optional<editor::Architecture>& plan = is32 ? inside32_ : inside16_;
        if (!plan) plan = is32 ? editor::architecture32() : editor::architecture16();
        insidePlan_ = &*plan;
        insideView_ = app::insideWhole(*plan, width_, height_);
        insideView_.level = level;
        homeOpen_ = false;
    }
    void closeInside() {
        insidePlan_ = nullptr;
        insideDragging_ = false;
        if (insideLiveMode_) {  // back to the board it was opened from
            insideLiveMode_ = false;
            insideLive_.reset();
            return;
        }
        homeOpen_ = true;
        homeComputers_ = true;
    }

    // A part of the board from the inside, live: the plan of the whole board, the part big in the middle, its
    // wires lit by what the running circuit has on them. A splitter or a joiner has no gates: its own view.
    void lookInside(editor::PartId id) {
        const editor::Part* part = session_.board().part(id);
        if (part == nullptr) return;
        if (editor::isBundle(part->kind)) {
            bundle_ = id;
            return;
        }
        const std::string board = session_.currentCircuit() != 0 ? session_.currentName() : projectName_;
        insideLive_ = editor::buildArchitecture(session_.board(), &session_.chipBoards(), board.empty() ? std::string("Bancada") : board);
        insideRevision_ = session_.revision();
        insidePlan_ = &*insideLive_;
        insideLiveMode_ = true;
        insideFocus_ = part->label.empty() ? std::string(i18n::tr(editor::partName(*part))) : part->label;
        for (const editor::CircuitEntry& circuit : session_.circuits()) {
            if (part->kind == editor::PartKind::Chip && circuit.id == part->chip) insideFocus_ = circuit.name;
        }
        insideView_ = app::insideWhole(*insidePlan_, width_, height_);
        for (const editor::ArchPlace& place : insidePlan_->places) {
            if (place.x == part->position.x && place.y == part->position.y) insideView_ = app::insideOn(*insidePlan_, place.rect, width_, height_);
        }
    }

    // The values the live plan shows: read from the running circuit every frame.
    void refreshInside() {
        if (!insideLiveMode_ || insidePlan_ == nullptr) return;
        try {
            insideValues_ = editor::archValues(*insidePlan_, session_.circuit());
        } catch (const std::exception&) {
            insideValues_.clear();
        }
        insideView_.values = &insideValues_;
        insideView_.running = session_.running();
        insideView_.clockHigh = session_.clockLevel();
        insideView_.focus = insideFocus_;
    }
    // Closer by factor, keeping the point under (x, y) where it is.
    void zoomInside(float x, float y, float factor) {
        const float whole = app::insideWhole(*insidePlan_, width_, height_).scale;
        const float next = std::clamp(insideView_.scale * factor, whole * 0.6f, 90.0f);
        const float k = next / insideView_.scale;
        insideView_.originX = x - (x - insideView_.originX) * k;
        insideView_.originY = y - (y - insideView_.originY) * k;
        insideView_.scale = next;
    }
    // For the screenshots: the plan zoomed by factor around a point given as a share of its width and height.
    void lookInside(float factor, float shareX, float shareY) {
        if (insidePlan_ == nullptr) return;
        const SDL_FRect area = app::insidePlanArea(width_, height_);
        const float px = insideView_.originX + shareX * insidePlan_->width * insideView_.scale;
        const float py = insideView_.originY + shareY * insidePlan_->height * insideView_.scale;
        insideView_.originX += area.x + area.w / 2.0f - px;
        insideView_.originY += area.y + area.h / 2.0f - py;
        zoomInside(area.x + area.w / 2.0f, area.y + area.h / 2.0f, factor);
    }
    void pressInside(Uint8 button, float x, float y) {
        if (button != SDL_BUTTON_LEFT) return;
        const SDL_FRect area = app::insidePlanArea(width_, height_);
        switch (app::insideAt(x, y, width_, height_, insideLiveMode_)) {
            case app::InsideChoice::Run: toggleRun(); return;
            case app::InsideChoice::Step: session_.stepClock(); return;
            case app::InsideChoice::Back: closeInside(); return;
            case app::InsideChoice::Gates: insideView_.level = app::InsideLevel::Gates; return;
            case app::InsideChoice::Nands: insideView_.level = app::InsideLevel::Nands; return;
            case app::InsideChoice::Transistors: insideView_.level = app::InsideLevel::Transistors; return;
            case app::InsideChoice::Closer: zoomInside(area.x + area.w / 2.0f, area.y + area.h / 2.0f, 1.6f); return;
            case app::InsideChoice::Farther: zoomInside(area.x + area.w / 2.0f, area.y + area.h / 2.0f, 1.0f / 1.6f); return;
            case app::InsideChoice::Whole: {
                const app::InsideLevel level = insideView_.level;
                insideView_ = app::insideWhole(*insidePlan_, width_, height_);
                insideView_.level = level;
                return;
            }
            case app::InsideChoice::None: break;
        }
        if (y >= area.y && y < area.y + area.h) insideDragging_ = true;
    }

    // A part from the inside: how the gallery builds it, on a board to look at. False if the gallery doesn't.
    bool openPeek(const editor::Part& part) {
        const editor::PartInside inside = editor::galleryInsideOf(part);
        if (inside.item == nullptr) return false;
        Peek peek;
        peek.title = i18n::tr(inside.item->title);
        peek.item = inside.item;
        peeks_.push_back(std::move(peek));
        showPeekVersion(static_cast<int>(inside.version));
        return true;
    }
    void showPeekVersion(int version) {
        Peek& peek = peeks_.back();
        peek.version = std::clamp(version, 1, static_cast<int>(peek.item->versions.size()) - 1);
        editor::GalleryProject project = editor::buildGalleryItem(*peek.item, static_cast<std::size_t>(peek.version));
        peek.session = std::make_unique<editor::Session>();
        peek.session->adoptProjectChips(std::move(project.chips));
        peek.session->adopt(std::move(project.board));
        if (!project.toggles.empty()) peek.session->setToggleValues(project.toggles);
        peek.camera = app::peekFit(*peek.session, width_, height_);
    }
    app::PeekView peekView() const {
        app::PeekView view;
        const Peek& peek = peeks_.back();
        view.session = peek.session.get();
        view.camera = peek.camera;
        for (const Peek& p : peeks_) view.trail.push_back(p.title);
        for (std::size_t i = 1; i < peek.item->versions.size(); ++i) view.versions.push_back(i18n::tr(peek.item->versions[i].name));
        view.version = peek.version - 1;
        view.how = i18n::tr(peek.item->versions[static_cast<std::size_t>(peek.version)].how);
        return view;
    }
    void pressPeek(Uint8 button, float x, float y) {
        if (button != SDL_BUTTON_LEFT) return;
        const app::PeekHit hit = app::peekAt(renderer_, x, y, width_, height_, peekView());
        switch (hit.choice) {
            case app::PeekChoice::Back: peeks_.pop_back(); return;
            case app::PeekChoice::Version: showPeekVersion(hit.version + 1); return;
            case app::PeekChoice::Board: {
                // A switch flips; anywhere else the board is dragged around.
                const SDL_FRect area = app::peekBoardArea(width_, height_);
                const Vec2 at = peeks_.back().camera.toBoard(x - area.x, y - area.y);
                if (!peeks_.back().session->flipToggleAt(at.x, at.y)) peekDragging_ = true;
                return;
            }
            case app::PeekChoice::None: return;
        }
    }
    // A double click inside: the part under it, from the inside too.
    void peekDeeper(float x, float y) {
        const SDL_FRect area = app::peekBoardArea(width_, height_);
        if (x < area.x || y < area.y || x >= area.x + area.w || y >= area.y + area.h) return;
        const Vec2 at = peeks_.back().camera.toBoard(x - area.x, y - area.y);
        const editor::Part* part =
            peeks_.back().session->board().partAtCell({static_cast<int>(std::floor(at.x)), static_cast<int>(std::floor(at.y))});
        if (part == nullptr) return;
        const editor::Part copy = *part;
        peekDragging_ = false;
        openPeek(copy);
    }

    void showHome() {
        homeOpen_ = true;
        homeComputers_ = false;
    }

    void showComputers() {
        homeOpen_ = true;
        homeComputers_ = true;
    }
    void setCodeTab(int tab) {
        if (code_) code_->tab = tab;
    }
    void unlockEverything() { devUnlocked_ = true; }
    void openCodeHelp(float scroll) {
        if (!code_) return;
        code_->help = true;
        code_->helpScroll = scroll;
    }
    void scrollReading(float amount) { readingScroll_ = amount; }
    void showLive(int which) {
        openExample();
        for (int i = 0; i < 12; ++i) session_.stepClock();
        const auto find = [&](auto test) -> editor::PartId {
            for (const editor::Part& part : session_.board().parts()) {
                if (test(part)) return part.id;
            }
            return 0;
        };
        const auto chipNamed = [&](const char* name) {
            for (const editor::CircuitEntry& circuit : session_.circuits()) {
                if (circuit.name.find(name) != std::string::npos) return circuit.id;
            }
            return editor::ChipId{0};
        };
        if (which == 0) {
            lookInside(find([&](const editor::Part& p) { return p.kind == editor::PartKind::Chip && p.chip == chipNamed("CPU"); }));
            return;
        }
        if (which < 0) {
            session_.openCircuit(chipNamed("CPU"));
            session_.openCircuit(chipNamed("Decodificador"));
            fit();
            return;
        }
        session_.openCircuit(chipNamed("Decodificador"));
        lookInside(find([&](const editor::Part& p) { return p.kind == (which == 1 ? editor::PartKind::Box : editor::PartKind::Splitter); }));
    }
    void showAct(int act, bool toEnd = false) {
        lessonAct_ = std::clamp(act, 0, static_cast<int>(editor::lessonActs().size()) - 1);
        lessonScroll_ = toEnd ? app::lessonListMaxScroll(width_, height_, lessonListView()) : 0.0f;
    }

    void openBoardExample(editor::Board board, const char* name) {
        io::Project project;
        project.name = name;
        project.board = std::move(board);
        applyProject(std::move(project), std::nullopt);
        tableScroll_ = 0;
        fit();
    }

    void openElectricExample(app::Example which) {
        switch (which) {
            case app::Example::LedCircuit:
                openBoardExample(editor::buildLedExample().board, "LED e resistor");
                showNote("Clique na chave para fechar o circuito. Passe o mouse nas peças para medir.", false);
                break;
            case app::Example::TransistorNand:
                openBoardExample(editor::buildTransistorNand().board, "NAND de transistores");
                showNote("Mude A e B: a tabela-verdade mostra que dois transistores fazem uma NAND.", false);
                break;
            case app::Example::Blinker:
                openBoardExample(editor::buildBlinker().board, "pisca-pisca");
                showNote("Ninguém manda os LEDs piscarem: os capacitores se carregam e se descarregam.", false);
                break;
            default: break;
        }
    }
    float boardRightEdge() const { return static_cast<float>(width_) - rightEdgeWidth(); }

    // The X-ray walks through the six steps of each instruction on its own, unless a step was chosen.
    // While the clock runs, instructions go by too fast for that: then every step shows at once.
    void advanceXray(const editor::ComputerState& computer, app::InterfaceText& text) { advanceXray(computer.pc, computer.halted, text); }

    void advanceXray(std::uint32_t pc, bool halted, app::InterfaceText& text) {
        constexpr Uint64 kStepMs = 1700;
        const Uint64 now = SDL_GetTicks();
        if (pc != xrayPc_ || halted != xrayHalted_) {
            xrayPc_ = pc;
            xrayHalted_ = halted;
            xrayStage_ = 0;
            xrayStart_ = now;
            xrayPinned_ = false;
        }
        if (session_.running()) {
            text.xrayStage = editor::kTraceStages - 1;
            text.xrayProgress = 0.0f;
            return;
        }
        const Uint64 hold = xrayStage_ == editor::kTraceStages - 1 ? 2 * kStepMs : kStepMs;
        if (!xrayPinned_ && now - xrayStart_ >= hold) {
            xrayStage_ = (xrayStage_ + 1) % editor::kTraceStages;
            xrayStart_ = now;
        }
        text.xrayStage = xrayStage_;
        text.xrayProgress = static_cast<float>(now - xrayStart_) / 1000.0f;
    }

    // Remembers the registers before each instruction, for the debugger to mark what changed, and stops a
    // running computer at a breakpoint. Running on from one first leaves it, so it doesn't stop at once.
    void followComputer(const editor::ComputerState& computer) {
        if (computer.pc != debugPc_) {
            if (debugPc_ != UINT32_MAX) {
                registersBefore_ = registersNow_;
                memoryBefore_ = memoryNow_;
                hasBefore_ = true;
            }
            debugPc_ = computer.pc;
            registersNow_ = computer.registers;
            memoryNow_ = computer.memory;
            if (computer.pc != leavingBreakpoint_) leavingBreakpoint_ = UINT32_MAX;
        }
        const bool stop = std::find(breakpoints_.begin(), breakpoints_.end(), computer.pc) != breakpoints_.end();
        if (session_.running() && stop && computer.pc != leavingBreakpoint_) {
            session_.setRunning(false);
            showNote("Parou no ponto de parada.", false);
        }
    }

    // The same for the 32-bit computer. Its breakpoints are words of the ROM, as the debugger lists them.
    void followComputer32(const editor::Computer32State& computer) {
        if (computer.pc != debugPc_) {
            if (debugPc_ != UINT32_MAX) {
                registers32Before_ = registers32Now_;
                hasBefore_ = true;
            }
            debugPc_ = computer.pc;
            registers32Now_ = computer.registers;
            if (computer.index != leavingBreakpoint_) leavingBreakpoint_ = UINT32_MAX;
        }
        const bool stop = std::find(breakpoints_.begin(), breakpoints_.end(), computer.index) != breakpoints_.end();
        if (session_.running() && stop && computer.index != leavingBreakpoint_) {
            session_.setRunning(false);
            showNote("Parou no ponto de parada.", false);
        }
    }

    // The console is outside the CPU, as a terminal is: just before each clock edge, a syscall that prints
    // has its number or character on the CPU's pins, and it's written here.
    void watchPrints() {
        for (const editor::Part& part : session_.board().parts()) {
            if (part.kind != editor::PartKind::Chip || part.interface == nullptr || part.interface->name != editor::kCpu32Name) continue;
            unsigned number = UINT32_MAX, character = UINT32_MAX, value = UINT32_MAX;
            for (unsigned pin = 0; pin < editor::pinCount(part); ++pin) {
                const std::string_view name = editor::pinName(part, pin);
                if (name == "ImprimeNum") number = pin;
                if (name == "ImprimeCar") character = pin;
                if (name == "Valor") value = pin;
            }
            if (number == UINT32_MAX || character == UINT32_MAX || value == UINT32_MAX) return;
            const auto printed = static_cast<std::uint32_t>(session_.circuit().pinValue(part.id, value));
            if (session_.circuit().pinOn(part.id, number)) console_ += std::to_string(static_cast<std::int32_t>(printed));
            if (session_.circuit().pinOn(part.id, character)) console_ += static_cast<char>(printed & 0xFFu);
            if (console_.size() > 8000) console_.erase(0, console_.size() - 6000);
            return;
        }
    }

    void resetComputer() {
        session_.resetSimulation();
        console_.clear();
        hasBefore_ = false;
        debugPc_ = UINT32_MAX;
    }

    // Play and pause, the same from the toolbar, the debugger and the space bar. Running on leaves a
    // breakpoint the computer is stopped at, instead of stopping there again at once; a program that already
    // ended at its HLT starts over from the beginning.
    void toggleRun() {
        if (session_.running()) {
            session_.setRunning(false);
            return;
        }
        const std::optional<editor::ComputerState> computer = editor::readComputer(session_.board(), session_.circuit(), session_.chipBoards());
        const std::optional<editor::Computer32State> computer32 = editor::readComputer32(session_.board(), session_.circuit(), session_.chipBoards());
        if ((computer && computer->halted) || (computer32 && computer32->halted)) resetComputer();
        if (const std::optional<editor::ComputerState> now = editor::readComputer(session_.board(), session_.circuit(), session_.chipBoards())) {
            leavingBreakpoint_ = now->pc;
        }
        if (const std::optional<editor::Computer32State> now = editor::readComputer32(session_.board(), session_.circuit(), session_.chipBoards())) {
            leavingBreakpoint_ = now->index;
        }
        session_.setRunning(true);
    }

    void pressDebugger(const app::DebugHit& hit) {
        switch (hit.action) {
            case app::DebugAction::Continue: toggleRun(); break;
            case app::DebugAction::Pause: session_.setRunning(false); break;
            case app::DebugAction::Step:
                session_.setRunning(false);
                session_.stepClock();
                session_.stepClock();
                break;
            case app::DebugAction::Stage:
                session_.setRunning(false);
                if (xrayStage_ + 1 < editor::kTraceStages) {
                    chooseXrayStage(xrayStage_ + 1);
                } else {
                    session_.stepClock();
                    session_.stepClock();
                }
                break;
            case app::DebugAction::Reset:
                session_.setRunning(false);
                resetComputer();
                break;
            case app::DebugAction::Breakpoint: {
                const auto found = std::find(breakpoints_.begin(), breakpoints_.end(), hit.address);
                if (found == breakpoints_.end()) {
                    breakpoints_.push_back(hit.address);
                } else {
                    breakpoints_.erase(found);
                }
                break;
            }
            case app::DebugAction::None: break;
        }
    }

    SDL_FRect debuggerArea() const { return {0.0f, app::kToolbarHeight, app::kDebugWidth, static_cast<float>(height_) - app::kToolbarHeight}; }

    // The area the panel or the X-ray takes, after the debugger.
    SDL_FRect viewArea() const {
        const app::ExampleStrip example{example_, exampleVersion_};
        const float about = app::programAboutHeight(example_ >= 0 ? &example : nullptr, view_);
        return {app::kDebugWidth, app::kToolbarHeight + about, boardRightEdge() - app::kDebugWidth,
                static_cast<float>(height_) - app::kToolbarHeight - app::kStatusHeight - about};
    }

    void chooseXrayStage(int stage) {
        xrayStage_ = stage;
        xrayStart_ = SDL_GetTicks();
        xrayPinned_ = true;
    }

    // The language chosen before, or else the computer's: Portuguese for a Portuguese one, English for
    // any other.
    void chooseStartingLanguage() {
        if (const auto saved = i18n::languageFromCode(profiles_.language())) {
            i18n::setLanguage(*saved);
            return;
        }
        int count = 0;
        SDL_Locale** locales = SDL_GetPreferredLocales(&count);
        if (locales != nullptr && count > 0 && locales[0]->language != nullptr) {
            i18n::setLanguage(std::string_view(locales[0]->language) == "pt" ? i18n::Language::Portuguese : i18n::Language::English);
        }
        SDL_free(locales);
    }

    void chooseLanguage(i18n::Language language) {
        i18n::setLanguage(language);
        profiles_.setLanguage(i18n::codeOf(language));
    }

    void chooseHome(app::HomeChoice choice) {
        if (choice == app::HomeChoice::None) return;
        if (choice == app::HomeChoice::Portuguese || choice == app::HomeChoice::English) {
            chooseLanguage(choice == app::HomeChoice::English ? i18n::Language::English : i18n::Language::Portuguese);
            return;
        }
        // The computers have a page of their own; the 32-bit one isn't open yet.
        if (choice == app::HomeChoice::Computers || choice == app::HomeChoice::Back) {
            homeComputers_ = choice != app::HomeChoice::Back;
            return;
        }
        if (choice == app::HomeChoice::Computer32) {
            homeComputers_ = false;
            openProgram32();
            return;
        }
        if (choice == app::HomeChoice::InsideLock) {
            openPrompt(Prompt::InsidePassword);
            return;
        }
        if (choice == app::HomeChoice::Inside16 || choice == app::HomeChoice::Inside32) {
            openInside(choice == app::HomeChoice::Inside32);
            return;
        }
        if (choice == app::HomeChoice::Computer16) choice = app::HomeChoice::Program;
        homeComputers_ = false;
        homeOpen_ = false;
        // Anything but continuing leaves a lesson for the player's own projects.
        if (choice != app::HomeChoice::Continue && choice != app::HomeChoice::Lessons && choice != app::HomeChoice::Example &&
            choice != app::HomeChoice::Program) {
            leaveLesson();
        }
        switch (choice) {
            case app::HomeChoice::Lessons: showLessons(); break;
            case app::HomeChoice::New:
                autosave();
                startBlank();
                break;
            case app::HomeChoice::Example: galleryOpen_ = true; break;
            case app::HomeChoice::Program: openProgram(); break;
            case app::HomeChoice::Open: chooseFileToOpen(); break;
            default: break;
        }
    }

    // An example from the gallery, as a project of its own. Whatever was open is saved first, and a
    // lesson left as it was.
    void openGalleryItem(int index, int version = 0) {
        if (index < 0 || index >= static_cast<int>(editor::galleryItems().size())) return;
        galleryOpen_ = false;
        homeOpen_ = false;
        leaveLesson();
        autosave();
        const editor::GalleryItem& item = editor::galleryItems()[static_cast<std::size_t>(index)];
        version = item.versions.empty() ? 0 : std::clamp(version, 0, static_cast<int>(item.versions.size()) - 1);
        editor::GalleryProject example = editor::buildGalleryItem(item, static_cast<std::size_t>(version));
        io::Project project;
        project.name = item.title;
        project.board = std::move(example.board);
        project.chips = std::move(example.chips);
        for (const auto& [id, value] : example.toggles) {
            const editor::Part* part = project.board.part(id);
            if (part != nullptr && part->width == 1) {
                project.togglesOn.push_back(id);
            } else {
                project.toggleValues.emplace_back(id, value);
            }
        }
        applyProject(std::move(project), std::nullopt);
        example_ = index;
        exampleVersion_ = version;
        view_ = example.computer ? app::kViewPanel : app::kViewBoard;
        tableScroll_ = 0;
        fit();
        showNote(example.note.c_str(), false);
    }

    void showGallery(int folder = -1) {
        homeOpen_ = false;
        galleryOpen_ = true;
        if (folder >= 0) galleryGroup_ = std::clamp(folder, 0, static_cast<int>(editor::galleryGroups().size()) - 1);
    }

    // The player's own computer, to write programs on: kept in a file of its own, apart from the
    // examples and the player's projects, and opened on the program, ready to write.
    void openProgram() {
        homeOpen_ = false;
        galleryOpen_ = false;
        leaveLesson();
        autosave();
        io::LoadedProject saved = io::loadProject(profiles_.programPath());
        // Only a computer is the player's program: anything else saved there by mistake is left aside.
        const auto isComputer = [](const io::Project& project) {
            return std::any_of(project.chips.begin(), project.chips.end(), [](const editor::ChipData& chip) { return chip.name == editor::kCpuName; });
        };
        if (saved.project && isComputer(*saved.project)) {
            saved.project->hasUnsavedChanges = false;
            applyProject(std::move(*saved.project), std::nullopt);
        } else {
            static constexpr const char* kStarter =
                "; O seu programa. Escreva aqui e aperte Ctrl+Enter para montar e gravar na ROM.\n"
                "; Depois feche o editor e veja rodar no Painel, no Raio-X ou com Rodar.\n"
                "; As instruções estão na lista à direita. Um exemplo para começar:\n"
                "\n"
                "        LI   R1, 5\n"
                "        LI   R2, 3\n"
                "        ADD  R3, R1, R2\n"
                "        OUT  R3\n"
                "        HLT\n";
            const assembly::Result program = assembly::assemble(kStarter);
            editor::ComputerProject computer = editor::buildComputer16(program.words, kStarter);
            io::Project project;
            project.board = std::move(computer.board);
            project.chips = std::move(computer.chips);
            applyProject(std::move(project), std::nullopt);
            fit();
            autosavedRevision_ = UINT64_MAX;  // written at the next autosave, so it's there next time
        }
        programMode_ = true;
        program32_ = false;
        projectName_ = "Meu programa";
        view_ = app::kViewPanel;
        for (const editor::Part& part : session_.board().parts()) {
            if (part.kind == editor::PartKind::Box && part.box == editor::boxFromKey("rom") && part.width == editor::kWordBits) {
                session_.selectOnly(part.id);
                openCode();
                break;
            }
        }
    }

    // For the screenshot scripts: the diagram this much closer, around its middle.
    // The numbers on the diagrams in hexadecimal (or back in decimal), as the Dec | Hex switch does.
    void showHex(bool on) { zoom_.hex = on; }
    void zoomDiagram(float factor) {
        draw("");
        if (const std::optional<SDL_FRect> plan = diagramArea()) zoomAt(plan->x + plan->w / 2.0f, plan->y + plan->h / 2.0f, factor);
    }

    // For the screenshot scripts: the editor on its Blocks tab, scrolled this many blocks down.
    void showBlocks(int scroll) {
        if (!code_) return;
        code_->tab = app::kCodeTabBlocks;
        code_->blocksFirst = scroll;
    }

    // The 32-bit computer, the same way: in a file of its own, opened on its program.
    void openProgram32() {
        homeOpen_ = false;
        galleryOpen_ = false;
        leaveLesson();
        autosave();
        io::LoadedProject saved = io::loadProject(profiles_.program32Path());
        const auto isComputer = [](const io::Project& project) {
            return std::any_of(project.chips.begin(), project.chips.end(), [](const editor::ChipData& chip) { return chip.name == editor::kCpu32Name; });
        };
        if (saved.project && isComputer(*saved.project)) {
            saved.project->hasUnsavedChanges = false;
            applyProject(std::move(*saved.project), std::nullopt);
        } else {
            static constexpr const char* kStarter =
                "# O seu programa em MIPS. Escreva aqui e aperte Ctrl+Enter para montar e gravar na ROM.\n"
                "# Os dados vão depois de .data, as instruções depois de .text. Rode e veja no Painel.\n"
                "# A aba Comandos, à direita, tem as instruções; Ajuda explica cada uma.\n"
                "\n"
                "        .data\n"
                "vetor:  .word 1, 2, 3, 4, 5\n"
                "n:      .word 5\n"
                "\n"
                "        .text\n"
                "main:   la   $t0, vetor      # onde o vetor começa\n"
                "        lw   $t1, n          # quantos são\n"
                "        li   $t2, 0          # a soma\n"
                "laco:   beq  $t1, $zero, fim\n"
                "        lw   $t3, 0($t0)\n"
                "        add  $t2, $t2, $t3\n"
                "        addi $t0, $t0, 4     # a próxima palavra\n"
                "        addi $t1, $t1, -1\n"
                "        j    laco\n"
                "fim:    move $a0, $t2        # mostra a soma\n"
                "        li   $v0, 1\n"
                "        syscall\n"
                "        li   $v0, 10         # e termina\n"
                "        syscall\n";
            const mips::Program program = mips::assemble(i18n::tr(kStarter));
            editor::Computer32Project computer = editor::buildComputer32(program, kStarter);
            io::Project project;
            project.board = std::move(computer.board);
            project.chips = std::move(computer.chips);
            applyProject(std::move(project), std::nullopt);
            fit();
            autosavedRevision_ = UINT64_MAX;  // written at the next autosave, so it's there next time
        }
        programMode_ = true;
        program32_ = true;
        console_.clear();
        projectName_ = "Meu programa 32 bits";
        view_ = app::kViewPanel;
        for (const editor::Part& part : session_.board().parts()) {
            if (part.kind == editor::PartKind::Box && part.box == editor::boxFromKey("rom") && part.width == editor::kWord32Bits) {
                session_.selectOnly(part.id);
                openCode();
                break;
            }
        }
    }

    // The computer from the library, as a project of its own, ready to run.
    void openExample() {
        const assembly::Result program = assembly::assemble(editor::exampleProgram());
        editor::ComputerProject computer = editor::buildComputer16(program.words, editor::exampleProgram());
        io::Project project;
        project.name = "computador";
        project.board = std::move(computer.board);
        project.chips = std::move(computer.chips);
        applyProject(std::move(project), std::nullopt);
        tableScroll_ = 0;
        fit();
        showNote("Aperte Rodar. Para mudar o programa, selecione a ROM.", false);
    }

    // A programming lesson's code, in the editor: the ROM chosen, the way a click on it and Editar programa would.
    void openLessonProgram() {
        for (const editor::Part& part : session_.board().parts()) {
            if (part.kind == editor::PartKind::Box && part.box == editor::boxFromKey("rom") &&
                (part.width == editor::kWordBits || part.width == editor::kWord32Bits)) {
                session_.selectOnly(part.id);
                openCode();
                return;
            }
        }
    }

    void openCode() {
        const std::optional<editor::Inspected> shown = session_.inspected();
        if (!shown) return;
        const bool mips = shown->part.width == editor::kWord32Bits;
        if (shown->part.width != editor::kWordBits && !mips) {
            showNote("O programa precisa de uma ROM de 16 ou de 32 bits.", true);
            return;
        }
        std::string source = shown->part.source;
        if (source.empty() && shown->part.rom != nullptr) {
            const std::vector<std::uint64_t>& words = *shown->part.rom;
            for (std::size_t i = 0; i < words.size(); ++i) {
                source += mips ? "        " + mips::disassemble(static_cast<std::uint32_t>(words[i]), mips::kTextBase + static_cast<std::uint32_t>(4 * i)) + "\n"
                               : assembly::disassemble(static_cast<std::uint16_t>(words[i]), i) + "\n";
            }
        }
        code_.emplace();
        code_->mips = mips;
        code_->rom = shown->part.id;
        code_->text = editor::TextEdit(source);
        code_->original = code_->text.text();
        code_->capacity = std::size_t{1} << shown->part.address;
        code_->tab = lesson_ >= 0 && editor::lessons()[static_cast<std::size_t>(lesson_)].check == editor::LessonCheck::Program ? app::kCodeTabTask
                                                                                                                              : app::kCodeTabCommands;
        codeShown_ = true;
        checkCode();
        SDL_StartTextInput(window_);
    }

    void tick() {
        const std::string title = "bancada - " + projectName_ + (session_.dirty() ? "*" : "");
        if (title != title_) {
            title_ = title;
            SDL_SetWindowTitle(window_, title_.c_str());
        }
        if (SDL_GetTicks() >= nextAutosave_) {
            nextAutosave_ = SDL_GetTicks() + kAutosaveEveryMs;
            autosave();
        }
        const Uint64 now = SDL_GetTicksNS();
        session_.advance(lastTick_ == 0 ? 0.0 : static_cast<double>(now - lastTick_) / 1e9);
        lastTick_ = now;
        checkLessonNow();
    }

    // Writes only if something changed since the last one. During a lesson, to that lesson's own file.
    void autosave() {
        if (session_.revision() == autosavedRevision_) return;
        io::Project project = snapshot();
        if (programMode_ && lesson_ < 0) {
            if (io::saveProject(program32_ ? profiles_.program32Path() : profiles_.programPath(), project)) autosavedRevision_ = session_.revision();
            return;
        }
        if (lesson_ >= 0) {
            const fs::path path = lessonFile(editor::lessons()[static_cast<std::size_t>(lesson_)]);
            std::error_code error;
            fs::create_directories(path.parent_path(), error);
            if (io::saveProject(path, project)) autosavedRevision_ = session_.revision();
            return;
        }
        if (currentFile_) project.origin = io::pathToUtf8(*currentFile_);
        project.hasUnsavedChanges = session_.dirty();
        if (io::saveProject(profiles_.autosavePath(), project)) autosavedRevision_ = session_.revision();
    }

    void restoreAutosave() {
        io::LoadedProject loaded = io::loadProject(profiles_.autosavePath());
        if (!loaded.project) {
            startBlank();
            return;
        }
        std::optional<fs::path> file;
        if (loaded.project->origin) file = io::pathFromUtf8(*loaded.project->origin);
        applyProject(std::move(*loaded.project), std::move(file));
    }

    bool openFile(const fs::path& path) {
        io::LoadedProject loaded = io::loadProject(path);
        if (!loaded.project) {
            showNote(io::describe(*loaded.error), true);
            return false;
        }
        loaded.project->hasUnsavedChanges = false;
        loaded.project->name = io::pathToUtf8(path.stem());
        applyProject(std::move(*loaded.project), path);
        tableScroll_ = 0;
        showNote("Projeto aberto.", false);
        return true;
    }

    bool saveTo(const fs::path& path) {
        projectName_ = io::pathToUtf8(path.stem());
        if (!io::saveProject(path, snapshot())) {
            showNote("Não deu para salvar nesse lugar.", true);
            return false;
        }
        currentFile_ = path;
        session_.markSaved();
        showNote("Salvo.", false);
        return true;
    }

private:
    static void postChoice(Uint32 type, const char* const* files) {
        if (files == nullptr || files[0] == nullptr) return;  // cancelled, or the dialog failed
        SDL_Event event{};
        event.type = type;
        event.user.data1 = SDL_strdup(files[0]);
        if (!SDL_PushEvent(&event)) SDL_free(event.user.data1);
    }

    // File dialogs answer from another thread, so these only pass the answer to the main loop.
    static void SDLCALL onOpenChosen(void* self, const char* const* files, int) {
        postChoice(static_cast<App*>(self)->openEvent_, files);
    }
    static void SDLCALL onSaveChosen(void* self, const char* const* files, int) {
        postChoice(static_cast<App*>(self)->saveEvent_, files);
    }
    static void SDLCALL onCodeOpenChosen(void* self, const char* const* files, int) {
        postChoice(static_cast<App*>(self)->codeOpenEvent_, files);
    }
    static void SDLCALL onCodeSaveChosen(void* self, const char* const* files, int) {
        postChoice(static_cast<App*>(self)->codeSaveEvent_, files);
    }

    // The program as a text file, to keep, send or open in another editor, and back. SDL reads and writes it,
    // so that on Android the file the system's picker gives back can be used too.
    void chooseCodeFile(bool saving) {
        if (!code_) return;
        static constexpr SDL_DialogFileFilter kAssembly[] = {{"Assembly", "asm;txt"}};
        static constexpr SDL_DialogFileFilter kMips[] = {{"Assembly MIPS", "s;asm;txt"}};
        const SDL_DialogFileFilter* filters = code_->mips ? kMips : kAssembly;
        dialogFolder_ = io::pathToUtf8(profiles_.projectsFolder());
        if (saving) {
            SDL_ShowSaveFileDialog(onCodeSaveChosen, this, window_, filters, 1, dialogFolder_.c_str());
        } else {
            SDL_ShowOpenFileDialog(onCodeOpenChosen, this, window_, filters, 1, dialogFolder_.c_str(), false);
        }
    }
    void openCodeFile(const std::string& path) {
        if (!code_) return;
        std::size_t size = 0;
        void* data = SDL_LoadFile(path.c_str(), &size);
        if (data == nullptr) {
            code_->warning = "Não deu para abrir esse arquivo.";
            return;
        }
        std::string text(static_cast<const char*>(data), size);
        SDL_free(data);
        if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);  // the mark some editors put first
        std::erase(text, '\r');
        code_->text.selectAll();
        if (!code_->text.insert(text)) code_->warning = kCodeTooBig;
        codeEdited();
    }
    void saveCodeFile(std::string path) {
        if (!code_) return;
        const std::string ending = code_->mips ? ".s" : ".asm";
        const std::size_t slash = path.find_last_of("/\\");
        if (path.find('.', slash == std::string::npos ? 0 : slash + 1) == std::string::npos) path += ending;
        const std::string text = code_->text.text();
        if (SDL_SaveFile(path.c_str(), text.data(), text.size())) {
            showNote("Programa salvo.", false);
        } else {
            code_->warning = "Não deu para salvar o arquivo.";
        }
    }

    void finishDialog(const SDL_Event& event) {
        if (event.type == codeOpenEvent_ || event.type == codeSaveEvent_) {
            auto* chosen = static_cast<char*>(event.user.data1);
            const std::string path(chosen);
            SDL_free(chosen);
            if (event.type == codeOpenEvent_) {
                openCodeFile(path);
            } else {
                saveCodeFile(path);
            }
            return;
        }
        auto* text = static_cast<char*>(event.user.data1);
        fs::path path = io::pathFromUtf8(text);
        SDL_free(text);
        if (event.type == openEvent_) {
            openFile(path);
        } else {
            if (path.extension() != ".bancada") path += ".bancada";
            saveTo(path);
        }
    }

    void chooseFileToOpen() {
        dialogFolder_ = io::pathToUtf8(profiles_.projectsFolder());
        SDL_ShowOpenFileDialog(onOpenChosen, this, window_, kFilters, 1, dialogFolder_.c_str(), false);
    }

    void chooseFileToSave() {
        dialogFolder_ = io::pathToUtf8(profiles_.projectsFolder());
        SDL_ShowSaveFileDialog(onSaveChosen, this, window_, kFilters, 1, dialogFolder_.c_str());
    }

    io::Project snapshot() const {
        io::Project project;
        project.name = projectName_;
        editor::ChipData main = session_.mainCircuit();
        project.board = std::move(main.board);
        project.chips = session_.projectChips();
        for (const auto& [id, value] : main.toggles) {
            if (value == 0 || project.board.part(id) == nullptr) continue;
            if (project.board.part(id)->width == 1) {
                project.togglesOn.push_back(id);
            } else {
                project.toggleValues.emplace_back(id, value);
            }
        }
        std::sort(project.togglesOn.begin(), project.togglesOn.end());
        std::sort(project.toggleValues.begin(), project.toggleValues.end());
        project.view = {camera_.originX, camera_.originY, camera_.cellPixels};
        return project;
    }

    void applyProject(io::Project project, std::optional<fs::path> file) {
        if (code_) closeCode();
        example_ = -1;  // anything opened replaces the example; opening an example sets it again after
        session_.adoptProjectChips(std::move(project.chips));
        session_.adopt(std::move(project.board), project.togglesOn, !project.hasUnsavedChanges);
        session_.setToggleValues(project.toggleValues);
        syncPalette();
        projectName_ = std::move(project.name);
        currentFile_ = std::move(file);
        camera_.originX = project.view.x;
        camera_.originY = project.view.y;
        camera_.cellPixels = std::clamp(project.view.cellPixels, 0.25f, 64.0f);
        autosavedRevision_ = session_.revision();
    }

    void startBlank() {
        example_ = -1;
        session_.adoptProjectChips({});
        session_.adopt(editor::Board{});
        syncPalette();
        projectName_ = "sem nome";
        currentFile_.reset();
        resetView();
        autosavedRevision_ = session_.revision();
    }

    void resetView() {
        camera_ = Camera{};
        camera_.originX = -14;
        camera_.originY = -6;
    }

    // A first request warns; repeating it soon after means the player really wants to discard.
    bool mayDiscard() {
        if (!session_.dirty() || SDL_GetTicks() < confirmUntil_) {
            confirmUntil_ = 0;
            return true;
        }
        confirmUntil_ = SDL_GetTicks() + kNoteMs;
        showNote("Há alterações não salvas. Repita o comando para descartá-las, ou salve com Ctrl+S.", true);
        return false;
    }

    void showNote(const char* text, bool isError) {
        note_ = text;
        noteIsError_ = isError;
        noteUntil_ = SDL_GetTicks() + kNoteMs;
    }

    app::ProfileList profileList() const {
        app::ProfileList list;
        for (std::size_t i = 0; i < profiles_.profiles().size(); ++i) {
            list.names.push_back(profiles_.profiles()[i].name);
            if (profiles_.profiles()[i].id == profiles_.current().id) list.current = i;
        }
        list.typing = typing_;
        return list;
    }

    void openProfiles() {
        session_.key(editor::Key::Escape);
        profilesOpen_ = true;
        typing_.clear();
        SDL_StartTextInput(window_);
    }

    void closeProfiles() {
        profilesOpen_ = false;
        SDL_StopTextInput(window_);
    }

    void chooseProfile(const std::string& id) {
        closeProfiles();
        if (id == profiles_.current().id) return;
        leaveLesson();  // a lesson's progress belongs to the profile it was made in
        autosave();
        profiles_.select(id);
        restoreAutosave();
    }

    void createProfile() {
        if (typing_.empty()) return;
        leaveLesson();
        autosave();
        if (profiles_.create(typing_)) {
            startBlank();
        } else {
            showNote("Não deu para criar o perfil.", true);
        }
        closeProfiles();
    }

    void sendCursor() {
        const Vec2 at = camera_.toBoard(mouseX_, mouseY_);
        const bool straight = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
        session_.mouseMove(at.x, at.y, straight);
    }

    void mouseMoved(float x, float y) {
        if (!peeks_.empty()) {
            if (peekDragging_) peeks_.back().camera.pan(x - mouseX_, y - mouseY_);
            mouseX_ = x;
            mouseY_ = y;
            return;
        }
        if (insidePlan_ != nullptr) {
            if (insideDragging_) {
                insideView_.originX += x - mouseX_;
                insideView_.originY += y - mouseY_;
            }
            mouseX_ = x;
            mouseY_ = y;
            return;
        }
        if (codeVisible()) {
            mouseX_ = x;
            mouseY_ = y;
            if (code_->selecting) placeCodeCursor(x, y - app::kToolbarHeight, true);
            return;
        }
        if (panning_) camera_.pan(x - mouseX_, y - mouseY_);
        if (zoomDragging_) {
            zoom_.panX += x - mouseX_;
            zoom_.panY += y - mouseY_;
        }
        mouseX_ = x;
        mouseY_ = y;
        if (!profilesOpen_) sendCursor();
    }

    float rightEdgeWidth() const {
        return tableVisible_ && view_ == app::kViewBoard && !quizLesson() && !codeVisible() ? app::rightPanelWidth(session_) : 0.0f;
    }

    // A lesson of questions: no parts, no table, just the questions.
    bool quizLesson() const { return lesson_ >= 0 && editor::lessons()[static_cast<std::size_t>(lesson_)].check == editor::LessonCheck::Answer; }

    bool overRightPanel(float x) const { return x >= static_cast<float>(width_) - rightEdgeWidth(); }

    bool overBoard(float x, float y) const {
        return x >= app::kPanelWidth && !overRightPanel(x) && y >= boardTop() && y < static_cast<float>(height_) - app::kStatusHeight;
    }

    bool overLessonStrip(float x, float y) const {
        return (lesson_ >= 0 || example_ >= 0) && view_ == app::kViewBoard && x >= app::kPanelWidth && x < boardRightEdge() && y >= app::kToolbarHeight &&
               y < boardTop();
    }

    bool electricValue() const {
        const std::optional<editor::Inspected> shown = session_.inspected();
        return shown && editor::valueQuantity(shown->part.kind).has_value();
    }

    bool modal() const {
        return profilesOpen_ || helpOpen_ || prompt_ != Prompt::None || codeVisible() || homeOpen_ || lessonsOpen_ || galleryOpen_ ||
               topic_ >= 0 || insidePlan_ != nullptr || !peeks_.empty();
    }

    app::CodeView codeView() const {
        app::CodeView view;
        view.text = &code_->text;
        view.problems = &code_->problems;
        view.words = code_->mips ? code_->words32.size() : code_->words.size();
        view.mips = code_->mips;
        view.program32 = code_->words32;
        view.capacity = code_->capacity;
        view.scrollLine = code_->scrollLine;
        view.scrollColumn = code_->scrollColumn;
        view.warning = code_->warning.c_str();
        view.help = code_->help;
        view.helpScroll = code_->helpScroll;
        view.tab = code_->tab;
        view.blocksFirst = code_->blocksFirst;
        view.program = code_->words;
        view.lines = code_->lines;
        if (const std::optional<editor::ComputerState> computer = editor::readComputer(session_.board(), session_.circuit(), session_.chipBoards())) {
            if (computer->source == code_->text.text() && computer->pc < code_->lines.size()) {
                view.runningAddress = computer->pc;
                view.runningLine = code_->lines[computer->pc] - 1;
            }
        } else if (const std::optional<editor::Computer32State> computer32 =
                       editor::readComputer32(session_.board(), session_.circuit(), session_.chipBoards())) {
            if (computer32->source == code_->text.text() && computer32->index < code_->lines.size()) {
                view.runningAddress = static_cast<int>(computer32->index);
                view.runningLine = code_->lines[computer32->index] - 1;
            }
        }
        if (lesson_ >= 0 && editor::lessons()[static_cast<std::size_t>(lesson_)].check == editor::LessonCheck::Program) {
            const app::LessonStrip strip = lessonStrip();
            view.lesson = lesson_;
            view.lessonMessage = strip.message;
            view.lessonPassed = strip.passed;
            view.lessonProblem = strip.problem;
        }
        return view;
    }

    // The Blocks tab follows the program: when another instruction starts running, the list moves to show it,
    // a third of the way down. In between, the wheel moves it freely.
    void followBlocks() {
        const int running = codeView().runningAddress;
        if (running == code_->blocksFollowed) return;
        code_->blocksFollowed = running;
        if (running < 0) return;
        const int fit = app::codeBlocksVisible(width_, codeHeight(), code_->mips);
        if (running >= code_->blocksFirst && running < code_->blocksFirst + fit - 1) return;
        const int count = static_cast<int>(code_->mips ? code_->words32.size() : code_->words.size());
        code_->blocksFirst = std::clamp(running - fit / 3, 0, std::max(0, count - fit));
    }

    // The program view: the draft kept from before, or the ROM's program opened.
    void showProgram() {
        if (code_) {
            codeShown_ = true;
            SDL_StartTextInput(window_);
            return;
        }
        openLessonProgram();
    }

    // Over the editor the toolbar keeps working: running, stepping, the speed, and the other views.
    void pressToolbarOverCode(float x, float y) {
        const auto hit = [&](const SDL_FRect& r) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; };
        if (computerHere_) {
            for (int which = 0; which < app::kViewCount; ++which) {
                if (hit(app::viewSwitchRect(boardRightEdge(), which))) {
                    codeShown_ = false;  // the draft stays, for the program button to bring back
                    SDL_StopTextInput(window_);
                    view_ = which;
                    xrayPc_ = UINT32_MAX;
                    return;
                }
            }
        }
        const int tool = app::toolbarAt(x, y);
        if (tool >= 0 && app::kToolbar[tool].sim != app::SimButton::None) pressSimButton(app::kToolbar[tool].sim);
    }

    void closeCode() {
        codeShown_ = false;
        code_.reset();
        SDL_StopTextInput(window_);
    }

    void checkCode() {
        CodeEditing& code = *code_;
        if (code.mips) {
            mips::Program program = mips::assemble(code.text.text());
            code.words32 = std::move(program.text);
            code.data32 = std::move(program.data);
            code.lines = std::move(program.lines);
            code.problems = std::move(program.errors);
            if (code.problems.empty() && code.words32.size() > code.capacity) {
                code.problems.push_back({0, i18n::format("O programa tem {} palavras, e esta ROM guarda {}.",
                                                         {std::to_string(code.words32.size()), std::to_string(code.capacity)})});
            }
            if (code.problems.empty() && code.data32.size() > mips::kMemoryWords) {
                code.problems.push_back({0, i18n::format("O .data tem {} palavras, e a RAM guarda {}.",
                                                         {std::to_string(code.data32.size()), std::to_string(mips::kMemoryWords)})});
            }
            return;
        }
        assembly::Result result = assembly::assemble(code.text.text());
        code.words = std::move(result.words);
        code.lines = std::move(result.lines);
        code.problems = std::move(result.errors);
        if (code.problems.empty() && code.words.size() > code.capacity) {
            code.problems.push_back(
                {0, i18n::format("O programa tem {} palavras, e esta ROM guarda {}.", {std::to_string(code.words.size()), std::to_string(code.capacity)})});
        }
    }

    void showCodeCursor() {
        CodeEditing& code = *code_;
        const app::CodeLayout layout = app::codeLayout(width_, codeHeight());
        const editor::TextPosition cursor = code.text.cursor();
        code.scrollLine = std::clamp(code.scrollLine, cursor.line - layout.lines + 1, cursor.line);
        const int column = code.text.displayColumn(cursor);
        code.scrollColumn = std::clamp(code.scrollColumn, column - layout.columns + 1, column);
    }

    // Checked on every change, the same way the truth table follows the board.
    void codeEdited() {
        checkCode();
        showCodeCursor();
    }

    void assembleCode() {
        checkCode();
        CodeEditing& code = *code_;
        if (!code.problems.empty()) {
            goToProblem(0);
            return;
        }
        if (code.mips) {
            const std::optional<editor::Computer32State> computer = editor::readComputer32(session_.board(), session_.circuit(), session_.chipBoards());
            const std::vector<std::uint64_t> words(code.words32.begin(), code.words32.end());
            const std::vector<std::uint64_t> data(code.data32.begin(), code.data32.end());
            session_.setMemories(code.rom, words, code.text.text(), computer ? computer->ram : 0, data);
            console_.clear();
            code.original = code.text.text();
            code.warning = i18n::format("Programa gravado: {} instruções e {} palavras de dados.", {std::to_string(words.size()), std::to_string(data.size())});
            return;
        }
        const std::vector<std::uint64_t> words(code.words.begin(), code.words.end());
        const std::string text = i18n::format("Programa gravado: {} palavras.", {std::to_string(words.size())});
        session_.setRomWords(words, code.text.text());
        code.original = code.text.text();
        code.warning = text;
    }

    // A draft nobody means to lose: leaving with changes takes a second request.
    void cancelCode() {
        if (code_->text.text() != code_->original && SDL_GetTicks() >= confirmUntil_) {
            confirmUntil_ = SDL_GetTicks() + kNoteMs;
            code_->warning = "Há mudanças que não foram gravadas. Repita para descartá-las.";
            return;
        }
        confirmUntil_ = 0;
        closeCode();
    }

    void goToProblem(std::size_t index) {
        const int line = code_->problems[index].line;
        if (line > 0) code_->text.place(line - 1, 0);
        showCodeCursor();
    }

    void placeCodeCursor(float x, float y, bool extend) {
        CodeEditing& code = *code_;
        const app::CodeLayout layout = app::codeLayout(width_, codeHeight());
        const int line = code.scrollLine + static_cast<int>(std::floor((y - layout.text.y) / app::kCodeLineHeight));
        const int column = code.scrollColumn + static_cast<int>(std::lround((x - layout.text.x) / app::kCodeCharWidth));
        code.text.place(line, column, extend);
        showCodeCursor();
    }

    static bool inside(const SDL_FRect& rect, float x, float y) {
        return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
    }

    void pressCode(float x, float y) {
        const app::CodeLayout layout = app::codeLayout(width_, codeHeight());
        if (const int tab = app::codeTabAt(codeView(), x, y, width_, codeHeight()); tab >= 0) {
            code_->tab = tab;
            code_->help = false;
            return;
        }
        if (inside(layout.help, x, y)) {
            code_->help = !code_->help;
            code_->helpScroll = 0.0f;
            return;
        }
        if (code_->help) {
            const float to = app::codeHelpGroupAt(renderer_, width_, codeHeight(), code_->mips, x, y);
            if (to >= 0.0f) code_->helpScroll = to;
            return;
        }
        if (inside(layout.assemble, x, y)) {
            assembleCode();
        } else if (inside(layout.open, x, y)) {
            chooseCodeFile(false);
        } else if (inside(layout.save, x, y)) {
            chooseCodeFile(true);
        } else if (inside(layout.close, x, y)) {
            cancelCode();
        } else if (inside(layout.text, x, y)) {
            placeCodeCursor(x, y, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
            code_->selecting = true;
        } else if (inside(layout.problems, x, y)) {
            const int problem = app::codeProblemAt(codeView(), y, width_, codeHeight());
            if (problem >= 0) goToProblem(static_cast<std::size_t>(problem));
        }
    }

    void scrollCode(float amount) {
        CodeEditing& code = *code_;
        if (code.tab == app::kCodeTabBlocks && !code.help && app::overCodeBlocks(width_, codeHeight(), mouseX_, mouseY_ - app::kToolbarHeight)) {
            const int count = static_cast<int>(code.mips ? code.words32.size() : code.words.size());
            const int fit = app::codeBlocksVisible(width_, codeHeight(), code.mips);
            code.blocksFirst = std::clamp(code.blocksFirst - static_cast<int>(amount), 0, std::max(0, count - fit));
            return;
        }
        if (code.help) {
            code.helpScroll = std::clamp(code.helpScroll - amount * 60.0f, 0.0f, app::codeHelpMaxScroll(renderer_, width_, codeHeight(), code.mips));
            return;
        }
        const int limit = std::max(0, code.text.lineCount() - 1);
        code.scrollLine = std::clamp(code.scrollLine - static_cast<int>(amount) * 3, 0, limit);
    }

    void codeKey(SDL_Keycode key, SDL_Keymod mod) {
        const bool ctrl = (mod & SDL_KMOD_CTRL) != 0;
        const bool shift = (mod & SDL_KMOD_SHIFT) != 0;
        editor::TextEdit& text = code_->text;
        code_->warning.clear();
        const int page = std::max(1, app::codeLayout(width_, codeHeight()).lines - 1);
        switch (key) {
            case SDLK_ESCAPE:
                if (code_->help) {
                    code_->help = false;
                    return;
                }
                cancelCode();
                return;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (ctrl) {
                    assembleCode();
                    return;
                }
                text.newline();
                break;
            case SDLK_BACKSPACE: text.backspace(); break;
            case SDLK_DELETE: text.deleteForward(); break;
            case SDLK_TAB: text.insert("    "); break;
            case SDLK_LEFT: text.moveLeft(shift); break;
            case SDLK_RIGHT: text.moveRight(shift); break;
            case SDLK_UP: text.moveVertically(-1, shift); break;
            case SDLK_DOWN: text.moveVertically(1, shift); break;
            case SDLK_PAGEUP: text.moveVertically(-page, shift); break;
            case SDLK_PAGEDOWN: text.moveVertically(page, shift); break;
            case SDLK_HOME:
                if (ctrl) {
                    text.moveToStart(shift);
                } else {
                    text.moveToLineStart(shift);
                }
                break;
            case SDLK_END:
                if (ctrl) {
                    text.moveToEnd(shift);
                } else {
                    text.moveToLineEnd(shift);
                }
                break;
            default:
                if (!ctrl) return;
                if (key == SDLK_A) text.selectAll();
                if (key == SDLK_Z && shift) text.redo();
                if (key == SDLK_Z && !shift) text.undo();
                if (key == SDLK_Y) text.redo();
                if ((key == SDLK_C || key == SDLK_X) && text.hasSelection()) SDL_SetClipboardText(text.selectedText().c_str());
                if (key == SDLK_X) text.deleteSelection();
                if (key == SDLK_V && SDL_HasClipboardText()) {
                    char* pasted = SDL_GetClipboardText();
                    if (pasted != nullptr && !text.insert(pasted)) code_->warning = kCodeTooBig;
                    SDL_free(pasted);
                }
                break;
        }
        codeEdited();
    }

    void syncPalette() {
        const int groups = static_cast<int>(libraryGroups().size());
        palette_.folder = std::clamp(palette_.folder, 0, groups - 1);
        clampPalette();
    }

    void clampPalette() {
        const std::vector<app::PaletteGroup> groups = libraryGroups(palette_.filter);
        const float room = static_cast<float>(height_) - app::paletteListTop(groups) - 46.0f;
        const float limit = std::max(0.0f, app::paletteContentHeight(groups, palette_) - room);
        palette_.scroll = std::clamp(palette_.scroll, 0.0f, limit);
    }

    // The diagram the mouse is over, if a computer's view shows one: the X-ray's, or the 16-bit panel's.
    std::optional<SDL_FRect> diagramArea() const {
        if (!computerHere_ || view_ == app::kViewBoard || modal()) return std::nullopt;
        if (view_ == app::kViewXray) return app::xrayParts(viewArea()).plan;
        if (!is32_) return app::panelPlanArea(viewArea());
        return std::nullopt;
    }

    // Closer or farther, keeping the point under the mouse where it is.
    void zoomAt(float x, float y, float factor) {
        const std::optional<SDL_FRect> plan = diagramArea();
        if (!plan) return;
        const float next = std::clamp(zoom_.zoom * factor, 0.5f, 5.0f);
        const float k = next / zoom_.zoom;
        const float cx = plan->x + plan->w / 2.0f;
        const float cy = plan->y + plan->h / 2.0f;
        zoom_.panX = (x - cx) - (x - cx - zoom_.panX) * k;
        zoom_.panY = (y - cy) - (y - cy - zoom_.panY) * k;
        zoom_.zoom = next;
    }

    void wheel(float amount) {
        if (!peeks_.empty()) {
            const SDL_FRect area = app::peekBoardArea(width_, height_);
            peeks_.back().camera.zoomAt(mouseX_ - area.x, mouseY_ - area.y, std::pow(1.25f, amount));
            return;
        }
        if (insidePlan_ != nullptr) {
            zoomInside(mouseX_, mouseY_, std::pow(1.25f, amount));
            return;
        }
        if (const std::optional<SDL_FRect> plan = diagramArea();
            plan && mouseX_ >= plan->x && mouseX_ < plan->x + plan->w && mouseY_ >= plan->y && mouseY_ < plan->y + plan->h) {
            zoomAt(mouseX_, mouseY_, amount > 0 ? 1.15f : 1.0f / 1.15f);
            return;
        }
        if (codeVisible()) {
            if (mouseY_ >= app::kToolbarHeight) scrollCode(amount);
            return;
        }
        if (lessonsOpen_) {
            lessonScroll_ = std::clamp(lessonScroll_ - amount * 60.0f, 0.0f, app::lessonListMaxScroll(width_, height_, lessonListView()));
            return;
        }
        if (topic_ >= 0) {
            readingScroll_ = std::clamp(readingScroll_ - amount * 70.0f, 0.0f, app::readingMaxScroll(renderer_, width_, height_, readingView()));
            return;
        }
        if (modal()) return;
        if (mouseX_ < app::kPanelWidth) {
            palette_.scroll -= amount * 40.0f;
            clampPalette();
        } else if (overRightPanel(mouseX_)) {
            const int rows = static_cast<int>(session_.truthTable().rows.size());
            const int limit = std::max(0, rows - app::truthRowsVisible(session_, height_));
            tableScroll_ = std::clamp(tableScroll_ - static_cast<int>(amount) * 3, 0, limit);
        } else {
            camera_.zoomAt(mouseX_, mouseY_, amount > 0 ? 1.15f : 1.0f / 1.15f);
            sendCursor();
        }
    }

    void pressRightPanel(float x, float y) {
        switch (app::rightPanelAt(session_, x, y, width_)) {
            case app::PanelButton::InputsDown: session_.changeInputs(-1); break;
            case app::PanelButton::InputsUp: session_.changeInputs(1); break;
            case app::PanelButton::BitsDown: session_.changeBits(-1); break;
            case app::PanelButton::BitsUp: session_.changeBits(1); break;
            case app::PanelButton::AddressDown: session_.changeAddress(-1); break;
            case app::PanelButton::AddressUp: session_.changeAddress(1); break;
            case app::PanelButton::SizeDown: session_.changeScale(-1); break;
            case app::PanelButton::SizeUp: session_.changeScale(1); break;
            case app::PanelButton::EditName: openPrompt(Prompt::Name); break;
            case app::PanelButton::EditValue: openPrompt(Prompt::Value); break;
            case app::PanelButton::EditContents: openPrompt(Prompt::Contents); break;
            case app::PanelButton::EditProgram: openCode(); break;
            case app::PanelButton::OpenChip: {
                const std::optional<editor::Inspected> shown = session_.inspected();
                if (shown) session_.openCircuit(shown->part.chip);
                break;
            }
            case app::PanelButton::LookInside: {
                const std::optional<editor::Inspected> shown = session_.inspected();
                if (shown) lookInside(shown->part.id);
                break;
            }
            default: break;
        }
    }

    const char* promptTitle() const {
        switch (prompt_) {
            case Prompt::Name: return "Nome";
            case Prompt::Value: {
                if (!electricValue()) return "Valor, em hexadecimal";
                switch (*editor::valueQuantity(session_.inspected()->part.kind)) {
                    case electric::Quantity::Ohms: return "Resistência (por exemplo: 330, 4k7, 10k)";
                    case electric::Quantity::Volts: return "Tensão (por exemplo: 5, 9, 3,3)";
                    case electric::Quantity::Farads: return "Capacitância (por exemplo: 10u, 100n)";
                }
                return "Valor";
            }
            case Prompt::Contents: return "Conteúdo: palavras em hexadecimal, separadas por espaco";
            case Prompt::NewChip: return "Nome do novo chip";
            case Prompt::DevPassword:
            case Prompt::InsidePassword: return "Senha de desenvolvedor";
            default: return "";
        }
    }

    std::size_t maxTyped() const { return prompt_ == Prompt::Contents ? 65536 : prompt_ == Prompt::None ? kMaxNameBytes : 32; }

    void openPrompt(Prompt prompt) {
        prompt_ = prompt;
        typing_.clear();
        if (const std::optional<editor::Inspected> shown = session_.inspected()) {
            char text[32];
            if (prompt == Prompt::Name) typing_ = shown->part.label;
            if (prompt == Prompt::Value) {
                if (const std::optional<electric::Quantity> quantity = editor::valueQuantity(shown->part.kind)) {
                    // Only the number is typed: the unit stands after it, out of reach of Backspace.
                    typing_ = electric::formatNumber(shown->part.value, *quantity);
                } else {
                    std::snprintf(text, sizeof text, "%llX", static_cast<unsigned long long>(shown->part.value));
                    typing_ = text;
                }
            }
            if (prompt == Prompt::Contents && shown->part.rom != nullptr) {
                for (std::uint64_t word : *shown->part.rom) {
                    std::snprintf(text, sizeof text, "%llX ", static_cast<unsigned long long>(word));
                    typing_ += text;
                }
            }
        }
        SDL_StartTextInput(window_);
    }

    void closePrompt() {
        prompt_ = Prompt::None;
        typing_.clear();
        SDL_StopTextInput(window_);
    }

    void confirmPrompt() {
        const std::string text = typing_;
        const Prompt prompt = prompt_;
        closePrompt();
        if (prompt == Prompt::DevPassword) {
            if (text == kDevPassword) {
                devUnlocked_ = true;
            } else {
                devDeniedUntil_ = SDL_GetTicks() + kNoteMs;
            }
            return;
        }
        if (prompt == Prompt::InsidePassword) {
            app::showInsideOnMenu(text == kDevPassword);  // for this run only, like the lessons' switch
            return;
        }
        if (prompt == Prompt::NewChip) {
            if (!text.empty()) {
                session_.createChip(text);
                syncPalette();
            }
            return;
        }
        if (prompt == Prompt::Name) session_.setLabel(text);
        if (prompt == Prompt::Value && electricValue()) {
            const std::optional<electric::Quantity> quantity = editor::valueQuantity(session_.inspected()->part.kind);
            const std::optional<std::uint64_t> value = electric::parseQuantity(text, *quantity);
            if (value) {
                session_.setValue(*value);
            } else {
                showNote("Esse valor não serve aqui. Exemplos: 4k7, 220, 10u, 9V.", true);
            }
            return;
        }
        if (prompt == Prompt::Value) {
            const std::optional<std::uint64_t> value = parseHex(text);
            if (value) {
                session_.setValue(*value);
            } else {
                showNote("Isso não é um número em hexadecimal.", true);
            }
        }
        if (prompt == Prompt::Contents) {
            std::vector<std::uint64_t> words;
            std::size_t at = 0;
            while (at < text.size()) {
                const std::size_t start = text.find_first_not_of(" ,\n\t", at);
                if (start == std::string::npos) break;
                std::size_t end = text.find_first_of(" ,\n\t", start);
                if (end == std::string::npos) end = text.size();
                const std::optional<std::uint64_t> word = parseHex(text.substr(start, end - start));
                if (!word) {
                    showNote("Há algo ali que não é hexadecimal.", true);
                    return;
                }
                words.push_back(*word);
                at = end;
            }
            session_.setRomWords(std::move(words));
        }
    }

    void pressSimButton(app::SimButton button) {
        switch (button) {
            case app::SimButton::RunPause: toggleRun(); break;
            case app::SimButton::Step: session_.stepClock(); break;
            case app::SimButton::Home:
                // Nothing is lost: the circuit is saved, and Continuar brings it back.
                session_.key(editor::Key::Escape);
                autosave();
                homeOpen_ = true;
                break;
            case app::SimButton::Reset: resetComputer(); break;
            case app::SimButton::Save:
                session_.key(editor::Key::Escape);
                if (currentFile_) {
                    saveTo(*currentFile_);
                } else {
                    chooseFileToSave();
                }
                break;
            case app::SimButton::Open:
                if (mayDiscard()) chooseFileToOpen();
                break;
            case app::SimButton::Slower: session_.changeSpeed(-1); break;
            case app::SimButton::Faster: session_.changeSpeed(1); break;
            default: break;
        }
    }

    void mouseDown(Uint8 button, float x, float y) {
        mouseX_ = x;
        mouseY_ = y;
        if (!peeks_.empty()) {
            pressPeek(button, x, y);
            return;
        }
        if (insidePlan_ != nullptr) {
            pressInside(button, x, y);
            return;
        }
        if (bundle_ != 0) {
            const SDL_FRect back = app::bundleBackRect(width_, height_);
            if (button == SDL_BUTTON_LEFT && x >= back.x && x < back.x + back.w && y >= back.y && y < back.y + back.h) bundle_ = 0;
            return;
        }
        if (codeVisible()) {
            if (button != SDL_BUTTON_LEFT) return;
            if (y >= app::kToolbarHeight) {
                pressCode(x, y - app::kToolbarHeight);
            } else {
                pressToolbarOverCode(x, y);
            }
            return;
        }
        if (button == SDL_BUTTON_LEFT && computerHere_ && y < app::kToolbarHeight) {
            const SDL_FRect r = app::programSwitchRect(boardRightEdge());
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
                showProgram();
                return;
            }
        }
        if (homeOpen_) {
            if (prompt_ != Prompt::None) return;  // the password first, or Esc
            if (button == SDL_BUTTON_LEFT) chooseHome(app::homeAt(x, y, width_, height_, homeComputers_));
            return;
        }
        if (galleryOpen_) {
            if (button != SDL_BUTTON_LEFT) return;
            const int chosen = app::galleryAt(x, y, width_, height_, galleryGroup_);
            if (chosen == app::kGalleryBack) {
                galleryOpen_ = false;
                homeOpen_ = true;
            } else if (chosen >= app::kGalleryGroupFirst && chosen < app::kGalleryGroupFirst + 100) {
                galleryGroup_ = chosen - app::kGalleryGroupFirst;
            } else if (chosen >= 0) {
                openGalleryItem(chosen);
            }
            return;
        }
        if (lessonsOpen_) {
            if (button != SDL_BUTTON_LEFT) return;
            const app::LessonListHit hit = app::lessonListAt(x, y, width_, height_, lessonListView());
            switch (hit.kind) {
                case app::LessonListKind::Back:
                    lessonsOpen_ = false;
                    homeOpen_ = true;
                    break;
                case app::LessonListKind::Reset: resetLessons(); break;
                case app::LessonListKind::Unlock:
                    // Opening asks for the password; closing again doesn't.
                    if (devUnlocked_) {
                        devUnlocked_ = false;
                    } else {
                        openPrompt(Prompt::DevPassword);
                    }
                    break;
                case app::LessonListKind::Act:
                    lessonAct_ = hit.index;
                    lessonScroll_ = 0.0f;
                    break;
                case app::LessonListKind::Reading: openReading(hit.index); break;
                case app::LessonListKind::Exercise: startLesson(hit.index); break;
                case app::LessonListKind::None:
                case app::LessonListKind::Planned: break;
            }
            return;
        }
        if (topic_ >= 0) {
            if (button != SDL_BUTTON_LEFT) return;
            const app::ReadingHit hit = app::readingAt(renderer_, x, y, width_, height_, readingView());
            switch (hit.button) {
                case app::ReadingButton::Back: showLessons(); break;
                case app::ReadingButton::Exercises: startReadingExercises(); break;
                case app::ReadingButton::Previous: showReadingPage(readingPage_ - 1); break;
                case app::ReadingButton::Next: showReadingPage(readingPage_ + 1); break;
                case app::ReadingButton::Option: {
                    std::vector<int>& answers = readingAnswers_[topic_];
                    answers.resize(editor::topics()[static_cast<std::size_t>(topic_)].blocks.size(), -1);
                    answers[static_cast<std::size_t>(hit.block)] = hit.option;
                    break;
                }
                case app::ReadingButton::Figure: app::pressReadingFigure(renderer_, x, y, width_, readingView(), hit.block); break;
                case app::ReadingButton::None: break;
            }
            return;
        }
        if (quizActive() && !overLessonStrip(x, y) && overBoard(x, y)) {
            if (button == SDL_BUTTON_LEFT) {
                const editor::Lesson& asked = editor::lessons()[static_cast<std::size_t>(lesson_)];
                const editor::Question* question = quizIndex_ < asked.questions.size() ? &asked.questions[quizIndex_] : nullptr;
                const SDL_FRect r = app::quizButtonRect(app::quizArea(boardRightEdge(), height_), question);
                if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) submitQuiz();
            }
            return;
        }
        if (overLessonStrip(x, y)) {
            if (button == SDL_BUTTON_LEFT && example_ >= 0) {
                const int chosen = app::exampleStripAt(lessonStripArea(), app::ExampleStrip{example_, exampleVersion_}, x, y);
                if (chosen == app::kExampleStripAll) showGallery();
                if (chosen >= 0 && chosen != exampleVersion_) openGalleryItem(example_, chosen);
            } else if (button == SDL_BUTTON_LEFT) {
                pressLessonButton(app::lessonStripAt(lessonStripArea(), lessonStrip(), x, y));
            }
            return;
        }
        if (button == SDL_BUTTON_LEFT && computerHere_ && y < app::kToolbarHeight) {
            for (int which = 0; which < app::kViewCount; ++which) {
                const SDL_FRect r = app::viewSwitchRect(boardRightEdge(), which);
                if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
                    view_ = which;
                    xrayPc_ = UINT32_MAX;  // the X-ray starts the instruction from its first step
                    return;
                }
            }
        }
        if (view_ != app::kViewBoard && x < app::kDebugWidth && y >= app::kToolbarHeight) {
            if (button == SDL_BUTTON_LEFT) {
                if (const std::optional<editor::ComputerState> computer =
                        editor::readComputer(session_.board(), session_.circuit(), session_.chipBoards())) {
                    pressDebugger(app::debuggerAt(*computer, debuggerArea(), x, y));
                } else if (const std::optional<editor::Computer32State> computer32 =
                               editor::readComputer32(session_.board(), session_.circuit(), session_.chipBoards())) {
                    pressDebugger(app::debugger32At(*computer32, debuggerArea(), x, y));
                }
            }
            return;
        }
        if (view_ != app::kViewBoard && overBoard(x, y)) {
            const SDL_FRect area = viewArea();
            const auto hit = [&](const SDL_FRect& r) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; };
            if (const std::optional<SDL_FRect> plan = diagramArea(); plan && button == SDL_BUTTON_LEFT) {
                if (hit(app::zoomButtonRect(*plan, 0))) return zoomAt(plan->x + plan->w / 2.0f, plan->y + plan->h / 2.0f, 1.0f / 1.25f);
                if (hit(app::zoomButtonRect(*plan, 1))) return zoomAt(plan->x + plan->w / 2.0f, plan->y + plan->h / 2.0f, 1.25f);
                if (hit(app::zoomButtonRect(*plan, 2))) {
                    const bool hex = zoom_.hex;  // the whole diagram again, in the same base
                    zoom_ = {};
                    zoom_.hex = hex;
                    return;
                }
                if (hit(app::zoomButtonRect(*plan, 3))) {
                    zoom_.hex = !zoom_.hex;
                    return;
                }
                if (hit(*plan)) {
                    zoomDragging_ = true;
                    return;
                }
            }
            if (button == SDL_BUTTON_LEFT && hit(app::panelNextButton(area))) {
                session_.stepClock();
                session_.stepClock();
            }
            if (button == SDL_BUTTON_LEFT && view_ == app::kViewXray) {
                for (int stage = 0; stage < editor::kTraceStages; ++stage) {
                    if (hit(app::xrayStageRect(area, stage))) chooseXrayStage(stage);
                }
            }
            return;
        }
        if (profilesOpen_) {
            if (button == SDL_BUTTON_LEFT) pressProfile(x, y);
            return;
        }
        if (helpOpen_) {
            helpOpen_ = false;
            return;
        }
        if (prompt_ != Prompt::None) return;
        if (button == SDL_BUTTON_LEFT) {
            const bool onSearch = x >= app::kSearchBox.x && x < app::kSearchBox.x + app::kSearchBox.w && y >= app::kSearchBox.y &&
                                  y < app::kSearchBox.y + app::kSearchBox.h;
            if (x < app::kPanelWidth && quizLesson() && view_ == app::kViewBoard) return;  // the questions' list: nothing to click
            if (onSearch) {
                startSearch();
                return;
            }
            stopSearch();
            if (x < app::kPanelWidth) {
                pressPalette(x, y);
                return;
            }
            if (session_.currentCircuit() != 0 && view_ == app::kViewBoard) {
                const std::vector<editor::CircuitEntry> trail = session_.trail();
                for (const app::TrailPiece& piece : app::trailPieces(renderer_, session_, app::kPanelWidth, boardTop())) {
                    if (x < piece.rect.x || x >= piece.rect.x + piece.rect.w || y < piece.rect.y || y >= piece.rect.y + piece.rect.h) continue;
                    if (piece.index < 0) {
                        session_.goBack();
                    } else {
                        session_.openCircuit(trail[static_cast<std::size_t>(piece.index)].id);
                    }
                    return;
                }
            }
            if (overRightPanel(x)) {
                pressRightPanel(x, y);
                return;
            }
            if (y < app::kToolbarHeight) {
                const int tool = app::toolbarAt(x, y);
                if (tool >= 0) {
                    const app::ToolButton& pressed = app::kToolbar[tool];
                    if (pressed.sim == app::SimButton::None) {
                        session_.key(pressed.key);
                    } else {
                        pressSimButton(pressed.sim);
                    }
                }
                return;
            }
            if (!overBoard(x, y)) return;
            sendCursor();
            session_.leftDown((SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
        } else if (button == SDL_BUTTON_RIGHT && session_.mode() != editor::Mode::Idle) {
            session_.rightDown();
        } else {
            panning_ = true;
        }
    }

    void pressProfile(float x, float y) {
        const std::vector<io::Profile>& all = profiles_.profiles();
        for (std::size_t i = 0; i < all.size(); ++i) {
            const SDL_FRect row = app::profileRow(i, all.size(), width_, height_);
            if (x >= row.x && x < row.x + row.w && y >= row.y && y < row.y + row.h) {
                const std::string id = all[i].id;
                chooseProfile(id);
                return;
            }
        }
    }

    void mouseUp(Uint8 button) {
        if (button == SDL_BUTTON_LEFT) zoomDragging_ = false;
        if (button == SDL_BUTTON_LEFT) insideDragging_ = false;
        if (button == SDL_BUTTON_LEFT) peekDragging_ = false;
        if (insidePlan_ != nullptr || !peeks_.empty()) return;
        if (codeVisible()) {
            code_->selecting = false;
            return;
        }
        if (button == SDL_BUTTON_LEFT) {
            if (pressedCircuit_) {
                // A project's circuit: dropped on the board it's a chip, and a plain click opens it.
                const editor::ChipId chip = *pressedCircuit_;
                pressedCircuit_.reset();
                if (overBoard(mouseX_, mouseY_)) {
                    session_.beginPlacingChip(chip);
                    if (session_.mode() == editor::Mode::Placing) {
                        sendCursor();
                        session_.leftDown();
                        session_.leftUp();
                        session_.key(editor::Key::Escape);
                    } else {
                        showNote("Um chip não pode ficar dentro de si mesmo.", true);
                    }
                } else if (chip != session_.currentCircuit()) {
                    session_.openCircuit(chip);
                }
                return;
            }
            if (draggingFromPalette_) {
                // Dropped on the board: placed, and done. Let go anywhere else and it's a plain click,
                // which leaves the part on the cursor to be placed by clicking.
                draggingFromPalette_ = false;
                if (overBoard(mouseX_, mouseY_)) {
                    sendCursor();
                    session_.leftDown();
                    session_.leftUp();
                    session_.key(editor::Key::Escape);
                }
                return;
            }
            if (!profilesOpen_) session_.leftUp();
        } else {
            panning_ = false;
        }
    }

    void pressPalette(float x, float y) {
        const std::vector<app::PaletteGroup> groups = libraryGroups(palette_.filter);
        syncPalette();
        const app::PaletteHit hit = app::paletteAt(groups, palette_, x, y, height_);
        if (hit.group < 0) return;
        const auto group = static_cast<std::size_t>(hit.group);
        if (hit.item < 0) {
            // A folder: it replaces a search, since a search shows every folder at once.
            palette_.folder = hit.group;
            palette_.filter.clear();
            palette_.scroll = 0;
            return;
        }
        const app::PaletteItem& item = groups[group].items[static_cast<std::size_t>(hit.item)];
        if (item.example == app::Example::Gallery) {
            showGallery();
            return;
        }
        if (item.example == app::Example::Computer) {
            if (mayDiscard()) openExample();
            return;
        }
        if (item.example == app::Example::LedCircuit || item.example == app::Example::TransistorNand || item.example == app::Example::Blinker) {
            if (mayDiscard()) openElectricExample(item.example);
            return;
        }
        if (item.example == app::Example::Cpu) {
            session_.beginPlacingChip(cpuChip());
            draggingFromPalette_ = session_.mode() == editor::Mode::Placing;
            syncPalette();
            return;
        }
        if (item.newChip) {
            openPrompt(Prompt::NewChip);
            return;
        }
        if (item.circuit) {
            pressedCircuit_ = item.chip;
            return;
        }
        session_.beginPlacing(item.kind, item.box);
        draggingFromPalette_ = true;
    }

    // The project's CPU chip, adding it the first time it's asked for.
    editor::ChipId cpuChip() {
        for (const editor::CircuitEntry& entry : session_.circuits()) {
            if (entry.name == editor::kCpuName) return entry.id;
        }
        const editor::ChipId first = session_.nextChipId();
        session_.addChips(editor::buildCpuChips(first));
        return first + editor::kCpuChipCount - 1;
    }

    void profileKey(SDL_Keycode key) {
        if (key == SDLK_ESCAPE) {
            closeProfiles();
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            createProfile();
        } else if (key == SDLK_BACKSPACE && !typing_.empty()) {
            // Whole characters, not bytes: names can have accents.
            do {
                typing_.pop_back();
            } while (!typing_.empty() && (static_cast<unsigned char>(typing_.back()) & 0xC0) == 0x80);
        }
    }

    void startSearch() {
        session_.key(editor::Key::Escape);
        palette_.searching = true;
        SDL_StartTextInput(window_);
    }

    void stopSearch() {
        if (!palette_.searching) return;
        palette_.searching = false;
        SDL_StopTextInput(window_);
    }

    void searchKey(SDL_Keycode key) {
        if (key == SDLK_ESCAPE) {
            palette_.filter.clear();
            palette_.scroll = 0;
            stopSearch();
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            stopSearch();
        } else if (key == SDLK_BACKSPACE && !palette_.filter.empty()) {
            do {
                palette_.filter.pop_back();
            } while (!palette_.filter.empty() && (static_cast<unsigned char>(palette_.filter.back()) & 0xC0) == 0x80);
            palette_.scroll = 0;
        }
    }

    // With a keyboard part picked on its own, the real keyboard types into it instead of giving shortcuts:
    // letters, digits, the space, Enter and Backspace. Esc still lets it go.
    bool typeOnKeyboard(SDL_Keycode key, SDL_Keymod mod, bool down) {
        const std::vector<editor::PartId>& selected = session_.selectedParts();
        if (selected.size() != 1 || code_ || prompt_ != Prompt::None || homeOpen_ || lessonsOpen_ || galleryOpen_) return false;
        const editor::Part* part = session_.board().part(selected[0]);
        if (part == nullptr || part->kind != editor::PartKind::Keyboard || key == SDLK_ESCAPE) return false;
        int code = -1;
        if (key >= SDLK_A && key <= SDLK_Z) code = static_cast<int>(key - SDLK_A) + ((mod & SDL_KMOD_SHIFT) != 0 ? 'A' : 'a');
        if (key >= SDLK_0 && key <= SDLK_9) code = static_cast<int>(key - SDLK_0) + '0';
        if (key == SDLK_SPACE) code = ' ';
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) code = '\n';
        if (key == SDLK_BACKSPACE) code = 8;
        if (key == SDLK_PERIOD) code = '.';
        if (key == SDLK_COMMA) code = ',';
        if (key == SDLK_MINUS) code = '-';
        if (code < 0) return false;
        session_.pressKey(part->id, static_cast<std::uint8_t>(code), down);
        return true;
    }

    void keyDown(SDL_Keycode key, SDL_Keymod mod) {
        if (!peeks_.empty()) {
            if (key == SDLK_ESCAPE) peeks_.pop_back();
            return;
        }
        if (bundle_ != 0 && insidePlan_ == nullptr) {
            if (key == SDLK_ESCAPE) bundle_ = 0;
            return;
        }
        if (insidePlan_ != nullptr) {
            const SDL_FRect area = app::insidePlanArea(width_, height_);
            if (key == SDLK_ESCAPE) closeInside();
            if (insideLiveMode_ && key == SDLK_SPACE) toggleRun();
            if (insideLiveMode_ && key == SDLK_N) session_.stepClock();
            if (key == SDLK_1 || key == SDLK_KP_1) insideView_.level = app::InsideLevel::Gates;
            if (key == SDLK_2 || key == SDLK_KP_2) insideView_.level = app::InsideLevel::Nands;
            if (key == SDLK_3 || key == SDLK_KP_3) insideView_.level = app::InsideLevel::Transistors;
            if (key == SDLK_EQUALS || key == SDLK_PLUS || key == SDLK_KP_PLUS) zoomInside(area.x + area.w / 2.0f, area.y + area.h / 2.0f, 1.6f);
            if (key == SDLK_MINUS || key == SDLK_KP_MINUS) zoomInside(area.x + area.w / 2.0f, area.y + area.h / 2.0f, 1.0f / 1.6f);
            return;
        }
        if (codeVisible()) {
            codeKey(key, mod);
            return;
        }
        if (homeOpen_ && prompt_ == Prompt::InsidePassword) {
            if (key == SDLK_ESCAPE) closePrompt();
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER) confirmPrompt();
            if (key == SDLK_BACKSPACE && !typing_.empty()) typing_.pop_back();
            return;
        }
        if (homeOpen_) {
            if (homeComputers_) {
                if (key == SDLK_ESCAPE) homeComputers_ = false;
            } else if (key == SDLK_ESCAPE || key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                homeOpen_ = false;
            }
            return;
        }
        if (topic_ >= 0) {
            const float page = static_cast<float>(height_) - 140.0f;
            const float limit = app::readingMaxScroll(renderer_, width_, height_, readingView());
            if (key == SDLK_ESCAPE) showLessons();
            if (key == SDLK_DOWN || key == SDLK_PAGEDOWN || key == SDLK_SPACE) {
                readingScroll_ = std::min(limit, readingScroll_ + (key == SDLK_DOWN ? 60.0f : page));
            }
            if (key == SDLK_UP || key == SDLK_PAGEUP) readingScroll_ = std::max(0.0f, readingScroll_ - (key == SDLK_UP ? 60.0f : page));
            // The screens: right goes on once the screen's questions are answered, left goes back.
            if (key == SDLK_RIGHT && readingPage_ + 1 < app::readingPageCount(topic_) && app::readingPageDone(renderer_, width_, readingView())) {
                showReadingPage(readingPage_ + 1);
            }
            if (key == SDLK_LEFT && readingPage_ > 0) showReadingPage(readingPage_ - 1);
            return;
        }
        if (lessonsOpen_ && prompt_ == Prompt::DevPassword) {
            if (key == SDLK_ESCAPE) closePrompt();
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER) confirmPrompt();
            if (key == SDLK_BACKSPACE && !typing_.empty()) typing_.pop_back();
            return;
        }
        if (lessonsOpen_ || galleryOpen_) {
            if (key == SDLK_ESCAPE) {
                lessonsOpen_ = false;
                galleryOpen_ = false;
                homeOpen_ = true;
            }
            return;
        }
        if (palette_.searching) {
            searchKey(key);
            return;
        }
        if (typeOnKeyboard(key, mod, true)) return;
        if (profilesOpen_) {
            profileKey(key);
            return;
        }
        if (prompt_ != Prompt::None) {
            if (key == SDLK_ESCAPE) closePrompt();
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER) confirmPrompt();
            if (key == SDLK_BACKSPACE && !typing_.empty()) {
                do {
                    typing_.pop_back();
                } while (!typing_.empty() && (static_cast<unsigned char>(typing_.back()) & 0xC0) == 0x80);
            }
            return;
        }
        if (key == SDLK_F1 || (helpOpen_ && key == SDLK_ESCAPE)) {
            helpOpen_ = !helpOpen_;
            return;
        }
        if (quizActive() && quizKey(key, mod)) return;
        const bool ctrl = (mod & SDL_KMOD_CTRL) != 0;
        const bool shift = (mod & SDL_KMOD_SHIFT) != 0;
        using editor::Key;
        if (ctrl && key == SDLK_Z) {
            session_.key(shift ? Key::Redo : Key::Undo);
        } else if (ctrl && key == SDLK_Y) {
            session_.key(Key::Redo);
        } else if (ctrl && key == SDLK_A) {
            session_.key(Key::SelectAll);
        } else if (ctrl && key == SDLK_C) {
            session_.key(Key::Copy);
        } else if (ctrl && key == SDLK_X) {
            session_.key(Key::Cut);
        } else if (ctrl && key == SDLK_V) {
            session_.key(Key::Paste);
        } else if (ctrl && key == SDLK_S) {
            if (currentFile_ && !shift) {
                saveTo(*currentFile_);
            } else {
                chooseFileToSave();
            }
        } else if (ctrl && key == SDLK_O) {
            if (mayDiscard()) chooseFileToOpen();
        } else if (ctrl && key == SDLK_N) {
            if (mayDiscard()) startBlank();
        } else if (key == SDLK_ESCAPE) {
            // With nothing to cancel, Esc goes back out of the chip that's open.
            const bool idle = session_.mode() == editor::Mode::Idle && session_.selectedParts().empty() && session_.selectedWires().empty();
            if (idle && session_.currentCircuit() != 0) {
                session_.goBack();
            } else {
                session_.key(Key::Escape);
            }
        } else if (key == SDLK_DELETE || key == SDLK_BACKSPACE) {
            session_.key(Key::Delete);
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            session_.key(Key::Enter);
        } else if (key == SDLK_R) {
            session_.key(Key::Rotate);
        } else if (key == SDLK_F) {
            fit();
        } else if (key == SDLK_P) {
            openProfiles();
        } else if (key == SDLK_T) {
            tableVisible_ = !tableVisible_;
        } else if (key == SDLK_EQUALS || key == SDLK_PLUS || key == SDLK_KP_PLUS) {
            session_.changeInputs(1);
        } else if (key == SDLK_MINUS || key == SDLK_KP_MINUS) {
            session_.changeInputs(-1);
        } else if (key == SDLK_RIGHTBRACKET) {
            session_.changeScale(1);
        } else if (key == SDLK_LEFTBRACKET) {
            session_.changeScale(-1);
        } else if (key == SDLK_PERIOD) {
            session_.changeBits(1);
        } else if (key == SDLK_COMMA) {
            session_.changeBits(-1);
        } else if (key == SDLK_SPACE) {
            toggleRun();
        } else if (key == SDLK_N) {
            session_.stepClock();
        }
    }

    static constexpr SDL_DialogFileFilter kFilters[] = {{"Projetos", "bancada"}};
    static constexpr const char* kCodeTooBig = "Esse texto não cabe: o programa ficaria grande demais.";

    SDL_Window* window_;
    SDL_Renderer* renderer_;
    io::ProfileStore profiles_;
    editor::Session session_;
    Camera camera_;
    int width_ = 0;
    int height_ = 0;
    float mouseX_ = 0;
    float mouseY_ = 0;
    bool panning_ = false;
    std::vector<std::pair<SDL_FingerID, SDL_FPoint>> fingers_;  // the fingers on the screen now
    bool pinching_ = false;                                     // two of them, moving the view
    float pinchX_ = 0.0f;
    float pinchY_ = 0.0f;
    float pinchDistance_ = 1.0f;
    float touchScroll_ = 0.0f;  // a finger's move not yet turned into scrolling
    bool quitting_ = false;

    std::string projectName_ = "sem nome";
    std::optional<fs::path> currentFile_;
    std::string title_;
    std::string dialogFolder_;
    Uint32 openEvent_ = 0;
    Uint32 saveEvent_ = 0;
    Uint32 codeOpenEvent_ = 0;  // a program's text file chosen, to open in the editor
    Uint32 codeSaveEvent_ = 0;  // or to save the editor's text in
    std::uint64_t autosavedRevision_ = 0;
    Uint64 nextAutosave_ = 0;
    Uint64 confirmUntil_ = 0;

    std::string note_;
    bool noteIsError_ = false;
    Uint64 noteUntil_ = 0;

    bool profilesOpen_ = false;
    std::string typing_;

    app::PaletteView palette_;
    bool draggingFromPalette_ = false;
    std::optional<editor::ChipId> pressedCircuit_;
    Prompt prompt_ = Prompt::None;
    std::optional<CodeEditing> code_;
    bool homeOpen_ = false;
    bool homeComputers_ = false;  // the home shows the page that picks a computer
    bool lessonsOpen_ = false;  // the list of lessons covers the window
    bool codeShown_ = false;    // the program editor is open as the view, under the toolbar
    int lessonAct_ = 0;         // the act it shows
    bool devUnlocked_ = false;  // the developer's switch: every act open, for this run only
    Uint64 devDeniedUntil_ = 0;  // a wrong password says so on the button until then
    float lessonScroll_ = 0.0f;
    int topic_ = -1;            // the reading that covers the window, or -1
    float readingScroll_ = 0.0f;
    int readingPage_ = 0;       // its screen on show
    std::map<int, std::vector<int>> readingAnswers_;  // per reading, the option chosen in each question: kept for this run
    bool galleryOpen_ = false;  // so does the gallery of examples
    std::optional<editor::Architecture> inside16_, inside32_;  // the computers drawn whole, once made
    const editor::Architecture* insidePlan_ = nullptr;
    // Parts seen from the inside, each opened from the one before: the last is on screen.
    struct Peek {
        std::string title;
        const editor::GalleryItem* item = nullptr;
        int version = 1;
        std::unique_ptr<editor::Session> session;
        app::Camera camera;
    };
    std::vector<Peek> peeks_;
    bool peekDragging_ = false;
    std::optional<editor::Architecture> insideLive_;  // a board's own plan, opened from a part on it
    bool insideLiveMode_ = false;
    std::uint64_t insideRevision_ = 0;
    std::vector<std::uint8_t> insideValues_;
    std::string insideFocus_;
    editor::PartId bundle_ = 0;  // a splitter or a joiner seen from the inside, or 0         // the one on screen, if any: it covers everything
    app::InsideView insideView_;
    bool insideDragging_ = false;
    int example_ = -1;          // the example on the board, from the gallery, with its explanation over it
    int exampleVersion_ = 0;    // and the version of it that's on the board
    bool programMode_ = false;  // the player's own program computer is on the board
    int lesson_ = -1;           // the lesson on the board, or -1 for the player's own project
    app::LibraryLimit lessonLimit_;
    editor::LessonResult lessonResult_;
    std::uint64_t lessonRevision_ = UINT64_MAX;  // the board the result is about
    app::XrayZoom zoom_;          // how close the computer's diagram is seen
    bool zoomDragging_ = false;   // the diagram is being moved with the mouse
    std::size_t quizIndex_ = 0;  // a lesson of questions: the one being asked
    std::string quizTyped_;
    bool quizWrong_ = false;
    bool quizRight_ = false;
    bool quizBadDigit_ = false;  // a key the base has no digit for was pressed
    bool lessonProblemShown_ = false;            // Verificar was pressed: say what's wrong
    int lessonHint_ = -1;                        // the hint shown, or -1
    std::vector<std::string> finished_;          // the lessons this profile finished
    Uint64 resetConfirmUntil_ = 0;               // the reset button was pressed once, and waits for a second
    int view_ = app::kViewBoard;
    std::uint32_t xrayPc_ = UINT32_MAX;  // the instruction the X-ray is going through
    bool xrayHalted_ = false;
    int xrayStage_ = 0;
    std::vector<std::uint16_t> breakpoints_;     // ROM addresses the debugger stops at
    std::uint32_t debugPc_ = UINT32_MAX;         // the instruction the registers below are about
    std::uint32_t leavingBreakpoint_ = UINT32_MAX;
    std::array<std::uint16_t, 8> registersNow_{};
    std::array<std::uint16_t, 8> registersBefore_{};
    std::vector<std::uint16_t> memoryNow_;
    std::vector<std::uint16_t> memoryBefore_;
    bool hasBefore_ = false;
    Uint64 xrayStart_ = 0;  // when the step began
    bool xrayPinned_ = false;  // a step the player chose: it stays until the next instruction
    bool computerHere_ = false;  // a computer on the board, as of the last frame
    bool is32_ = false;          // and it's the 32-bit one
    std::string console_;        // what the 32-bit computer printed with syscall since it last started
    std::array<std::uint32_t, 32> registers32Now_{};
    std::array<std::uint32_t, 32> registers32Before_{};
    bool program32_ = false;  // the player's program computer is the 32-bit one
    std::string reading_;
    Uint64 lastTick_ = 0;
    bool tableVisible_ = true;
    int tableScroll_ = 0;
    bool helpOpen_ = false;
    int galleryGroup_ = 0;  // the folder of examples on show
};

// Sends events as the window system would, in screen coordinates, through App::handle.
class Script {
public:
    Script(App& app, SDL_Window* window) : app_(app), window_(SDL_GetWindowID(window)) {}

    void moveScreen(float x, float y) {
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.windowID = window_;
        event.motion.x = x;
        event.motion.y = y;
        app_.handle(event);
    }

    void button(Uint32 type, float x, float y) {
        SDL_Event event{};
        event.type = type;
        event.button.windowID = window_;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.x = x;
        event.button.y = y;
        app_.handle(event);
    }

    void clickScreen(float x, float y) {
        moveScreen(x, y);
        button(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
        button(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
    }

    void click(editor::Point p) {
        const Vec2 s = app_.camera().toScreen(static_cast<float>(p.x), static_cast<float>(p.y));
        clickScreen(s.x, s.y);
    }

    void drag(editor::Point from, editor::Point to) {
        const Vec2 a = app_.camera().toScreen(static_cast<float>(from.x), static_cast<float>(from.y));
        const Vec2 b = app_.camera().toScreen(static_cast<float>(to.x), static_cast<float>(to.y));
        moveScreen(a.x, a.y);
        button(SDL_EVENT_MOUSE_BUTTON_DOWN, a.x, a.y);
        moveScreen(b.x, b.y);
        button(SDL_EVENT_MOUSE_BUTTON_UP, b.x, b.y);
    }

    void key(SDL_Keycode code) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.windowID = window_;
        event.key.key = code;
        app_.handle(event);
    }

    void keyWithControl(SDL_Keycode code) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.windowID = window_;
        event.key.key = code;
        event.key.mod = SDL_KMOD_LCTRL;
        app_.handle(event);
    }

    void text(const char* typed) {
        SDL_Event event{};
        event.type = SDL_EVENT_TEXT_INPUT;
        event.text.windowID = window_;
        event.text.text = typed;
        app_.handle(event);
    }

    // Tiles outside the list's visible part can't be pressed: roll it until this one shows.
    void rollPalette(float amount) {
        moveScreen(60.0f, static_cast<float>(app_.height()) - 120.0f);
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_WHEEL;
        event.wheel.windowID = window_;
        event.wheel.y = amount;
        app_.handle(event);
    }

    // Opens the tile's folder, the way a person would, and rolls the list until the tile shows.
    Vec2 showTile(int group, int item) {
        if (app_.palette().filter.empty() && app_.palette().folder != group) {
            const SDL_FRect folder = app::paletteFolderRect(group);
            clickScreen(folder.x + folder.w / 2.0f, folder.y + folder.h / 2.0f);
        }
        for (int tries = 0; tries < 200; ++tries) {
            const std::vector<app::PaletteGroup> groups = app_.libraryGroups(app_.palette().filter);
            const SDL_FRect r = app::paletteTileRect(groups, app_.palette(), group, item);
            const float middle = r.y + r.h / 2.0f;
            if (middle > static_cast<float>(app_.height()) - 60.0f) {
                rollPalette(-1.0f);
            } else if (middle < app::paletteListTop(groups) + 14.0f) {
                rollPalette(1.0f);
            } else {
                return {r.x + r.w / 2.0f, middle};
            }
        }
        return {0, 0};
    }

    // The first tile that matches, given the item and whether it's in the project's own folder.
    template <typename Match>
    Vec2 findTile(Match match) {
        const std::vector<app::PaletteGroup> groups = app_.libraryGroups();
        for (int g = 0; g < static_cast<int>(groups.size()); ++g) {
            const std::vector<app::PaletteItem>& items = groups[static_cast<std::size_t>(g)].items;
            for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                if (match(items[static_cast<std::size_t>(i)], g + 1 == static_cast<int>(groups.size()))) return showTile(g, i);
            }
        }
        return {0, 0};
    }

    Vec2 partTile(editor::PartKind kind, editor::BoxId box) {
        return findTile([&](const app::PaletteItem& item, bool) {
            return item.kind == kind && item.box == box && !item.circuit && !item.newChip && item.example == app::Example::None;
        });
    }

    Vec2 circuitTile(const std::string& name) {
        return findTile([&](const app::PaletteItem& item, bool project) { return project && item.name == name; });
    }

    Vec2 namedTile(const std::string& name) {
        return findTile([&](const app::PaletteItem& item, bool) { return item.name == name; });
    }

    void pressNamedRow(const std::string& name) {
        const Vec2 at = namedTile(name);
        clickScreen(at.x, at.y);
    }

    void pressCircuitRow(const std::string& name) {
        const Vec2 at = circuitTile(name);
        clickScreen(at.x, at.y);
    }

    void dragCircuitOntoBoard(const std::string& name, editor::Point centre) {
        const Vec2 from = circuitTile(name);
        moveScreen(from.x, from.y);
        button(SDL_EVENT_MOUSE_BUTTON_DOWN, from.x, from.y);
        const Vec2 at = app_.camera().toScreen(static_cast<float>(centre.x), static_cast<float>(centre.y));
        moveScreen(at.x, at.y);
        button(SDL_EVENT_MOUSE_BUTTON_UP, at.x, at.y);
    }

    // Drags a part from the library onto the board, the way a person would.
    editor::PartId place(editor::PartKind kind, editor::Point centre, editor::BoxId box = editor::kNoBox) {
        const Vec2 from = partTile(kind, box);
        moveScreen(from.x, from.y);
        button(SDL_EVENT_MOUSE_BUTTON_DOWN, from.x, from.y);
        const Vec2 s = app_.camera().toScreen(static_cast<float>(centre.x), static_cast<float>(centre.y));
        moveScreen(s.x, s.y);
        button(SDL_EVENT_MOUSE_BUTTON_UP, s.x, s.y);
        return app_.session().board().parts().back().id;
    }

    void pressPanel(app::PanelButton which) {
        const float left = static_cast<float>(app_.width()) - app::rightPanelWidth(app_.session());
        const SDL_FRect r = app::panelButtonRect(app_.session(), which, left);
        clickScreen(r.x + r.w / 2.0f, r.y + r.h / 2.0f);
    }

    // By what the button does, so that adding a button doesn't change which one a script presses.
    void pressTool(editor::Key key) {
        for (int i = 0; i < static_cast<int>(std::size(app::kToolbar)); ++i) {
            if (app::kToolbar[i].sim == app::SimButton::None && app::kToolbar[i].key == key) {
                clickScreen(app::toolbarButtonX(i) + 10.0f, 16.0f);
                return;
            }
        }
    }

    void selectPart(editor::PartId part) {
        const editor::Rect bounds = boundsOf(*app_.session().board().part(part));
        const Vec2 at = app_.camera().toScreen(static_cast<float>(bounds.left + bounds.right) / 2.0f,
                                               static_cast<float>(bounds.top + bounds.bottom) / 2.0f);
        moveScreen(at.x, at.y);
        button(SDL_EVENT_MOUSE_BUTTON_DOWN, at.x, at.y);
        button(SDL_EVENT_MOUSE_BUTTON_UP, at.x, at.y);
    }

    editor::Point pin(editor::PartId part, unsigned index) {
        return pinPosition(*app_.session().board().part(part), index);
    }

private:
    App& app_;
    Uint32 window_;
};

void buildXor(App& app, SDL_Window* window) {
    Script s(app, window);
    const editor::PartId a = s.place(editor::PartKind::Toggle, {3, 5});
    const editor::PartId b = s.place(editor::PartKind::Toggle, {3, 13});
    const editor::PartId n1 = s.place(editor::PartKind::Nand, {12, 10});
    const editor::PartId n2 = s.place(editor::PartKind::Nand, {20, 5});
    const editor::PartId n3 = s.place(editor::PartKind::Nand, {20, 15});
    const editor::PartId n4 = s.place(editor::PartKind::Nand, {29, 10});
    const editor::PartId lamp = s.place(editor::PartKind::Led, {36, 10});

    s.click(s.pin(a, 0));
    s.click(s.pin(n2, 0));
    s.click(s.pin(b, 0));
    s.click(s.pin(n3, 1));
    s.click(s.pin(n1, 2));
    s.click(s.pin(n2, 1));
    s.click(s.pin(n2, 2));
    s.click(s.pin(n4, 0));
    s.click(s.pin(n3, 2));
    s.click(s.pin(n4, 1));
    s.click(s.pin(n4, 2));
    s.click(s.pin(lamp, 0));

    const auto nextToPin = [&](editor::Point pin) {
        const editor::Wire* wire = app.session().board().wireAt(pin);
        editor::Point point = wire->path[0];
        const editor::Point after = wire->path[1];
        point.x += (after.x > point.x) - (after.x < point.x);
        point.y += (after.y > point.y) - (after.y < point.y);
        return point;
    };
    s.drag(nextToPin(s.pin(a, 0)), s.pin(n1, 0));
    s.drag(nextToPin(s.pin(b, 0)), s.pin(n1, 1));
    s.drag(nextToPin(s.pin(n1, 2)), s.pin(n3, 0));

    // Switch A on: the lamp must light.
    const editor::Rect bounds = boundsOf(*app.session().board().part(a));
    const Vec2 centre = app.camera().toScreen(static_cast<float>(bounds.left + bounds.right) / 2.0f,
                                              static_cast<float>(bounds.top + bounds.bottom) / 2.0f);
    s.clickScreen(centre.x, centre.y);
    s.moveScreen(900, 80);
    std::printf("xor: %zu parts, %zu wires, %zu joints, lamp %s\n", app.session().board().parts().size(),
                app.session().board().wires().size(), app.session().board().joints().size(),
                app.session().circuit().outputOn(lamp) ? "on" : "off");
    std::fflush(stdout);
}

// Types a new profile's name, places a part in it, goes back to the first one, and leaves the list open.
void useProfiles(App& app, SDL_Window* window) {
    Script s(app, window);
    s.place(editor::PartKind::Nand, {6, 4});
    s.key(SDLK_P);
    s.text("Ana");
    s.key(SDLK_RETURN);
    const std::size_t inNewProfile = app.session().board().parts().size();
    s.key(SDLK_P);
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    const SDL_FRect first = app::profileRow(0, 2, width, height);
    s.clickScreen(first.x + 20, first.y + 10);
    std::printf("profiles: new one had %zu parts, first has %zu again\n", inNewProfile, app.session().board().parts().size());
    std::fflush(stdout);
    s.key(SDLK_P);
}

// One of each gate, then a NAND in each of the other rotations, placed from the palette.
void showGates(App& app, SDL_Window* window) {
    Script s(app, window);
    app.camera().cellPixels = 16.0f;
    app.camera().originX = -14.0f;
    app.camera().originY = -4.0f;
    int row = 0;
    const std::vector<app::PaletteGroup> groups = app.libraryGroups();  // kept: the loop below reads from it
    for (const app::PaletteItem& item : groups[0].items) {
        s.place(item.kind, {2 + (row % 3) * 7, 2 + (row / 3) * 7});
        ++row;
    }
    app.camera().cellPixels = 16.0f;
    s.moveScreen(600, 680);
}

// Everything added in one go: a 3-input gate and a bigger one set from the panel, a box dragged from the
// library, an LED, a 0/1 output, copy and paste, all through the same events a person makes.
void showRefinements(App& app, SDL_Window* window) {
    Script s(app, window);
    using editor::PartKind;
    app.camera().originX = -13.0f;
    app.camera().originY = -4.0f;

    const editor::PartId a = s.place(PartKind::Toggle, {0, 4});
    const editor::PartId b = s.place(PartKind::Toggle, {0, 10});
    const editor::PartId c = s.place(PartKind::Toggle, {0, 16});

    const editor::PartId gate = s.place(PartKind::And, {12, 10});
    s.selectPart(gate);
    s.pressPanel(app::PanelButton::InputsUp);
    s.click(s.pin(a, 0));
    s.click(s.pin(gate, 0));
    s.click(s.pin(b, 0));
    s.click(s.pin(gate, 1));
    s.click(s.pin(c, 0));
    s.click(s.pin(gate, 2));
    const editor::PartId led = s.place(PartKind::Led, {24, 10});
    s.click(s.pin(gate, 3));
    s.click(s.pin(led, 0));

    const editor::PartId probe = s.place(PartKind::Probe, {24, 16});
    const editor::Point branch = app.session().board().wireAt(s.pin(led, 0))->path[0];
    s.drag({branch.x + 1, branch.y}, s.pin(probe, 0));

    const editor::BoxId adder = editor::boxFromKey("full_adder");
    const editor::PartId box = s.place(PartKind::Box, {10, 24}, adder);
    s.click(s.pin(a, 0));
    s.click(s.pin(box, 0));

    // The toolbar and the copy and paste keys, through the window: delete, undo, then copy and paste.
    const auto partCount = [&] { return app.session().board().parts().size(); };
    const std::size_t atStart = partCount();
    s.selectPart(probe);
    s.pressTool(editor::Key::Delete);
    const std::size_t afterDelete = partCount();
    s.pressTool(editor::Key::Undo);
    const std::size_t afterUndo = partCount();
    s.selectPart(probe);
    s.keyWithControl(SDLK_C);
    s.keyWithControl(SDLK_V);
    s.clickScreen(860.0f, 560.0f);
    const std::size_t afterPaste = partCount();
    s.pressTool(editor::Key::Undo);
    std::printf("buttons: %zu parts, delete %zu, undo %zu, paste %zu, undo %zu\n", atStart, afterDelete, afterUndo, afterPaste, partCount());

    s.selectPart(a);
    s.selectPart(c);
    s.selectPart(b);
    s.moveScreen(800, 680);
    std::printf("refino: %zu parts, %zu wires, table status %d, %zu rows\n", app.session().board().parts().size(),
                app.session().board().wires().size(), static_cast<int>(app.session().truthTable().status),
                app.session().truthTable().rows.size());
    std::fflush(stdout);
}

// An 8-bit counter on a real clock, its bus shown whole and split in half, and a chip made and used, all
// through the same events a person makes.
void showComputer(App& app, SDL_Window* window) {
    Script s(app, window);
    using editor::PartKind;
    app.camera().originX = -16.0f;
    app.camera().originY = -5.0f;
    app.camera().cellPixels = 14.0f;

    const editor::PartId run = s.place(PartKind::Toggle, {3, 0});
    const editor::PartId clock = s.place(PartKind::Clock, {3, 12});
    const editor::PartId counter = s.place(PartKind::Box, {20, 6}, editor::boxFromKey("counter"));
    s.click(s.pin(run, 0));
    s.click(s.pin(counter, 0));
    s.click(s.pin(clock, 0));
    s.click(s.pin(counter, 2));

    const editor::PartId probe = s.place(PartKind::Probe, {46, 2});
    s.selectPart(probe);
    for (int i = 0; i < 7; ++i) s.pressPanel(app::PanelButton::BitsUp);
    s.click(s.pin(counter, 3));
    s.click(s.pin(probe, 0));

    const editor::PartId split = s.place(PartKind::Splitter, {36, 16});
    const editor::PartId low = s.place(PartKind::Probe, {46, 14});
    s.selectPart(low);
    for (int i = 0; i < 3; ++i) s.pressPanel(app::PanelButton::BitsUp);
    const editor::Wire* bus = app.session().board().wireAt(s.pin(probe, 0));
    const editor::Point onBus{(bus->path[0].x + bus->path[1].x) / 2, bus->path[0].y};
    s.drag(onBus, s.pin(split, 0));
    s.click(s.pin(split, 1));
    s.click(s.pin(low, 0));

    // A chip: an inverter, made in its own circuit, then used on the main board.
    s.pressCircuitRow("+ Novo chip");
    s.text("inversor");
    s.key(SDLK_RETURN);
    const editor::PartId in = s.place(PartKind::Toggle, {0, 4});
    const editor::PartId gate = s.place(PartKind::Not, {14, 4});
    const editor::PartId out = s.place(PartKind::Led, {28, 4});
    s.click(s.pin(in, 0));
    s.click(s.pin(gate, 0));
    s.click(s.pin(gate, 1));
    s.click(s.pin(out, 0));
    const std::size_t insideChip = app.session().board().parts().size();
    s.pressCircuitRow("principal");

    s.dragCircuitOntoBoard("inversor", {20, 20});
    const editor::PartId chip = app.session().board().parts().back().id;
    const editor::PartId led = s.place(PartKind::Led, {38, 26});
    s.click(s.pin(run, 0));
    s.click(s.pin(chip, 0));
    s.click(s.pin(chip, 1));
    s.click(s.pin(led, 0));
    const bool ledBefore = app.session().circuit().outputOn(led);

    s.selectPart(run);
    const bool ledAfter = app.session().circuit().outputOn(led);
    s.moveScreen(600, 680);
    for (int i = 0; i < 2 * 37; ++i) s.key(SDLK_N);
    std::printf("computer: %zu parts, %zu in the chip, counter %llu, low bits %llu, chip led %d then %d, width conflict %d\n",
                app.session().board().parts().size(), insideChip,
                static_cast<unsigned long long>(app.session().circuit().outputValue(probe)),
                static_cast<unsigned long long>(app.session().circuit().outputValue(low)), ledBefore ? 1 : 0, ledAfter ? 1 : 0,
                app.session().circuit().hasWidthConflict() ? 1 : 0);
    std::fflush(stdout);
}

struct ExampleParts {
    editor::PartId rom = 0;
    editor::PartId output = 0;
    editor::PartId halt = 0;
};

ExampleParts openComputer(App& app, Script& s) {
    s.pressNamedRow("Computador pronto");
    ExampleParts parts;
    for (const editor::Part& part : app.session().board().parts()) {
        if (part.kind == editor::PartKind::Box && part.box == editor::boxFromKey("rom")) parts.rom = part.id;
        if (part.label == "Saída") parts.output = part.id;
        if (part.label == "Parou") parts.halt = part.id;
    }
    return parts;
}

void openProgram(Script& s, const ExampleParts& parts) {
    s.selectPart(parts.rom);
    s.pressPanel(app::PanelButton::EditProgram);
}

constexpr const char* kFactorial =
    "; fatorial de 5, com uma sub-rotina que multiplica\n"
    "        LI   R1, 1\n"
    "        LI   R2, 5\n"
    "la\xC3\xA7o:   MOV  R3, R1\n"
    "        MOV  R4, R2\n"
    "        CALL vezes\n"
    "        MOV  R1, R5\n"
    "        OUT  R1\n"
    "        ADDI R2, R2, -1\n"
    "        BNE  R2, R0, la\xC3\xA7o\n"
    "        HLT\n"
    "vezes:  LI   R5, 0\n"
    "soma:   ADD  R5, R5, R3\n"
    "        ADDI R4, R4, -1\n"
    "        BNE  R4, R0, soma\n"
    "        RET\n";

// The whole way a person would go: the computer from the library, a wrong program refused, a right one
// typed and assembled into the ROM, and the clock run until it stops.
void runProgram(App& app, SDL_Window* window) {
    Script s(app, window);
    const ExampleParts parts = openComputer(app, s);
    openProgram(s, parts);
    const bool opened = app.codeOpen();
    s.keyWithControl(SDLK_A);
    s.text("FOO R1");
    const std::size_t problems = app.codeProblems();
    s.keyWithControl(SDLK_RETURN);
    const bool refused = app.codeOpen();

    s.keyWithControl(SDLK_A);
    s.key(SDLK_BACKSPACE);
    s.text(kFactorial);
    const std::size_t afterFix = app.codeProblems();
    const app::CodeLayout layout = app::codeLayout(app.width(), app.height() - static_cast<int>(app::kToolbarHeight));
    s.clickScreen(layout.assemble.x + 20.0f, layout.assemble.y + 10.0f + app::kToolbarHeight);
    s.key(SDLK_ESCAPE);  // assembled, the editor stays; Esc closes it
    const bool closed = !app.codeOpen();

    const editor::Part* rom = app.session().board().part(parts.rom);
    const std::size_t words = rom->rom->size();
    s.moveScreen(600, 680);
    int halves = 0;
    while (!app.session().circuit().outputOn(parts.halt) && halves < 4000) {
        s.key(SDLK_N);
        ++halves;
    }
    std::printf("program: opened %d, %zu problem, refused %d, then %zu problems, closed %d, %zu words, output %llu, halted %d after %d cycles, source %s\n",
                opened ? 1 : 0, problems, refused ? 1 : 0, afterFix, closed ? 1 : 0, words,
                static_cast<unsigned long long>(app.session().circuit().outputValue(parts.output)),
                app.session().circuit().outputOn(parts.halt) ? 1 : 0, halves / 2,
                app.session().board().part(parts.rom)->source == kFactorial ? "kept" : "LOST");
    std::fflush(stdout);
}

// Leaves the editor open with a mistake in view, for a look at it.
void showCode(App& app, SDL_Window* window) {
    Script s(app, window);
    const ExampleParts parts = openComputer(app, s);
    openProgram(s, parts);
    s.keyWithControl(SDLK_A);
    s.key(SDLK_BACKSPACE);
    s.text(kFactorial);
    s.text("        ADD R1, R2\n        LI R1, 900");
}

// A loop that never ends at the clock's top speed, so that the frames after this measure the computer.
void startSpeedRun(App& app, SDL_Window* window) {
    Script s(app, window);
    const ExampleParts parts = openComputer(app, s);
    openProgram(s, parts);
    s.keyWithControl(SDLK_A);
    s.key(SDLK_BACKSPACE);
    s.text("laco: ADDI R1, R1, 1\nOUT R1\nJMP laco\n");
    s.keyWithControl(SDLK_RETURN);
    for (int i = 0; i < 20; ++i) app.session().changeSpeed(1);
    app.session().setRunning(true);
}

void showInside(App& app, SDL_Window* window, int circuit) {
    Script s(app, window);
    openComputer(app, s);
    app.session().openCircuit(app.session().circuits()[static_cast<std::size_t>(circuit)].id);
    app.fit();
    s.moveScreen(1200, 700);
}

void buildStress(App& app, int count) {
    editor::Board board;
    const int perRow = 200;
    editor::PartId previous = 0;
    for (int i = 0; i < count; ++i) {
        const editor::Point at{(i % perRow) * 8, (i / perRow) * 7};
        const editor::PartId id = *board.addPart(editor::PartKind::Nand, at);
        if (previous != 0 && i % perRow != 0) {
            const editor::Point from = pinPosition(*board.part(previous), 2);
            const editor::Point to = pinPosition(*board.part(id), 0);
            const editor::Drop start{editor::Drop::Kind::Pin, from, previous, 2};
            const editor::Drop end{editor::Drop::Kind::Pin, to, id, 0};
            const editor::Point mid{from.x + 2, from.y};
            board.connect(start, end, {from, mid, {mid.x, to.y}, to});
        }
        previous = id;
    }
    app.session().adopt(std::move(board));
}

bool saveScreenshot(SDL_Renderer* renderer, const char* path) {
    SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
    if (surface == nullptr) return false;
    const bool saved = SDL_SaveBMP(surface, path);
    SDL_DestroySurface(surface);
    return saved;
}

}  // namespace

int main(int argc, char** argv) {
    long maxFrames = -1;
    const char* screenshot = nullptr;
    const char* script = nullptr;
    const char* dataArgument = nullptr;
    const char* openPath = nullptr;
    const char* savePath = nullptr;
    const char* languageArgument = nullptr;
    int stress = 0;
    int windowWidth = 1280;  // --tamanho WxH, for screenshots
    int windowHeight = 720;
    bool devArgument = false;  // --dev: every act open from the start
    for (int i = 1; i < argc; i += 2) {
        if (std::strcmp(argv[i], "--dev") == 0) {
            devArgument = true;
            --i;  // it takes no value
            continue;
        }
        if (i + 1 >= argc) break;
        if (std::strcmp(argv[i], "--frames") == 0) maxFrames = std::strtol(argv[i + 1], nullptr, 10);
        if (std::strcmp(argv[i], "--screenshot") == 0) screenshot = argv[i + 1];
        if (std::strcmp(argv[i], "--script") == 0) script = argv[i + 1];
        if (std::strcmp(argv[i], "--stress") == 0) stress = std::atoi(argv[i + 1]);
        if (std::strcmp(argv[i], "--data") == 0) dataArgument = argv[i + 1];
        if (std::strcmp(argv[i], "--open") == 0) openPath = argv[i + 1];
        if (std::strcmp(argv[i], "--save") == 0) savePath = argv[i + 1];
        if (std::strcmp(argv[i], "--idioma") == 0) languageArgument = argv[i + 1];
        if (std::strcmp(argv[i], "--tamanho") == 0) std::sscanf(argv[i + 1], "%dx%d", &windowWidth, &windowHeight);
    }

    // On Android the Back button comes to the game as a key, instead of closing it.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("bancada", windowWidth, windowHeight, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        std::fprintf(stderr, "window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, stress > 0 ? 0 : 1);
#ifdef SDL_PLATFORM_ANDROID
    SDL_SetWindowFullscreen(window, true);  // the whole screen, the system's bars hidden
#endif

    // Only looked up when needed: asking for the user's folder also creates it.
    bancada::app::DataLocation data;
    if (dataArgument != nullptr) {
        data.directory = io::pathFromUtf8(dataArgument);
    } else {
        data = bancada::app::findDataLocation();
    }
    const char* const dataNote = data.directory.empty() ? "sem pasta de dados"
                                 : dataArgument != nullptr ? "pasta indicada"
                                 : data.portable           ? "modo portátil"
                                                           : "pasta do usuário";
    std::printf("video=%s renderer=%s data=%s\n", SDL_GetCurrentVideoDriver(), SDL_GetRendererName(renderer), dataNote);
    std::fflush(stdout);

    fs::path dataDirectory = data.directory;
    if (dataDirectory.empty()) dataDirectory = fs::temp_directory_path() / "bancada";

    App app(window, renderer, dataDirectory);
    if (devArgument) app.unlockEverything();
    // For this run only, without saving it: the screenshots of each language.
    if (languageArgument != nullptr) {
        if (const auto language = i18n::languageFromCode(languageArgument)) i18n::setLanguage(*language);
    }
    int exitCode = 0;
    if (openPath != nullptr) {
        if (!app.openFile(io::pathFromUtf8(openPath))) {
            std::printf("open failed\n");
            exitCode = 3;
        }
    } else if (script == nullptr && stress == 0) {
        app.restoreAutosave();
        app.showHome();
    }
    if (script != nullptr && std::strcmp(script, "xor") == 0) buildXor(app, window);
    if (script != nullptr && std::strcmp(script, "perfis") == 0) useProfiles(app, window);
    if (script != nullptr && std::strcmp(script, "portas") == 0) showGates(app, window);
    if (script != nullptr && std::strcmp(script, "refino") == 0) showRefinements(app, window);
    if (script != nullptr && std::strcmp(script, "computador") == 0) showComputer(app, window);
    if (script != nullptr && std::strcmp(script, "programa") == 0) runProgram(app, window);
    if (script != nullptr && std::strcmp(script, "codigo") == 0) showCode(app, window);
    if (script != nullptr && std::strncmp(script, "ajuda-codigo", 12) == 0) {
        showCode(app, window);
        app.openCodeHelp(script[12] == ':' ? static_cast<float>(std::atof(script + 13)) : 0.0f);
    }
    // ajuda-mips[:S], the 32-bit computer's instruction help, scrolled S pixels down.
    if (script != nullptr && std::strncmp(script, "ajuda-mips", 10) == 0) {
        app.openProgram32();
        app.openCodeHelp(script[10] == ':' ? static_cast<float>(std::atof(script + 11)) : 0.0f);
    }
    if (script != nullptr && std::strcmp(script, "velocidade") == 0) startSpeedRun(app, window);
    if (script != nullptr && std::strncmp(script, "dentro:", 7) == 0) showInside(app, window, std::atoi(script + 7));
    if (script != nullptr && std::strcmp(script, "inicio") == 0) app.showHome();
    // aovivo:N, the example computer from the inside, live, a few clock cycles in: 0 its CPU, 1 the first box inside
    // the CPU's decoder, 2 the decoder's first splitter.
    if (script != nullptr && std::strncmp(script, "aovivo:", 7) == 0) app.showLive(std::atoi(script + 7));
    // trilha: the example computer, two chips in, CPU and then its decoder, with the way back on show.
    if (script != nullptr && std::strcmp(script, "trilha") == 0) app.showLive(-1);
    if (script != nullptr && std::strcmp(script, "atalhos") == 0) {  // the board's shortcuts, as F1 shows them
        Script s(app, window);
        s.key(SDLK_F1);
    }
    if (script != nullptr && std::strcmp(script, "computadores") == 0) app.showComputers();
    if (script != nullptr && std::strcmp(script, "programar32") == 0) app.openProgram32();
    // blocos32:N:R, the 32-bit program N instructions in, on the Blocks tab scrolled R blocks down.
    if (script != nullptr && std::strncmp(script, "blocos32:", 9) == 0) {
        app.openProgram32();
        for (int i = 0; i < 2 * std::atoi(script + 9); ++i) app.session().stepClock();
        const char* scroll = std::strchr(script + 9, ':');
        app.draw("");
        app.showBlocks(scroll != nullptr ? std::atoi(scroll + 1) : 0);
    }
    // rodar32:N:V[:S[:Z[:H]]]: the 32-bit program computer, N instructions in, on view V (1 Panel, 2 X-ray), at X-ray step S, zoomed Z times,
    // in hexadecimal if H is 1.
    if (script != nullptr && std::strncmp(script, "rodar32:", 8) == 0) {
        Script s(app, window);
        app.openProgram32();
        for (int i = 0; i < 2 * std::atoi(script + 8); ++i) app.session().stepClock();
        app.draw("");
        const char* view = std::strchr(script + 8, ':');
        if (view != nullptr) {
            const SDL_FRect r = app::viewSwitchRect(app.boardRightEdge(), std::atoi(view + 1));
            s.clickScreen(r.x + 20.0f, r.y + 10.0f);
            if (const char* stage = std::strchr(view + 1, ':'); stage != nullptr) {
                app.draw("");
                app.chooseXrayStage(std::atoi(stage + 1));
                if (const char* zoom = std::strchr(stage + 1, ':'); zoom != nullptr) {
                    app.zoomDiagram(static_cast<float>(std::atof(zoom + 1)));
                    if (const char* hex = std::strchr(zoom + 1, ':'); hex != nullptr) app.showHex(std::atoi(hex + 1) == 1);
                }
            }
        }
        s.moveScreen(700, 400);
    }
    // pordentro:B:L[:Z:X:Y]: the B-bit computer from the inside, picture L (0 gates, 1 NANDs, 2 transistors), zoomed Z times
    // around the point at X, Y (shares of the plan's width and height).
    if (script != nullptr && std::strncmp(script, "pordentro:", 10) == 0) {
        int bits = 16, level = 0;
        float zoom = 1.0f, px = 0.5f, py = 0.5f;
        std::sscanf(script + 10, "%d:%d:%f:%f:%f", &bits, &level, &zoom, &px, &py);
        app.openInside(bits == 32, static_cast<app::InsideLevel>(std::clamp(level, 0, 2)));
        if (zoom != 1.0f) app.lookInside(zoom, px, py);
    }
    if (script != nullptr && std::strcmp(script, "aulas") == 0) app.showLessons();
    // aulas:N[:end], the list on act N, counted from 0, scrolled to its end.
    if (script != nullptr && std::strncmp(script, "aulas:", 6) == 0) {
        app.showLessons();
        app.showAct(std::atoi(script + 6), std::strstr(script + 6, ":end") != nullptr);
    }
    // leitura:N, the reading of topic N; leitura:N:S, scrolled S pixels down.
    if (script != nullptr && std::strncmp(script, "leitura:", 8) == 0) {
        app.openReading(std::atoi(script + 8));
        int page = 0, answered = 0;
        float scroll = 0.0f, clickX = -1.0f, clickY = -1.0f;
        std::sscanf(script + 8, "%*d:%f:%d:%d:%f:%f", &scroll, &page, &answered, &clickX, &clickY);
        if (answered == 1) app.answerReading();
        app.showReadingPage(page);
        app.scrollReading(scroll);
        if (clickX >= 0.0f) {  // and a click there, the way a person would
            Script s(app, window);
            app.draw("");
            s.clickScreen(clickX, clickY);
        }
    }
    // peek:KEY[:V], a ready part (its box key) or a gate (and, or, not, nand, nor, xor, xnor) from the inside, in its
    // version V; peek:KEY:V:PART, and then that part of it, by its box key or gate, a level deeper.
    if (script != nullptr && std::strncmp(script, "peek:", 5) == 0) {
        const auto partFor = [](const std::string& key) {
            static const std::pair<const char*, editor::PartKind> gates[] = {
                {"and", editor::PartKind::And}, {"or", editor::PartKind::Or},   {"not", editor::PartKind::Not},  {"nand", editor::PartKind::Nand},
                {"nor", editor::PartKind::Nor}, {"xor", editor::PartKind::Xor}, {"xnor", editor::PartKind::Xnor}};
            for (const auto& [name, kind] : gates) {
                if (key == name) return editor::makePart(kind, {}, 0, 2, 1, editor::kNoBox);
            }
            return editor::makePart(editor::PartKind::Box, {}, 0, 0, 1, editor::boxFromKey(key));
        };
        std::string rest(script + 5);
        std::vector<std::string> fields;
        for (std::size_t colon; (colon = rest.find(':')) != std::string::npos; rest = rest.substr(colon + 1)) fields.push_back(rest.substr(0, colon));
        fields.push_back(rest);
        const bool opened = app.openPeek(partFor(fields[0]));
        if (!opened) std::printf("no inside for %s\n", fields[0].c_str());
        if (opened && fields.size() > 1) app.showPeekVersion(std::atoi(fields[1].c_str()));
        if (opened && fields.size() > 2 && !app.openPeek(partFor(fields[2]))) std::printf("no inside for %s\n", fields[2].c_str());
    }
    // senha:TEXTO[:enter], the lesson list with the developer's password field open and TEXTO typed (and confirmed).
    // dentro-senha:TEXT: the computers' page, with TEXT typed as the password for "Por dentro" and Enter pressed.
    if (script != nullptr && std::strncmp(script, "dentro-senha:", 13) == 0) {
        Script s(app, window);
        app.showComputers();
        app.draw("");
        const SDL_FRect lock = app::insideLockRect(app.width(), app.height());
        s.clickScreen(lock.x + lock.w / 2.0f, lock.y + lock.h / 2.0f);
        s.text(script + 13);
        s.key(SDLK_RETURN);
        s.moveScreen(10.0f, 300.0f);
    }
    if (script != nullptr && std::strncmp(script, "senha:", 6) == 0) {
        Script s(app, window);
        app.showLessons();
        app.showAct(7);
        app.draw("");
        s.clickScreen(static_cast<float>(app.width()) - 24.0f - 230.0f - 12.0f - 100.0f, 40.0f);
        const std::string spec(script + 6);
        const std::size_t colon = spec.find(':');
        s.text(spec.substr(0, colon).c_str());
        if (colon != std::string::npos) s.key(SDLK_RETURN);
        s.moveScreen(10.0f, 300.0f);
    }
    // codigo:STEPS:TAB, the "conte até 5" lesson with its program written and assembled, STEPS instructions run
    // and the editor's right column on TAB (0 task, 1 commands, 2 blocks).
    if (script != nullptr && std::strncmp(script, "codigo:", 7) == 0) {
        Script s(app, window);
        app.startLesson(editor::lessonIndex("conte-ate-cinco"));
        app.draw("");
        s.keyWithControl(SDLK_A);
        s.text("        LI   R1, 1\n        LI   R2, 6\nla\xC3\xA7o:   OUT  R1\n        ADDI R1, R1, 1\n        BNE  R1, R2, la\xC3\xA7o\n        HLT\n");
        s.keyWithControl(SDLK_RETURN);
        const int steps = std::atoi(script + 7);
        for (int i = 0; i < steps; ++i) {
            app.session().stepClock();
            app.session().stepClock();
            app.draw("");
        }
        if (const char* colon = std::strchr(script + 7, ':')) app.setCodeTab(std::atoi(colon + 1));
        s.moveScreen(600, 400);
    }
    // ato:N, the list of lessons on act N.
    if (script != nullptr && std::strncmp(script, "ato:", 4) == 0) {
        app.showLessons();
        app.showAct(std::atoi(script + 4));
    }
    // galeria[:N], the examples on folder N.
    if (script != nullptr && std::strncmp(script, "galeria", 7) == 0) app.showGallery(script[7] == ':' ? std::atoi(script + 8) : 0);
    if (script != nullptr && std::strcmp(script, "programar") == 0) app.openProgram();
    // exemplo:ID, exemplo:ID:N for its version N, and @NAME at the end to pick the label with that name.
    if (script != nullptr && std::strncmp(script, "exemplo:", 8) == 0) {
        Script s(app, window);
        const std::string_view spec(script + 8);
        const std::size_t at = spec.find('@');
        const std::string_view head = spec.substr(0, at);
        const std::size_t colon = head.find(':');
        const std::string_view id = head.substr(0, colon);
        const int version = colon == std::string_view::npos ? 0 : std::atoi(std::string(head.substr(colon + 1)).c_str());
        for (int i = 0; i < static_cast<int>(editor::galleryItems().size()); ++i) {
            if (editor::galleryItems()[static_cast<std::size_t>(i)].id == id) app.openGalleryItem(i, version);
        }
        if (at != std::string_view::npos) {
            const std::string_view name = spec.substr(at + 1);
            for (const editor::Part& part : app.session().board().parts()) {
                if (part.kind == editor::PartKind::Tunnel && part.label == name) {
                    app.session().selectOnly(part.id);
                    break;
                }
            }
        }
        app.draw("");
        s.moveScreen(700, 120);
    }
    // proximo:N, lesson N and then its Next: the next exercise, or the next reading.
    if (script != nullptr && std::strncmp(script, "proximo:", 8) == 0) {
        app.startLesson(std::atoi(script + 8));
        app.goToNext();
    }
    // perguntas:N:KEYS, lesson N of questions with KEYS typed: digits and letters, '.' for Enter, 'n' for Next, 'h' for Dica.
    if (script != nullptr && std::strncmp(script, "perguntas:", 10) == 0) {
        Script s(app, window);
        app.startLesson(std::atoi(script + 10));
        app.draw("");
        if (const char* keys = std::strchr(script + 10, ':'); keys != nullptr) {
            for (const char* c = keys + 1; *c != '\0'; ++c) {
                if (*c == '.') s.key(SDLK_RETURN);
                else if (*c >= '0' && *c <= '9') s.key(static_cast<SDL_Keycode>(SDLK_0 + (*c - '0')));
                else if (*c >= 'a' && *c <= 'f') s.key(static_cast<SDL_Keycode>(SDLK_A + (*c - 'a')));
                else if (*c == 'n') app.goToNext();
                else if (*c == 'h') app.pressLessonButton(app::LessonButton::Hint);
            }
        }
        s.moveScreen(700, 560);
    }
    if (script != nullptr && std::strncmp(script, "aula:", 5) == 0) {
        Script s(app, window);
        // By its number, or by its id.
        const bool numbered = std::isdigit(static_cast<unsigned char>(script[5])) != 0;
        app.startLesson(numbered ? std::atoi(script + 5) : editor::lessonIndex(std::string_view(script + 5, std::strcspn(script + 5, ":"))));
        app.draw("");
        if (std::strstr(script + 5, ":f") != nullptr) s.key(SDLK_ESCAPE);  // the code editor closed, back on the board
        if (std::strstr(script + 5, ":v") != nullptr) {
            const SDL_FRect strip = app.lessonStripArea();
            s.clickScreen(strip.x + strip.w - 106.0f, strip.y + 33.0f);
        }
        s.moveScreen(700, 560);
        if (std::strstr(script + 5, ":r") != nullptr) {
            // The first resistor's value, opened for editing.
            for (const editor::Part& part : app.session().board().parts()) {
                if (part.kind != editor::PartKind::Resistor) continue;
                s.selectPart(part.id);
                app.draw("");
                s.pressPanel(app::PanelButton::EditValue);
                s.key(SDLK_BACKSPACE);
                break;
            }
        }
    }
    if (script != nullptr && std::strncmp(script, "pasta:", 6) == 0) {
        Script s(app, window);
        const SDL_FRect folder = app::paletteFolderRect(std::atoi(script + 6));
        s.clickScreen(folder.x + folder.w / 2.0f, folder.y + folder.h / 2.0f);
        s.moveScreen(700, 400);
    }
    if (script != nullptr && (std::strcmp(script, "led") == 0 || std::strcmp(script, "nand") == 0 || std::strcmp(script, "pisca") == 0)) {
        Script s(app, window);
        const char* row = std::strcmp(script, "led") == 0 ? "LED e resistor" : std::strcmp(script, "nand") == 0 ? "NAND de transistores" : "Pisca-pisca";
        s.pressNamedRow(row);
        if (std::strcmp(script, "led") == 0) {
            const editor::Part& toggle = app.session().board().parts()[1];
            s.selectPart(toggle.id);
            const editor::Part& light = app.session().board().parts()[3];
            const editor::Rect bounds = boundsOf(light);
            const Vec2 over = app.camera().toScreen(static_cast<float>(bounds.left) + 2.0f, static_cast<float>(bounds.top) + 1.0f);
            s.moveScreen(over.x, over.y);
            const std::optional<editor::PartReading> reading = app.session().circuit().reading(light.id);
            std::printf("led: %s\n", reading ? app::describeReading(light, *reading).c_str() : "no reading");
        }
        if (std::strcmp(script, "nand") == 0) {
            const editor::TruthTable& table = app.session().truthTable();
            std::string rows;
            for (const std::string& r : table.rows) rows += r;
            std::printf("nand: table %s\n", rows.c_str());
            s.moveScreen(900, 600);
        }
        std::fflush(stdout);
    }
    if (script != nullptr && std::strcmp(script, "painel") == 0) {
        Script s(app, window);
        openComputer(app, s);
        for (int i = 0; i < 2 * 9; ++i) s.key(SDLK_N);
        app.draw("");
        const SDL_FRect panel = app::viewSwitchRect(app.boardRightEdge(), 1);
        s.clickScreen(panel.x + 20.0f, panel.y + 10.0f);
        s.moveScreen(700, 400);
    }
    // rodando:ID, the gallery program ID on the Panel, running.
    if (script != nullptr && std::strncmp(script, "rodando:", 8) == 0) {
        Script s(app, window);
        const std::string_view id(script + 8);
        for (int i = 0; i < static_cast<int>(editor::galleryItems().size()); ++i) {
            if (editor::galleryItems()[static_cast<std::size_t>(i)].id == id) app.openGalleryItem(i);
        }
        app.draw("");
        app.toggleRun();
        s.moveScreen(700, 400);
    }
    // raiox@ID:S:I: the gallery program ID on the X-ray, I instructions in, step S chosen.
    if (script != nullptr && std::strncmp(script, "raiox@", 6) == 0) {
        Script s(app, window);
        const std::string spec(script + 6);
        const std::size_t colon = spec.find(':');
        const std::string id = spec.substr(0, colon);
        int stage = 5;
        int instructions = 0;
        if (colon != std::string::npos) {
            stage = std::atoi(spec.c_str() + colon + 1);
            const std::size_t second = spec.find(':', colon + 1);
            if (second != std::string::npos) instructions = std::atoi(spec.c_str() + second + 1);
        }
        for (int i = 0; i < static_cast<int>(editor::galleryItems().size()); ++i) {
            if (editor::galleryItems()[static_cast<std::size_t>(i)].id == id) app.openGalleryItem(i);
        }
        app.draw("");
        const SDL_FRect view = app::viewSwitchRect(app.boardRightEdge(), app::kViewXray);
        s.clickScreen(view.x + 20.0f, view.y + 10.0f);
        for (int i = 0; i < instructions; ++i) {
            app.draw("");
            s.key(SDLK_N);
            s.key(SDLK_N);
        }
        app.draw("");
        const SDL_FRect chip = app::xrayStageRect(app.viewArea(), stage);
        s.clickScreen(chip.x + chip.w / 2.0f, chip.y + chip.h / 2.0f);
        s.moveScreen(700, 120);
    }
    if (script != nullptr && std::strncmp(script, "raiox", 5) == 0 && script[5] != '@') {
        // "raiox:S:I": I instructions into the example (9, the loop's branch, if not given), step S chosen.
        Script s(app, window);
        openComputer(app, s);
        int stage = -1;
        int instructions = 9;
        if (script[5] == ':') {
            stage = std::atoi(script + 6);
            if (const char* second = std::strchr(script + 6, ':')) instructions = std::atoi(second + 1);
        }
        app.draw("");  // the view buttons appear once the computer has been seen
        const SDL_FRect view = app::viewSwitchRect(app.boardRightEdge(), app::kViewXray);
        s.clickScreen(view.x + 20.0f, view.y + 10.0f);
        // One instruction at a time, drawn in between, the way the debugger sees them go by.
        for (int i = 0; i < instructions; ++i) {
            app.draw("");
            s.key(SDLK_N);
            s.key(SDLK_N);
        }
        app.draw("");
        if (stage >= 0) {
            const SDL_FRect chip = app::xrayStageRect(app.viewArea(), stage);
            s.clickScreen(chip.x + chip.w / 2.0f, chip.y + chip.h / 2.0f);
        }
        s.moveScreen(700, 120);
    }
    if (script != nullptr && std::strcmp(script, "computador16") == 0) {
        Script s(app, window);
        openComputer(app, s);
        s.moveScreen(1200, 700);
    }
    if (stress > 0) {
        buildStress(app, stress);
        app.fit();

        // Every edit copies the board and recompiles the circuit, so time one.
        editor::Session& session = app.session();
        const Uint64 before = SDL_GetTicksNS();
        session.beginPlacing(editor::PartKind::Toggle);
        session.mouseMove(-20.0f, -20.0f);
        session.leftDown();
        session.leftUp();
        session.key(editor::Key::Escape);
        std::printf("one edit: %.1f ms\n", static_cast<double>(SDL_GetTicksNS() - before) / 1e6);
    }

    int vsync = 0;
    SDL_GetRenderVSync(renderer, &vsync);

    Uint64 measuredNs = 0;
    long frames = 0;
    while (!app.quitting()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) app.handle(event);
        if (stress == 0) app.tick();

        const Uint64 start = SDL_GetTicksNS();
        app.draw(dataNote);
        ++frames;
        const bool last = maxFrames >= 0 && frames >= maxFrames;
        if (last && screenshot != nullptr && !saveScreenshot(renderer, screenshot)) {
            std::fprintf(stderr, "screenshot: %s\n", SDL_GetError());
        }
        SDL_RenderPresent(renderer);
        measuredNs += SDL_GetTicksNS() - start;
        if (last) break;
        if (vsync == 0 && stress == 0) SDL_DelayNS(1'000'000);
    }

    if (stress > 0 && frames > 0) {
        std::printf("stress: %d parts, %.2f ms per frame\n", stress,
                    static_cast<double>(measuredNs) / 1e6 / static_cast<double>(frames));
    }
    if (script != nullptr && std::strcmp(script, "velocidade") == 0) {
        std::printf("speed: %.0f half cycles per second\n", app.session().measuredSpeed());
    }
    if (savePath != nullptr && !app.saveTo(io::pathFromUtf8(savePath))) exitCode = 4;
    if (stress == 0) app.autosave();

    bancada::app::releaseDrawing();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exitCode;
}
