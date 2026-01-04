// NOTE: we are putting these at the top, so that IMGUI_DEFINE_MATH_OPERATORS can correctly propagate for this translation unit
// NOTE: imgui marks the operators as `static`, so there are no ODR violations here
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>

#include "imcmd_command_palette.h"
#include "imcmd_fuzzy_search.h"

#include <algorithm>
#include <cstddef>
// NOTE: we try to use as much ImGui's helpers as possible, in order to reduce
// work if the end user decide to swap out some standard library functions for
// their own.
#include <cstring>
#include <limits>
#include <utility>

namespace ImCmd
{
// =================================================================
// Private forward decls
// =================================================================

struct StackFrame;
class ExecutionManager;

struct SearchResult;
class SearchManager;

struct CommandOperationRegister;
struct CommandOperationUnregister;
struct CommandOperation;
struct Context;

struct Instance;

// =================================================================
// Private interface
// =================================================================

struct StackFrame
{
    ImCmdInputType InputType = ImCmdInputType_Discrete;

    // For discrete options
    std::vector<std::string> Options;
    int SelectedOption = -1;

    // For text/numeric input
    char InputBuffer[256] = {};
    std::string Hint;
    HelpFunc GetHelpText;
    ValidationFunc ValidateInput;

    // For widget
    std::function<bool()> WidgetCallback;
    std::string WidgetHelpText;
};

class ExecutionManager
{
private:
    Instance* m_Instance;
    Command* m_ExecutingCommand = nullptr;
    std::vector<StackFrame> m_CallStack;

public:
    ExecutionManager(Instance& instance)
        : m_Instance{ &instance } {}

    int GetItemCount() const;
    const char* GetItem(int idx) const;
    const char* GetIcon(int idx) const;
    const char* GetShortcut(int idx) const;
    bool HasSubsequent(int idx) const;
    bool IsChecked(int idx) const;
    void SelectItem(int idx);

    void PushOptions(std::vector<std::string> options);
    void PushPrompt(const PromptConfig& config);
    void SubmitTextInput(const std::string& input);
    void SubmitWidget();
    ImCmdInputType GetCurrentInputType() const;
    const StackFrame* GetCurrentFrame() const;
};

struct SearchResult
{
    int ItemIndex;
    int Score;
    int MatchCount;
    uint8_t Matches[32];
    int MatchedNameIndex = 0; // 0 if matched primary name, >0 for aliases (index into Names vector)
};

class SearchManager
{
private:
    Instance* m_Instance;

public:
    std::vector<SearchResult> SearchResults;
    char SearchText[std::numeric_limits<uint8_t>::max() + 1 /* for null terminator */] = {};

public:
    SearchManager(Instance& instance)
        : m_Instance{ &instance }
    {
    }

    int GetItemCount() const;
    const char* GetItem(int idx) const;
    const char* GetIcon(int idx) const;
    const char* GetShortcut(int idx) const;
    bool HasSubsequent(int idx) const;
    bool IsChecked(int idx) const;

    bool IsActive() const;

    void SetSearchText(const char* text);
    void ClearSearchText();
    void RefreshSearchResults();
};

struct CommandOperationRegister
{
    Command Candidate;
};

struct CommandOperationUnregister
{
    const char* Name;
};

struct CommandOperation
{
    enum OpType
    {
        OpType_Register,
        OpType_Unregister,
    };

    OpType Type;
    int Index;
};

struct Context
{
    ImGuiStorage Instances;
    Instance* CurrentCommandPalette = nullptr;
    std::vector<Command> Commands;
    std::vector<CommandOperationRegister> PendingRegisterOps;
    std::vector<CommandOperationUnregister> PendingUnregisterOps;
    std::vector<CommandOperation> PendingOps;
    ImFont* TextStyleFonts[ImCmdTextType_COUNT] = {};
    ImU32 TextStyleColors[ImCmdTextType_COUNT] = {};
    ImU32 TextStyleFlags[ImCmdTextType_COUNT] = {};
    int CommandStorageLocks = 0;
    bool TextStyleHasColorOverride[ImCmdTextType_COUNT] = {};
    bool IsExecuting = false;
    bool IsTerminating = false;

    struct
    {
        bool ItemSelected = false;
    } LastCommandPaletteStatus;

    struct
    {
        const char* NewSearchText = nullptr;
        bool FocusSearchBox = false;
    } NextCommandPaletteActions;

    void RegisterCommand(Command command)
    {
        auto location = std::lower_bound(
            Commands.begin(),
            Commands.end(),
            command,
            [](const Command& a, const Command& b) -> bool {
                return a.Priority > b.Priority || (a.Priority == b.Priority && ImStricmp(a.Names[0].c_str(), b.Names[0].c_str()) > 0);
            });
        Commands.insert(location, std::move(command));
    }

    bool UnregisterCommand(const char* name)
    {
        struct Comparator
        {
            bool operator()(const Command& command, const char* str) const
            {
                return ImStricmp(command.Names[0].c_str(), str) < 0;
            }

            bool operator()(const char* str, const Command& command) const
            {
                return ImStricmp(str, command.Names[0].c_str()) < 0;
            }
        };

        auto range = std::equal_range(Commands.begin(), Commands.end(), name, Comparator{});
        Commands.erase(range.first, range.second);

        return range.first != range.second;
    }

    bool CommitOps()
    {
        if (IsCommandStorageLocked()) {
            return false;
        }

        for (auto& operation : PendingOps) {
            switch (operation.Type) {
                case CommandOperation::OpType_Register: {
                    auto& op = PendingRegisterOps[operation.Index];
                    RegisterCommand(std::move(op.Candidate));
                } break;

                case CommandOperation::OpType_Unregister: {
                    auto& op = PendingUnregisterOps[operation.Index];
                    UnregisterCommand(op.Name);
                } break;
            }
        }

        bool had_action = !PendingOps.empty();
        PendingRegisterOps.clear();
        PendingUnregisterOps.clear();
        PendingOps.clear();

        return had_action;
    }

    bool IsCommandStorageLocked() const
    {
        return CommandStorageLocks > 0;
    }
};

struct Instance
{
    ExecutionManager Session;
    SearchManager Search;
    float NextFrameScrollTo = -1.f;

    int CurrentSelectedItem = 0;

    struct
    {
        bool RefreshSearch = false;
        bool ClearSearch = false;
        bool FocusInput = false;
    } PendingActions;

    Instance()
        : Session(*this)
        , Search(*this) {}
};

static Context* gContext = nullptr;

// =================================================================
// Private implementation
// =================================================================

int ExecutionManager::GetItemCount() const
{
    if (m_ExecutingCommand) {
        return static_cast<int>(m_CallStack.back().Options.size());
    } else {
        return static_cast<int>(gContext->Commands.size());
    }
}

const char* ExecutionManager::GetItem(int idx) const
{
    if (m_ExecutingCommand) {
        return m_CallStack.back().Options[idx].c_str();
    } else {
        return gContext->Commands[idx].Names[0].c_str();
    }
}

const char* ExecutionManager::GetIcon(int idx) const
{
    if (m_ExecutingCommand) {
        return "";
    } else {
        return gContext->Commands[idx].Icon.c_str();
    }
}

const char* ExecutionManager::GetShortcut(int idx) const
{
    if (m_ExecutingCommand) {
        return "";
    } else {
        return gContext->Commands[idx].Shortcut.c_str();
    }
}

bool ExecutionManager::HasSubsequent(int idx) const
{
    if (m_ExecutingCommand) {
        return false;
    } else {
        return gContext->Commands[idx].SubsequentCallback != nullptr;
    }
}

bool ExecutionManager::IsChecked(int idx) const
{
    return m_ExecutingCommand ? false : (gContext->Commands[idx].IsChecked && *gContext->Commands[idx].IsChecked);
}

template <class TFunc, class... Ts>
static void InvokeSafe(const TFunc& func, Ts&&... args)
{
    if (func) {
        func(std::forward<Ts>(args)...);
    }
}

void ExecutionManager::SelectItem(int idx)
{
    auto cmd = m_ExecutingCommand;
    size_t initial_call_stack_height = m_CallStack.size();

    // Guarding aginst invalid index.
    if (idx >= GetItemCount()) return;
    IM_ASSERT(idx < GetItemCount());

    if (cmd == nullptr) {
        cmd = m_ExecutingCommand = &gContext->Commands[idx];
        ++gContext->CommandStorageLocks;

        gContext->IsExecuting = true;
        InvokeSafe(m_ExecutingCommand->InitialCallback); // Calls ::Prompt()
        gContext->IsExecuting = false;
    } else {
        m_CallStack.back().SelectedOption = idx;

        gContext->IsExecuting = true;
        InvokeSafe(cmd->SubsequentCallback, idx); // Calls ::Prompt()
        gContext->IsExecuting = false;
    }

    size_t final_call_stack_height = m_CallStack.size();
    if (initial_call_stack_height == final_call_stack_height) {

        gContext->IsTerminating = true;
        InvokeSafe(m_ExecutingCommand->TerminatingCallback); // Shouldn't call ::Prompt()
        gContext->IsTerminating = false;

        m_ExecutingCommand = nullptr;
        m_CallStack.clear();
        --gContext->CommandStorageLocks;

        // If the executed command involved subcommands...
        if (final_call_stack_height > 0) {
            m_Instance->PendingActions.ClearSearch = true;
            m_Instance->CurrentSelectedItem = 0;
        }

        gContext->LastCommandPaletteStatus.ItemSelected = true;
    } else {
        // Something new is prompted
        // It doesn't make sense for "current selected item" to persists through completely different set of options
        m_Instance->PendingActions.ClearSearch = true;
        m_Instance->CurrentSelectedItem = 0;
    }
}

void ExecutionManager::PushOptions(std::vector<std::string> options)
{
    m_CallStack.push_back({});
    auto& frame = m_CallStack.back();

    frame.InputType = ImCmdInputType_Discrete;
    frame.Options = std::move(options);

    m_Instance->PendingActions.ClearSearch = true;
}

void ExecutionManager::PushPrompt(const PromptConfig& config)
{
    m_CallStack.push_back({});
    auto& frame = m_CallStack.back();

    frame.InputType = config.Type;

    switch (config.Type)
    {
        case ImCmdInputType_Discrete:
            frame.Options = config.Options;
            break;
        case ImCmdInputType_Text:
        case ImCmdInputType_Int:
        case ImCmdInputType_Float:
            frame.Hint = config.Hint;
            frame.GetHelpText = config.GetHelpText;
            frame.ValidateInput = config.ValidateInput;
            m_Instance->PendingActions.FocusInput = true;
            break;
        case ImCmdInputType_Widget:
            frame.WidgetCallback = config.WidgetCallback;
            frame.WidgetHelpText = config.WidgetHelpText;
            break;
    }

    m_Instance->PendingActions.ClearSearch = true;
}

void ExecutionManager::SubmitTextInput(const std::string& input)
{
    auto cmd = m_ExecutingCommand;
    if (!cmd) return;

    size_t initial_call_stack_height = m_CallStack.size();

    gContext->IsExecuting = true;
    InvokeSafe(cmd->TextInputCallback, input);
    gContext->IsExecuting = false;

    size_t final_call_stack_height = m_CallStack.size();
    if (initial_call_stack_height == final_call_stack_height)
    {
        // Command completed
        gContext->IsTerminating = true;
        InvokeSafe(m_ExecutingCommand->TerminatingCallback);
        gContext->IsTerminating = false;

        m_ExecutingCommand = nullptr;
        m_CallStack.clear();
        --gContext->CommandStorageLocks;

        m_Instance->PendingActions.ClearSearch = true;
        m_Instance->CurrentSelectedItem = 0;

        gContext->LastCommandPaletteStatus.ItemSelected = true;
    } else
    {
        m_Instance->PendingActions.ClearSearch = true;
        m_Instance->CurrentSelectedItem = 0;
    }
}

void ExecutionManager::SubmitWidget()
{
    auto cmd = m_ExecutingCommand;
    if (!cmd) return;

    size_t initial_call_stack_height = m_CallStack.size();

    gContext->IsExecuting = true;
    InvokeSafe(cmd->WidgetCompleteCallback);
    gContext->IsExecuting = false;

    size_t final_call_stack_height = m_CallStack.size();
    if (initial_call_stack_height == final_call_stack_height)
    {
        // Command completed
        gContext->IsTerminating = true;
        InvokeSafe(m_ExecutingCommand->TerminatingCallback);
        gContext->IsTerminating = false;

        m_ExecutingCommand = nullptr;
        m_CallStack.clear();
        --gContext->CommandStorageLocks;

        m_Instance->PendingActions.ClearSearch = true;
        m_Instance->CurrentSelectedItem = 0;

        gContext->LastCommandPaletteStatus.ItemSelected = true;
    } else
    {
        m_Instance->PendingActions.ClearSearch = true;
        m_Instance->CurrentSelectedItem = 0;
    }
}

ImCmdInputType ExecutionManager::GetCurrentInputType() const
{
    if (m_CallStack.empty())
        return ImCmdInputType_Discrete;
    return m_CallStack.back().InputType;
}

const StackFrame* ExecutionManager::GetCurrentFrame() const
{
    if (m_CallStack.empty())
        return nullptr;
    return &m_CallStack.back();
}

int SearchManager::GetItemCount() const
{
    return static_cast<int>(SearchResults.size());
}

const char* SearchManager::GetItem(int idx) const
{
    int actualIdx = SearchResults[idx].ItemIndex;
    return m_Instance->Session.GetItem(actualIdx);
}

const char* SearchManager::GetIcon(int idx) const
{
    int actualIdx = SearchResults[idx].ItemIndex;
    return m_Instance->Session.GetIcon(actualIdx);
}

const char* SearchManager::GetShortcut(int idx) const
{
    int actualIdx = SearchResults[idx].ItemIndex;
    return m_Instance->Session.GetShortcut(actualIdx);
}

bool SearchManager::HasSubsequent(int idx) const
{
    int actualIdx = SearchResults[idx].ItemIndex;
    return m_Instance->Session.HasSubsequent(actualIdx);
}

bool SearchManager::IsChecked(int idx) const
{
    int actualIdx = SearchResults[idx].ItemIndex;
    return m_Instance->Session.IsChecked(actualIdx);
}

bool SearchManager::IsActive() const
{
    return SearchText[0] != '\0';
}

void SearchManager::SetSearchText(const char* text)
{
    // Copy at most IM_ARRAYSIZE(SearchText) chars from `text` to `SearchText`
    ImStrncpy(SearchText, text, IM_ARRAYSIZE(SearchText));
    RefreshSearchResults();
}

void SearchManager::ClearSearchText()
{
    // ImGui doesn't have a ImMemset either, they use std::memset too
    std::memset(SearchText, 0, IM_ARRAYSIZE(SearchText));
    SearchResults.clear();
}

void SearchManager::RefreshSearchResults()
{
    m_Instance->CurrentSelectedItem = 0;
    SearchResults.clear();

    int item_count = m_Instance->Session.GetItemCount();
    for (int i = 0; i < item_count; ++i) {
        const char* text = m_Instance->Session.GetItem(i);
        SearchResult best_result;
        best_result.ItemIndex = i;
        best_result.Score = -1;
        best_result.MatchedNameIndex = 0;

        // Try matching against all names (primary name at index 0, aliases at 1+)
        if (i < (int)gContext->Commands.size()) {
            const auto& names = gContext->Commands[i].Names;
            for (int name_idx = 0; name_idx < (int)names.size(); ++name_idx) {
                SearchResult name_result;
                if (FuzzySearch(SearchText, names[name_idx].c_str(), name_result.Score, name_result.Matches, IM_ARRAYSIZE(name_result.Matches), name_result.MatchCount)) {
                    // Aliases get lower scores
                    if (name_idx > 0)
                        name_result.Score = name_result.Score * 0.75f;

                    if (name_result.Score > best_result.Score) {
                        best_result = name_result;
                        best_result.ItemIndex = i;
                        best_result.MatchedNameIndex = name_idx;
                    }
                }
            }
        } else {
            // For subcommand options, just match against the text directly
            SearchResult name_result;
            if (FuzzySearch(SearchText, text, name_result.Score, name_result.Matches, IM_ARRAYSIZE(name_result.Matches), name_result.MatchCount)) {
                best_result = name_result;
                best_result.ItemIndex = i;
                best_result.MatchedNameIndex = 0;
            }
        }

        if (best_result.Score >= 0) {
            SearchResults.push_back(best_result);
        }
    }

    std::sort(
        SearchResults.begin(),
        SearchResults.end(),
        [](const SearchResult& a, const SearchResult& b) -> bool {
            // We want the biggest element first
            return a.Score > b.Score;
        });
}

// =================================================================
// API implementation
// =================================================================

Context* CreateContext()
{
    auto ctx = new Context();
    if (!gContext) {
        gContext = ctx;
    }
    return ctx;
}

void DestroyContext()
{
    DestroyContext(gContext);
    gContext = nullptr;
}

void DestroyContext(Context* context)
{
    delete context;
}

void SetCurrentContext(Context* context)
{
    gContext = context;
}

Context* GetCurrentContext()
{
    return gContext;
}

void AddCommand(Command command)
{
    IM_ASSERT(gContext != nullptr);

    if (gContext->IsCommandStorageLocked()) {
        gContext->PendingRegisterOps.push_back(CommandOperationRegister{ std::move(command) });
        CommandOperation op;
        op.Type = CommandOperation::OpType_Register;
        op.Index = static_cast<int>(gContext->PendingRegisterOps.size()) - 1;
        gContext->PendingOps.push_back(op);
    } else {
        gContext->RegisterCommand(std::move(command));
    }

    if (auto current = gContext->CurrentCommandPalette) {
        current->PendingActions.RefreshSearch = true;
    }
}

void RemoveCommand(const char* name)
{
    IM_ASSERT(gContext != nullptr);

    if (gContext->IsCommandStorageLocked()) {
        gContext->PendingUnregisterOps.push_back(CommandOperationUnregister{ name });
        CommandOperation op;
        op.Type = CommandOperation::OpType_Unregister;
        op.Index = static_cast<int>(gContext->PendingUnregisterOps.size()) - 1;
        gContext->PendingOps.push_back(op);
    } else {
        gContext->UnregisterCommand(name);
    }

    if (auto current = gContext->CurrentCommandPalette) {
        current->PendingActions.RefreshSearch = true;
    }
}

bool GetStyleFlag(ImCmdTextType type, ImCmdTextFlag flag)
{
    IM_ASSERT(gContext != nullptr);
    return gContext->TextStyleFlags[type] & (1 << flag);
}

void SetStyleFlag(ImCmdTextType type, ImCmdTextFlag flag, bool enabled)
{
    IM_ASSERT(gContext != nullptr);
    if (enabled) {
        gContext->TextStyleFlags[type] |= 1 << flag;
    } else {
        gContext->TextStyleFlags[type] &= ~(1 << flag);
    }
}

ImFont* GetStyleFont(ImCmdTextType type)
{
    IM_ASSERT(gContext != nullptr);
    return gContext->TextStyleFonts[type];
}

void SetStyleFont(ImCmdTextType type, ImFont* font)
{
    IM_ASSERT(gContext != nullptr);
    gContext->TextStyleFonts[type] = font;
}

ImU32 GetStyleColor(ImCmdTextType type)
{
    IM_ASSERT(gContext != nullptr);
    return gContext->TextStyleColors[type];
}

void SetStyleColor(ImCmdTextType type, ImU32 color)
{
    IM_ASSERT(gContext != nullptr);
    gContext->TextStyleColors[type] = color;
    gContext->TextStyleHasColorOverride[type] = true;
}

void ClearStyleColor(ImCmdTextType type)
{
    IM_ASSERT(gContext != nullptr);
    gContext->TextStyleHasColorOverride[type] = false;
}

void SetNextCommandPaletteSearch(const char* text)
{
    IM_ASSERT(gContext != nullptr);
    IM_ASSERT(text != nullptr);
    gContext->NextCommandPaletteActions.NewSearchText = text;
}

void SetNextCommandPaletteSearchBoxFocused()
{
    IM_ASSERT(gContext != nullptr);
    gContext->NextCommandPaletteActions.FocusSearchBox = true;
}

void CommandPalette(const char* name, const char* hint)
{
    IM_ASSERT(gContext != nullptr);

    ImGuiContext& g = *GImGui;
    const auto& style = g.Style;
    auto& gg = *gContext;
    auto& gi = *[&]() {
        auto id = ImHashStr(name);
        if (auto ptr = gg.Instances.GetVoidPtr(id)) {
            return reinterpret_cast<Instance*>(ptr);
        } else {
            auto instance = new Instance();
            gg.Instances.SetVoidPtr(id, instance);
            return instance;
        }
    }();

    // BEGIN this command palette
    gg.CurrentCommandPalette = &gi;
    ImGui::PushID(name);

    gg.LastCommandPaletteStatus = {};

    // BEGIN processing PendingActions
    bool refresh_search = gi.PendingActions.RefreshSearch;
    refresh_search |= gg.CommitOps();

    if (auto text = gg.NextCommandPaletteActions.NewSearchText) {
        refresh_search = false;
        if (text[0] == '\0') {
            gi.Search.ClearSearchText();
        } else {
            gi.Search.SetSearchText(text);
        }
        gg.NextCommandPaletteActions.NewSearchText = nullptr;
    } else if (gi.PendingActions.ClearSearch) {
        refresh_search = false;
        gi.Search.ClearSearchText();
    }

    if (refresh_search) {
        gi.Search.RefreshSearchResults();
    }

    // Save the FocusInput flag before clearing PendingActions
    bool should_focus_input = gi.PendingActions.FocusInput;

    gi.PendingActions = {};
    // END procesisng PendingActions

    // Determine the current input type
    ImCmdInputType input_type = gi.Session.GetCurrentInputType();
    const StackFrame* current_frame = gi.Session.GetCurrentFrame();

    bool should_focus = gg.NextCommandPaletteActions.FocusSearchBox;
    if (should_focus) {
        gg.NextCommandPaletteActions.FocusSearchBox = false;
    }

    float available_width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(available_width);

    // Render appropriate input control based on input type
    if (input_type == ImCmdInputType_Discrete)
    {
        // Original discrete search behavior
        if (should_focus) {
            ImGui::SetKeyboardFocusHere(0);
        }
        if (ImGui::InputTextWithHint("##SearchBox", hint, gi.Search.SearchText, IM_ARRAYSIZE(gi.Search.SearchText))) {
            // Search string updated, update search results
            gi.Search.RefreshSearchResults();
        }
    } else if (input_type == ImCmdInputType_Text || input_type == ImCmdInputType_Int || input_type == ImCmdInputType_Float)
    {
        // Text/numeric input field
        char* input_buffer = current_frame ? const_cast<char*>(current_frame->InputBuffer) : nullptr;
        const char* hint_text = current_frame ? current_frame->Hint.c_str() : "";

        if (input_buffer)
        {
            ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_EnterReturnsTrue;
            if (input_type == ImCmdInputType_Int || input_type == ImCmdInputType_Float)
                input_flags |= ImGuiInputTextFlags_CharsDecimal;

            // Focus the input field if requested
            if (should_focus_input) {
                ImGui::SetKeyboardFocusHere(0);
            }
            bool submitted = ImGui::InputTextWithHint("##InputBox", hint_text, input_buffer, 256, input_flags);

            // Validate input
            std::string validation_error;
            if (current_frame && current_frame->ValidateInput)
            {
                validation_error = current_frame->ValidateInput(input_buffer);
            } else if (input_type == ImCmdInputType_Int && strlen(input_buffer) > 0)
            {
                // Default integer validation
                char* end;
                std::strtol(input_buffer, &end, 10);
                if (*end != '\0')
                    validation_error = "Must be an integer";
            } else if (input_type == ImCmdInputType_Float && strlen(input_buffer) > 0)
            {
                // Default float validation
                char* end;
                std::strtof(input_buffer, &end);
                if (*end != '\0')
                    validation_error = "Must be a number";
            }

            // Show validation error or help text
            if (!validation_error.empty())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", validation_error.c_str());
            } else if (current_frame && current_frame->GetHelpText)
            {
                std::string help_text = current_frame->GetHelpText(input_buffer);
                if (!help_text.empty())
                {
                    ImGui::TextWrapped("%s", help_text.c_str());
                }
            }

            // Submit on Enter key if validation passes
            if (submitted && validation_error.empty() && strlen(input_buffer) > 0)
            {
                gi.Session.SubmitTextInput(input_buffer);
            } else if (submitted)
            {
                // If Enter was pressed but validation failed, restore focus to the input
                gi.PendingActions.FocusInput = true;
            }
        }

        // Skip the list rendering for text/numeric input
        ImGui::PopID();
        return;
    } else if (input_type == ImCmdInputType_Widget)
    {
        // Custom widget rendering
        if (current_frame && current_frame->WidgetCallback)
        {
            // Show help text if available
            if (!current_frame->WidgetHelpText.empty())
            {
                ImGui::TextWrapped("%s", current_frame->WidgetHelpText.c_str());
                ImGui::Spacing();
            }

            // Call the widget callback
            bool widget_complete = current_frame->WidgetCallback();

            if (widget_complete)
            {
                gi.Session.SubmitWidget();
            }
        }

        // Skip the list rendering for widget input
        ImGui::PopID();
        return;
    }

    int item_count = gi.Search.IsActive() ? gi.Search.GetItemCount() : gi.Session.GetItemCount();

    float remaining_height = ImGui::GetMainViewport()->Size.y - ImGui::GetCursorScreenPos().y;
    float search_result_window_height = ImGui::GetTextLineHeightWithSpacing() * item_count + style.FramePadding.y;

    // Use the available width to prevent horizontal overflow
    // The child window should fit within the parent window's content region
    ImGui::BeginChild("SearchResults", ImVec2(available_width, ImMin(search_result_window_height, remaining_height - 100.f)), ImGuiChildFlags_FrameStyle, ImGuiWindowFlags_NoSavedSettings);

    auto font_regular = gg.TextStyleFonts[ImCmdTextType_Regular];
    if (!font_regular) {
        font_regular = ImGui::GetDrawListSharedData()->Font;
    }
    auto font_highlight = gg.TextStyleFonts[ImCmdTextType_Highlight];
    if (!font_highlight) {
        font_highlight = ImGui::GetDrawListSharedData()->Font;
    }

    ImU32 text_color_regular;
    ImU32 text_color_highlight;
    if (gg.TextStyleHasColorOverride[ImCmdTextType_Regular]) {
        text_color_regular = gg.TextStyleColors[ImCmdTextType_Regular];
    } else {
        text_color_regular = ImGui::GetColorU32(ImGuiCol_Text);
    }
    if (gg.TextStyleHasColorOverride[ImCmdTextType_Highlight]) {
        text_color_highlight = gg.TextStyleColors[ImCmdTextType_Highlight];
    } else {
        text_color_highlight = ImGui::GetColorU32(ImGuiCol_Text);
    }

    bool underline_regular = gg.TextStyleFlags[ImCmdTextType_Regular] & (1 << ImCmdTextFlag_Underline);
    bool underline_highlight = gg.TextStyleFlags[ImCmdTextType_Highlight] & (1 << ImCmdTextFlag_Underline);

    auto window = ImGui::GetCurrentContext()->CurrentWindow;
    auto draw_list = window->DrawList;
    auto offsets = &window->DC.MenuColumns;

    // Reset the MenuColumns to ensure we don't accumulate widths from previous frames or other command lists
    // This prevents horizontal scrolling when switching between commands with/without shortcuts
    offsets->Update(style.ItemSpacing.x, true);

    // Check if any item in the full unfiltered list has an icon
    // If so, always reserve space for icons even when filtered results don't have any
    bool has_any_icon = false;
    float reserved_icon_width = 0.0f;
    int full_item_count = gi.Session.GetItemCount();
    for (int i = 0; i < full_item_count; ++i) {
        auto icon = gi.Session.GetIcon(i);
        if (icon && icon[0]) {
            has_any_icon = true;
            // Calculate the actual width of this icon to use as reserved space
            reserved_icon_width = ImGui::CalcTextSize(icon, NULL).x;
            break;
        }
    }

    // Flag used to delay item selection until after the loop ends
    bool select_focused_item = false;
    const ImGuiSelectableFlags selectable_flags = ImGuiSelectableFlags_SelectOnRelease | ImGuiSelectableFlags_NoSetKeyOwner | ImGuiSelectableFlags_SetNavIdOnHover | ImGuiSelectableFlags_SpanAvailWidth;
    for (int i = 0; i < item_count; ++i) {
        // Implement a custom button-like control

        // We are doing this so that it can be highlighted without losing focus on the ImGui::InputText,
        // allowing the user to navigate with up/down arrow keys while typing.

        auto id = window->GetID(i);
        ImGui::PushID(id);

        // Calculate sizes and offsets for the icon, label, shortcut and checkarmk/arrow
        auto icon = gi.Search.IsActive() ? gi.Search.GetIcon(i) : gi.Session.GetIcon(i);
        auto text = gi.Search.IsActive() ? gi.Search.GetItem(i) : gi.Session.GetItem(i);
        auto shortcut = gi.Search.IsActive() ? gi.Search.GetShortcut(i) : gi.Session.GetShortcut(i);

        ImVec2 text_size = ImGui::CalcTextSize(text, NULL, true);
        float icon_w = (icon && icon[0]) ? ImGui::CalcTextSize(icon, NULL).x : 0.0f;
        // If any item in the full list has an icon, reserve space even if current item doesn't
        if (has_any_icon && icon_w == 0.0f) {
            icon_w = reserved_icon_width; // Use the actual width of an existing icon
        }
        float shortcut_w = (shortcut && shortcut[0]) ? ImGui::CalcTextSize(shortcut, NULL).x : 0.0f;
        float checkmark_w = IM_TRUNC(g.FontSize * 1.20f);
        float min_w = offsets->DeclColumns(icon_w, text_size.x, shortcut_w, checkmark_w); // Feedback for next frame
        float stretch_w = ImMax(0.0f, ImGui::GetContentRegionAvail().x - min_w);

        auto pos = window->DC.CursorPos;

        if (ImGui::Selectable("", gi.CurrentSelectedItem == i, selectable_flags, ImVec2(min_w, text_size.y)))
        {
            select_focused_item = true;
            gi.CurrentSelectedItem = i;
        }

        if (gi.CurrentSelectedItem == i && gi.NextFrameScrollTo >= 0.f)
        {
            if (!ImGui::IsItemVisible())
                ImGui::SetScrollHereY(gi.NextFrameScrollTo);
            gi.NextFrameScrollTo = -1.f;
        }

        // Draw the icon, shortcut, and checkmark/arrow
        if (icon_w > 0.0f)
            draw_list->AddText(pos + ImVec2(offsets->OffsetIcon, 0.0f), text_color_regular, icon);
        if (shortcut_w > 0.0f)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
            draw_list->AddText(pos + ImVec2(offsets->OffsetShortcut + stretch_w, 0.0f), ImGui::GetColorU32(ImGuiCol_Text), shortcut);
            ImGui::PopStyleColor();
        }
        if (gi.Search.IsActive() ? gi.Search.HasSubsequent(i) : gi.Session.HasSubsequent(i))
            ImGui::RenderArrow(window->DrawList, pos + ImVec2(offsets->OffsetMark + stretch_w + g.FontSize * 0.30f, 0.0f), ImGui::GetColorU32(ImGuiCol_Text), ImGuiDir_Right);
        else if (gi.Search.IsActive() ? gi.Search.IsChecked(i) : gi.Session.IsChecked(i))
            ImGui::RenderCheckMark(window->DrawList, pos + ImVec2(offsets->OffsetMark + stretch_w + g.FontSize * 0.40f, g.FontSize * 0.134f * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), g.FontSize * 0.866f);

        auto text_pos = pos + ImVec2(offsets->OffsetLabel, 0.0f);

        // Now draw the text/name
        if (gi.Search.IsActive()) {
            // If we have started searching, draw text with highlights at matched chars

            auto& search_result = gi.Search.SearchResults[i];
            bool is_alias_match = search_result.MatchedNameIndex > 0;

            // Helper lambda to draw text with highlighted matches
            auto DrawHighlightedText = [&](const char* display_text) {
                int range_begin;
                int range_end;
                int last_range_end = 0;

                auto DrawRange = [&]() {
#ifdef IMGUI_HAS_TEXTURES
                    auto fsz = ImGui::GetFontSize();
#else
                    auto fsz = font_regular->FontSize;
#endif

                    if (range_begin != last_range_end) {
                        // Draw normal text between last highlighted range end and current highlighted range start
                        auto begin = display_text + last_range_end;
                        auto end = display_text + range_begin;

                        draw_list->AddText(text_pos, text_color_regular, begin, end);
                        auto segment_size = font_regular->CalcTextSizeA(fsz, std::numeric_limits<float>::max(), 0.0f, begin, end);

                        if (underline_regular) {
                            float x1 = text_pos.x;
                            float x2 = text_pos.x + segment_size.x;
                            float y = text_pos.y + segment_size.y;
                            // TODO adjust this to be at text baseline instead
                            draw_list->AddLine(ImVec2(x1, y), ImVec2(x2, y), text_color_regular);
                        }

                        text_pos.x += segment_size.x;
                    }

                    auto begin = display_text + range_begin;
                    auto end = display_text + range_end;

#ifdef IMGUI_HAS_TEXTURES
                    fsz = ImGui::GetFontSize();
#else
                    fsz = font_highlight->FontSize;
#endif

                    draw_list->AddText(font_highlight, fsz, text_pos, text_color_highlight, begin, end);
                    auto segment_size = font_highlight->CalcTextSizeA(fsz, std::numeric_limits<float>::max(), 0.0f, begin, end);

                    if (underline_highlight) {
                        float x1 = text_pos.x;
                        float x2 = text_pos.x + segment_size.x;
                        // TODO adjust this to be at text baseline instead
                        float y = text_pos.y + segment_size.y;
                        draw_list->AddLine(ImVec2(x1, y), ImVec2(x2, y), text_color_highlight);
                    }

                    text_pos.x += segment_size.x;
                };

                IM_ASSERT(search_result.MatchCount >= 1);
                range_begin = search_result.Matches[0];
                range_end = range_begin;

                int last_char_idx = -1;
                for (int j = 0; j < search_result.MatchCount; ++j) {
                    int char_idx = search_result.Matches[j];

                    if ( // These 2 indices are consecutive, extend our current range by 1
                        char_idx == last_char_idx + 1) {
                        ++range_end;
                    } else {
                        DrawRange();
                        last_range_end = range_end;
                        range_begin = char_idx;
                        range_end = char_idx + 1;
                    }

                    last_char_idx = char_idx;
                }
                // Draw the remaining range (if any)

                if (range_begin != range_end) {
                    DrawRange();
                }

                // Draw the text after the last range (if any)
                const char* remaining_text = display_text + range_end;
                draw_list->AddText(text_pos, text_color_regular, remaining_text);
#ifdef IMGUI_HAS_TEXTURES
                auto fsz = ImGui::GetFontSize();
#else
                auto fsz = font_regular->FontSize;
#endif
                auto remaining_size = font_regular->CalcTextSizeA(fsz, std::numeric_limits<float>::max(), 0.0f, remaining_text);
                text_pos.x += remaining_size.x;
            };

            if (is_alias_match) {
                // Matched an alias: show command name (unhighlighted) then alias (highlighted) in parentheses
                draw_list->AddText(text_pos, text_color_regular, text);
#ifdef IMGUI_HAS_TEXTURES
                auto fsz = ImGui::GetFontSize();
#else
                auto fsz = font_regular->FontSize;
#endif
                auto name_size = font_regular->CalcTextSizeA(fsz, std::numeric_limits<float>::max(), 0.0f, text);
                text_pos.x += name_size.x;

                const char* space_paren = " (";
                draw_list->AddText(text_pos, text_color_regular, space_paren);
                auto space_size = font_regular->CalcTextSizeA(fsz, std::numeric_limits<float>::max(), 0.0f, space_paren);
                text_pos.x += space_size.x;

                // Draw the highlighted alias
                int actual_idx = search_result.ItemIndex;
                const char* alias_text = gContext->Commands[actual_idx].Names[search_result.MatchedNameIndex].c_str();
                DrawHighlightedText(alias_text);

                draw_list->AddText(text_pos, text_color_regular, ")");
            } else {
                // Normal case: matched the main name, highlight it
                DrawHighlightedText(text);
            }
        } else {
            // Otherwise, just draw text as-is, there are no highlights

            draw_list->AddText(text_pos, text_color_regular, text);
        }

        ImGui::PopID();
    }

    ImGui::EndChild();

    if (select_focused_item) {
        SelectFocusedItem();
    }

    ImGui::PopID();
}

void SelectFocusedItem()
{
    IM_ASSERT(gContext != nullptr);
    IM_ASSERT(gContext->CurrentCommandPalette != nullptr);
    auto& gi = *gContext->CurrentCommandPalette;
    if (gi.Search.IsActive()) {
        if (!gi.Search.SearchResults.empty())
        {
            auto idx = gi.Search.SearchResults[gi.CurrentSelectedItem].ItemIndex;
            gi.Session.SelectItem(idx);
        } else
        {
            // If the search is active and there are no results, we should focus the search box again
            SetNextCommandPaletteSearchBoxFocused();
        }
    } else {
        gi.Session.SelectItem(gi.CurrentSelectedItem);
    }
}

void FocusPreviousItem()
{
    IM_ASSERT(gContext != nullptr);
    IM_ASSERT(gContext->CurrentCommandPalette != nullptr);
    auto& gi = *gContext->CurrentCommandPalette;
    int item_count = gi.Search.IsActive() ? gi.Search.GetItemCount() : gi.Session.GetItemCount();
    gi.CurrentSelectedItem = ImMax(gi.CurrentSelectedItem - 1, 0);
    gi.NextFrameScrollTo = 1.f;
}

void FocusNextItem()
{
    IM_ASSERT(gContext != nullptr);
    IM_ASSERT(gContext->CurrentCommandPalette != nullptr);
    auto& gi = *gContext->CurrentCommandPalette;
    int item_count = gi.Search.IsActive() ? gi.Search.GetItemCount() : gi.Session.GetItemCount();
    gi.CurrentSelectedItem = ImMin(gi.CurrentSelectedItem + 1, item_count - 1);
    gi.NextFrameScrollTo = 0.f;
}

void Submit()
{
    IM_ASSERT(gContext != nullptr);
    IM_ASSERT(gContext->CurrentCommandPalette != nullptr);
    auto& gi = *gContext->CurrentCommandPalette;

    ImCmdInputType input_type = gi.Session.GetCurrentInputType();
    const StackFrame* current_frame = gi.Session.GetCurrentFrame();

    if (input_type == ImCmdInputType_Text || input_type == ImCmdInputType_Int || input_type == ImCmdInputType_Float)
    {
        // For text/numeric inputs, validate and submit
        const char* input_buffer = current_frame ? current_frame->InputBuffer : "";

        if (strlen(input_buffer) == 0)
        {
            gi.PendingActions.FocusInput = true;
            return;
        }

        // Validate input
        std::string validation_error;
        if (current_frame && current_frame->ValidateInput)
        {
            validation_error = current_frame->ValidateInput(input_buffer);
        } else if (input_type == ImCmdInputType_Int)
        {
            // Default integer validation
            char* end;
            std::strtol(input_buffer, &end, 10);
            if (*end != '\0')
                validation_error = "Must be an integer";
        } else if (input_type == ImCmdInputType_Float)
        {
            // Default float validation
            char* end;
            std::strtof(input_buffer, &end);
            if (*end != '\0')
                validation_error = "Must be a number";
        }

        if (validation_error.empty())
        {
            gi.Session.SubmitTextInput(input_buffer);
        } else
        {
            // If validation failed, restore focus to the input
            gi.PendingActions.FocusInput = true;
        }
    } else if (input_type == ImCmdInputType_Widget)
    {
        // For widget inputs, trigger submission
        gi.Session.SubmitWidget();
    } else
    {
        // For discrete choices, select the focused item
        SelectFocusedItem();
    }
}

void EndCommandPalette()
{
    IM_ASSERT(gContext != nullptr);
    gContext->CurrentCommandPalette = nullptr;
}

bool IsAnyItemSelected()
{
    IM_ASSERT(gContext != nullptr);
    return gContext->LastCommandPaletteStatus.ItemSelected;
}

void RemoveCache(const char* name)
{
    IM_ASSERT(gContext != nullptr);

    auto& instances = gContext->Instances;
    auto id = ImHashStr(name);
    if (auto ptr = instances.GetVoidPtr(id)) {
        auto instance = reinterpret_cast<Instance*>(ptr);
        instances.SetVoidPtr(id, nullptr);
        delete instance;
    }
}

void RemoveAllCaches()
{
    IM_ASSERT(gContext != nullptr);

    auto& instances = gContext->Instances;
    for (auto& entry : instances.Data) {
        auto instance = reinterpret_cast<Instance*>(entry.val_p);
        entry.val_p = nullptr;
        delete instance;
    }
    instances = {};
}

void SetNextWindowAffixedTop(ImGuiCond cond)
{
    auto viewport = ImGui::GetMainViewport()->Size;

    // Center window horizontally, align top vertically
    ImGui::SetNextWindowPos(ImVec2(viewport.x / 2, 0), cond, ImVec2(0.5f, 0.0f));
}

void CommandPaletteWindow(const char* name, bool* p_open)
{
    auto viewport = ImGui::GetMainViewport()->Size;

    SetNextWindowAffixedTop();
    ImGui::SetNextWindowSize(ImVec2(viewport.x * 0.3f, 0.0f));
    ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);

    if (ImGui::IsWindowAppearing()) {
        SetNextCommandPaletteSearchBoxFocused();
    }

    CommandPalette(name);

    if (!IsAnyItemSelected())
    {
        if (ImGui::Shortcut(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat)) {
            FocusPreviousItem();
        } else if (ImGui::Shortcut(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat)) {
            FocusNextItem();
        } else if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            SelectFocusedItem();
        }
    }

    EndCommandPalette();

    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) || IsAnyItemSelected()) {
        // Close popup when user unfocused the command palette window (clicking elsewhere)
        *p_open = false;
    }

    ImGui::End();
}

void Prompt(std::vector<std::string> options)
{
    IM_ASSERT(gContext != nullptr);
    IM_ASSERT(gContext->CurrentCommandPalette != nullptr);
    IM_ASSERT(gContext->IsExecuting);
    IM_ASSERT(!gContext->IsTerminating);

    auto& gi = *gContext->CurrentCommandPalette;
    gi.Session.PushOptions(std::move(options));
}

void Prompt(const PromptConfig& config)
{
    IM_ASSERT(gContext != nullptr);
    IM_ASSERT(gContext->CurrentCommandPalette != nullptr);
    IM_ASSERT(gContext->IsExecuting);
    IM_ASSERT(!gContext->IsTerminating);

    auto& gi = *gContext->CurrentCommandPalette;
    gi.Session.PushPrompt(config);
}

void PromptText(const std::string& hint, HelpFunc help_func, ValidationFunc validate_func)
{
    PromptConfig config;
    config.Type = ImCmdInputType_Text;
    config.Hint = hint;
    config.GetHelpText = help_func;
    config.ValidateInput = validate_func;
    Prompt(config);
}

void PromptInt(const std::string& hint, HelpFunc help_func, ValidationFunc validate_func)
{
    PromptConfig config;
    config.Type = ImCmdInputType_Int;
    config.Hint = hint;
    config.GetHelpText = help_func;
    config.ValidateInput = validate_func;
    Prompt(config);
}

void PromptFloat(const std::string& hint, HelpFunc help_func, ValidationFunc validate_func)
{
    PromptConfig config;
    config.Type = ImCmdInputType_Float;
    config.Hint = hint;
    config.GetHelpText = help_func;
    config.ValidateInput = validate_func;
    Prompt(config);
}

void PromptWidget(std::function<bool()> widget_func, const std::string& help_text)
{
    PromptConfig config;
    config.Type = ImCmdInputType_Widget;
    config.WidgetCallback = widget_func;
    config.WidgetHelpText = help_text;
    Prompt(config);
}

ValidationFunc ValidateIntRange(int min_val, int max_val)
{
    return [min_val, max_val](const char* input) -> std::string {
        if (strlen(input) == 0)
            return ""; // Empty is OK, user still typing

        char* end;
        long val = std::strtol(input, &end, 10);

        if (*end != '\0')
            return "Must be an integer";

        if (val < min_val || val > max_val)
        {
            char buf[256];
            ImFormatString(buf, IM_ARRAYSIZE(buf), "Must be between %d and %d", min_val, max_val);
            return buf;
        }

        return "";
    };
}

ValidationFunc ValidateFloatRange(float min_val, float max_val)
{
    return [min_val, max_val](const char* input) -> std::string {
        if (strlen(input) == 0)
            return "";

        char* end;
        float val = std::strtof(input, &end);

        if (*end != '\0')
            return "Must be a number";

        if (val < min_val || val > max_val)
        {
            char buf[256];
            ImFormatString(buf, IM_ARRAYSIZE(buf), "Must be between %.2f and %.2f", min_val, max_val);
            return buf;
        }

        return "";
    };
}

ValidationFunc ValidateNotEmpty()
{
    return [](const char* input) -> std::string {
        return (strlen(input) == 0) ? "Input required" : "";
    };
}

ValidationFunc CombineValidators(std::vector<ValidationFunc> validators)
{
    return [validators = std::move(validators)](const char* input) -> std::string {
        for (const auto& validator : validators)
        {
            if (validator)
            {
                std::string error = validator(input);
                if (!error.empty())
                    return error;
            }
        }
        return "";
    };
}
} // namespace ImCmd
