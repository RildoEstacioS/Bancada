#include <stdexcept>
#include <utility>
#include <vector>

#include "sim/backend.hpp"
#include "sim/evaluate.hpp"

namespace bancada::sim {

namespace {

// Evaluates everything, every time. It is here to be simple enough to trust: event.cpp is tested
// against it on random circuits, and any future backend will be too.
class Interpreter final : public Backend {
public:
    explicit Interpreter(Program program) : program_(std::move(program)) {
        values_.resize(program_.slotCount);
        lastClock_.resize(program_.dffs.size());
        sampled_.resize(program_.dffs.size());
        lastPortClock_.resize(program_.ports.size());
        memories_.resize(program_.memories.size());
        reset();
    }

    void reset() override {
        values_.assign(values_.size(), 0);
        for (const auto& [slot, value] : program_.constants) values_[slot] = value;
        for (const DffInstr& dff : program_.dffs) values_[dff.q] = dff.resetValue;
        for (std::size_t i = 0; i < memories_.size(); ++i) resetMemory(memories_[i], program_.memories[i]);
        evaluateAll();
        for (std::size_t i = 0; i < program_.dffs.size(); ++i) lastClock_[i] = values_[program_.dffs[i].clock] & 1;
        for (std::size_t i = 0; i < program_.ports.size(); ++i) lastPortClock_[i] = values_[program_.ports[i].clock] & 1;
        evaluations_ = 0;
    }

    void set(NetId input, std::uint64_t value) override {
        if (input >= program_.isInput.size() || !program_.isInput[input]) {
            throw std::invalid_argument("only an input can be set");
        }
        values_[program_.slotOfNet[input]] = value & widthMask(program_.netWidth[input]);
    }

    std::uint64_t get(NetId net) const override {
        if (net >= program_.slotOfNet.size()) throw std::invalid_argument("unknown net");
        return values_[program_.slotOfNet[net]];
    }

    SettleResult settle() override {
        for (int round = 0; round < kMaxEdgeRounds; ++round) {
            const bool stuck = evaluateAll();
            if (!clockEdges()) return {stuck};
        }
        return {true};
    }

    std::uint64_t gateEvaluations() const override { return evaluations_; }

    std::uint64_t memoryWord(std::uint32_t memory, std::uint64_t address) const override {
        if (memory >= memories_.size() || address >= memories_[memory].size()) return 0;
        return memories_[memory][static_cast<std::size_t>(address)];
    }

private:
    // Returns true if a loop of gates never came to rest.
    bool evaluateAll() {
        bool stuck = false;
        const Instr* const instrs = program_.instrs.data();
        std::uint64_t* const values = values_.data();
        const ConcatPart* const parts = program_.concatParts.data();
        const MemoryWords* const memories = memories_.data();
        for (const Block& block : program_.blocks) {
            const Instr* const begin = instrs + block.first;
            const Instr* const end = begin + block.count;
            if (!block.loop) {
                for (const Instr* instr = begin; instr != end; ++instr) values[instr->dst] = evaluate(*instr, values, parts, memories);
                evaluations_ += block.count;
                continue;
            }
            const std::uint64_t limit = loopPassLimit(block.count);
            bool changed = true;
            for (std::uint64_t pass = 0; changed && pass < limit; ++pass) {
                changed = false;
                for (const Instr* instr = begin; instr != end; ++instr) {
                    const std::uint64_t value = evaluate(*instr, values, parts, memories);
                    if (value != values[instr->dst]) {
                        values[instr->dst] = value;
                        changed = true;
                    }
                }
                evaluations_ += block.count;
            }
            stuck = stuck || changed;
        }
        return stuck;
    }

    // All flip-flops on a rising clock sample first and store after, so that they act as one.
    bool clockEdges() {
        const std::size_t count = program_.dffs.size();
        bool any = false;
        for (std::size_t i = 0; i < count; ++i) {
            const DffInstr& dff = program_.dffs[i];
            const std::uint8_t clock = values_[dff.clock] & 1;
            const bool rising = clock != 0 && lastClock_[i] == 0;
            lastClock_[i] = clock;
            sampled_[i] = rising ? values_[dff.d] : values_[dff.q];
            any = any || rising;
        }
        writes_.clear();
        for (std::size_t i = 0; i < program_.ports.size(); ++i) {
            const PortInstr& port = program_.ports[i];
            const std::uint8_t clock = values_[port.clock] & 1;
            const bool rising = clock != 0 && lastPortClock_[i] == 0;
            lastPortClock_[i] = clock;
            any = any || rising;
            if (rising && (values_[port.enable] & 1) != 0) writes_.push_back({port.memory, values_[port.address], values_[port.data]});
        }
        if (!any) return false;
        for (std::size_t i = 0; i < count; ++i) values_[program_.dffs[i].q] = sampled_[i];
        for (const Write& write : writes_) memories_[write.memory][write.address] = write.data;
        return true;
    }

    Program program_;
    std::vector<std::uint64_t> values_;
    std::vector<std::uint8_t> lastClock_;
    std::vector<std::uint64_t> sampled_;
    std::vector<std::uint8_t> lastPortClock_;
    std::vector<MemoryWords> memories_;
    struct Write {
        std::uint32_t memory;
        std::uint64_t address;
        std::uint64_t data;
    };
    std::vector<Write> writes_;
    std::uint64_t evaluations_ = 0;
};

}  // namespace

std::unique_ptr<Backend> makeInterpreter(Program program) {
    return std::make_unique<Interpreter>(std::move(program));
}

}  // namespace bancada::sim
