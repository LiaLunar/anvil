// The panel's words: English, then French, German, Spanish, Simplified and
// Traditional Chinese, Japanese, Korean and Polish. Drafted by the agent; each
// language waits for a native speaker's check (docs/script-errors-panel.md).
#include "texts.hpp"

#include <cstring>

namespace script_errors {

namespace {

const TextEntry kEntries[] = {
    {"Script errors",
     {"Erreurs de script", "Skriptfehler", "Errores de script", "脚本错误", "腳本錯誤", "スクリプトエラー", "스크립트 오류",
      "Błędy skryptu"}},
    {"Messages",
     {"Messages", "Meldungen", "Mensajes", "消息", "訊息", "メッセージ", "메시지", "Komunikaty"}},
    {"{0}, {1} times",
     {"{0}, {1} fois", "{0}, {1}-mal", "{0}, {1} veces", "{0} 条，共 {1} 次", "{0} 則，共 {1} 次", "{0} 件、{1} 回",
      "{0}개, {1}회", "{0}, razy: {1}"}},
    {"Counted since the panel was turned on: earlier lines are not in it.",
     {"Compté depuis l'activation du panneau : les lignes antérieures n'y sont pas.",
      "Gezählt seit dem Einschalten des Panels: frühere Zeilen fehlen.",
      "Contado desde que se activó el panel: las líneas anteriores no están.",
      "自面板开启后计数：之前的行不在其中。", "自面板開啟後計數：之前的行不在其中。",
      "パネルをオンにしてから数えています。それより前の行は含まれません。",
      "패널을 켠 뒤부터 셉니다. 그 전의 줄은 들어 있지 않습니다.",
      "Liczone od włączenia panelu: wcześniejszych linii tu nie ma."}},
    {"{0} lines of further messages are not counted: the table is full.",
     {"{0} lignes d'autres messages ne sont pas comptées : la table est pleine.",
      "{0} Zeilen weiterer Meldungen werden nicht gezählt: die Tabelle ist voll.",
      "{0} líneas de otros mensajes no se cuentan: la tabla está llena.",
      "另有 {0} 行新消息未计数：表已满。", "另有 {0} 行新訊息未計數：表已滿。",
      "ほかのメッセージの {0} 行は数えていません。表がいっぱいです。",
      "다른 메시지의 {0}줄은 세지 않습니다. 표가 가득 찼습니다.",
      "Linii dalszych komunikatów nie liczę, bo tabela jest pełna: {0}."}},
    {"Show every message",
     {"Afficher tous les messages", "Alle Meldungen zeigen", "Mostrar todos los mensajes", "显示全部消息",
      "顯示全部訊息", "すべてのメッセージを表示", "모든 메시지 보기", "Pokaż wszystkie komunikaty"}},
    {"Show only new ({0})",
     {"N'afficher que les nouveaux ({0})", "Nur neue zeigen ({0})", "Mostrar solo los nuevos ({0})",
      "只显示新的（{0}）", "只顯示新的（{0}）", "新しいものだけ表示（{0}）", "새 항목만 보기({0})",
      "Pokaż tylko nowe ({0})"}},
    {"Mark all seen",
     {"Tout marquer comme vu", "Alle als gesehen markieren", "Marcar todo como visto", "全部标为已读",
      "全部標為已讀", "すべて既読にする", "모두 읽음으로 표시", "Oznacz wszystko jako przejrzane"}},
    {"Everything so far is marked seen.",
     {"Tout ce qui précède est marqué comme vu.", "Alles bisherige ist als gesehen markiert.",
      "Todo lo anterior está marcado como visto.", "目前的一切已标为已读。", "目前的一切已標為已讀。",
      "ここまでのものをすべて既読にしました。", "지금까지의 항목을 모두 읽음으로 표시했습니다.",
      "Wszystko dotąd oznaczone jako przejrzane."}},
    {"No message in the log yet.",
     {"Aucun message dans le journal pour l'instant.", "Noch keine Meldung im Log.",
      "Todavía no hay mensajes en el registro.", "日志中还没有消息。", "記錄中還沒有訊息。",
      "ログにはまだメッセージがありません。", "아직 로그에 메시지가 없습니다.", "W logu nie ma jeszcze komunikatów."}},
    {"With a file and line",
     {"Avec un fichier et une ligne", "Mit Datei und Zeile", "Con archivo y línea", "带文件和行号", "帶檔案和行號",
      "ファイルと行あり", "파일과 줄 있음", "Z plikiem i linią"}},
    {"Script: triggers and effects",
     {"Script : déclencheurs et effets", "Skript: Auslöser und Effekte", "Script: activadores y efectos",
      "脚本：触发器和效果", "腳本：觸發器和效果", "スクリプト：トリガーとエフェクト", "스크립트: 트리거와 효과",
      "Skrypt: warunki i efekty"}},
    {"Undefined event targets",
     {"Cibles d'événement non définies", "Undefinierte Event-Ziele", "Objetivos de evento sin definir",
      "未定义的事件目标", "未定義的事件目標", "未定義のイベントターゲット", "정의되지 않은 이벤트 대상",
      "Niezdefiniowane event targety"}},
    {"Localisation",
     {"Localisation", "Lokalisierung", "Localización", "本地化", "在地化", "ローカライズ", "현지화", "Lokalizacja"}},
    {"Interface (.gui, .gfx)",
     {"Interface (.gui, .gfx)", "Oberfläche (.gui, .gfx)", "Interfaz (.gui, .gfx)", "界面（.gui、.gfx）",
      "介面（.gui、.gfx）", "インターフェース（.gui、.gfx）", "인터페이스(.gui, .gfx)", "Interfejs (.gui, .gfx)"}},
    {"Graphics and fonts",
     {"Graphismes et polices", "Grafik und Schriften", "Gráficos y fuentes", "图形和字体", "圖形和字型",
      "グラフィックとフォント", "그래픽과 글꼴", "Grafika i czcionki"}},
    {"Other",
     {"Autres", "Sonstiges", "Otros", "其他", "其他", "その他", "기타", "Inne"}},
    {"Files the game could not open or check",
     {"Fichiers que le jeu n'a pas pu ouvrir ou vérifier", "Dateien, die das Spiel nicht öffnen oder prüfen konnte",
      "Archivos que el juego no pudo abrir ni comprobar", "游戏无法打开或校验的文件", "遊戲無法開啟或校驗的檔案",
      "ゲームが開けない、または検査できないファイル", "게임이 열거나 검사하지 못한 파일",
      "Pliki, których gra nie otworzyła albo nie sprawdziła"}},
    {"{0}: {1} messages, {2} times, {3} new",
     {"{0} : {1} messages, {2} fois, {3} nouveaux", "{0}: {1} Meldungen, {2}-mal, {3} neu",
      "{0}: {1} mensajes, {2} veces, {3} nuevos", "{0}：{1} 条消息，{2} 次，{3} 条新的", "{0}：{1} 則訊息，{2} 次，{3} 則新的",
      "{0}：{1} 件、{2} 回、新規 {3} 件", "{0}: 메시지 {1}개, {2}회, 새 항목 {3}개",
      "{0}: komunikaty: {1}, razy: {2}, nowe: {3}"}},
    {"{0}: {1} messages, {2} times",
     {"{0} : {1} messages, {2} fois", "{0}: {1} Meldungen, {2}-mal", "{0}: {1} mensajes, {2} veces",
      "{0}：{1} 条消息，{2} 次", "{0}：{1} 則訊息，{2} 次", "{0}：{1} 件、{2} 回", "{0}: 메시지 {1}개, {2}회",
      "{0}: komunikaty: {1}, razy: {2}"}},
    {"{0}: {1} files",
     {"{0} : {1} fichiers", "{0}: {1} Dateien", "{0}: {1} archivos", "{0}：{1} 个文件", "{0}：{1} 個檔案",
      "{0}：{1} ファイル", "{0}: 파일 {1}개", "{0}: plików: {1}"}},
    {"{0} more",
     {"{0} de plus", "{0} weitere", "{0} más", "还有 {0} 项", "還有 {0} 項", "ほか {0} 件", "{0}개 더",
      "i jeszcze {0}"}},
    {"{0} more files",
     {"{0} fichiers de plus", "{0} weitere Dateien", "{0} archivos más", "还有 {0} 个文件", "還有 {0} 個檔案",
      "ほか {0} ファイル", "파일 {0}개 더", "i jeszcze plików: {0}"}},
    {"Event target",
     {"Cible d'événement", "Event-Ziel", "Objetivo de evento", "事件目标", "事件目標", "イベントターゲット",
      "이벤트 대상", "Event target"}},
    {"Count",
     {"Nombre", "Anzahl", "Cantidad", "次数", "次數", "回数", "횟수", "Liczba"}},
    {"{0} in script, {1} in tooltips",
     {"{0} dans le script, {1} dans les infobulles", "{0} im Skript, {1} in Tooltips",
      "{0} en el script, {1} en descripciones emergentes", "脚本中 {0} 次，提示中 {1} 次", "腳本中 {0} 次，提示中 {1} 次",
      "スクリプトで {0} 回、ツールチップで {1} 回", "스크립트에서 {0}회, 툴팁에서 {1}회",
      "w skrypcie: {0}, w podpowiedziach: {1}"}},
    {"Message",
     {"Message", "Meldung", "Mensaje", "消息", "訊息", "メッセージ", "메시지", "Komunikat"}},
    {"{0}: {1} written, {2} counted by the throttle",
     {"{0} : {1} écrites, {2} comptées par le limiteur", "{0}: {1} geschrieben, {2} vom Drosseln gezählt",
      "{0}: {1} escritas, {2} contadas por el limitador", "{0}：写入 {1} 次，限流计数 {2} 次",
      "{0}：寫入 {1} 次，限流計數 {2} 次", "{0}：書き込み {1} 回、抑制で計数 {2} 回",
      "{0}: 기록 {1}회, 제한으로 셈 {2}회", "{0}: zapisane: {1}, policzone przez ograniczanie: {2}"}},
    {"Game code",
     {"Code du jeu", "Spielcode", "Código del juego", "游戏代码", "遊戲程式碼", "ゲームのコード", "게임 코드",
      "Kod gry"}},
    {"[{0}:{1}], the game's C++ code, not a mod's file",
     {"[{0}:{1}], le code C++ du jeu, pas un fichier de mod", "[{0}:{1}], der C++-Code des Spiels, keine Mod-Datei",
      "[{0}:{1}], el código C++ del juego, no un archivo de mod", "[{0}:{1}]，游戏的 C++ 代码，不是模组文件",
      "[{0}:{1}]，遊戲的 C++ 程式碼，不是模組檔案", "[{0}:{1}]、ゲームの C++ コードで、MOD のファイルではありません",
      "[{0}:{1}], 게임의 C++ 코드이며 모드 파일이 아닙니다", "[{0}:{1}], kod C++ gry, nie plik moda"}},
    {"File",
     {"Fichier", "Datei", "Archivo", "文件", "檔案", "ファイル", "파일", "Plik"}},
    {"The game names no file or line.",
     {"Le jeu n'indique ni fichier ni ligne.", "Das Spiel nennt keine Datei und keine Zeile.",
      "El juego no indica archivo ni línea.", "游戏没有给出文件或行号。", "遊戲沒有給出檔案或行號。",
      "ゲームはファイルも行も示していません。", "게임이 파일이나 줄을 알려 주지 않습니다.",
      "Gra nie podaje pliku ani linii."}},
    {"{0} in {1}",
     {"{0} dans {1}", "{0} in {1}", "{0} en {1}", "{1} 中的 {0}", "{1} 中的 {0}", "{1} の {0}", "{1}의 {0}",
      "{0} w {1}"}},
    {"Open in VS Code",
     {"Ouvrir dans VS Code", "In VS Code öffnen", "Abrir en VS Code", "在 VS Code 中打开", "在 VS Code 中開啟",
      "VS Code で開く", "VS Code에서 열기", "Otwórz w VS Code"}},
    {"Copy the path and line",
     {"Copier le chemin et la ligne", "Pfad und Zeile kopieren", "Copiar la ruta y la línea", "复制路径和行号",
      "複製路徑和行號", "パスと行をコピー", "경로와 줄 복사", "Kopiuj ścieżkę i linię"}},
    {"{0}: no layer of the playset has it",
     {"{0} : aucune couche de la liste de lecture ne l'a", "{0}: keine Ebene der Playlist hat sie",
      "{0}: ninguna capa de la lista de reproducción lo tiene", "{0}：播放集的任何一层都没有它",
      "{0}：播放集的任何一層都沒有它", "{0}：プレイセットのどの層にもありません", "{0}: 플레이셋의 어느 층에도 없습니다",
      "{0}: nie ma go w żadnej warstwie zestawu"}},
    {"Find '{0}' in the playset's script",
     {"Chercher « {0} » dans le script de la liste de lecture", "„{0}“ im Skript der Playlist suchen",
      "Buscar «{0}» en el script de la lista de reproducción", "在播放集的脚本中查找“{0}”", "在播放集的腳本中尋找「{0}」",
      "プレイセットのスクリプトで「{0}」を検索", "플레이셋 스크립트에서 '{0}' 찾기",
      "Szukaj „{0}” w skryptach zestawu"}},
    {"Searching events, common, decisions, missions and history/countries...",
     {"Recherche dans events, common, decisions, missions et history/countries...",
      "Suche in events, common, decisions, missions und history/countries...",
      "Buscando en events, common, decisions, missions y history/countries...",
      "正在搜索 events、common、decisions、missions 和 history/countries...",
      "正在搜尋 events、common、decisions、missions 和 history/countries...",
      "events、common、decisions、missions、history/countries を検索中...",
      "events, common, decisions, missions, history/countries 검색 중...",
      "Szukam w events, common, decisions, missions i history/countries..."}},
    {"Found",
     {"Trouvé", "Gefunden", "Encontrado", "找到", "找到", "検索結果", "찾음", "Znaleziono"}},
    {"{0} uses in {1} files ({2} ms)",
     {"{0} utilisations dans {1} fichiers ({2} ms)", "{0} Verwendungen in {1} Dateien ({2} ms)",
      "{0} usos en {1} archivos ({2} ms)", "{1} 个文件中 {0} 处（{2} 毫秒）", "{1} 個檔案中 {0} 處（{2} 毫秒）",
      "{1} ファイルに {0} か所（{2} ms）", "파일 {1}개에서 {0}곳({2}ms)", "użycia: {0} w plikach: {1} ({2} ms)"}},
    {"saved at {0}",
     {"enregistrée à {0}", "gespeichert in {0}", "guardado en {0}", "保存于 {0}", "儲存於 {0}", "保存：{0}",
      "저장 위치: {0}", "zapisany w {0}"}},
    {"used at {0}",
     {"utilisée à {0}", "verwendet in {0}", "usado en {0}", "使用于 {0}", "使用於 {0}", "使用：{0}", "사용 위치: {0}",
      "użyty w {0}"}},
    {"Opened {0}",
     {"Ouvert : {0}", "Geöffnet: {0}", "Abierto: {0}", "已打开 {0}", "已開啟 {0}", "開きました：{0}", "열었습니다: {0}",
      "Otwarto {0}"}},
    {"No program opened {0}.",
     {"Aucun programme n'a ouvert {0}.", "Kein Programm hat {0} geöffnet.", "Ningún programa abrió {0}.",
      "没有程序打开 {0}。", "沒有程式開啟 {0}。", "{0} を開くプログラムがありません。", "{0}을(를) 연 프로그램이 없습니다.",
      "Żaden program nie otworzył {0}."}},
    {"Copied {0}",
     {"Copié : {0}", "Kopiert: {0}", "Copiado: {0}", "已复制 {0}", "已複製 {0}", "コピーしました：{0}",
      "복사했습니다: {0}", "Skopiowano {0}"}},
};

}  // namespace

const TextEntry* text_entries(std::size_t& count) {
    count = std::size(kEntries);
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

}  // namespace script_errors
