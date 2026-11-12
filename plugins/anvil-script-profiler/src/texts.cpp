// The panel's words: English, then French, German, Spanish, Simplified and
// Traditional Chinese, Japanese, Korean and Polish. Drafted by the agent; each
// language waits for a native speaker's check (docs/script-profiler.md).
#include "texts.hpp"

#include <cstring>

namespace script_profiler {

namespace {

const TextEntry kEntries[] = {
    {"Script profiler",
     {"Profileur de script", "Skript-Profiler", "Perfilador de script", "脚本性能分析器", "腳本效能分析器",
      "スクリプトプロファイラー", "스크립트 프로파일러", "Profiler skryptów"}},
    {"State", {"État", "Zustand", "Estado", "状态", "狀態", "状態", "상태", "Stan"}},
    {"Measuring: {0} of {1} days, {2} s",
     {"Mesure : {0} jours sur {1}, {2} s", "Messung: {0} von {1} Tagen, {2} s", "Midiendo: {0} de {1} días, {2} s",
      "测量中：{0}/{1} 天，{2} 秒", "測量中：{0}/{1} 天，{2} 秒", "計測中：{0}/{1} 日、{2} 秒",
      "측정 중: {1}일 중 {0}일, {2}초", "Pomiar: {0} z {1} dni, {2} s"}},
    {"Measuring: {0} days, {1} s",
     {"Mesure : {0} jours, {1} s", "Messung: {0} Tage, {1} s", "Midiendo: {0} días, {1} s", "测量中：{0} 天，{1} 秒",
      "測量中：{0} 天，{1} 秒", "計測中：{0} 日、{1} 秒", "측정 중: {0}일, {1}초", "Pomiar: {0} dni, {1} s"}},
    {"Not measuring: nothing in the game is hooked.",
     {"Aucune mesure : rien n'est intercepté dans le jeu.", "Keine Messung: im Spiel ist nichts eingehängt.",
      "Sin medir: no hay nada interceptado en el juego.", "未在测量：游戏中没有任何挂钩。", "未在測量：遊戲中沒有任何掛鉤。",
      "計測していません。ゲームには何もフックしていません。", "측정하지 않음: 게임에 걸린 후크가 없습니다.",
      "Brak pomiaru: w grze nic nie jest podpięte."}},
    {"In multiplayer the measurement slows your game down, and every player waits for the slowest.",
     {"En multijoueur, la mesure ralentit votre partie, et tous les joueurs attendent le plus lent.",
      "Im Mehrspielermodus verlangsamt die Messung dein Spiel, und alle Spieler warten auf den langsamsten.",
      "En multijugador, la medición ralentiza tu partida y todos los jugadores esperan al más lento.",
      "多人游戏中，测量会拖慢你的游戏，而所有玩家都要等最慢的那位。",
      "多人遊戲中，測量會拖慢你的遊戲，而所有玩家都要等最慢的那位。",
      "マルチプレイでは計測であなたのゲームが遅くなり、全員が最も遅いプレイヤーを待ちます。",
      "멀티플레이에서는 측정이 게임을 느리게 하며, 모든 플레이어가 가장 느린 플레이어를 기다립니다.",
      "W grze wieloosobowej pomiar spowalnia Twoją grę, a wszyscy gracze czekają na najwolniejszego."}},
    {"Stop", {"Arrêter", "Stopp", "Detener", "停止", "停止", "停止", "중지", "Stop"}},
    {"Start", {"Démarrer", "Start", "Iniciar", "开始", "開始", "開始", "시작", "Start"}},
    {"Measure {0} days",
     {"Mesurer {0} jours", "{0} Tage messen", "Medir {0} días", "测量 {0} 天", "測量 {0} 天", "{0} 日間計測",
      "{0}일 측정", "Zmierz {0} dni"}},
    {"Load a game to measure.",
     {"Chargez une partie pour mesurer.", "Lade ein Spiel, um zu messen.", "Carga una partida para medir.",
      "载入游戏后才能测量。", "載入遊戲後才能測量。", "計測するにはゲームを読み込んでください。",
      "측정하려면 게임을 불러오세요.", "Wczytaj grę, żeby mierzyć."}},
    {"Times the game's triggers and effects by the event, decision, on_action, casus belli, triggered modifier or "
     "mission they belong to. The game runs slower while it measures.",
     {"Chronomètre les déclencheurs et les effets du jeu selon l'événement, la décision, l'on_action, le casus belli, "
      "le modificateur déclenché ou la mission auxquels ils appartiennent. Le jeu tourne plus lentement pendant la "
      "mesure.",
      "Misst die Zeit der Auslöser und Effekte des Spiels nach dem Ereignis, der Entscheidung, der on_action, dem "
      "Kriegsgrund, dem ausgelösten Modifikator oder der Mission, zu denen sie gehören. Während der Messung läuft das "
      "Spiel langsamer.",
      "Mide el tiempo de los activadores y efectos del juego según el evento, la decisión, la on_action, el casus "
      "belli, el modificador activado o la misión a los que pertenecen. El juego va más lento mientras mide.",
      "按所属的事件、决议、on_action、宣战理由、触发修正或任务，为游戏的触发器和效果计时。测量期间游戏运行较慢。",
      "按所屬的事件、決議、on_action、宣戰理由、觸發修正或任務，為遊戲的觸發器和效果計時。測量期間遊戲執行較慢。",
      "ゲームのトリガーとエフェクトの時間を、所属するイベント、ディシジョン、on_action、開戦事由、トリガー修正、"
      "ミッションごとに計ります。計測中はゲームが遅くなります。",
      "게임의 트리거와 효과에 걸리는 시간을 그것이 속한 이벤트, 결정, on_action, 전쟁 명분, 조건부 수정치, 임무별로 "
      "잽니다. 측정하는 동안 게임이 느려집니다.",
      "Mierzy czas warunków i efektów gry według eventu, decyzji, on_action, casus belli, triggered modifier albo "
      "misji, do których należą. Podczas pomiaru gra działa wolniej."}},
    {"{0} days ({1} to {2}), {3} s",
     {"{0} jours (du {1} au {2}), {3} s", "{0} Tage ({1} bis {2}), {3} s", "{0} días ({1} a {2}), {3} s",
      "{0} 天（{1} 至 {2}），{3} 秒", "{0} 天（{1} 至 {2}），{3} 秒", "{0} 日間（{1}～{2}）、{3} 秒",
      "{0}일({1}~{2}), {3}초", "{0} dni (od {1} do {2}), {3} s"}},
    {"no day passed, {0} s",
     {"aucun jour écoulé, {0} s", "kein Tag vergangen, {0} s", "no pasó ningún día, {0} s", "未经过任何一天，{0} 秒",
      "未經過任何一天，{0} 秒", "1 日も経過せず、{0} 秒", "하루도 지나지 않음, {0}초", "nie minął żaden dzień, {0} s"}},
    {"Last measurement",
     {"Dernière mesure", "Letzte Messung", "Última medición", "上次测量", "上次測量", "前回の計測", "마지막 측정",
      "Ostatni pomiar"}},
    {"Interrupted: the game was loaded or left. The results are what was measured until then.",
     {"Interrompue : une partie a été chargée ou quittée. Les résultats sont ce qui a été mesuré jusque-là.",
      "Unterbrochen: ein Spiel wurde geladen oder verlassen. Die Ergebnisse zeigen, was bis dahin gemessen wurde.",
      "Interrumpida: se cargó o se abandonó la partida. Los resultados son lo medido hasta entonces.",
      "已中断：载入或退出了游戏。结果为此前测得的内容。", "已中斷：載入或離開了遊戲。結果為此前測得的內容。",
      "中断：ゲームが読み込まれたか終了しました。結果はそれまでに計測した分です。",
      "중단됨: 게임을 불러왔거나 나갔습니다. 결과는 그때까지 측정한 내용입니다.",
      "Przerwany: wczytano grę albo z niej wyjście. Wyniki obejmują to, co zmierzono do tej chwili."}},
    {"Could not write {0}",
     {"Impossible d'écrire {0}", "{0} konnte nicht geschrieben werden", "No se pudo escribir {0}", "无法写入 {0}",
      "無法寫入 {0}", "{0} を書き込めませんでした", "{0}을(를) 쓸 수 없습니다", "Nie udało się zapisać {0}"}},
    {"Saved to {0}",
     {"Enregistré dans {0}", "Gespeichert in {0}", "Guardado en {0}", "已保存到 {0}", "已儲存到 {0}",
      "{0} に保存しました", "{0}에 저장했습니다", "Zapisano w {0}"}},
    {"Triggers",
     {"Déclencheurs", "Auslöser", "Activadores", "触发器", "觸發器", "トリガー", "트리거", "Warunki"}},
    {"Effects", {"Effets", "Effekte", "Efectos", "效果", "效果", "エフェクト", "효과", "Efekty"}},
    {"{0} in {1} calls",
     {"{0} en {1} appels", "{0} in {1} Aufrufen", "{0} en {1} llamadas", "{1} 次调用共 {0}", "{1} 次呼叫共 {0}",
      "{1} 回の呼び出しで {0}", "{1}회 호출에 {0}", "{0} w {1} wywołaniach"}},
    {"{0}, {1} calls, mean {2}, max {3}",
     {"{0}, {1} appels, moyenne {2}, max {3}", "{0}, {1} Aufrufe, Mittel {2}, max. {3}",
      "{0}, {1} llamadas, media {2}, máx. {3}", "{0}，{1} 次调用，平均 {2}，最长 {3}", "{0}，{1} 次呼叫，平均 {2}，最長 {3}",
      "{0}、{1} 回、平均 {2}、最大 {3}", "{0}, {1}회 호출, 평균 {2}, 최대 {3}", "{0}, wywołań: {1}, średnio {2}, maks. {3}"}},
    {"Event", {"Événement", "Ereignis", "Evento", "事件", "事件", "イベント", "이벤트", "Event"}},
    {"Event option",
     {"Option d'événement", "Ereignisoption", "Opción de evento", "事件选项", "事件選項", "イベントの選択肢",
      "이벤트 선택지", "Opcja eventu"}},
    {"Decision", {"Décision", "Entscheidung", "Decisión", "决议", "決議", "ディシジョン", "결정", "Decyzja"}},
    {"On action", {"On action", "On action", "On action", "on_action", "on_action", "on_action", "on_action", "On action"}},
    {"Casus belli",
     {"Casus belli", "Kriegsgrund", "Casus belli", "宣战理由", "宣戰理由", "開戦事由", "전쟁 명분", "Casus belli"}},
    {"Triggered modifier",
     {"Modificateur déclenché", "Ausgelöster Modifikator", "Modificador activado", "触发修正", "觸發修正",
      "トリガー修正", "조건부 수정치", "Triggered modifier"}},
    {"Province triggered modifier",
     {"Modificateur de province déclenché", "Ausgelöster Provinzmodifikator", "Modificador de provincia activado",
      "省份触发修正", "省份觸發修正", "州のトリガー修正", "프로빈스 조건부 수정치", "Triggered modifier prowincji"}},
    {"Mission", {"Mission", "Mission", "Misión", "任务", "任務", "ミッション", "임무", "Misja"}},
    {"Church aspect",
     {"Aspect de l'Église", "Kirchenaspekt", "Aspecto de la Iglesia", "教会方针", "教會方針", "教会の側面", "교회 측면",
      "Aspekt kościoła"}},
    {"Scripted trigger",
     {"Déclencheur scripté", "Skript-Auslöser", "Activador programado", "脚本触发器", "腳本觸發器", "スクリプトトリガー",
      "스크립트 트리거", "Scripted trigger"}},
    {"Scripted triggers",
     {"Déclencheurs scriptés", "Skript-Auslöser", "Activadores programados", "脚本触发器", "腳本觸發器",
      "スクリプトトリガー", "스크립트 트리거", "Scripted triggers"}},
    {"No owner found",
     {"Sans propriétaire trouvé", "Kein Besitzer gefunden", "Sin propietario encontrado", "未找到所属对象",
      "未找到所屬物件", "所属先が見つかりません", "소속을 찾지 못함", "Bez znalezionego właściciela"}},
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

}  // namespace script_profiler
