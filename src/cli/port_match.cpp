#include "cli/port_match.hpp"

#include "core/pe_image.hpp"

#include <Zydis/Zydis.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>

namespace anvil::cli {
namespace {

constexpr size_t kMaxInstruction = 15;
constexpr uint32_t kFirstImageOffset = 0x1000;  // below it lie only the headers
constexpr uint32_t kMinPairedSize = 16;  // shorter code proves nothing by being unique
constexpr uint32_t kMinShapePairedSize = 64;  // with the operands gone, it takes more
constexpr uint32_t kMinWindow = 16;
constexpr uint32_t kMinShapeWindow = 32;
constexpr size_t kMaxTableWalk = 4096;
constexpr size_t kMaxCopies = 64;
constexpr int kMaxTableReach = 256;
constexpr int kMinTableEvidence = 4;  // slots that agree when the slot's own function changed

uint64_t fnv1a(const uint8_t* data, size_t size) {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

std::string hex(uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
    return text;
}

// The first offsets of `needle` in `haystack`, at most `limit` of them.
std::vector<uint32_t> find_all(const uint8_t* haystack, uint32_t start, uint32_t end, const uint8_t* needle, uint32_t size,
                               size_t limit) {
    std::vector<uint32_t> found;
    if (size == 0 || end < start + size) return found;
    const std::boyer_moore_horspool_searcher searcher(needle, needle + size);
    const uint8_t* at = haystack + start;
    const uint8_t* const stop = haystack + end;
    while (found.size() < limit) {
        const uint8_t* hit = std::search(at, stop, searcher);
        if (hit == stop) break;
        found.push_back(static_cast<uint32_t>(hit - haystack));
        at = hit + 1;
    }
    return found;
}

// The longest run of `pairs`, sorted by `from`, in which `to` rises too: pairs
// of two builds keep the order the linker gave them, so what falls out is wrong.
template <typename Pair> std::vector<Pair> rising_run(const std::vector<Pair>& pairs) {
    std::vector<uint32_t> tails, tail_index, previous(pairs.size());
    for (uint32_t i = 0; i < pairs.size(); ++i) {
        const auto place = std::lower_bound(tails.begin(), tails.end(), pairs[i].to);
        const auto length = static_cast<size_t>(place - tails.begin());
        previous[i] = length > 0 ? tail_index[length - 1] : UINT32_MAX;
        if (place == tails.end()) {
            tails.push_back(pairs[i].to);
            tail_index.push_back(i);
        } else {
            *place = pairs[i].to;
            tail_index[length] = i;
        }
    }
    std::vector<Pair> kept;
    for (uint32_t i = tail_index.empty() ? UINT32_MAX : tail_index.back(); i != UINT32_MAX; i = previous[i]) kept.push_back(pairs[i]);
    std::reverse(kept.begin(), kept.end());
    return kept;
}

// Whether the memory operand's base or index is one of `registers`, a bit per register.
bool uses_image_base(const ZydisDecodedInstruction& instruction, uint32_t registers) {
    const auto& raw = instruction.raw;
    const bool sib = (instruction.attributes & ZYDIS_ATTRIB_HAS_SIB) != 0;
    const uint32_t base = (sib ? raw.sib.base : raw.modrm.rm) | (raw.rex.B << 3);
    const uint32_t index = raw.sib.index | (raw.rex.X << 3);
    return ((registers >> base) & 1) != 0 || (sib && ((registers >> index) & 1) != 0);
}

bool same_shape(const BuildImage::Code& a, const BuildImage::Code& b) {
    return a.shape_hash == b.shape_hash && a.end - a.start == b.end - b.start;
}

}  // namespace

std::optional<BuildImage> BuildImage::load(const std::filesystem::path& exe, std::string& error) {
    const auto reader = FileImageReader::load(exe, error);
    if (!reader) return std::nullopt;
    const auto pe = PeImage::parse(*reader, error);
    if (!pe) return std::nullopt;

    BuildImage image;
    image.base_ = reader->base();
    image.bytes_.resize(pe->size_of_image());
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!reader->read(0, &dos, sizeof(dos)) || !reader->read(dos.e_lfanew, &nt, sizeof(nt))) {
        error = "cannot read the PE headers";
        return std::nullopt;
    }
    for (const Section& s : pe->sections()) {
        if (s.rva + s.size > image.bytes_.size() || !reader->read(s.rva, image.bytes_.data() + s.rva, s.size)) {
            error = "cannot read section " + s.name;
            return std::nullopt;
        }
        image.sections_.push_back({s.rva, s.rva + s.size, s.kind == MemoryKind::Code});
    }
    image.norm_ = image.bytes_;
    image.shape_ = image.bytes_;

    // .pdata names every function with a frame; leaf functions lie in the gaps.
    const IMAGE_DATA_DIRECTORY unwind = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    std::vector<std::pair<uint32_t, uint32_t>> entries;
    if (unwind.VirtualAddress != 0 && unwind.VirtualAddress + unwind.Size <= image.bytes_.size()) {
        const uint8_t* table = image.bytes_.data() + unwind.VirtualAddress;
        for (size_t i = 0; i + sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY) <= unwind.Size; i += sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY)) {
            IMAGE_RUNTIME_FUNCTION_ENTRY entry{};
            std::memcpy(&entry, table + i, sizeof(entry));
            if (entry.BeginAddress < entry.EndAddress) entries.emplace_back(entry.BeginAddress, entry.EndAddress);
        }
    }
    std::sort(entries.begin(), entries.end());
    for (const Span& section : image.sections_) {
        if (!section.code) continue;
        // A gap between entries: leaf functions separated by int3 padding.
        const auto add_gap = [&](uint32_t start, uint32_t end) {
            uint32_t at = start;
            while (at < end) {
                while (at < end && image.bytes_[at] == 0xCC) ++at;
                uint32_t stop = at;
                while (stop < end && !(image.bytes_[stop] == 0xCC && stop + 1 < end && image.bytes_[stop + 1] == 0xCC)) ++stop;
                if (stop > at) image.code_.push_back({at, stop, 0});
                at = stop;
            }
        };
        uint32_t at = section.start;
        for (const auto& [begin, end] : entries) {
            if (begin < at || end > section.end) continue;
            add_gap(at, begin);
            image.code_.push_back({begin, end, 0});
            at = end;
        }
        add_gap(at, section.end);
    }
    for (Code& code : image.code_) image.zero_code(code);
    std::sort(image.references_.begin(), image.references_.end(),
              [](const Reference& a, const Reference& b) { return a.field < b.field; });
    return image;
}

void BuildImage::zero_code(Code& code) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    const uint32_t image_size = size();
    const auto blank = [&](uint32_t where, uint32_t width) {
        std::memset(norm_.data() + where, 0, width);
        std::memset(shape_.data() + where, 0, width);
    };
    std::set<uint32_t> tables;  // jump tables this code has named, ahead of `at`
    // Registers `lea reg, [__ImageBase]` has loaded in this code. Only through them
    // is a displacement an RVA (a jump table: `[r8+rax*4+rva]`): by its value alone
    // it may as well be a large struct offset, which is an operand like any other.
    // Code that got the register elsewhere looks changed, the safe way to be wrong.
    uint32_t image_base_registers = 0;
    uint32_t at = code.start;
    while (at < code.end) {
        if (!tables.empty() && at >= *tables.begin()) {
            // RVAs into this code, one after another, until something else starts.
            at = *tables.begin();
            tables.erase(tables.begin());
            while (at + 4 <= code.end) {
                uint32_t entry = 0;
                std::memcpy(&entry, bytes_.data() + at, 4);
                if (entry < code.start || entry >= code.end) break;
                blank(at, 4);
                at += 4;
            }
            continue;
        }
        ZydisDecodedInstruction instruction;
        const size_t available = std::min<size_t>(code.end - at, kMaxInstruction);
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, nullptr, bytes_.data() + at, available, &instruction))) {
            ++at;
            continue;
        }
        const uint32_t next = at + instruction.length;
        for (const auto& imm : instruction.raw.imm) {
            if (imm.size == 0) continue;
            const uint32_t width = imm.size / 8u;
            if (!imm.is_relative) {
                std::memset(shape_.data() + at + imm.offset, 0, width);
                continue;
            }
            blank(at + imm.offset, width);
            const uint32_t target = next + static_cast<uint32_t>(static_cast<int32_t>(imm.value.s));
            if (width == 4 && (target < code.start || target >= code.end)) references_.push_back({at + imm.offset, target});
        }
        const auto& raw = instruction.raw;
        if (raw.disp.size == 32) {
            uint32_t disp = 0;
            std::memcpy(&disp, bytes_.data() + at + raw.disp.offset, 4);
            const bool rip = (instruction.attributes & ZYDIS_ATTRIB_HAS_MODRM) != 0 && raw.modrm.mod == 0 && raw.modrm.rm == 5;
            if (rip) {
                blank(at + raw.disp.offset, 4);
                references_.push_back({at + raw.disp.offset, next + disp});
                if (instruction.mnemonic == ZYDIS_MNEMONIC_LEA && next + disp == 0) {
                    image_base_registers |= 1u << (raw.modrm.reg | (raw.rex.R << 3));
                }
            } else if (disp >= kFirstImageOffset && disp < image_size && uses_image_base(instruction, image_base_registers)) {
                blank(at + raw.disp.offset, 4);
                references_.push_back({at + raw.disp.offset, disp});
                if (disp > at && disp < code.end && raw.sib.scale == 2) tables.insert(disp);
            } else {
                std::memset(shape_.data() + at + raw.disp.offset, 0, 4);
            }
        } else if (raw.disp.size == 8) {
            std::memset(shape_.data() + at + raw.disp.offset, 0, 1);
        }
        at = next;
    }
    code.hash = fnv1a(norm_.data() + code.start, code.end - code.start);
    code.shape_hash = fnv1a(shape_.data() + code.start, code.end - code.start);
}

const BuildImage::Code* BuildImage::code_at(uint32_t rva) const {
    const auto next = std::upper_bound(code_.begin(), code_.end(), rva, [](uint32_t value, const Code& c) { return value < c.start; });
    if (next == code_.begin()) return nullptr;
    const Code& code = *std::prev(next);
    return rva < code.end ? &code : nullptr;
}

bool BuildImage::is_code(uint32_t rva) const {
    return std::any_of(sections_.begin(), sections_.end(), [&](const Span& s) { return s.code && rva >= s.start && rva < s.end; });
}

bool BuildImage::is_data(uint32_t rva) const {
    return std::any_of(sections_.begin(), sections_.end(), [&](const Span& s) { return !s.code && rva >= s.start && rva < s.end; });
}

std::optional<uint32_t> BuildImage::pointer_at(uint32_t rva) const {
    if (uint64_t{rva} + 8 > bytes_.size()) return std::nullopt;
    uint64_t pointer = 0;
    std::memcpy(&pointer, bytes_.data() + rva, 8);
    if (pointer < base_ || pointer - base_ >= bytes_.size()) return std::nullopt;
    return static_cast<uint32_t>(pointer - base_);
}

std::vector<uint32_t> BuildImage::slots_holding(uint32_t target) const {
    std::vector<uint32_t> slots;
    const uint64_t wanted = base_ + target;
    for (const Span& section : sections_) {
        if (section.code) continue;
        for (uint32_t at = (section.start + 7) & ~7u; at + 8 <= section.end; at += 8) {
            uint64_t value = 0;
            std::memcpy(&value, bytes_.data() + at, 8);
            if (value == wanted) slots.push_back(at);
        }
    }
    return slots;
}

BuildMatcher::BuildMatcher(const BuildImage& from, const BuildImage& to) : from_(from), to_(to) {
    pair_functions();
    keep_order();
}

void BuildMatcher::pair_functions() {
    // Code that occurs once in each build, by `hash` or `shape_hash`.
    using Index = std::unordered_map<uint64_t, const BuildImage::Code*>;
    const auto index = [](const BuildImage& image, bool shape) {
        Index out;
        out.reserve(image.code().size() * 2);
        for (const auto& code : image.code()) {
            const uint32_t size = code.end - code.start;
            if (size < (shape ? kMinShapePairedSize : kMinPairedSize)) continue;
            const auto [it, inserted] = out.try_emplace((shape ? code.shape_hash : code.hash) ^ (uint64_t{size} << 40), &code);
            if (!inserted) it->second = nullptr;
        }
        return out;
    };
    const auto unique_pairs = [](const Index& from, const Index& to) {
        std::vector<Pair> pairs;
        for (const auto& [key, code] : from) {
            if (code == nullptr) continue;
            const auto other = to.find(key);
            if (other != to.end() && other->second != nullptr) pairs.push_back({code->start, other->second->start});
        }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.from < b.from; });
        return rising_run(pairs);
    };
    for (const Pair& pair : unique_pairs(index(from_, false), index(to_, false))) {
        same_.emplace(pair.from, pair.to);
        worklist_.push_back(pair);
    }
    drain();
    // Then what only a moved field or another immediate sets apart.
    for (const Pair& pair : unique_pairs(index(from_, true), index(to_, true))) {
        if (same_.contains(pair.from) || !changed_.emplace(pair.from, pair.to).second) continue;
        worklist_.push_back(pair);
    }
    drain();
    take_tables();
}

void BuildMatcher::drain() {
    while (!worklist_.empty()) {
        const Pair pair = worklist_.back();
        worklist_.pop_back();
        const auto* from_code = from_.code_at(pair.from);
        const auto* to_code = to_.code_at(pair.to);
        if (from_code != nullptr && to_code != nullptr) take_references(*from_code, *to_code);
    }
}

// A vtable pairs up what its slots hold, if its layout did not change: one
// slot added in the middle would pair every later method with its neighbour,
// and those look alike. So a table counts only as a whole, and only if nothing
// already known speaks against it.
void BuildMatcher::take_tables() {
    for (size_t t = 0; t < tables_.size(); ++t) {
        const Pair table = tables_[t];
        std::vector<Pair> slots;
        bool agrees = true;
        for (size_t i = 0; i < kMaxTableWalk; ++i) {
            const uint32_t step = static_cast<uint32_t>(i * 8);
            const auto p = from_.pointer_at(table.from + step);
            const auto q = to_.pointer_at(table.to + step);
            const bool in_from = p && from_.is_code(*p), in_to = q && to_.is_code(*q);
            if (in_from != in_to) agrees = false;
            if (!in_from || !in_to) break;
            if (const auto expected = known(*p); expected && *expected != *q) {
                agrees = false;
                break;
            }
            slots.push_back({*p, *q});
        }
        if (!agrees) continue;
        for (const Pair& slot : slots) vote(slot.from, slot.to);
        drain();
    }
}

void BuildMatcher::take_references(const BuildImage::Code& from, const BuildImage::Code& to) {
    const auto by_field = [](const BuildImage::Reference& r, uint32_t field) { return r.field < field; };
    const auto& from_refs = from_.references();
    const auto& to_refs = to_.references();
    auto a = std::lower_bound(from_refs.begin(), from_refs.end(), from.start, by_field);
    auto b = std::lower_bound(to_refs.begin(), to_refs.end(), to.start, by_field);
    while (a != from_refs.end() && a->field < from.end && b != to_refs.end() && b->field < to.end) {
        const uint32_t x = a->field - from.start, y = b->field - to.start;
        if (x < y) {
            ++a;
        } else if (y < x) {
            ++b;
        } else {
            vote(a->target, b->target);
            ++a;
            ++b;
        }
    }
}

void BuildMatcher::vote(uint32_t from, uint32_t to) {
    const auto [it, inserted] = votes_.try_emplace(from, Vote{to, 0, false});
    Vote& entry = it->second;
    if (entry.rva != to) {
        entry.conflict = true;
        return;
    }
    ++entry.count;
    if (!inserted) return;

    if (from_.is_code(from) && to_.is_code(to)) {
        const auto* a = from_.code_at(from);
        const auto* b = to_.code_at(to);
        if (a == nullptr || b == nullptr || a->start != from || b->start != to) return;
        if (a->hash == b->hash && a->end - a->start == b->end - b->start) {
            if (same_.emplace(from, to).second) worklist_.push_back({from, to});
        } else if (changed_.emplace(from, to).second && same_shape(*a, *b)) {
            worklist_.push_back({from, to});  // the same layout: its references line up too
        }
    } else if (from_.is_data(from) && to_.is_data(to)) {
        tables_.push_back({from, to});
    }
}

void BuildMatcher::keep_order() {
    order_.clear();
    for (const auto& [from, to] : same_) order_.push_back({from, to});
    for (const auto& [from, to] : changed_) order_.push_back({from, to});
    std::sort(order_.begin(), order_.end(), [](const Pair& a, const Pair& b) { return a.from < b.from; });
    const std::vector<Pair> kept = rising_run(order_);
    std::unordered_map<uint32_t, uint32_t> keep;
    for (const Pair& pair : kept) keep.emplace(pair.from, pair.to);
    for (auto* map : {&same_, &changed_}) {
        std::erase_if(*map, [&](const auto& entry) {
            const auto found = keep.find(entry.first);
            if (found != keep.end() && found->second == entry.second) return false;
            if (const auto vote = votes_.find(entry.first); vote != votes_.end()) vote->second.conflict = true;
            return true;
        });
    }
    order_ = kept;
}

std::optional<uint32_t> BuildMatcher::known(uint32_t rva) const {
    if (const auto it = same_.find(rva); it != same_.end()) return it->second;
    if (const auto it = changed_.find(rva); it != changed_.end()) return it->second;
    if (const auto it = votes_.find(rva); it != votes_.end() && !it->second.conflict) return it->second.rva;
    return std::nullopt;
}

bool BuildMatcher::in_order(uint32_t from, uint32_t to) const {
    const auto next = std::upper_bound(order_.begin(), order_.end(), from, [](uint32_t value, const Pair& p) { return value < p.from; });
    if (next != order_.end() && to > next->to) return false;
    if (next != order_.begin() && to < std::prev(next)->to) return false;
    return true;
}

// Whether no paired function lies between the two addresses of the old build.
bool BuildMatcher::same_gap(uint32_t a, uint32_t b) const {
    const auto gap = [&](uint32_t rva) {
        return std::upper_bound(order_.begin(), order_.end(), rva, [](uint32_t value, const Pair& p) { return value < p.from; });
    };
    return gap(a) == gap(b);
}

bool BuildMatcher::references_agree(uint32_t from, uint32_t to, uint32_t size) const {
    const auto by_field = [](const BuildImage::Reference& r, uint32_t field) { return r.field < field; };
    const auto& refs = from_.references();
    const auto& other = to_.references();
    auto b = std::lower_bound(other.begin(), other.end(), to, by_field);
    for (auto a = std::lower_bound(refs.begin(), refs.end(), from, by_field); a != refs.end() && a->field < from + size; ++a) {
        const uint32_t offset = a->field - from;
        while (b != other.end() && b->field < to + offset) ++b;
        if (b == other.end() || b->field != to + offset) continue;
        if (const auto expected = known(a->target); expected && *expected != b->target) return false;
    }
    return true;
}

PortMatch BuildMatcher::search(uint32_t rva, uint32_t size, Range from, Range to, Range limit, bool everywhere, bool by_shape) const {
    PortMatch result;
    const uint8_t* old_code = by_shape ? from_.shape() : from_.norm();
    const uint8_t* new_code = by_shape ? to_.shape() : to_.norm();
    const uint32_t min_window = by_shape ? kMinShapeWindow : kMinWindow;
    bool ambiguous_old = false, ambiguous_new = false, lost = false, unordered = false, disagree = false;
    // Grow around the declared bytes, on both sides, then on one: a change next
    // to the bytes must not hide them. A window that fails stays failed when it grows.
    enum Side { Both, After, Before };
    for (const Side side : {Both, After, Before}) {
        for (const uint32_t context : {0u, 8u, 16u, 32u, 64u, 128u, 256u, 512u}) {
            uint32_t start = side == After ? rva : (rva - std::min(context, rva - limit.start));
            uint32_t end = side == Before ? rva + size : std::min(rva + size + context, limit.end);
            if (end - start < min_window) {
                if (side != Before) end = std::min(start + min_window, std::max(limit.end, rva + size));
                if (side != After && end - start < min_window) start = end - std::min(min_window, end - limit.start);
            }
            const uint32_t window = end - start;
            auto in_old = find_all(old_code, from.start, from.end, old_code + start, window, kMaxCopies);
            auto in_new = find_all(new_code, to.start, to.end, old_code + start, window, kMaxCopies);
            if (in_new.empty()) {
                lost = true;
                break;
            }
            if (in_old.size() == kMaxCopies || in_new.size() == kMaxCopies) {  // not all of them are in the lists
                (in_old.size() == kMaxCopies ? ambiguous_old : ambiguous_new) = true;
                continue;
            }
            // Copies of the same code (a template, an inlined helper): the copy
            // between the same two paired functions is this one.
            if (everywhere && (in_old.size() > 1 || in_new.size() > 1)) {
                const uint32_t shift = rva - start;
                std::erase_if(in_old, [&](uint32_t at) { return !same_gap(rva, at + shift); });
                std::erase_if(in_new, [&](uint32_t at) { return !in_order(rva, at + shift); });
                if (in_new.empty()) {
                    unordered = true;
                    break;
                }
            }
            if (in_old.size() > 1) {
                ambiguous_old = true;
                continue;
            }
            if (in_new.size() > 1) {
                ambiguous_new = true;
                continue;
            }
            const uint32_t found = in_new[0] + (rva - start);
            if (everywhere && !in_order(rva, found)) {
                unordered = true;
                result.candidate = found;
                break;
            }
            if (!references_agree(start, in_new[0], window)) {
                disagree = true;
                result.candidate = found;
                break;
            }
            result.rva = found;
            result.candidate.reset();
            result.method = std::string(everywhere ? "search" : "search in the same function") + (by_shape ? " by shape" : "");
            return result;
        }
    }
    if (disagree) result.reason = "the only match refers to other functions or data than this code does";
    else if (unordered) result.reason = "the only match is out of order with the functions around it";
    else if (ambiguous_old) result.reason = "the code around it occurs more than once in the old build";
    else if (ambiguous_new) result.reason = "the code around it occurs more than once in the new build";
    else if (lost) result.reason = "the code around it is not in the new build";
    else result.reason = "no match";
    return result;
}

PortMatch BuildMatcher::code(uint32_t rva, uint32_t size) const {
    const auto* code = from_.code_at(rva);
    if (code == nullptr) {
        PortMatch result;
        result.reason = "not in the code of the old build";
        return result;
    }
    const uint32_t end = std::max(code->end, rva + size);
    if (const auto same = same_.find(code->start); same != same_.end() && rva + size <= code->end) {
        PortMatch result;
        result.rva = same->second + (rva - code->start);
        result.method = "same function";
        result.paired = true;
        return result;
    }
    if (const auto changed = changed_.find(code->start); changed != changed_.end()) {
        const auto* other = to_.code_at(changed->second);
        if (other != nullptr && same_shape(*code, *other) && rva + size <= code->end) {
            PortMatch result;
            result.rva = other->start + (rva - code->start);
            result.method = "same shape";
            result.paired = true;
            return result;
        }
        if (other != nullptr) {
            const Range old_range{code->start, end}, new_range{other->start, other->end};
            PortMatch inside = search(rva, size, old_range, new_range, old_range, false, false);
            if (inside.rva) return inside;
            const PortMatch shaped = search(rva, size, old_range, new_range, old_range, false, true);
            if (shaped.rva) inside.candidate = shaped.rva;
            inside.reason = "its function is at " + hex(changed->second) + " but changed: " + inside.reason;
            return inside;
        }
    }
    const Range old_code{from_.code().front().start, from_.code().back().end};
    const Range new_code{to_.code().front().start, to_.code().back().end};
    PortMatch found = search(rva, size, old_code, new_code, {code->start, end}, true, false);
    if (found.rva || found.candidate) return found;
    // With other operands, and nothing that pairs its function: a place to look at, no more.
    const PortMatch shaped = search(rva, size, old_code, new_code, {code->start, end}, true, true);
    if (shaped.rva) found.candidate = shaped.rva;
    return found;
}

bool BuildMatcher::same_shape_at(uint32_t from, uint32_t to) const {
    const auto* a = from_.code_at(from);
    const auto* b = to_.code_at(to);
    return a != nullptr && b != nullptr && a->start == from && b->start == to && same_shape(*a, *b);
}

std::vector<uint32_t> BuildMatcher::operands_differ(uint32_t from, uint32_t to, uint32_t size) const {
    std::vector<uint32_t> offsets;
    for (uint32_t i = 0; i < size; ++i) {
        if (from_.norm()[from + i] != to_.norm()[to + i]) offsets.push_back(i);
    }
    return offsets;
}

PortMatch BuildMatcher::slot(uint32_t rva, uint32_t target) const {
    PortMatch result;
    // Slots around this one, walking out until the table ends in the old build.
    std::vector<std::pair<int, uint32_t>> table;
    if (from_.pointer_at(rva) == target) table.emplace_back(0, target);
    for (const int direction : {-1, 1}) {
        for (int i = 1; i <= kMaxTableReach; ++i) {
            const auto pointer = from_.pointer_at(rva + direction * i * 8);
            if (!pointer || !from_.is_code(*pointer)) break;
            table.emplace_back(direction * i * 8, *pointer);
        }
    }
    // Every slot that holds a known function of the table names a place for it.
    std::set<uint32_t> places;
    for (const auto& [offset, pointer] : table) {
        const auto expected = known(pointer);
        if (!expected) continue;
        for (const uint32_t held : to_.slots_holding(*expected)) places.insert(held - offset);
    }
    if (places.empty()) {
        const PortMatch function = code(target, kMinWindow);
        if (function.rva) {
            for (const uint32_t held : to_.slots_holding(*function.rva)) places.insert(held);
        }
        if (places.empty()) {
            result.reason = "no slot holds a function of its table: " + function.reason;
            return result;
        }
    }
    // How far the table holds around a place, on each side: a method added far
    // away shifts the rest but not the slots here. A getter of a moved field has
    // another operand and may be folded with a neighbour, so it says nothing.
    struct Side {
        int agree = 0;
        bool broken = false;
    };
    const auto walk = [&](uint32_t place, int direction) {
        Side side;
        for (int i = 1; i <= kMaxTableReach; ++i) {
            const int offset = direction * i * 8;
            const auto p = from_.pointer_at(rva + offset);
            const auto q = to_.pointer_at(place + offset);
            const bool in_from = p && from_.is_code(*p), in_to = q && to_.is_code(*q);
            if (!in_from && !in_to) break;
            if (!in_from || !in_to) {
                side.broken = true;
                break;
            }
            const auto expected = known(*p);
            if (expected == q) {
                ++side.agree;
            } else if (!same_shape_at(*p, *q)) {
                side.broken = true;
                break;
            }
        }
        return side;
    };
    uint32_t best = 0;
    int agree = -1;
    bool tied = false, broken = false;
    for (const uint32_t place : places) {
        const Side before = walk(place, -1), after = walk(place, 1);
        const auto held = to_.pointer_at(place);
        const int score = before.agree + after.agree + (held && known(target) == held ? 1 : 0);
        if (score == agree) tied = true;
        if (score > agree) {
            best = place;
            agree = score;
            tied = false;
            broken = before.broken || after.broken;
        }
    }
    result.candidate = best;
    const auto held = to_.pointer_at(best);
    const bool same_function = held && same_shape_at(target, *held);
    if (tied) {
        result.reason = "several tables hold the functions around it";
    } else if (!held || !to_.is_code(*held)) {
        result.reason = "that place holds no pointer to code";
    } else if (same_function ? agree < 1 : (broken || agree < kMinTableEvidence)) {
        result.reason = "the function in that slot is not the same code";
    } else {
        result.candidate.reset();
        result.rva = best;
        result.method = "slot among " + std::to_string(agree) + " known";
    }
    return result;
}

PortMatch BuildMatcher::data(uint32_t rva, uint32_t size) const {
    PortMatch result;
    if (const auto it = votes_.find(rva); it != votes_.end()) {
        if (it->second.conflict) {
            result.reason = "the code that matched refers to it at more than one place in the new build";
            return result;
        }
        result.rva = it->second.rva;
        result.method = "reference from " + std::to_string(it->second.count) + " instruction(s)";
        return result;
    }
    if (size >= kMinWindow) {
        const uint8_t* bytes = from_.bytes();
        const auto in_old = find_all(bytes, 0x1000, from_.size(), bytes + rva, size, 2);
        const auto in_new = find_all(to_.bytes(), 0x1000, to_.size(), bytes + rva, size, 2);
        if (in_old.size() == 1 && in_new.size() == 1) {
            result.rva = in_new[0];
            result.method = "search";
            return result;
        }
    }
    result.reason = "no code that matched refers to it";
    return result;
}

}  // namespace anvil::cli
