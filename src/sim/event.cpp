// Evaluates only what can have changed. Instructions are kept in dependency order and "needs a look"
// is one bit per instruction, so walking the set bits upward visits each gate at most once, after
// everything it depends on, and a change can only ever mark gates further along. No queue, no sorting.
// A second layer of bits says which words have anything set, so a huge circuit that is mostly still
// costs nothing to walk.
//
// A loop of gates runs as a whole, and one that never came to rest runs again on every pass, because
// its outputs are still moving even though no input changed.
#include <algorithm>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "sim/backend.hpp"
#include "sim/evaluate.hpp"

namespace bancada::sim {

namespace {

constexpr std::int32_t kNotInLoop = -1;

enum ClockState : std::uint8_t { NotAClock, Clock, ClockTouched };

class EventSimulator final : public Backend {
public:
    explicit EventSimulator(Program program) : program_(std::move(program)) {
        values_.resize(program_.slotCount);
        dirty_.resize((program_.instrs.size() + 63) / 64);
        dirtyWords_.resize((dirty_.size() + 63) / 64);
        lastClock_.resize(program_.slotCount);
        clockState_.resize(program_.slotCount, NotAClock);
        memories_.resize(program_.memories.size());
        buildFanout();
        buildClockGroups();
        loopOf_.assign(program_.instrs.size(), kNotInLoop);
        for (std::size_t b = 0; b < program_.blocks.size(); ++b) {
            const Block& block = program_.blocks[b];
            if (!block.loop) continue;
            for (std::uint32_t i = 0; i < block.count; ++i) loopOf_[block.first + i] = static_cast<std::int32_t>(b);
        }
        reset();
    }

    void reset() override {
        values_.assign(values_.size(), 0);
        for (const auto& [slot, value] : program_.constants) values_[slot] = value;
        for (const DffInstr& dff : program_.dffs) values_[dff.q] = dff.resetValue;
        for (std::size_t i = 0; i < memories_.size(); ++i) resetMemory(memories_[i], program_.memories[i]);

        stuckBlocks_.clear();
        for (std::size_t i = 0; i < program_.instrs.size(); ++i) mark(static_cast<std::uint32_t>(i));
        evaluatePass();

        touchedClocks_.clear();
        for (Slot slot = 0; slot < program_.slotCount; ++slot) {
            if (clockState_[slot] == NotAClock) continue;
            clockState_[slot] = Clock;
            lastClock_[slot] = values_[slot] & 1;
        }
        evaluations_ = 0;
    }

    void set(NetId input, std::uint64_t value) override {
        if (input >= program_.isInput.size() || !program_.isInput[input]) {
            throw std::invalid_argument("only an input can be set");
        }
        const Slot slot = program_.slotOfNet[input];
        const std::uint64_t masked = value & widthMask(program_.netWidth[input]);
        if (values_[slot] == masked) return;
        values_[slot] = masked;
        changed(slot);
    }

    std::uint64_t get(NetId net) const override {
        if (net >= program_.slotOfNet.size()) throw std::invalid_argument("unknown net");
        return values_[program_.slotOfNet[net]];
    }

    SettleResult settle() override {
        for (int round = 0; round < kMaxEdgeRounds; ++round) {
            const bool stuck = evaluatePass();
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
    void buildFanout() {
        std::vector<std::vector<std::uint32_t>> users(program_.slotCount);
        std::vector<Slot> reads;
        readers_.resize(program_.memories.size());
        for (std::size_t i = 0; i < program_.instrs.size(); ++i) {
            const Instr& instr = program_.instrs[i];
            reads.clear();
            if (instr.op == Op::MemRead) readers_[instr.b].push_back(static_cast<std::uint32_t>(i));
            if (instr.op == Op::Concat) {
                for (Slot p = 0; p < instr.b; ++p) reads.push_back(program_.concatParts[instr.a + p].slot);
            } else {
                reads.push_back(instr.a);
                if (instr.op == Op::Nand) reads.push_back(instr.b);
            }
            std::sort(reads.begin(), reads.end());
            reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
            for (Slot slot : reads) users[slot].push_back(static_cast<std::uint32_t>(i));
        }
        fanoutStart_.reserve(users.size() + 1);
        fanoutStart_.push_back(0);
        for (const std::vector<std::uint32_t>& list : users) {
            fanout_.insert(fanout_.end(), list.begin(), list.end());
            fanoutStart_.push_back(static_cast<std::uint32_t>(fanout_.size()));
        }
    }

    // Flip-flops sorted by clock, so that a clock's flip-flops are one run of dffsByClock_.
    void buildClockGroups() {
        dffsByClock_ = program_.dffs;
        std::stable_sort(dffsByClock_.begin(), dffsByClock_.end(),
                         [](const DffInstr& a, const DffInstr& b) { return a.clock < b.clock; });
        clockStart_.assign(program_.slotCount + std::size_t{1}, 0);
        for (const DffInstr& dff : dffsByClock_) {
            ++clockStart_[dff.clock + std::size_t{1}];
            clockState_[dff.clock] = Clock;
        }
        for (std::size_t slot = 1; slot < clockStart_.size(); ++slot) clockStart_[slot] += clockStart_[slot - 1];

        portsByClock_ = program_.ports;
        std::stable_sort(portsByClock_.begin(), portsByClock_.end(),
                         [](const PortInstr& a, const PortInstr& b) { return a.clock < b.clock; });
        portStart_.assign(program_.slotCount + std::size_t{1}, 0);
        for (const PortInstr& port : portsByClock_) {
            ++portStart_[port.clock + std::size_t{1}];
            clockState_[port.clock] = Clock;
        }
        for (std::size_t slot = 1; slot < portStart_.size(); ++slot) portStart_[slot] += portStart_[slot - 1];
    }

    void mark(std::uint32_t instr) {
        const std::uint32_t word = instr >> 6;
        dirty_[word] |= std::uint64_t{1} << (instr & 63);
        dirtyWords_[word >> 6] |= std::uint64_t{1} << (word & 63);
    }

    // Something read from this slot is out of date now.
    void changed(Slot slot) {
        const std::uint32_t end = fanoutStart_[slot + std::size_t{1}];
        for (std::uint32_t i = fanoutStart_[slot]; i < end; ++i) mark(fanout_[i]);
        if (clockState_[slot] == Clock) {
            clockState_[slot] = ClockTouched;
            touchedClocks_.push_back(slot);
        }
    }

    // Returns true if a loop of gates never came to rest.
    bool evaluatePass() {
        for (std::uint32_t block : stuckBlocks_) mark(program_.blocks[block].first);
        stuckBlocks_.clear();

        const Instr* const instrs = program_.instrs.data();
        const ConcatPart* const parts = program_.concatParts.data();
        const MemoryWords* const memories = memories_.data();
        for (std::size_t summary = 0; summary < dirtyWords_.size(); ++summary) {
            while (dirtyWords_[summary] != 0) {
                const auto wordBit = static_cast<unsigned>(std::countr_zero(dirtyWords_[summary]));
                const std::size_t word = summary * 64 + wordBit;
                while (dirty_[word] != 0) {
                    const auto bit = static_cast<unsigned>(std::countr_zero(dirty_[word]));
                    const auto index = static_cast<std::uint32_t>(word * 64 + bit);
                    dirty_[word] &= dirty_[word] - 1;
                    if (loopOf_[index] != kNotInLoop) {
                        runLoop(static_cast<std::uint32_t>(loopOf_[index]));
                        continue;
                    }
                    const Instr& instr = instrs[index];
                    const std::uint64_t value = evaluate(instr, values_.data(), parts, memories);
                    ++evaluations_;
                    if (value != values_[instr.dst]) {
                        values_[instr.dst] = value;
                        changed(instr.dst);
                    }
                }
                // Cleared last: gates in this word may have marked other gates in it. A word left
                // empty by runLoop is simply passed over here.
                dirtyWords_[summary] &= ~(std::uint64_t{1} << wordBit);
            }
        }
        return !stuckBlocks_.empty();
    }

    void runLoop(std::uint32_t blockIndex) {
        const Block& block = program_.blocks[blockIndex];
        const Instr* const begin = program_.instrs.data() + block.first;
        const Instr* const end = begin + block.count;
        std::uint64_t* const values = values_.data();
        const ConcatPart* const parts = program_.concatParts.data();
        const MemoryWords* const memories = memories_.data();

        before_.clear();
        for (const Instr* instr = begin; instr != end; ++instr) before_.push_back(values[instr->dst]);

        const std::uint64_t limit = loopPassLimit(block.count);
        bool moving = true;
        for (std::uint64_t pass = 0; moving && pass < limit; ++pass) {
            moving = false;
            for (const Instr* instr = begin; instr != end; ++instr) {
                const std::uint64_t value = evaluate(*instr, values, parts, memories);
                if (value != values[instr->dst]) {
                    values[instr->dst] = value;
                    moving = true;
                }
            }
            evaluations_ += block.count;
        }
        if (moving) stuckBlocks_.push_back(blockIndex);

        for (std::uint32_t i = 0; i < block.count; ++i) {
            const Slot slot = begin[i].dst;
            if (values[slot] != before_[i]) changed(slot);
        }
        // The loop just ran, so nothing inside it needs another look on this pass.
        for (std::uint32_t i = block.first; i < block.first + block.count; ++i) {
            dirty_[i >> 6] &= ~(std::uint64_t{1} << (i & 63));
        }
    }

    // All flip-flops on a rising clock sample first and store after, so that they act as one.
    bool clockEdges() {
        rising_.clear();
        writes_.clear();
        bool anyRose = false;
        for (Slot slot : touchedClocks_) {
            clockState_[slot] = Clock;
            const std::uint8_t clock = values_[slot] & 1;
            const bool rose = clock != 0 && lastClock_[slot] == 0;
            lastClock_[slot] = clock;
            if (!rose) continue;
            anyRose = true;
            for (std::uint32_t i = clockStart_[slot]; i < clockStart_[slot + std::size_t{1}]; ++i) {
                rising_.emplace_back(dffsByClock_[i].q, values_[dffsByClock_[i].d]);
            }
            for (std::uint32_t i = portStart_[slot]; i < portStart_[slot + std::size_t{1}]; ++i) {
                const PortInstr& port = portsByClock_[i];
                if ((values_[port.enable] & 1) != 0) writes_.push_back({port.memory, values_[port.address], values_[port.data]});
            }
        }
        touchedClocks_.clear();
        if (!anyRose) return false;
        for (const auto& [q, value] : rising_) {
            if (values_[q] == value) continue;
            values_[q] = value;
            changed(q);
        }
        for (const Write& write : writes_) {
            std::uint64_t& word = memories_[write.memory][write.address];
            if (word == write.data) continue;
            word = write.data;
            for (std::uint32_t reader : readers_[write.memory]) mark(reader);
        }
        return true;
    }

    Program program_;
    std::vector<std::uint64_t> values_;
    std::vector<std::uint64_t> dirty_;          // one bit per instruction
    std::vector<std::uint64_t> dirtyWords_;     // one bit per word of dirty_
    std::vector<std::uint32_t> fanoutStart_;    // by slot, indexes fanout_
    std::vector<std::uint32_t> fanout_;         // instructions that read a slot
    std::vector<std::int32_t> loopOf_;          // by instruction: its block, if it's in a loop

    std::vector<DffInstr> dffsByClock_;
    std::vector<std::uint32_t> clockStart_;     // by slot, indexes dffsByClock_
    std::vector<std::uint8_t> clockState_;      // by slot
    std::vector<std::uint8_t> lastClock_;       // by slot
    std::vector<Slot> touchedClocks_;

    std::vector<PortInstr> portsByClock_;
    std::vector<std::uint32_t> portStart_;      // by slot, indexes portsByClock_
    std::vector<MemoryWords> memories_;
    std::vector<std::vector<std::uint32_t>> readers_;  // by memory: instructions that read it
    struct Write {
        std::uint32_t memory;
        std::uint64_t address;
        std::uint64_t data;
    };
    std::vector<Write> writes_;

    std::vector<std::uint32_t> stuckBlocks_;
    std::vector<std::uint64_t> before_;
    std::vector<std::pair<Slot, std::uint64_t>> rising_;
    std::uint64_t evaluations_ = 0;
};

}  // namespace

std::unique_ptr<Backend> makeEventSimulator(Program program) {
    return std::make_unique<EventSimulator>(std::move(program));
}

}  // namespace bancada::sim
