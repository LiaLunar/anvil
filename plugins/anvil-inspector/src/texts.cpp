// The window's words: English, then French, German, Spanish, Simplified and
// Traditional Chinese, Japanese, Korean and Polish. Drafted by the agent; each
// language waits for a native speaker's check (docs/inspector.md).
#include "texts.hpp"

#include <cstring>

namespace anvil_inspector {

namespace {

const TextEntry kEntries[] = {
    {"Inspector",
     {"Inspecteur", "Inspektor", "Inspector", "检查器", "檢查器", "インスペクター", "검사기", "Inspektor"}},
    {"Date", {"Date", "Datum", "Fecha", "日期", "日期", "日付", "날짜", "Data"}},
    {"Refresh", {"Actualiser", "Aktualisieren", "Actualizar", "刷新", "重新整理", "更新", "새로 고침", "Odśwież"}},
    {"Read when the window opens, at a new date (at most once a second), at another province view and with Refresh.",
     {"Lu à l'ouverture de la fenêtre, à chaque nouvelle date (au plus une fois par seconde), à une autre vue de "
      "province et avec Actualiser.",
      "Gelesen beim Öffnen des Fensters, bei einem neuen Datum (höchstens einmal pro Sekunde), bei einer anderen "
      "Provinzansicht und mit Aktualisieren.",
      "Se lee al abrir la ventana, en cada fecha nueva (como mucho una vez por segundo), al abrir otra vista de "
      "provincia y con Actualizar.",
      "在窗口打开时、日期变化时（每秒最多一次）、打开另一个省份视图时以及点击“刷新”时读取。",
      "在視窗開啟時、日期變化時（每秒最多一次）、開啟另一個省份檢視時以及點擊「重新整理」時讀取。",
      "ウィンドウを開いたとき、日付が変わったとき（最大で毎秒1回）、別の州ビューを開いたとき、「更新」を押したときに読み取ります。",
      "창을 열 때, 날짜가 바뀔 때(최대 1초에 한 번), 다른 프로빈스 보기를 열 때, 새로 고침을 누를 때 읽습니다.",
      "Odczyt przy otwarciu okna, przy nowej dacie (najwyżej raz na sekundę), przy innym widoku prowincji i po "
      "kliknięciu Odśwież."}},
    {"No game is running.",
     {"Aucune partie en cours.", "Es läuft kein Spiel.", "No hay ninguna partida en curso.", "没有正在进行的游戏。",
      "沒有正在進行的遊戲。", "ゲームが実行されていません。", "진행 중인 게임이 없습니다.", "Żadna gra nie jest uruchomiona."}},
    {"Open a province's view to see it and its owner here.",
     {"Ouvrez la vue d'une province pour la voir ici avec son propriétaire.",
      "Öffnen Sie die Ansicht einer Provinz, um sie und ihren Besitzer hier zu sehen.",
      "Abra la vista de una provincia para verla aquí junto con su propietario.",
      "打开一个省份的视图，即可在此看到它及其拥有者。", "開啟一個省份的檢視，即可在此看到它及其擁有者。",
      "州のビューを開くと、その州と所有国がここに表示されます。",
      "프로빈스 보기를 열면 여기에서 그 프로빈스와 소유국을 볼 수 있습니다.",
      "Otwórz widok prowincji, aby zobaczyć tu ją i jej właściciela."}},
    {"The province view needs a newer anvil-game.",
     {"La vue de province nécessite un anvil-game plus récent.", "Die Provinzansicht braucht ein neueres anvil-game.",
      "La vista de provincia necesita un anvil-game más reciente.", "省份视图需要更新版本的 anvil-game。",
      "省份檢視需要更新版本的 anvil-game。", "州ビューには新しい anvil-game が必要です。",
      "프로빈스 보기에는 더 새로운 anvil-game이 필요합니다.", "Widok prowincji wymaga nowszego anvil-game."}},
    {"Province: {0} ({1})",
     {"Province : {0} ({1})", "Provinz: {0} ({1})", "Provincia: {0} ({1})", "省份：{0}（{1}）", "省份：{0}（{1}）",
      "州：{0}（{1}）", "프로빈스: {0} ({1})", "Prowincja: {0} ({1})"}},
    {"Province {0}",
     {"Province {0}", "Provinz {0}", "Provincia {0}", "省份 {0}", "省份 {0}", "州 {0}", "프로빈스 {0}", "Prowincja {0}"}},
    {"Owner: {0}",
     {"Propriétaire : {0}", "Besitzer: {0}", "Propietario: {0}", "拥有者：{0}", "擁有者：{0}", "所有国：{0}",
      "소유국: {0}", "Właściciel: {0}"}},
    {"Your country: {0}",
     {"Votre pays : {0}", "Ihr Land: {0}", "Su país: {0}", "你的国家：{0}", "你的國家：{0}", "あなたの国：{0}",
      "당신의 국가: {0}", "Twój kraj: {0}"}},
    {"Owner and your country: {0}",
     {"Propriétaire et votre pays : {0}", "Besitzer und Ihr Land: {0}", "Propietario y su país: {0}",
      "拥有者，也是你的国家：{0}", "擁有者，也是你的國家：{0}", "所有国（あなたの国）：{0}", "소유국이자 당신의 국가: {0}",
      "Właściciel i Twój kraj: {0}"}},
    {"Flags ({0})",
     {"Drapeaux ({0})", "Flags ({0})", "Marcas ({0})", "标记（{0}）", "標記（{0}）", "フラグ（{0}）", "플래그({0})",
      "Flagi ({0})"}},
    {"Variables ({0})",
     {"Variables ({0})", "Variablen ({0})", "Variables ({0})", "变量（{0}）", "變數（{0}）", "変数（{0}）", "변수({0})",
      "Zmienne ({0})"}},
    {"Timed modifiers ({0})",
     {"Modificateurs temporaires ({0})", "Zeitlich begrenzte Modifikatoren ({0})", "Modificadores temporales ({0})",
      "限时修正（{0}）", "限時修正（{0}）", "期間付き修正（{0}）", "기간 수정치({0})", "Modyfikatory czasowe ({0})"}},
    {"Ruler, heir and consort",
     {"Souverain, héritier et consort", "Herrscher, Erbe und Gemahl", "Gobernante, heredero y consorte",
      "统治者、继承人与配偶", "統治者、繼承人與配偶", "君主・後継者・配偶者", "통치자, 후계자, 배우자",
      "Władca, następca i małżonek"}},
    {"Ruler", {"Souverain", "Herrscher", "Gobernante", "统治者", "統治者", "君主", "통치자", "Władca"}},
    {"Heir", {"Héritier", "Erbe", "Heredero", "继承人", "繼承人", "後継者", "후계자", "Następca"}},
    {"Consort", {"Consort", "Gemahl", "Consorte", "配偶", "配偶", "配偶者", "배우자", "Małżonek"}},
    {"none", {"aucun", "keiner", "ninguno", "无", "無", "なし", "없음", "brak"}},
    {"set {0}",
     {"posé le {0}", "gesetzt am {0}", "puesta el {0}", "设于 {0}", "設於 {0}", "{0} に設定", "{0}에 설정",
      "ustawiona {0}"}},
    {"until {0}",
     {"jusqu'au {0}", "bis {0}", "hasta el {0}", "至 {0}", "至 {0}", "{0} まで", "{0}까지", "do {0}"}},
    {"no end date",
     {"sans date de fin", "ohne Enddatum", "sin fecha de fin", "无结束日期", "無結束日期", "終了日なし", "종료일 없음",
      "bez daty końca"}},
    {"hidden", {"caché", "verborgen", "oculto", "隐藏", "隱藏", "非表示", "숨김", "ukryty"}},
    {"ruler modifier",
     {"modificateur du souverain", "Herrschermodifikator", "modificador del gobernante", "统治者修正", "統治者修正",
      "君主修正", "통치자 수정치", "modyfikator władcy"}},
    {"{0} more",
     {"{0} de plus", "{0} weitere", "{0} más", "另有 {0} 项", "另有 {0} 項", "ほか {0} 件", "{0}개 더", "jeszcze {0}"}},
    {"Not available on this game build.",
     {"Non disponible sur cette version du jeu.", "Auf diesem Spiel-Build nicht verfügbar.",
      "No disponible en esta versión del juego.", "此游戏版本不支持。", "此遊戲版本不支援。",
      "このゲームのビルドでは使えません。", "이 게임 빌드에서는 사용할 수 없습니다.", "Niedostępne w tej wersji gry."}},
    {"Needs a newer anvil-game.",
     {"Nécessite un anvil-game plus récent.", "Braucht ein neueres anvil-game.", "Necesita un anvil-game más reciente.",
      "需要更新版本的 anvil-game。", "需要更新版本的 anvil-game。", "新しい anvil-game が必要です。",
      "더 새로운 anvil-game이 필요합니다.", "Wymaga nowszego anvil-game."}},
    {"Could not be read ({0}).",
     {"Lecture impossible ({0}).", "Konnte nicht gelesen werden ({0}).", "No se pudo leer ({0}).", "无法读取（{0}）。",
      "無法讀取（{0}）。", "読み取れませんでした（{0}）。", "읽을 수 없습니다({0}).", "Nie udało się odczytać ({0})."}},
};

}  // namespace

const TextEntry* text_entries(std::size_t& count) {
    count = sizeof(kEntries) / sizeof(kEntries[0]);
    return kEntries;
}

Lang lang_of(std::string_view code) {
    if (code == "fr") return Lang::fr;
    if (code == "de") return Lang::de;
    if (code == "es") return Lang::es;
    if (code == "zh-Hans") return Lang::zh_hans;
    if (code == "zh-Hant") return Lang::zh_hant;
    if (code == "ja") return Lang::ja;
    if (code == "ko") return Lang::ko;
    if (code == "pl") return Lang::pl;
    return Lang::en;
}

const char* Texts::operator()(const char* english) const {
    if (lang == Lang::en) return english;
    for (const TextEntry& e : kEntries) {
        if (std::strcmp(e.english, english) == 0) return e.translated[static_cast<std::size_t>(lang) - 1];
    }
    return english;
}

std::string Texts::f(const char* english, std::initializer_list<std::string_view> args) const {
    const std::string_view pattern = (*this)(english);
    std::string out;
    out.reserve(pattern.size() + 32);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{' && i + 2 < pattern.size() && pattern[i + 1] >= '0' && pattern[i + 1] <= '9' &&
            pattern[i + 2] == '}') {
            const std::size_t n = static_cast<std::size_t>(pattern[i + 1] - '0');
            if (n < args.size()) out += *(args.begin() + n);
            i += 2;
            continue;
        }
        out += pattern[i];
    }
    return out;
}

}  // namespace anvil_inspector
