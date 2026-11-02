#include "benchmark_send.hpp"

#include "i18n/translate.hpp"
#include "json.hpp"

namespace anvil::manager {

std::string receiver_message(std::string_view body) {
    std::string error;
    const auto doc = parse_json(body, error);
    if (!doc || !(*doc)["message"].is_string()) return "";
    std::string message = (*doc)["message"].as_string();
    if (message.size() > 300) message.resize(300);
    return message;
}

std::string send_outcome_text(const SendOutcome& o) {
    if (o.status == 0) return i18n::trf("The report was not sent: {0}. You can copy it instead.", {o.error});
    if (o.status == 201) return i18n::tr("Sent. Thank you.");
    if (o.status == 200) return i18n::tr("This report has already arrived.");
    if (o.status == 429 || o.status == 503) return i18n::tr("The receiver is busy. Try again later.");
    const std::string message = receiver_message(o.body);
    if (o.status == 413 || o.status == 422) return message.empty() ? std::string(i18n::tr("The receiver refused the report.")) : message;
    if (message.empty()) return i18n::trf("The receiver answered {0}.", {std::to_string(o.status)});
    return i18n::trf("The receiver answered {0}: {1}", {std::to_string(o.status), message});
}

}  // namespace anvil::manager
