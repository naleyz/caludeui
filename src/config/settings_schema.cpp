#include "config/settings_schema.hpp"

namespace config
{
namespace
{
constexpr const char* kSession   = "Сессия";
constexpr const char* kModel     = "Модель";
constexpr const char* kWorkflows = "Workflows и артефакты";
constexpr const char* kInterface = "Интерфейс";
constexpr const char* kPerms     = "Разрешения и git";
constexpr const char* kCopy      = "Копирование";
constexpr const char* kAgents    = "Агенты";
constexpr const char* kNotify    = "Уведомления";
constexpr const char* kUpdates   = "Обновления";

constexpr const char* kLocalScope =
    "Claude Code из /config пишет это в .claude/settings.local.json текущего проекта. ClaudeUI пишет в "
    "~/.claude/settings.json (для всех проектов); если в проекте задано своё значение, оно главнее.";

std::vector<SettingDef> Build()
{
    std::vector<SettingDef> v;

    // ── Сессия ───────────────────────────────────────────────────────
    v.push_back({.id = "autoCompact", .label = "Auto-compact",
                 .description = "Автоматически сжимать историю, когда контекст подходит к пределу окна модели.",
                 .group = kSession, .path = {"autoCompactEnabled"}, .defaultValue = true,
                 .legacyGlobalFallback = true});
    v.push_back({.id = "autoContinueAtUsageLimit", .label = "Continue automatically at usage limit",
                 .description = "Упёрлись в лимит использования — Claude сам продолжит работу, когда лимит сбросится.",
                 .group = kSession, .path = {"autoContinueAtUsageLimit"}, .defaultValue = true});
    v.push_back({.id = "switchModelsOnFlag", .label = "Switch models when a message is flagged",
                 .description = "Если защитные фильтры модели пометили сообщение — переключиться на другую модель "
                                "автоматически, а не спрашивать каждый раз.",
                 .group = kSession, .path = {"switchModelsOnFlag"},
                 .defaultNote = "не задано — Claude спросит при первом срабатывании"});
    v.push_back({.id = "thinking", .label = "Thinking mode",
                 .description = "Расширенное размышление перед ответом.",
                 .group = kSession, .path = {"alwaysThinkingEnabled"}, .defaultValue = true,
                 .writeMode = WriteMode::OmitDefault});
    v.push_back({.id = "fast", .label = "Fast mode (Opus 5)",
                 .description = "Быстрый режим: Opus с ускоренным выводом.",
                 .group = kSession, .path = {"fastMode"}, .defaultValue = false,
                 .writeMode = WriteMode::OmitDefault});
    v.push_back({.id = "promptSuggestionEnabled", .label = "Prompt suggestions",
                 .description = "Подсказки следующего запроса в поле ввода.",
                 .group = kSession, .path = {"promptSuggestionEnabled"}, .defaultValue = true,
                 .writeMode = WriteMode::OmitDefault});
    v.push_back({.id = "recap", .label = "Session recap",
                 .description = "Краткое резюме происходившего, когда вы возвращаетесь в сессию после паузы.",
                 .group = kSession, .path = {"awaySummaryEnabled"}, .defaultValue = true,
                 .writeMode = WriteMode::OmitDefault});
    v.push_back({.id = "checkpoints", .label = "Rewind code (checkpoints)",
                 .description = "Снимки файлов перед правками — код можно откатить через /rewind (Esc Esc).",
                 .group = kSession, .path = {"fileCheckpointingEnabled"}, .defaultValue = true,
                 .legacyGlobalFallback = true});

    // ── Модель ───────────────────────────────────────────────────────
    v.push_back({.id = "model", .label = "Model",
                 .description = "Модель по умолчанию для новых сессий: семейство (всегда последняя версия) или "
                                "конкретная версия, при желании с 1M контекстом.",
                 .group = kModel, .path = {"model"}, .kind = Kind::Model, .defaultValue = "default",
                 .writeMode = WriteMode::OmitDefault});
    v.push_back({.id = "effortLevel", .label = "Effort level",
                 .description = "Сохранённый уровень усилий (/effort) для моделей, которые его поддерживают.",
                 .group = kModel, .path = {"effortLevel"}, .kind = Kind::Enum,
                 .defaultNote = "зависит от модели",
                 .options = {{"low", ""}, {"medium", ""}, {"high", ""}, {"xhigh", ""}}});
    v.push_back({.id = "tui", .label = "Renderer (tui)",
                 .description = "fullscreen — рендер без мерцания в альтернативном экране с виртуальной прокруткой; "
                                "default — классический рендер в основном экране.",
                 .group = kModel, .path = {"tui"}, .kind = Kind::Enum, .defaultValue = "default",
                 .options = {{"default", ""}, {"fullscreen", ""}}});
    v.push_back({.id = "askUserQuestionTimeout", .label = "Question auto-continue timeout",
                 .description = "Через сколько бездействия вопросы Claude продолжаются с уже выбранными ответами.",
                 .group = kModel, .path = {"askUserQuestionTimeout"}, .kind = Kind::Enum, .defaultValue = "never",
                 .options = {{"60s", ""}, {"5m", ""}, {"10m", ""}, {"never", ""}}});

    // ── Workflows и артефакты ────────────────────────────────────────
    v.push_back({.id = "workflows", .label = "Dynamic workflows",
                 .description = "Динамические workflows: для больших задач Claude запускает много агентов параллельно.",
                 .group = kWorkflows, .path = {"enableWorkflows"},
                 .defaultNote = "зависит от плана и включённых функций",
                 .clearAlso = {{"disableWorkflows"}}, .forcedOffBy = {"disableWorkflows"}});
    v.push_back({.id = "workflowKeywordTriggerEnabled", .label = "Ultracode keyword trigger",
                 .description = "Слово «ultracode» в запросе запускает workflow.",
                 .group = kWorkflows, .path = {"workflowKeywordTriggerEnabled"}, .defaultValue = true,
                 .writeMode = WriteMode::OmitDefault});
    v.push_back({.id = "workflowSizeGuideline", .label = "Dynamic workflow size",
                 .description = "Ориентир по размеру workflow — сколько агентов Claude старается не превышать.",
                 .group = kWorkflows, .store = Store::GlobalConfig, .path = {"workflowSizeGuideline"},
                 .kind = Kind::Enum, .defaultNote = "зависит от плана (medium или small)",
                 .options = {{"unrestricted", "unrestricted"}, {"small", "small (< 5 агентов)"},
                             {"medium", "medium (< 10 агентов)"}, {"large", "large (< 50 агентов)"}}});
    v.push_back({.id = "artifacts", .label = "Artifacts",
                 .description = "Публикация HTML-страниц (Artifacts) прямо из сессии.",
                 .group = kWorkflows, .path = {"enableArtifact"}, .defaultNote = "зависит от аккаунта",
                 .clearAlso = {{"disableArtifact"}}, .forcedOffBy = {"disableArtifact"}});

    // ── Интерфейс ────────────────────────────────────────────────────
    v.push_back({.id = "tips", .label = "Show tips",
                 .description = "Советы в строке ожидания ответа.",
                 .group = kInterface, .path = {"spinnerTipsEnabled"}, .defaultValue = true, .scopeNote = kLocalScope});
    v.push_back({.id = "feedbackDrafts", .label = "Claude-drafted feedback",
                 .description = "Черновики отзывов о проблемах Claude Code: notify — сообщать о них, quiet — молча "
                                "сохранять, off — не готовить.",
                 .group = kInterface, .path = {"feedbackDrafts"}, .kind = Kind::Enum, .defaultValue = "notify",
                 .options = {{"notify", ""}, {"quiet", ""}, {"off", ""}}});
    v.push_back({.id = "reduceMotion", .label = "Reduce motion",
                 .description = "Меньше анимаций: без мерцания спиннера и вспышек.",
                 .group = kInterface, .path = {"prefersReducedMotion"}, .defaultValue = false,
                 .scopeNote = kLocalScope});
    v.push_back({.id = "verbose", .label = "Verbose output",
                 .description = "Подробный вывод: полные результаты инструментов в транскрипте.",
                 .group = kInterface, .path = {"verbose"}, .defaultValue = false, .legacyGlobalFallback = true});
    v.push_back({.id = "progressBar", .label = "Terminal progress bar",
                 .description = "Индикатор прогресса через OSC 9;4. ClaudeUI показывает его полоской на вкладке сессии.",
                 .group = kInterface, .path = {"terminalProgressBarEnabled"}, .defaultValue = true,
                 .legacyGlobalFallback = true});
    v.push_back({.id = "turnDuration", .label = "Show turn duration",
                 .description = "Показывать, сколько длился каждый ход.",
                 .group = kInterface, .path = {"showTurnDuration"}, .defaultValue = true,
                 .legacyGlobalFallback = true});
    v.push_back({.id = "timestamps", .label = "Show message timestamps",
                 .description = "Время у каждого сообщения в транскрипте.",
                 .group = kInterface, .path = {"showMessageTimestamps"}, .defaultValue = false,
                 .legacyGlobalFallback = true});
    v.push_back({.id = "timeFormat", .label = "Time format",
                 .description = "Формат времени в интерфейсе.",
                 .group = kInterface, .path = {"timeFormat"}, .kind = Kind::Enum, .defaultValue = "auto",
                 .options = {{"auto", ""}, {"12-hour", ""}, {"24-hour", ""}, {"24-hour-utc", ""}}});
    v.push_back({.id = "theme", .label = "Theme",
                 .description = "Цветовая тема Claude Code. Свои темы — через /theme.",
                 .group = kInterface, .path = {"theme"}, .kind = Kind::Enum, .defaultValue = "dark",
                 .options = {{"auto", ""}, {"dark", "Dark mode"}, {"light", "Light mode"},
                             {"dark-daltonized", "Dark (daltonized)"}, {"light-daltonized", "Light (daltonized)"},
                             {"dark-ansi", "Dark (ANSI colors)"}, {"light-ansi", "Light (ANSI colors)"}},
                 .legacyGlobalFallback = true});
    v.push_back({.id = "outputStyle", .label = "Output style",
                 .description = "Стиль ответов. Можно указать имя своего стиля из ~/.claude/output-styles.",
                 .group = kInterface, .path = {"outputStyle"}, .kind = Kind::Text, .defaultValue = "default",
                 .options = {{"default", ""}, {"Proactive", ""}, {"Concise", ""}, {"Explanatory", ""}, {"Learning", ""}},
                 .scopeNote = kLocalScope});
    v.push_back({.id = "language", .label = "Language",
                 .description = "Язык ответов Claude и голосового ввода: название языка или ISO-код (ru, ja…).",
                 .group = kInterface, .path = {"language"}, .kind = Kind::Text, .defaultValue = "default",
                 .options = {{"default", "Default (English)"}, {"russian", ""}, {"english", ""}, {"ukrainian", ""},
                             {"german", ""}, {"japanese", ""}},
                 .writeMode = WriteMode::OmitDefault});
    v.push_back({.id = "editor", .label = "Editor mode",
                 .description = "Режим редактирования поля ввода: обычный или vim.",
                 .group = kInterface, .path = {"editorMode"}, .kind = Kind::Enum, .defaultValue = "normal",
                 .options = {{"normal", ""}, {"vim", ""}}, .legacyGlobalFallback = true});
    v.push_back({.id = "autoScroll", .label = "Auto-scroll",
                 .description = "Прокручивать к новому выводу (fullscreen-рендер).",
                 .group = kInterface, .path = {"autoScrollEnabled"}, .defaultValue = true,
                 .legacyGlobalFallback = true});

    // ── Разрешения и git ─────────────────────────────────────────────
    v.push_back({.id = "permissionMode", .label = "Default permission mode",
                 .description = "Режим разрешений, с которым стартуют сессии.",
                 .group = kPerms, .path = {"permissions", "defaultMode"}, .kind = Kind::Enum,
                 .defaultValue = "default",
                 .options = {{"default", "Manual"}, {"plan", "Plan"}, {"acceptEdits", "Accept edits"},
                             {"auto", "Auto"}, {"dontAsk", "Don't ask"}}});
    v.push_back({.id = "worktreeBaseRef", .label = "Worktree base ref",
                 .description = "От чего создавать git worktree: fresh — от актуальной основной ветки, head — от "
                                "текущего HEAD.",
                 .group = kPerms, .path = {"worktree", "baseRef"}, .kind = Kind::Enum, .defaultValue = "fresh",
                 .options = {{"fresh", ""}, {"head", ""}}});
    v.push_back({.id = "useAutoModeDuringPlan", .label = "Use auto mode during plan",
                 .description = "Использовать auto mode разрешений, пока идёт планирование (plan mode).",
                 .group = kPerms, .path = {"useAutoModeDuringPlan"}, .defaultValue = true});
    v.push_back({.id = "gitignore", .label = "Respect .gitignore in file picker",
                 .description = "Не показывать файлы из .gitignore в @-выборе файлов.",
                 .group = kPerms, .store = Store::GlobalConfig, .path = {"respectGitignore"}, .defaultValue = true});

    // ── Копирование ──────────────────────────────────────────────────
    v.push_back({.id = "copyFullResponse", .label = "Skip the /copy picker",
                 .description = "/copy сразу копирует ответ целиком, без выбора блока кода.",
                 .group = kCopy, .store = Store::GlobalConfig, .path = {"copyFullResponse"}, .defaultValue = false});
    v.push_back({.id = "copyOnSelect", .label = "Copy on select",
                 .description = "Выделение мышью сразу копирует текст (fullscreen-рендер). В ClaudeUI работает через "
                                "OSC 52 — текст попадает в системный буфер.",
                 .group = kCopy, .store = Store::GlobalConfig, .path = {"copyOnSelect"}, .defaultValue = true});

    // ── Агенты ───────────────────────────────────────────────────────
    v.push_back({.id = "defaultToAgentsView", .label = "Open agents view by default",
                 .description = "Открывать панель агентов при запуске.",
                 .group = kAgents, .store = Store::GlobalConfig, .path = {"defaultToAgentsView"},
                 .defaultValue = false});
    v.push_back({.id = "leftArrowOpensAgents", .label = "\xe2\x86\x90 opens agents",
                 .description = "Стрелка ← в пустом поле ввода открывает панель агентов.",
                 .group = kAgents, .store = Store::GlobalConfig, .path = {"leftArrowOpensAgents"},
                 .defaultValue = true});

    // ── Уведомления ──────────────────────────────────────────────────
    v.push_back({.id = "notifChannel", .label = "Local notifications",
                 .description = "Как сообщать о событиях. С terminal_bell, iterm2 или kitty ClaudeUI подсвечивает "
                                "вкладку сессии, которая требует внимания.",
                 .group = kNotify, .path = {"preferredNotifChannel"}, .kind = Kind::Enum, .defaultValue = "auto",
                 .options = {{"auto", "Auto"}, {"terminal_bell", ""}, {"iterm2", ""}, {"iterm2_with_bell", ""},
                             {"kitty", ""}, {"ghostty", ""}, {"notifications_disabled", "disabled"}},
                 .legacyGlobalFallback = true});
    v.push_back({.id = "inputNeededNotifEnabled", .label = "Push when actions required",
                 .description = "Push-уведомление на телефон, когда нужно ваше действие.",
                 .group = kNotify, .path = {"inputNeededNotifEnabled"}, .defaultValue = false,
                 .legacyGlobalFallback = true});
    v.push_back({.id = "agentPushNotifEnabled", .label = "Push when Claude decides",
                 .description = "Push-уведомления, которые Claude отправляет сам — например, по окончании долгой задачи.",
                 .group = kNotify, .path = {"agentPushNotifEnabled"}, .defaultValue = false,
                 .legacyGlobalFallback = true});
    v.push_back({.id = "chrome", .label = "Claude in Chrome enabled by default",
                 .description = "Подключать расширение Claude in Chrome в новых сессиях.",
                 .group = kNotify, .store = Store::GlobalConfig, .path = {"claudeInChromeDefaultEnabled"},
                 .defaultValue = false});

    // ── Обновления ───────────────────────────────────────────────────
    v.push_back({.id = "autoUpdatesChannel", .label = "Auto-update channel",
                 .description = "Канал автообновлений Claude Code.",
                 .group = kUpdates, .path = {"autoUpdatesChannel"}, .kind = Kind::Enum, .defaultValue = "latest",
                 .options = {{"latest", ""}, {"stable", ""}}});
    return v;
}
} // namespace

const std::vector<SettingDef>& AllSettings()
{
    static const std::vector<SettingDef> kAll = Build();
    return kAll;
}

const std::vector<std::string>& SettingGroups()
{
    static const std::vector<std::string> kGroups = {kSession, kModel,  kWorkflows, kInterface, kPerms,
                                                     kCopy,    kAgents, kNotify,    kUpdates};
    return kGroups;
}
} // namespace config
