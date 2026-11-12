// Draws a session, and says what's where on screen. Nothing here decides anything: it only reads.
#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "asm/assembler.hpp"
#include "editor/computer32_view.hpp"
#include "editor/computer_view.hpp"
#include "editor/session.hpp"
#include "editor/text_edit.hpp"

namespace bancada::app {

struct Vec2 {
    float x = 0;
    float y = 0;
};

// Board coordinates to screen pixels. Zoom is continuous, around the point under the mouse.
struct Camera {
    float originX = 0;  // the board point at the screen's top-left corner
    float originY = 0;
    float cellPixels = 16;

    Vec2 toScreen(float x, float y) const { return {(x - originX) * cellPixels, (y - originY) * cellPixels}; }
    Vec2 toBoard(float x, float y) const { return {x / cellPixels + originX, y / cellPixels + originY}; }

    void pan(float dx, float dy) {
        originX -= dx / cellPixels;
        originY -= dy / cellPixels;
    }

    void zoomAt(float x, float y, float factor) {
        const Vec2 before = toBoard(x, y);
        cellPixels = std::clamp(cellPixels * factor, 0.25f, 64.0f);
        const Vec2 after = toBoard(x, y);
        originX += before.x - after.x;
        originY += before.y - after.y;
    }
};

constexpr float kPanelWidth = 204.0f;
constexpr float kToolbarHeight = 44.0f;
constexpr float kStatusHeight = 30.0f;
constexpr float kPaletteTop = 92.0f;  // the folder buttons start here, under the search box
constexpr float kFolderHeight = 26.0f;
constexpr float kFolderGap = 4.0f;
constexpr int kFolderColumns = 3;
constexpr float kTileWidth = 88.0f;
constexpr float kTileHeight = 76.0f;
constexpr float kTileGap = 8.0f;
inline constexpr SDL_FRect kSearchBox{12.0f, 52.0f, 180.0f, 30.0f};

// The ready-made things in the library's Exemplos folder.
enum class Example : std::uint8_t { None, Computer, Cpu, LedCircuit, TransistorNand, Blinker, Gallery };

// The library: folders of parts, each of which can be dragged onto the board. The last folder holds the
// project's own circuits, so the list depends on the session.
struct PaletteItem {
    editor::PartKind kind = editor::PartKind::Nand;
    editor::BoxId box = editor::kNoBox;
    editor::ChipId chip = 0;
    std::string name;
    bool newChip = false;  // the row that makes a new one
    bool circuit = false;  // one of the project's circuits
    Example example = Example::None;
};

struct PaletteGroup {
    std::string name;
    std::vector<PaletteItem> items;
};

// In a lesson the library offers only the lesson's parts.
struct LibraryLimit {
    std::vector<editor::PartKind> kinds;
    std::vector<editor::BoxId> boxes;
};

// With a filter, only the parts whose names contain it, ignoring case and accents. The folders stay, in the
// same order, so that a folder's place in the list doesn't depend on what's being searched for. With a
// limit, only the parts it allows, in the folders that still have any.
std::vector<PaletteGroup> paletteGroups(const editor::Session& session, const std::string& filter = {}, const LibraryLimit* limit = nullptr);

// One folder at a time, its parts as tiles two to a row. A search shows every part that matches instead.
struct PaletteView {
    int folder = 0;
    float scroll = 0;
    std::string filter;
    bool searching = false;
};

struct PaletteHit {
    int group = -1;
    int item = -1;  // -1 on the folder's button
};

// Rectangles are on screen. A tile may be scrolled out of sight, and one that isn't shown has no height.
SDL_FRect paletteFolderRect(int group);
float paletteListTop(const std::vector<PaletteGroup>& groups);
std::vector<PaletteHit> paletteShown(const std::vector<PaletteGroup>& groups, const PaletteView& view);
SDL_FRect paletteTileRect(const std::vector<PaletteGroup>& groups, const PaletteView& view, int group, int item);
PaletteHit paletteAt(const std::vector<PaletteGroup>& groups, const PaletteView& view, float x, float y, int height);
float paletteContentHeight(const std::vector<PaletteGroup>& groups, const PaletteView& view);

// The buttons along the top, so that nothing depends on a key the keyboard might not have.
// Home isn't about the simulation: it's the way back to the first screen.
enum class SimButton : std::uint8_t { None, RunPause, Step, Reset, Slower, Faster, SpeedLabel, Home };

enum class Icon : std::uint8_t {
    None, Trash, Copy, Cut, Paste, Rotate, Undo, Redo, Play, Pause, Step, Reset, Minus, Plus, ChevronRight, ChevronDown, Search, Home
};

struct ToolButton {
    const char* label;  // shown when the mouse rests on it
    editor::Key key;
    float width;
    SimButton sim = SimButton::None;
    Icon icon = Icon::None;
    float gapAfter = 0.0f;
};

inline constexpr ToolButton kToolbar[] = {
    {"Tela inicial", editor::Key::Escape, 34, SimButton::Home, Icon::Home, 18.0f},
    {"Apagar (Del)", editor::Key::Delete, 34, SimButton::None, Icon::Trash},
    {"Copiar (Ctrl+C)", editor::Key::Copy, 34, SimButton::None, Icon::Copy},
    {"Recortar (Ctrl+X)", editor::Key::Cut, 34, SimButton::None, Icon::Cut},
    {"Colar (Ctrl+V)", editor::Key::Paste, 34, SimButton::None, Icon::Paste},
    {"Girar (R)", editor::Key::Rotate, 34, SimButton::None, Icon::Rotate},
    {"Desfazer (Ctrl+Z)", editor::Key::Undo, 34, SimButton::None, Icon::Undo},
    {"Refazer (Ctrl+Y)", editor::Key::Redo, 34, SimButton::None, Icon::Redo, 18.0f},
    {"Rodar (Espa\u00e7o)", editor::Key::Escape, 34, SimButton::RunPause, Icon::Play},
    {"Avan\u00e7ar meio ciclo (N)", editor::Key::Escape, 34, SimButton::Step, Icon::Step},
    {"Zerar", editor::Key::Escape, 34, SimButton::Reset, Icon::Reset, 18.0f},
    {"Mais devagar", editor::Key::Escape, 28, SimButton::Slower, Icon::Minus},
    {"", editor::Key::Escape, 72, SimButton::SpeedLabel},
    {"Mais r\u00e1pido", editor::Key::Escape, 28, SimButton::Faster, Icon::Plus},
};

int toolbarAt(float x, float y);  // an index into kToolbar, or -1
float toolbarButtonX(int index);

// The panel on the right: the selected part's properties, above the truth table.
enum class PanelButton : std::uint8_t {
    None, InputsDown, InputsUp, BitsDown, BitsUp, AddressDown, AddressUp, SizeDown, SizeUp, EditName, EditValue, EditContents, OpenChip,
    EditProgram
};

struct PanelControl {
    PanelButton button;
    SDL_FRect rect;
};

float rightPanelWidth(const editor::Session& session);
std::vector<PanelControl> panelControls(const editor::Session& session, float panelLeft);
PanelButton rightPanelAt(const editor::Session& session, float x, float y, int width);
SDL_FRect panelButtonRect(const editor::Session& session, PanelButton button, float panelLeft);
int truthRowsVisible(const editor::Session& session, int height);

struct LessonStrip;
struct ExampleStrip;
struct DebugView;

struct InterfaceText {
    bool programShown = false;  // the program editor is the view now
    const char* dataNote = "";
    const char* profile = "";
    const char* project = "";
    bool dirty = false;
    const char* note = nullptr;  // a passing message, shown instead of the usual hint
    bool noteIsError = false;
    bool showTable = true;
    int tableScroll = 0;
    bool showHelp = false;
    float mouseX = -1.0f;
    float mouseY = -1.0f;
    bool searching = false;
    const editor::ComputerState* computer = nullptr;  // a computer on this board, which the panel can show
    const editor::Computer32State* computer32 = nullptr;  // or the 32-bit one
    const std::string* console = nullptr;                 // what the 32-bit one printed with syscall
    const LessonStrip* lesson = nullptr;              // during a lesson
    const ExampleStrip* example = nullptr;            // while an example from the gallery is open
    const LibraryLimit* limit = nullptr;              // the library a lesson allows
    int view = 0;                                     // kViewBoard, kViewPanel or kViewXray
    int xrayStage = 0;                                // the step the X-ray shows, 0 to 5
    float xrayProgress = 0.0f;                        // seconds into it, for the values moving along the wires
    const DebugView* debug = nullptr;                 // the debugger beside the X-ray
    const char* reading = "";  // what the electric part under the mouse is doing
};

// An electric part's reading in a sentence: "Resistor 330 Ω: 3 V, 9,1 mA, 27,5 mW".
std::string describeReading(const editor::Part& part, const editor::PartReading& reading);

// The board, the panel that shows a computer running on it, or the X-ray of the instruction it's on.
constexpr int kViewBoard = 0;
constexpr int kViewPanel = 1;
constexpr int kViewXray = 2;
constexpr int kViewCount = 3;
SDL_FRect viewSwitchRect(float rightEdge, int which);
SDL_FRect panelNextButton(const SDL_FRect& area);
SDL_FRect xrayStageRect(const SDL_FRect& area, int stage);  // the button for one of the six steps

// The debugger beside the X-ray, the way a debugger in an IDE looks: the program with the line about to
// run and the breakpoints, the registers with the ones the last instruction changed, the memory, and the
// buttons to run, pause and step.
constexpr float kDebugWidth = 340.0f;

struct DebugView {
    const std::vector<std::uint16_t>* breakpoints = nullptr;  // ROM addresses
    std::array<std::uint16_t, 8> before{};                    // the registers before the last instruction
    std::array<std::uint32_t, 32> before32{};                 // the same, on the 32-bit computer
    std::vector<std::uint16_t> memoryBefore;
    bool hasBefore = false;
    bool running = false;
};

enum class DebugAction : std::uint8_t { None, Continue, Pause, Step, Stage, Reset, Breakpoint };

struct DebugHit {
    DebugAction action = DebugAction::None;
    std::uint16_t address = 0;  // for Breakpoint
};

DebugHit debuggerAt(const editor::ComputerState& state, const SDL_FRect& area, float x, float y);
void drawDebugger(SDL_Renderer* renderer, const editor::ComputerState& state, const DebugView& view, const SDL_FRect& area, float mouseX, float mouseY);

// The same for the 32-bit computer, and its panel: all 32 registers, the console, the data and the stack. Its
// X-ray is still to come; for now that view says so.
DebugHit debugger32At(const editor::Computer32State& state, const SDL_FRect& area, float x, float y);
void drawDebugger32(SDL_Renderer* renderer, const editor::Computer32State& state, const DebugView& view, const std::string& console,
                    const SDL_FRect& area, float mouseX, float mouseY);
void drawComputer32Panel(SDL_Renderer* renderer, const editor::Computer32State& state, const std::string& console, const DebugView* debug,
                         const SDL_FRect& area, float mouseX, float mouseY);
void drawXray32(SDL_Renderer* renderer, const SDL_FRect& area);

// The first screen. lessonsLine says how far the player got in the lessons.
// Portuguese and English are the language switch, top right: they don't leave the home screen.
enum class HomeChoice : std::uint8_t {
    None, Continue, New, Example, Open, Lessons, Program, Portuguese, English, Computers, Computer16, Computer32, Back,
};
// The home's first page, or, with computers set, the page that picks a computer to program.
HomeChoice homeAt(float x, float y, int width, int height, bool computers);
void drawHome(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY, const std::string& lessonsLine, bool computers);

// The gallery: every example, group by group in columns.
constexpr int kGalleryBack = -2;
int galleryAt(float x, float y, int width, int height);  // an index into editor::galleryItems(), kGalleryBack, or -1
void drawGallery(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY);

// The lessons: the acts down the left, each with its progress and a lock until the one before is done;
// on the right, the chosen act's readings, each followed by its exercises.
struct LessonListView {
    int act = 0;
    float scroll = 0.0f;
    const std::vector<std::string>* finished = nullptr;
    bool confirming = false;  // the reset button was pressed once, and asks to be pressed again
    bool unlocked = false;    // the developer's switch: every act open, whatever is done
    bool denied = false;      // a wrong password was just typed
};
enum class LessonListKind : std::uint8_t { None, Back, Reset, Act, Reading, Exercise, Unlock };
struct LessonListHit {
    LessonListKind kind = LessonListKind::None;
    int index = -1;  // the act, the topic or the lesson
};
LessonListHit lessonListAt(float x, float y, int width, int height, const LessonListView& view);
float lessonListMaxScroll(int width, int height, const LessonListView& view);
void drawLessonList(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY, const LessonListView& view);

// A reading: the theory of a topic, text and live circuits, before its exercises.
struct ReadingView {
    int topic = 0;
    float scroll = 0.0f;
    bool read = false;  // already read once
};
enum class ReadingButton : std::uint8_t { None, Back, Exercises };
ReadingButton readingAt(SDL_Renderer* renderer, float x, float y, int width, int height, const ReadingView& view);
float readingMaxScroll(SDL_Renderer* renderer, int width, int height, const ReadingView& view);
void drawReading(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY, const ReadingView& view);

// The strip over the board during a lesson: what it's about, the steps, and its buttons.
constexpr float kLessonStripHeight = 236.0f;
enum class LessonButton : std::uint8_t { None, Check, Hint, Restart, List, Next, Program };

struct LessonStrip {
    int index = 0;
    std::string message;   // shown under the steps
    bool passed = false;
    bool problem = false;  // the message says what's wrong
    bool hint = false;     // the message is a hint
    bool hasNext = false;
    bool program = false;  // a programming lesson: its first button opens the code editor
};

LessonButton lessonStripAt(const SDL_FRect& area, const LessonStrip& strip, float x, float y);
void drawLessonStrip(SDL_Renderer* renderer, const SDL_FRect& area, const LessonStrip& strip, float mouseX, float mouseY);

// The strip over the board while an example is open, as high as the lesson's: what the example is, how
// the version on the board works and what to try, and on the right a button for each version and one
// back to every example.
struct ExampleStrip {
    int item = 0;     // into editor::galleryItems()
    int version = 0;
};
constexpr int kExampleStripAll = -2;
int exampleStripAt(const SDL_FRect& area, const ExampleStrip& strip, float x, float y);  // a version, kExampleStripAll, or -1

// Over the panel and the X-ray, a program from the gallery says what it does: this much room, or none.
float programAboutHeight(const ExampleStrip* example, int view);
void drawExampleStrip(SDL_Renderer* renderer, const SDL_FRect& area, const ExampleStrip& strip, float mouseX, float mouseY);

struct ProfileList {
    std::vector<std::string> names;
    std::size_t current = 0;
    std::string typing;
};

constexpr float kProfileBoxWidth = 420.0f;
constexpr float kProfileRowHeight = 30.0f;

// The screen rectangle of a row in the profile list.
SDL_FRect profileRow(std::size_t row, std::size_t rows, int width, int height);

// The program editor covers the window: code on the left, the problems under it, and the instructions on
// the right, so that nobody has to leave to look one up.
constexpr float kCodeBarHeight = 40.0f;
constexpr float kCodeReferenceWidth = 348.0f;
constexpr float kCodeProblemsHeight = 104.0f;
constexpr float kCodeGutter = 52.0f;
constexpr float kCodeCharWidth = 8.0f;
constexpr float kCodeLineHeight = 20.0f;

struct CodeLayout {
    SDL_FRect help;
    SDL_FRect text;  // the code itself, without the line numbers
    SDL_FRect problems;
    SDL_FRect assemble;
    SDL_FRect close;
    int lines = 1;  // how many fit
    int columns = 1;
};

CodeLayout codeLayout(int width, int height);

struct CodeView {
    const editor::TextEdit* text = nullptr;
    const std::vector<assembly::Error>* problems = nullptr;
    std::size_t words = 0;
    std::size_t capacity = 0;
    int scrollLine = 0;
    int scrollColumn = 0;
    const char* warning = "";
    bool help = false;        // the list of every instruction covers the code
    float helpScroll = 0.0f;
    // The column on the right has tabs: the lesson's task (in a lesson), the instructions, and the program as
    // blocks, one per word, the one running lit.
    int tab = 0;
    std::vector<std::uint16_t> program;  // the program as it assembles now
    bool mips = false;                     // the 32-bit computer's ROM: MIPS, with its own commands and help
    std::vector<std::uint32_t> program32;  // its program as it assembles now
    std::vector<int> lines;            // for each word, its line, counted from 1
    int runningLine = -1;              // the line the computer is on, when the ROM holds this very text
    int runningAddress = -1;
    // During a programming lesson, its task is the first tab.
    int lesson = -1;
    std::string lessonMessage;
    bool lessonPassed = false;
    bool lessonProblem = false;
};

// How far the instruction list scrolls.
float codeHelpMaxScroll(SDL_Renderer* renderer, int width, int height, bool mips);

// The tabs of the editor's right column: which one is at this point, or -1.
constexpr int kCodeTabTask = 0;
constexpr int kCodeTabCommands = 1;
constexpr int kCodeTabBlocks = 2;
int codeTabAt(const CodeView& view, float x, float y, int width, int height);

// The toolbar's button that shows the program, beside Placa, Painel and Raio-X.
SDL_FRect programSwitchRect(float rightEdge);

// Which problem is listed at this height, or -1.
int codeProblemAt(const CodeView& view, float y, int width, int height);

void drawCode(SDL_Renderer* renderer, const CodeView& view, int width, int height);

// Lets go of the font texture. Call before destroying the renderer.
void releaseDrawing();
void drawBoard(SDL_Renderer* renderer, const editor::Session& session, const Camera& camera, int width, int height);
void drawInterface(SDL_Renderer* renderer, const editor::Session& session, const PaletteView& palette, int width, int height,
                   const InterfaceText& text);
void drawProfiles(SDL_Renderer* renderer, const ProfileList& list, int width, int height);
// A field to type in. The unit, if any, stands after the text and isn't part of it.
void drawPrompt(SDL_Renderer* renderer, const char* title, const std::string& text, int width, int height, const char* unit = "");

}  // namespace bancada::app
