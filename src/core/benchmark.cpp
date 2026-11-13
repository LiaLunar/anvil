#include "benchmark.hpp"

#include "plugin_set.hpp"
#include "manifest.hpp"

#include <cmath>
#include <cstring>

namespace anvil {
namespace {

Benchmark* g_benchmark = nullptr;

const char* kind_name(AnvilBenchmarkKind kind) {
    switch (kind) {
    case ANVIL_BENCHMARK_INT: return "an int";
    case ANVIL_BENCHMARK_NUMBER: return "a number";
    case ANVIL_BENCHMARK_ENUM: return "an enum";
    case ANVIL_BENCHMARK_FLAG: return "a flag";
    }
    return "unknown";
}

// At most 64 bytes of a name a plugin passed, for its log line.
std::string printable(const char* text) {
    if (text == nullptr) return "(null)";
    std::string out;
    for (size_t i = 0; text[i] != '\0' && i < ANVIL_BENCHMARK_MAX_NAME; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        out += c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?';
    }
    return out;
}

bool valid_label(const char* label) {
    if (label == nullptr) return true;
    const size_t n = strnlen(label, ANVIL_BENCHMARK_MAX_LABEL + 1);
    if (n > ANVIL_BENCHMARK_MAX_LABEL) return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(label[i]);
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

// Why the fields of one declare call cannot be added to `declared`.
AnvilResult check_fields(const std::vector<BenchmarkField>& declared, const AnvilBenchmarkField* fields,
                         uint32_t count, std::string& why) {
    if (count > 0 && fields == nullptr) {
        why = "no fields given";
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    if (declared.size() + count > ANVIL_BENCHMARK_MAX_FIELDS) {
        why = "more than " + std::to_string(ANVIL_BENCHMARK_MAX_FIELDS) + " fields";
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    std::set<std::string_view> names;
    for (const auto& d : declared) names.insert(d.name);
    for (uint32_t i = 0; i < count; ++i) {
        const AnvilBenchmarkField& f = fields[i];
        if (f.name == nullptr || !is_valid_benchmark_name(f.name)) {
            why = "field name '" + printable(f.name) + "' is not group.field or field of [a-z0-9_] (at most " +
                  std::to_string(ANVIL_BENCHMARK_MAX_NAME) + " characters)";
            return ANVIL_ERROR_INVALID_ARGUMENT;
        }
        const std::string name = f.name;
        if (!names.insert(f.name).second) {
            why = "field '" + name + "' is declared twice";
            return ANVIL_ERROR_ALREADY_EXISTS;
        }
        if (f.kind < ANVIL_BENCHMARK_INT || f.kind > ANVIL_BENCHMARK_FLAG) {
            why = "field '" + name + "' has an unknown kind " + std::to_string(f.kind);
            return ANVIL_ERROR_INVALID_ARGUMENT;
        }
        if (f.unit > ANVIL_BENCHMARK_UNIT_PERCENT) {
            why = "field '" + name + "' has an unknown unit " + std::to_string(f.unit);
            return ANVIL_ERROR_INVALID_ARGUMENT;
        }
        if ((f.kind == ANVIL_BENCHMARK_ENUM || f.kind == ANVIL_BENCHMARK_FLAG) && f.unit != ANVIL_BENCHMARK_UNIT_NONE) {
            why = "field '" + name + "': only int and number fields have a unit";
            return ANVIL_ERROR_INVALID_ARGUMENT;
        }
        if (!valid_label(f.label)) {
            why = "field '" + name + "': the label is longer than " + std::to_string(ANVIL_BENCHMARK_MAX_LABEL) +
                  " bytes or has control characters";
            return ANVIL_ERROR_INVALID_ARGUMENT;
        }
        if (f.kind != ANVIL_BENCHMARK_ENUM) {
            if (f.options != nullptr || f.option_count != 0) {
                why = "field '" + name + "': only enum fields have options";
                return ANVIL_ERROR_INVALID_ARGUMENT;
            }
            continue;
        }
        if (f.options == nullptr || f.option_count == 0 || f.option_count > ANVIL_BENCHMARK_MAX_OPTIONS) {
            why = "field '" + name + "' needs 1 to " + std::to_string(ANVIL_BENCHMARK_MAX_OPTIONS) + " options";
            return ANVIL_ERROR_INVALID_ARGUMENT;
        }
        std::set<std::string_view> options;
        for (uint32_t k = 0; k < f.option_count; ++k) {
            if (f.options[k] == nullptr || !is_valid_benchmark_name(f.options[k])) {
                why = "field '" + name + "': option '" + printable(f.options[k]) + "' is not a name of [a-z0-9_]";
                return ANVIL_ERROR_INVALID_ARGUMENT;
            }
            if (!options.insert(f.options[k]).second) {
                why = "field '" + name + "': option '" + std::string(f.options[k]) + "' is listed twice";
                return ANVIL_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    return ANVIL_OK;
}

BenchmarkField copy_field(const AnvilBenchmarkField& f) {
    BenchmarkField out;
    out.name = f.name;
    out.kind = static_cast<AnvilBenchmarkKind>(f.kind);
    out.unit = static_cast<AnvilBenchmarkUnit>(f.unit);
    if (f.label != nullptr) out.label = f.label;
    for (uint32_t k = 0; f.kind == ANVIL_BENCHMARK_ENUM && k < f.option_count; ++k) out.options.emplace_back(f.options[k]);
    return out;
}

// ---- the C table ---------------------------------------------------------------

int svc_active() { return g_benchmark != nullptr && g_benchmark->active() ? 1 : 0; }

AnvilResult svc_declare(AnvilPlugin* self, const AnvilBenchmarkField* fields, uint32_t count) {
    return g_benchmark != nullptr ? g_benchmark->declare(self, fields, count) : ANVIL_ERROR_INTERNAL;
}
AnvilResult svc_set_int(AnvilPlugin* self, const char* field, int64_t value) {
    return g_benchmark != nullptr ? g_benchmark->set_int(self, field, value) : ANVIL_ERROR_INTERNAL;
}
AnvilResult svc_set_number(AnvilPlugin* self, const char* field, double value) {
    return g_benchmark != nullptr ? g_benchmark->set_number(self, field, value) : ANVIL_ERROR_INTERNAL;
}
AnvilResult svc_set_enum(AnvilPlugin* self, const char* field, const char* option) {
    return g_benchmark != nullptr ? g_benchmark->set_enum(self, field, option) : ANVIL_ERROR_INTERNAL;
}
AnvilResult svc_set_flag(AnvilPlugin* self, const char* field, int value) {
    return g_benchmark != nullptr ? g_benchmark->set_flag(self, field, value) : ANVIL_ERROR_INTERNAL;
}
AnvilResult svc_done(AnvilPlugin* self) {
    return g_benchmark != nullptr ? g_benchmark->done(self) : ANVIL_ERROR_INTERNAL;
}
AnvilResult svc_learning(AnvilPlugin* self, const char* reason) {
    return g_benchmark != nullptr ? g_benchmark->learning(self, reason) : ANVIL_ERROR_INTERNAL;
}
AnvilResult svc_open_cost(AnvilPlugin* self, uint64_t files, double ms) {
    return g_benchmark != nullptr ? g_benchmark->open_cost(self, files, ms) : ANVIL_ERROR_INTERNAL;
}

constexpr AnvilBenchmarkService kService = {
    sizeof(AnvilBenchmarkService), svc_active, svc_declare, svc_set_int,  svc_set_number, svc_set_enum,
    svc_set_flag,                  svc_done,   svc_learning, svc_open_cost,
};

}  // namespace

std::optional<BenchmarkArm> parse_benchmark_arm(std::string_view text, std::string& error) {
    BenchmarkArm arm;
    std::string_view id;
    if (text.starts_with("with:")) {
        arm.with = true;
        id = text.substr(5);
    } else if (text.starts_with("without:")) {
        id = text.substr(8);
    } else {
        error = "expected with:<plugin id> or without:<plugin id>";
        return std::nullopt;
    }
    if (!is_valid_plugin_id(id)) {
        error = "'" + std::string(id) + "' is not a plugin id ([a-z0-9-]+, at most 64 characters)";
        return std::nullopt;
    }
    arm.plugin = id;
    return arm;
}

bool is_valid_benchmark_name(std::string_view name) {
    if (name.empty() || name.size() > ANVIL_BENCHMARK_MAX_NAME) return false;
    int dots = 0;
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '.') {
            if (++dots > 1 || i == 0 || i + 1 == name.size()) return false;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

Benchmark::Benchmark(bool active, LogSink log) : active_(active), log_(std::move(log)) { g_benchmark = this; }

Benchmark::~Benchmark() {
    if (g_benchmark == this) g_benchmark = nullptr;
}

const AnvilBenchmarkService* Benchmark::service() const { return &kService; }

BenchmarkParticipant* Benchmark::find(const AnvilPlugin* self) {
    for (auto& p : participants_) {
        if (p.plugin == self) return &p;
    }
    return nullptr;
}

AnvilResult Benchmark::refuse(const AnvilPlugin& self, const std::string& call, const std::string& what,
                              const std::string& why, AnvilResult result, bool once) {
    if (once) {
        std::lock_guard lock(mutex_);
        const std::string key = self.name + '\n' + call + '\n' + what + '\n' + std::to_string(result);
        if (!warned_.insert(key).second) return result;
    }
    if (log_) {
        log_(self, std::string(ANVIL_BENCHMARK_SERVICE " ") + call + (what.empty() ? "" : " " + what) +
                       " refused: " + why);
    }
    return result;
}

AnvilResult Benchmark::declare(AnvilPlugin* self, const AnvilBenchmarkField* fields, uint32_t count) {
    if (self == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!self->loading) {
        return refuse(*self, "declare", "", "fields are declared inside anvil_plugin_load only", ANVIL_ERROR_WRONG_PHASE,
                      false);
    }
    std::string why;
    AnvilResult r = ANVIL_OK;
    {
        std::lock_guard lock(mutex_);
        BenchmarkParticipant* p = find(self);
        if (p == nullptr) {
            participants_.push_back(BenchmarkParticipant{});
            p = &participants_.back();
            p->plugin = self;
            p->id = self->name;
        }
        r = check_fields(p->fields, fields, count, why);
        for (uint32_t i = 0; r == ANVIL_OK && i < count; ++i) {
            p->fields.push_back(copy_field(fields[i]));
            p->values.emplace_back();
        }
    }
    return r == ANVIL_OK ? r : refuse(*self, "declare", "", why, r, false);
}

AnvilResult Benchmark::find_field(const BenchmarkParticipant* p, const char* field, AnvilBenchmarkKind kind,
                                  size_t& index) const {
    if (p == nullptr) return ANVIL_ERROR_NOT_DECLARED;
    for (index = 0; index < p->fields.size(); ++index) {
        if (p->fields[index].name == field) break;
    }
    if (index == p->fields.size()) return ANVIL_ERROR_NOT_DECLARED;
    if (p->fields[index].kind != kind) return ANVIL_ERROR_WRONG_KIND;
    if (p->done) return ANVIL_ERROR_WRONG_PHASE;
    return ANVIL_OK;
}

template <class Assign>
AnvilResult Benchmark::assign(AnvilPlugin* self, const char* call, const char* field, AnvilBenchmarkKind kind,
                              Assign&& value) {
    if (self == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (field == nullptr) return refuse(*self, call, "", "no field name", ANVIL_ERROR_INVALID_ARGUMENT, true);
    std::string why;
    AnvilResult r = ANVIL_OK;
    {
        std::lock_guard lock(mutex_);
        BenchmarkParticipant* p = find(self);
        size_t index = 0;
        r = find_field(p, field, kind, index);
        if (r == ANVIL_ERROR_NOT_DECLARED) {
            why = p == nullptr ? "the plugin declared no benchmark fields in anvil_plugin_load" : "no such field";
        } else if (r == ANVIL_ERROR_WRONG_KIND) {
            why = std::string("the field is ") + kind_name(p->fields[index].kind);
        } else if (r == ANVIL_ERROR_WRONG_PHASE) {
            why = "the plugin already called done()";
        } else {
            r = value(p->fields[index], p->values[index], why);
        }
    }
    return r == ANVIL_OK ? r : refuse(*self, call, printable(field), why, r, true);
}

AnvilResult Benchmark::set_int(AnvilPlugin* self, const char* field, int64_t value) {
    return assign(self, "set_int", field, ANVIL_BENCHMARK_INT, [&](const BenchmarkField&, BenchmarkValue& out, std::string&) {
        out = value;
        return ANVIL_OK;
    });
}

AnvilResult Benchmark::set_number(AnvilPlugin* self, const char* field, double value) {
    return assign(self, "set_number", field, ANVIL_BENCHMARK_NUMBER,
                  [&](const BenchmarkField&, BenchmarkValue& out, std::string& why) {
                      if (!std::isfinite(value)) {
                          why = "NaN and infinity are not numbers here";
                          return ANVIL_ERROR_INVALID_ARGUMENT;
                      }
                      out = value;
                      return ANVIL_OK;
                  });
}

AnvilResult Benchmark::set_enum(AnvilPlugin* self, const char* field, const char* option) {
    return assign(self, "set_enum", field, ANVIL_BENCHMARK_ENUM,
                  [&](const BenchmarkField& f, BenchmarkValue& out, std::string& why) {
                      for (uint32_t k = 0; option != nullptr && k < f.options.size(); ++k) {
                          if (f.options[k] == option) {
                              out = BenchmarkOption{k};
                              return ANVIL_OK;
                          }
                      }
                      why = "'" + printable(option) + "' is not one of its options";
                      return ANVIL_ERROR_INVALID_ARGUMENT;
                  });
}

AnvilResult Benchmark::set_flag(AnvilPlugin* self, const char* field, int value) {
    return assign(self, "set_flag", field, ANVIL_BENCHMARK_FLAG, [&](const BenchmarkField&, BenchmarkValue& out, std::string&) {
        out = value != 0;
        return ANVIL_OK;
    });
}

AnvilResult Benchmark::done(AnvilPlugin* self) {
    if (self == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    {
        std::lock_guard lock(mutex_);
        if (BenchmarkParticipant* p = find(self)) {
            p->done = true;
            return ANVIL_OK;
        }
    }
    return refuse(*self, "done", "", "the plugin declared no benchmark fields in anvil_plugin_load",
                  ANVIL_ERROR_NOT_DECLARED, true);
}

AnvilResult Benchmark::learning(AnvilPlugin* self, const char* reason) {
    if (self == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (reason == nullptr || !is_valid_benchmark_name(reason)) {
        return refuse(*self, "learning", "", "the reason '" + printable(reason) + "' is not a name of [a-z0-9_]",
                      ANVIL_ERROR_INVALID_ARGUMENT, true);
    }
    AnvilResult r = ANVIL_OK;
    {
        std::lock_guard lock(mutex_);
        BenchmarkParticipant* p = find(self);
        if (p == nullptr) {
            r = ANVIL_ERROR_NOT_DECLARED;
        } else if (p->done) {
            r = ANVIL_ERROR_WRONG_PHASE;
        } else {
            p->learning = reason;
        }
    }
    if (r == ANVIL_OK) return r;
    return refuse(*self, "learning", "",
                  r == ANVIL_ERROR_NOT_DECLARED ? "the plugin declared no benchmark fields in anvil_plugin_load"
                                                : "the plugin already called done()",
                  r, true);
}

AnvilResult Benchmark::open_cost(AnvilPlugin* self, uint64_t files, double ms) {
    if (self == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!std::isfinite(ms) || ms < 0) {
        return refuse(*self, "open_cost", "", "ms must be a finite number >= 0", ANVIL_ERROR_INVALID_ARGUMENT, true);
    }
    AnvilResult r = ANVIL_OK;
    {
        std::lock_guard lock(mutex_);
        BenchmarkParticipant* p = find(self);
        if (p == nullptr) {
            r = ANVIL_ERROR_NOT_DECLARED;
        } else if (p->done) {
            r = ANVIL_ERROR_WRONG_PHASE;
        } else {
            p->open_cost = BenchmarkOpenCost{files, ms};
        }
    }
    if (r == ANVIL_OK) return r;
    return refuse(*self, "open_cost", "",
                  r == ANVIL_ERROR_NOT_DECLARED ? "the plugin declared no benchmark fields in anvil_plugin_load"
                                                : "the plugin already called done()",
                  r, true);
}

void Benchmark::forget(const AnvilPlugin* plugin) {
    std::lock_guard lock(mutex_);
    std::erase_if(participants_, [&](const BenchmarkParticipant& p) { return p.plugin == plugin; });
}

std::vector<BenchmarkParticipant> Benchmark::participants() const {
    std::lock_guard lock(mutex_);
    return participants_;
}
}  // namespace anvil
