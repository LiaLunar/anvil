#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace anvil::cli {

// One eu4.exe laid out as it is mapped, with a second copy of its code in which
// what the linker decides is zeroed: the targets of relative jumps and calls,
// RIP-relative displacements, image-relative displacements and jump tables.
// Two builds of the same source then compare byte for byte wherever the code
// is the same, whatever moved around it. A third copy also has every other
// displacement and immediate zeroed: what is left is the shape of the code,
// which a moved struct field does not change. Reads the file; writes nothing.
class BuildImage {
public:
    // A stretch of code: an entry of .pdata, or a leaf function between them.
    struct Code {
        uint32_t start = 0;
        uint32_t end = 0;
        uint64_t hash = 0;        // of `norm`
        uint64_t shape_hash = 0;  // of `shape`
    };
    // An address stored in the code, in a field `norm` has zeroed.
    struct Reference {
        uint32_t field = 0;
        uint32_t target = 0;
    };

    static std::optional<BuildImage> load(const std::filesystem::path& exe, std::string& error);

    uint64_t base() const { return base_; }
    uint32_t size() const { return static_cast<uint32_t>(bytes_.size()); }
    const uint8_t* bytes() const { return bytes_.data(); }
    const uint8_t* norm() const { return norm_.data(); }
    const uint8_t* shape() const { return shape_.data(); }
    const std::vector<Code>& code() const { return code_; }
    const std::vector<Reference>& references() const { return references_; }

    // The stretch of code holding `rva`, or nullptr.
    const Code* code_at(uint32_t rva) const;
    bool is_code(uint32_t rva) const;
    bool is_data(uint32_t rva) const;
    // The RVA in the pointer stored at `rva`, if it holds one into the image.
    std::optional<uint32_t> pointer_at(uint32_t rva) const;
    std::vector<uint32_t> slots_holding(uint32_t target) const;

private:
    struct Span {
        uint32_t start = 0;
        uint32_t end = 0;
        bool code = false;
    };
    void find_code();
    void zero_code(Code& code);

    uint64_t base_ = 0;
    std::vector<uint8_t> bytes_;
    std::vector<uint8_t> norm_;
    std::vector<uint8_t> shape_;
    std::vector<Span> sections_;
    std::vector<Code> code_;  // by start
    std::vector<Reference> references_;  // by field
};

// Where something of the old build is in the new one. `rva` is set only when
// the match is certain enough to write down; otherwise `reason` says why not,
// and `candidate` may name a place to look at, which is never written.
struct PortMatch {
    std::optional<uint32_t> rva;
    std::string method;
    std::string reason;
    std::optional<uint32_t> candidate;
    bool paired = false;  // found through its function's pair, not by a search of the build
};

// What two builds have in common. Functions whose zeroed bytes are the same and
// occur once in each build are pairs (anchors); their references pair up more
// functions and data, and whatever keeps the order of the anchors stays. Every
// match below must agree with that map: a wrong address is worse than none.
class BuildMatcher {
public:
    BuildMatcher(const BuildImage& from, const BuildImage& to);

    // `size` bytes of code at `rva`.
    PortMatch code(uint32_t rva, uint32_t size) const;
    // A pointer slot at `rva` holding `target`.
    PortMatch slot(uint32_t rva, uint32_t target) const;
    // Data at `rva`: `bytes` of it, if it has any to compare.
    PortMatch data(uint32_t rva, uint32_t size) const;
    // The offsets in `size` bytes where the operands of the two builds differ,
    // beyond the addresses that move anyway.
    std::vector<uint32_t> operands_differ(uint32_t from, uint32_t to, uint32_t size) const;
    // Whether the code starting at each is the same but for its operands.
    bool same_shape_at(uint32_t from, uint32_t to) const;

    size_t functions() const { return from_.code().size(); }
    size_t functions_paired() const { return same_.size(); }
    size_t functions_changed() const { return changed_.size(); }
    size_t addresses_paired() const { return votes_.size(); }

private:
    struct Vote {
        uint32_t rva = 0;
        uint32_t count = 0;
        bool conflict = false;
    };
    struct Range {
        uint32_t start = 0;
        uint32_t end = 0;
    };
    struct Pair {
        uint32_t from = 0;
        uint32_t to = 0;
    };

    void pair_functions();
    void drain();
    void take_tables();
    void take_references(const BuildImage::Code& from, const BuildImage::Code& to);
    void vote(uint32_t from, uint32_t to);
    void keep_order();
    std::optional<uint32_t> known(uint32_t rva) const;
    bool in_order(uint32_t from, uint32_t to) const;
    bool same_gap(uint32_t a, uint32_t b) const;
    bool references_agree(uint32_t from, uint32_t to, uint32_t size) const;
    PortMatch search(uint32_t rva, uint32_t size, Range from, Range to, Range limit, bool everywhere, bool by_shape) const;

    const BuildImage& from_;
    const BuildImage& to_;
    std::unordered_map<uint32_t, uint32_t> same_;     // code start -> code start, the same zeroed bytes
    std::unordered_map<uint32_t, uint32_t> changed_;  // code start -> code start, other bytes (see same_shape)
    std::unordered_map<uint32_t, Vote> votes_;        // any address a pair refers to
    std::vector<Pair> order_;                         // by `from`; `to` rises too
    std::vector<Pair> worklist_;
    std::vector<Pair> tables_;  // data the pairs refer to, which may hold pointers to code
};

}  // namespace anvil::cli
