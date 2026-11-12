// The languages: the catalog covers what a player reads, and the translations keep their blanks.
#include <cstdio>
#include <string>
#include <string_view>

#include "asm/assembler.hpp"
#include "check.hpp"
#include "editor/board.hpp"
#include "editor/boxes.hpp"
#include "editor/course.hpp"
#include "editor/gallery.hpp"
#include "editor/lessons.hpp"
#include "i18n/i18n.hpp"

using namespace bancada;
using namespace bancada::editor;

namespace {

bool inCatalog(std::string_view portuguese) {
    for (const i18n::Entry& entry : i18n::englishCatalog()) {
        if (entry.portuguese == portuguese) return true;
    }
    return false;
}

// Collects what's missing instead of failing on the first, so one run lists every gap.
struct Missing {
    std::string list;
    void need(std::string_view text) {
        if (!text.empty() && !inCatalog(text)) list += "\n    " + std::string(text);
    }
};

struct English {
    English() { i18n::setLanguage(i18n::Language::English); }
    ~English() { i18n::setLanguage(i18n::Language::Portuguese); }
    English(const English&) = delete;
    English& operator=(const English&) = delete;
};

}  // namespace

TEST(portuguese_is_the_text_itself_and_english_looks_it_up) {
    CHECK(std::string(i18n::tr("Aulas")) == std::string("Aulas"));
    CHECK(i18n::format("Linha {}: {}", {"3", "x"}) == std::string("Linha 3: x"));
    English english;
    CHECK(std::string(i18n::tr("Aulas")) == std::string("Lessons"));
    CHECK(i18n::format("Linha {}: {}", {"3", "x"}) == std::string("Line 3: x"));
    // Missing from the catalog: shown as it is, never blank.
    CHECK(std::string(i18n::tr("uma frase que não existe")) == std::string("uma frase que não existe"));
}

TEST(languages_have_codes_and_names) {
    CHECK(i18n::languageFromCode("en") == i18n::Language::English);
    CHECK(i18n::languageFromCode("pt") == i18n::Language::Portuguese);
    CHECK(!i18n::languageFromCode("xx").has_value());
    CHECK(std::string(i18n::codeOf(i18n::Language::English)) == std::string("en"));
    CHECK(std::string(i18n::nameOf(i18n::Language::Portuguese)) == std::string("Português"));
}

TEST(every_translation_keeps_its_blanks_and_formats) {
    for (const i18n::Entry& entry : i18n::englishCatalog()) {
        const auto count = [](std::string_view text, std::string_view what) {
            std::size_t n = 0;
            for (std::size_t at = text.find(what); at != std::string_view::npos; at = text.find(what, at + what.size())) ++n;
            return n;
        };
        CHECK_EQ(count(entry.portuguese, "{}"), count(entry.english, "{}"));
        CHECK_EQ(count(entry.portuguese, "%"), count(entry.english, "%"));
    }
}

TEST(the_lessons_are_all_in_english) {
    Missing missing;
    for (const LessonAct& act : lessonActs()) {
        missing.need(act.title);
        missing.need(act.line);
        for (const char* planned : act.planned) missing.need(planned);
    }
    for (const Lesson& lesson : lessons()) {
        missing.need(lesson.title);
        missing.need(lesson.story);
        missing.need(lesson.success);
        for (const char* step : lesson.steps) missing.need(step);
        for (const char* hint : lesson.hints) missing.need(hint);
    }
    if (!missing.list.empty()) std::printf("  missing:%s\n", missing.list.c_str());
    CHECK(missing.list.empty());
}

TEST(the_gallery_parts_and_boxes_are_all_in_english) {
    Missing missing;
    for (const char* group : galleryGroups()) missing.need(group);
    for (const GalleryItem& item : galleryItems()) {
        missing.need(item.title);
        missing.need(item.line);
        missing.need(item.about);
        missing.need(item.tryIt);
        for (const GalleryVersion& version : item.versions) {
            missing.need(version.name);
            missing.need(version.how);
        }
    }
    for (const ComputerProgram& program : computerPrograms()) {
        missing.need(program.title);
        missing.need(program.line);
    }
    for (std::size_t kind = 0; kind < kPartKindCount; ++kind) missing.need(partName(makePart(static_cast<PartKind>(kind))));
    for (std::size_t box = 1; box <= boxCount(); ++box) {
        missing.need(boxSpec(static_cast<BoxId>(box)).name);
        missing.need(boxSpec(static_cast<BoxId>(box)).group);
    }
    // Names that are the same in both languages need no entry.
    std::string list;
    for (std::size_t at = 0; at < missing.list.size();) {
        const std::size_t end = missing.list.find('\n', at + 1);
        const std::string line = missing.list.substr(at, end == std::string::npos ? std::string::npos : end - at);
        const std::string name = line.substr(5);
        static constexpr std::string_view kSame[] = {"NAND", "NOT", "AND", "OR", "NOR", "XOR", "XNOR", "Clock", "Chip", "Resistor", "NPN", "PNP",
                                                     "NMOS", "PMOS", "Capacitor", "LED", "MUX 2:1", "MUX 4:1", "DEMUX 1:2", "Latch SR", "Latch D",
                                                     "Flip-flop D", "Flip-flop T", "Flip-flop JK", "ROM", "RAM"};
        bool same = false;
        for (std::string_view each : kSame) same = same || name == each;
        if (!same) list += line;
        at = end == std::string::npos ? missing.list.size() : end;
    }
    if (!list.empty()) std::printf("  missing:%s\n", list.c_str());
    CHECK(list.empty());
}

// A ROM's program is shown in the chosen language, with the words assembled from the Portuguese: the
// two must make the very same words.
TEST(an_english_program_assembles_to_the_same_words) {
    int programs = 0;
    for (const i18n::Entry& entry : i18n::englishCatalog()) {
        if (entry.portuguese.empty() || entry.portuguese[0] != ';' || entry.portuguese.find('\n') == std::string_view::npos) continue;
        ++programs;
        const assembly::Result portuguese = assembly::assemble(std::string(entry.portuguese));
        const assembly::Result english = assembly::assemble(std::string(entry.english));
        CHECK(portuguese.errors.empty());
        CHECK(english.errors.empty());
        CHECK(portuguese.words == english.words);
    }
    CHECK(programs >= 10);
}

TEST(a_lesson_check_speaks_english) {
    English english;
    // A message that's a whole sentence is translated where it's shown, like every text on screen...
    const Lesson& first = lessons()[static_cast<std::size_t>(lessonIndex("acender-led"))];
    const LessonResult dark = checkLesson(first, first.start());
    CHECK(!dark.passed);
    CHECK(std::string(i18n::tr(dark.message)) == "Put an LED on the board.");
    // ...and one put together with numbers comes out of the check already in English.
    const Lesson& two = lessons()[static_cast<std::size_t>(lessonIndex("chaves-e"))];
    const LessonResult switches = checkLesson(two, two.start());
    CHECK(switches.message == "This lesson uses 2 switches.");
}

// The readings, every title, line and block, have their English.
TEST(the_readings_are_in_english) {
    Missing missing;
    for (const Topic& topic : topics()) {
        missing.need(topic.title);
        missing.need(topic.line);
        for (const TheoryBlock& block : topic.blocks) missing.need(block.text);
    }
    if (!missing.list.empty()) std::printf("readings without English:%s\n", missing.list.c_str());
    CHECK(missing.list.empty());
}

int main() { return check::runAll(); }
