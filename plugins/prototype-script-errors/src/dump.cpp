// PROTOTYPE (eu4-engine-lab#11), throwaway. Prints the panel's rows without
// the game, from a saved error.log:
//   prototype_script_errors_dump <error.log> [<dlc_load.json> <user dir> <game dir>]
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

#include "model.hpp"
#include "rows.hpp"

namespace se = script_errors;

static void print(const std::vector<se::Row>& rows) {
    for (const se::Row& r : rows) {
        if (r.kind == se::kSeparator) {
            std::cout << "  ----------------------------------------\n";
        } else if (r.kind == se::kButton) {
            std::cout << "  [" << r.label << "]\n";
        } else {
            std::cout << "  " << r.label << (r.has_value ? "  |  " + r.value : "") << "\n";
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::ifstream in(argv[1], std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    se::Log log;
    log.feed(s.str());
    std::vector<se::Root> roots;
    if (argc >= 5) roots = se::playset_roots(se::path_from_utf8(argv[2]), se::path_from_utf8(argv[3]), se::path_from_utf8(argv[4]));
    for (const se::Root& r : roots) std::cout << "layer: " << r.name << " = " << se::utf8_from_path(r.path) << "\n";
    auto t0 = std::chrono::steady_clock::now();
    for (se::Message& m : log.messages()) se::resolve(m, roots);
    auto ms = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(); };
    std::cout << "resolve: " << log.messages().size() << " messages, " << ms() << " ms\n";
    se::View view;
    view.source = argv[1];
    // The first target and the first script error, searched.
    std::size_t top_target = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < log.messages().size(); ++i) {
        se::Message& m = log.messages()[i];
        if (m.kind == se::Kind::target && top_target == static_cast<std::size_t>(-1)) top_target = i;
    }
    if (top_target != static_cast<std::size_t>(-1) && !roots.empty()) {
        se::Message& m = log.messages()[top_target];
        t0 = std::chrono::steady_clock::now();
        se::search(roots, m, m.hits, m.hit_total, m.hit_files);
        std::cout << "search '" << m.token << "': " << m.hit_total << " uses in " << m.hit_files << " files, " << ms() << " ms\n";
        m.search = 2;
        view.selected = top_target;
    }
    for (int v = 0; v < se::kVariants; ++v) {
        view.variant = v;
        view.open.clear();
        if (v == 1) {
            for (int k = 0; k < se::kKinds; ++k) view.open.insert(static_cast<std::uint32_t>(k));
        }
        if (v == 2) {
            // every group of C
            for (const se::Row& r : se::build_rows(log, view)) {
                if (se::action_of(r.id) == se::Action::toggle) view.open.insert(se::first_of(r.id));
            }
        }
        std::cout << "\n=== variant " << se::variant_name(v) << " ===\n";
        print(se::build_rows(log, view));
    }
    return 0;
}
