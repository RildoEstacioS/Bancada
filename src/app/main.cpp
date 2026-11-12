// The window: translates SDL events into editor events, and keeps projects on disk. The rules of both
// live in src/editor and src/io.
//
//   --data DIR         use DIR for profiles and autosaves
//   --open FILE        open a project at start, instead of the autosave
//   --save FILE        save the project to FILE just before quitting
//   --frames N         quit after N frames
//   --screenshot FILE  save the last frame as a BMP
//   --script NAME      send real window events through the normal path: "xor", "perfis", "portas", "refino",
//                      "computador", "programa", "codigo", "velocidade", "computador16", "painel", "inicio", "led", "nand",
//                      "pisca", "dentro:N" (the Nth circuit of the example), "pasta:N" (the Nth library folder) or
//                      "raiox[:S[:I]]" (the X-ray, I instructions into the example, on step S), "aulas" (the list)
//                      or "aula:N[:v][:r]" (lesson N, with Verificar pressed, or its first resistor's value open
//                      with one Backspace typed)
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
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/draw.hpp"
#include "app/paths.hpp"
#include "asm/assembler.hpp"
#include "editor/boxes.hpp"
#include "editor/computer16.hpp"
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

enum class Prompt : std::uint8_t { None, Name, Value, Contents, NewChip };

// A program being edited. It's only a draft until it's assembled into the ROM.
struct CodeEditing {
    editor::TextEdit text;
    std::string original;
    std::vector<assembly::Error> problems;
    std::vector<std::uint16_t> words;
    std::size_t capacity = 0;
    int scrollLine = 0;
    int scrollColumn = 0;
    bool selecting = false;
    std::string warning;
    bool help = false;  // the list of every instruction is open over the code
    float helpScroll = 0.0f;
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
        openEvent_ = SDL_RegisterEvents(2);
        saveEvent_ = openEvent_ + 1;
        chooseStartingLanguage();
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
    std::size_t codeProblems() const { return code_ ? code_->problems.size() : 0; }

    void updateSize() { SDL_GetCurrentRenderOutputSize(renderer_, &width_, &height_); }

    void handle(SDL_Event event) {
        if (event.type == openEvent_ || event.type == saveEvent_) {
            finishDialog(event);
            return;
        }
        SDL_ConvertEventToRenderCoordinates(renderer_, &event);
        switch (event.type) {
            case SDL_EVENT_QUIT:
                quitting_ = true;
                break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                updateSize();
                break;
            case SDL_EVENT_MOUSE_MOTION:
                mouseMoved(event.motion.x, event.motion.y);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                mouseDown(event.button.button, event.button.x, event.button.y);
                if (event.button.button == SDL_BUTTON_LEFT && event.button.clicks == 2 && overBoard(mouseX_, mouseY_) && !modal()) {
                    session_.leftUp();
                    session_.openChipUnderCursor();
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                mouseUp(event.button.button);
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                wheel(event.wheel.y);
                break;
            case SDL_EVENT_KEY_DOWN:
                keyDown(event.key.key, event.key.mod);
                break;
            case SDL_EVENT_KEY_UP:
                typeOnKeyboard(event.key.key, event.key.mod, false);
                break;
            case SDL_EVENT_TEXT_INPUT:
                if (code_) {
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
        const float areaHeight = static_cast<float>(height_) - boardTop() - app::kStatusHeight - 40.0f;
        const float zoom = std::min(areaWidth / static_cast<float>(right - left), areaHeight / static_cast<float>(bottom - top));
        // A board with little on it isn't blown up to fill the window: it stays at a size to build on,
        // in the middle, with room around for what comes next.
        camera_.cellPixels = std::clamp(zoom, 0.25f, 18.0f);
        const float spareX = std::max(0.0f, (areaWidth - static_cast<float>(right - left) * camera_.cellPixels) / 2.0f);
        const float spareY = std::max(0.0f, (areaHeight - static_cast<float>(bottom - top) * camera_.cellPixels) / 2.0f);
        camera_.originX = static_cast<float>(left) - (app::kPanelWidth + 20.0f + spareX) / camera_.cellPixels;
        camera_.originY = static_cast<float>(top) - (boardTop() + 20.0f + spareY) / camera_.cellPixels;
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
            if (!editor::actOpen(a, finished_)) break;
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
        finished_ = profiles_.finishedLessons();
    }

    app::ReadingView readingView() const {
        app::ReadingView view;
        view.topic = topic_;
        view.scroll = readingScroll_;
        view.read = topic_ >= 0 && finishedHere(editor::readingId(editor::topics()[static_cast<std::size_t>(topic_)]));
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
        if (!editor::actOpen(editor::lessons()[static_cast<std::size_t>(index)].act, finished_)) {
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

    // Opens a lesson where the player left it, or from its first board. Whatever was open is saved first.
    void startLesson(int index) {
        if (index < 0 || index >= static_cast<int>(editor::lessons().size())) return;
        autosave();
        lessonsOpen_ = false;
        homeOpen_ = false;
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
        lessonRevision_ = UINT64_MAX;
        lessonProblemShown_ = false;
        lessonHint_ = -1;
        palette_.folder = lessonFolder(lesson);
        syncPalette();
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
        lessonProblemShown_ = false;
        lessonHint_ = -1;
        lessonRevision_ = UINT64_MAX;
    }

    // Checked after every change: a lesson finishes the moment it works. What's wrong only shows once
    // the player asks, with Verificar, so that it doesn't nag while they're still building.
    void checkLessonNow() {
        if (lesson_ < 0 || session_.currentCircuit() != 0 || session_.revision() == lessonRevision_) return;
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
                lessonRevision_ = UINT64_MAX;
                checkLessonNow();
                lessonProblemShown_ = true;
                lessonHint_ = -1;
                break;
            case app::LessonButton::Hint:
                lessonHint_ = (lessonHint_ + 1) % static_cast<int>(lesson.hints.size());
                lessonProblemShown_ = false;
                break;
            case app::LessonButton::Restart: restartLesson(); break;
            case app::LessonButton::List:
                autosave();
                showLessons();
                break;
            case app::LessonButton::Next: goToLesson(lesson_ + 1); break;
            case app::LessonButton::None: break;
        }
    }

    app::LessonStrip lessonStrip() const {
        app::LessonStrip strip;
        strip.index = lesson_;
        strip.passed = lessonResult_.passed;
        strip.hasNext = lesson_ + 1 < static_cast<int>(editor::lessons().size());
        const editor::Lesson& lesson = editor::lessons()[static_cast<std::size_t>(lesson_)];
        if (strip.passed) {
            strip.message = lessonResult_.message;
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
        text.showTable = tableVisible_;
        text.tableScroll = tableScroll_;
        text.showHelp = helpOpen_;
        text.mouseX = mouseX_;
        text.mouseY = mouseY_;
        text.searching = palette_.searching;
        const std::optional<editor::ComputerState> computer = editor::readComputer(session_.board(), session_.circuit(), session_.chipBoards());
        computerHere_ = computer.has_value();
        if (!computerHere_) view_ = app::kViewBoard;
        text.computer = computer ? &*computer : nullptr;
        text.view = view_;
        if (view_ == app::kViewXray && computer) advanceXray(*computer, text);
        app::DebugView debug;
        if (computer) {
            followComputer(*computer);
            debug.breakpoints = &breakpoints_;
            debug.before = registersBefore_;
            debug.memoryBefore = memoryBefore_;
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
        if (lesson_ >= 0) {
            strip = lessonStrip();
            text.lesson = &strip;
            text.limit = &lessonLimit_;
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
        if (code_) app::drawCode(renderer_, codeView(), width_, height_);
        if (homeOpen_) app::drawHome(renderer_, width_, height_, mouseX_, mouseY_, lessonsLine());
        if (lessonsOpen_) app::drawLessonList(renderer_, width_, height_, mouseX_, mouseY_, lessonListView());
        if (topic_ >= 0) app::drawReading(renderer_, width_, height_, mouseX_, mouseY_, readingView());
        if (galleryOpen_) app::drawGallery(renderer_, width_, height_, mouseX_, mouseY_);
    }

    void showHome() { homeOpen_ = true; }
    void openCodeHelp(float scroll) {
        if (!code_) return;
        code_->help = true;
        code_->helpScroll = scroll;
    }
    void scrollReading(float amount) { readingScroll_ = amount; }
    void showAct(int act) { lessonAct_ = std::clamp(act, 0, static_cast<int>(editor::lessonActs().size()) - 1); }

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
    void advanceXray(const editor::ComputerState& computer, app::InterfaceText& text) {
        constexpr Uint64 kStepMs = 1700;
        const Uint64 now = SDL_GetTicks();
        if (computer.pc != xrayPc_ || computer.halted != xrayHalted_) {
            xrayPc_ = computer.pc;
            xrayHalted_ = computer.halted;
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

    // Play and pause, the same from the toolbar, the debugger and the space bar. Running on leaves a
    // breakpoint the computer is stopped at, instead of stopping there again at once; a program that already
    // ended at its HLT starts over from the beginning.
    void toggleRun() {
        if (session_.running()) {
            session_.setRunning(false);
            return;
        }
        const std::optional<editor::ComputerState> computer = editor::readComputer(session_.board(), session_.circuit(), session_.chipBoards());
        if (computer && computer->halted) {
            session_.resetSimulation();
            hasBefore_ = false;
            debugPc_ = UINT32_MAX;
        }
        if (const std::optional<editor::ComputerState> now = editor::readComputer(session_.board(), session_.circuit(), session_.chipBoards())) {
            leavingBreakpoint_ = now->pc;
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
                session_.resetSimulation();
                hasBefore_ = false;
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

    void showGallery() {
        homeOpen_ = false;
        galleryOpen_ = true;
    }

    // The player's own computer, to write programs on: kept in a file of its own, apart from the
    // examples and the player's projects, and opened on the program, ready to write.
    void openProgram() {
        homeOpen_ = false;
        galleryOpen_ = false;
        leaveLesson();
        autosave();
        io::LoadedProject saved = io::loadProject(profiles_.programPath());
        if (saved.project) {
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

    void openCode() {
        const std::optional<editor::Inspected> shown = session_.inspected();
        if (!shown) return;
        if (shown->part.width != editor::kWordBits) {
            showNote("O programa precisa de uma ROM de 16 bits.", true);
            return;
        }
        std::string source = shown->part.source;
        if (source.empty() && shown->part.rom != nullptr) {
            const std::vector<std::uint64_t>& words = *shown->part.rom;
            for (std::size_t i = 0; i < words.size(); ++i) {
                source += assembly::disassemble(static_cast<std::uint16_t>(words[i]), i) + "\n";
            }
        }
        code_.emplace();
        code_->text = editor::TextEdit(source);
        code_->original = code_->text.text();
        code_->capacity = std::size_t{1} << shown->part.address;
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
        if (programMode_) {
            if (io::saveProject(profiles_.programPath(), project)) autosavedRevision_ = session_.revision();
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

    void finishDialog(const SDL_Event& event) {
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
        if (code_) {
            mouseX_ = x;
            mouseY_ = y;
            if (code_->selecting) placeCodeCursor(x, y, true);
            return;
        }
        if (panning_) camera_.pan(x - mouseX_, y - mouseY_);
        mouseX_ = x;
        mouseY_ = y;
        if (!profilesOpen_) sendCursor();
    }

    float rightEdgeWidth() const { return tableVisible_ && view_ == app::kViewBoard ? app::rightPanelWidth(session_) : 0.0f; }

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
        return profilesOpen_ || helpOpen_ || prompt_ != Prompt::None || code_.has_value() || homeOpen_ || lessonsOpen_ || galleryOpen_ ||
               topic_ >= 0;
    }

    app::CodeView codeView() const {
        app::CodeView view;
        view.text = &code_->text;
        view.problems = &code_->problems;
        view.words = code_->words.size();
        view.capacity = code_->capacity;
        view.scrollLine = code_->scrollLine;
        view.scrollColumn = code_->scrollColumn;
        view.warning = code_->warning.c_str();
        view.help = code_->help;
        view.helpScroll = code_->helpScroll;
        return view;
    }

    void closeCode() {
        code_.reset();
        SDL_StopTextInput(window_);
    }

    void checkCode() {
        CodeEditing& code = *code_;
        assembly::Result result = assembly::assemble(code.text.text());
        code.words = std::move(result.words);
        code.problems = std::move(result.errors);
        if (code.problems.empty() && code.words.size() > code.capacity) {
            code.problems.push_back(
                {0, i18n::format("O programa tem {} palavras, e esta ROM guarda {}.", {std::to_string(code.words.size()), std::to_string(code.capacity)})});
        }
    }

    void showCodeCursor() {
        CodeEditing& code = *code_;
        const app::CodeLayout layout = app::codeLayout(width_, height_);
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
        const std::vector<std::uint64_t> words(code.words.begin(), code.words.end());
        const std::string text = i18n::format("Programa gravado: {} palavras.", {std::to_string(words.size())});
        session_.setRomWords(words, code.text.text());
        closeCode();
        showNote(text.c_str(), false);
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
        const app::CodeLayout layout = app::codeLayout(width_, height_);
        const int line = code.scrollLine + static_cast<int>(std::floor((y - layout.text.y) / app::kCodeLineHeight));
        const int column = code.scrollColumn + static_cast<int>(std::lround((x - layout.text.x) / app::kCodeCharWidth));
        code.text.place(line, column, extend);
        showCodeCursor();
    }

    static bool inside(const SDL_FRect& rect, float x, float y) {
        return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
    }

    void pressCode(float x, float y) {
        const app::CodeLayout layout = app::codeLayout(width_, height_);
        if (inside(layout.help, x, y)) {
            code_->help = !code_->help;
            code_->helpScroll = 0.0f;
            return;
        }
        if (code_->help) return;
        if (inside(layout.assemble, x, y)) {
            assembleCode();
        } else if (inside(layout.close, x, y)) {
            cancelCode();
        } else if (inside(layout.text, x, y)) {
            placeCodeCursor(x, y, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
            code_->selecting = true;
        } else if (inside(layout.problems, x, y)) {
            const int problem = app::codeProblemAt(codeView(), y, width_, height_);
            if (problem >= 0) goToProblem(static_cast<std::size_t>(problem));
        }
    }

    void scrollCode(float amount) {
        CodeEditing& code = *code_;
        if (code.help) {
            code.helpScroll = std::clamp(code.helpScroll - amount * 60.0f, 0.0f, app::codeHelpMaxScroll(renderer_, width_, height_));
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
        const int page = std::max(1, app::codeLayout(width_, height_).lines - 1);
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

    void wheel(float amount) {
        if (code_) {
            scrollCode(amount);
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
            case app::SimButton::Reset: session_.resetSimulation(); break;
            case app::SimButton::Slower: session_.changeSpeed(-1); break;
            case app::SimButton::Faster: session_.changeSpeed(1); break;
            default: break;
        }
    }

    void mouseDown(Uint8 button, float x, float y) {
        mouseX_ = x;
        mouseY_ = y;
        if (code_) {
            if (button == SDL_BUTTON_LEFT) pressCode(x, y);
            return;
        }
        if (homeOpen_) {
            if (button == SDL_BUTTON_LEFT) chooseHome(app::homeAt(x, y, width_, height_));
            return;
        }
        if (galleryOpen_) {
            if (button != SDL_BUTTON_LEFT) return;
            const int chosen = app::galleryAt(x, y, width_, height_);
            if (chosen == app::kGalleryBack) {
                galleryOpen_ = false;
                homeOpen_ = true;
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
                case app::LessonListKind::Act:
                    lessonAct_ = hit.index;
                    lessonScroll_ = 0.0f;
                    break;
                case app::LessonListKind::Reading: openReading(hit.index); break;
                case app::LessonListKind::Exercise: startLesson(hit.index); break;
                case app::LessonListKind::None: break;
            }
            return;
        }
        if (topic_ >= 0) {
            if (button != SDL_BUTTON_LEFT) return;
            switch (app::readingAt(renderer_, x, y, width_, height_, readingView())) {
                case app::ReadingButton::Back: showLessons(); break;
                case app::ReadingButton::Exercises: startReadingExercises(); break;
                case app::ReadingButton::None: break;
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
                }
            }
            return;
        }
        if (view_ != app::kViewBoard && overBoard(x, y)) {
            const SDL_FRect area = viewArea();
            const auto hit = [&](const SDL_FRect& r) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; };
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
            if (onSearch) {
                startSearch();
                return;
            }
            stopSearch();
            if (x < app::kPanelWidth) {
                pressPalette(x, y);
                return;
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
        if (code_) {
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
        if (code_) {
            codeKey(key, mod);
            return;
        }
        if (homeOpen_) {
            if (key == SDLK_ESCAPE || key == SDLK_RETURN || key == SDLK_KP_ENTER) homeOpen_ = false;
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
            session_.key(Key::Escape);
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
    bool quitting_ = false;

    std::string projectName_ = "sem nome";
    std::optional<fs::path> currentFile_;
    std::string title_;
    std::string dialogFolder_;
    Uint32 openEvent_ = 0;
    Uint32 saveEvent_ = 0;
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
    bool lessonsOpen_ = false;  // the list of lessons covers the window
    int lessonAct_ = 0;         // the act it shows
    float lessonScroll_ = 0.0f;
    int topic_ = -1;            // the reading that covers the window, or -1
    float readingScroll_ = 0.0f;
    bool galleryOpen_ = false;  // so does the gallery of examples
    int example_ = -1;          // the example on the board, from the gallery, with its explanation over it
    int exampleVersion_ = 0;    // and the version of it that's on the board
    bool programMode_ = false;  // the player's own program computer is on the board
    int lesson_ = -1;           // the lesson on the board, or -1 for the player's own project
    app::LibraryLimit lessonLimit_;
    editor::LessonResult lessonResult_;
    std::uint64_t lessonRevision_ = UINT64_MAX;  // the board the result is about
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
    std::string reading_;
    Uint64 lastTick_ = 0;
    bool tableVisible_ = true;
    int tableScroll_ = 0;
    bool helpOpen_ = false;
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
    for (const app::PaletteItem& item : app.libraryGroups()[0].items) {
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
    const app::CodeLayout layout = app::codeLayout(app.width(), app.height());
    s.clickScreen(layout.assemble.x + 20.0f, layout.assemble.y + 10.0f);
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
    for (int i = 1; i + 1 < argc; i += 2) {
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
    if (script != nullptr && std::strcmp(script, "velocidade") == 0) startSpeedRun(app, window);
    if (script != nullptr && std::strncmp(script, "dentro:", 7) == 0) showInside(app, window, std::atoi(script + 7));
    if (script != nullptr && std::strcmp(script, "inicio") == 0) app.showHome();
    if (script != nullptr && std::strcmp(script, "aulas") == 0) app.showLessons();
    // leitura:N, the reading of topic N; leitura:N:S, scrolled S pixels down.
    if (script != nullptr && std::strncmp(script, "leitura:", 8) == 0) {
        app.openReading(std::atoi(script + 8));
        if (const char* colon = std::strchr(script + 8, ':')) app.scrollReading(static_cast<float>(std::atof(colon + 1)));
    }
    // ato:N, the list of lessons on act N.
    if (script != nullptr && std::strncmp(script, "ato:", 4) == 0) {
        app.showLessons();
        app.showAct(std::atoi(script + 4));
    }
    if (script != nullptr && std::strcmp(script, "galeria") == 0) app.showGallery();
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
    if (script != nullptr && std::strncmp(script, "aula:", 5) == 0) {
        Script s(app, window);
        app.startLesson(std::atoi(script + 5));
        app.draw("");
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
