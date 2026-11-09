#include "release.hpp"

#include "i18n/translate.hpp"

namespace overlay {

using anvil::i18n::tr;
using anvil::i18n::trf;

ReleasePlan release_plan(ReleaseConsent consent, std::optional<bool> manager_answer, bool benchmark) {
    if (benchmark) return ReleasePlan::None;
    switch (consent) {
    case ReleaseConsent::On: return ReleasePlan::Check;
    case ReleaseConsent::Off: return ReleasePlan::None;
    case ReleaseConsent::Ask: break;
    }
    if (manager_answer) return *manager_answer ? ReleasePlan::Check : ReleasePlan::None;
    return ReleasePlan::Question;
}

Notice release_question() {
    Notice n;
    n.id = "release-question";
    n.kind = NoticeKind::Info;
    n.until_closed = true;
    n.title = tr("Check for new Anvil releases?");
    n.lines.push_back(tr("Should Anvil ask GitHub for its latest release when the game starts? It only shows a notice "
                         "with a link; nothing is downloaded. Anvil Manager asks the same, and one answer counts for "
                         "both. You can change it in the panel's settings."));
    n.actions.push_back({NoticeAction::Type::ReleaseCheckOn, {}, tr("Yes, check for releases"), {}});
    n.actions.push_back({NoticeAction::Type::ReleaseCheckOff, {}, tr("No"), {}});
    return n;
}

std::optional<Notice> release_notice(const anvil::net::Release& latest, std::string_view running,
                                     std::string_view dismissed) {
    if (!anvil::net::newer_release(latest.tag, running) || latest.tag == dismissed) return std::nullopt;
    Notice n;
    n.id = "release";
    n.kind = NoticeKind::Info;
    n.title = trf("Anvil {0} is available.", {latest.tag});
    n.lines.push_back(trf("This game runs Anvil {0}. To update, download AnvilManager.exe from the release page and "
                          "run it.",
                          {running}));
    n.lines.push_back(tr("The panel's settings can turn this check off."));
    n.actions.push_back({NoticeAction::Type::OpenLink, {}, tr("Open its page"), latest.url});
    n.actions.push_back({NoticeAction::Type::CopyLink, {}, tr("Copy link"), latest.url});
    return n;
}

std::string dismissed_tag(std::string_view text) {
    text = text.substr(0, text.find('\n'));
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    return std::string(text);
}

}  // namespace overlay
