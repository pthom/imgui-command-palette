#pragma once

#include <imgui.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// TODO support std::string_view
// TODO support function pointer callback in addition to std::function

enum ImCmdTextType
{
    ImCmdTextType_Regular,
    ImCmdTextType_Highlight,
    ImCmdTextType_COUNT,
};

enum ImCmdInputType
{
    ImCmdInputType_Discrete, // List of discrete options (current behavior)
    ImCmdInputType_Text, // Free-form text input
    ImCmdInputType_Int, // Integer input (validated)
    ImCmdInputType_Float, // Float input (validated)
    ImCmdInputType_Widget, // Custom ImGui widget
};

enum ImCmdTextFlag
{
    /// Whether the text is underlined. Default false.
    ImCmdTextFlag_Underline,
    ImCmdTextFlag_COUNT,
};

namespace ImCmd
{
// Type aliases for callback functions
using ValidationFunc = std::function<std::string(const char*)>;
using HelpFunc = std::function<std::string(const char*)>;

struct Command
{
    std::string Name;
    std::function<void()> InitialCallback;
    std::function<void(int selected_option)> SubsequentCallback;
    std::function<void(const std::string& input_text)> TextInputCallback;
    std::function<void()> WidgetCompleteCallback;
    std::function<void()> TerminatingCallback;
    std::string Icon = "";
    std::string Shortcut = "";
    bool* IsChecked = nullptr;
    int Priority = 0; //< Higher priority commands are listed first. Commands with the same Priority are sorted alphabetically by Name.
};

struct PromptConfig
{
    ImCmdInputType Type = ImCmdInputType_Discrete;

    // For ImCmdInputType_Discrete
    std::vector<std::string> Options;

    // For text/numeric input (Text, Int, Float)
    std::string Hint;
    HelpFunc GetHelpText;
    ValidationFunc ValidateInput;

    // For ImCmdInputType_Widget
    std::function<bool()> WidgetCallback;
    std::string WidgetHelpText;
};

// Initialization
struct Context;

/// Create a new context object. If there is currently no context bound, it will also be bound as the current context.
Context* CreateContext();
/// Destroys the currently bound context.
void DestroyContext();
void DestroyContext(Context* context);

void SetCurrentContext(Context* context);
Context* GetCurrentContext();

// Command management
void AddCommand(Command command);
void RemoveCommand(const char* name);

// Styling
bool GetStyleFlag(ImCmdTextType type, ImCmdTextFlag flag);
void SetStyleFlag(ImCmdTextType type, ImCmdTextFlag flag, bool enabled);
ImFont* GetStyleFont(ImCmdTextType type);
void SetStyleFont(ImCmdTextType type, ImFont* font);
ImU32 GetStyleColor(ImCmdTextType type);
void SetStyleColor(ImCmdTextType type, ImU32 color);
void ClearStyleColor(ImCmdTextType type); //< Clear the style color for the given type, defaulting to ImGuiCol_Text

// Command palette widget
void SetNextCommandPaletteSearch(const char* text);
void SetNextCommandPaletteSearchBoxFocused();
// Must call EndCommandPalette() if CommandPalette() was called
void CommandPalette(const char* name, const char* hint = nullptr);
// The following 4 functions can only be called between CommandPalette() and EndCommandPalette()
void SelectFocusedItem();
void FocusPreviousItem();
void FocusNextItem();
void Submit(); //< Submits the current input (text/widget) or selects the focused item (discrete choice)
// Must call EndCommandPalette() if CommandPalette() was called
void EndCommandPalette();
bool IsAnyItemSelected();

void RemoveCache(const char* name);
void RemoveAllCaches();

// Command palette widget in a window helper
void SetNextWindowAffixedTop(ImGuiCond cond = 0);
void CommandPaletteWindow(const char* name, bool* p_open);

// Validation helper functions
ValidationFunc ValidateIntRange(int min_val, int max_val);
ValidationFunc ValidateFloatRange(float min_val, float max_val);
ValidationFunc ValidateNotEmpty();
ValidationFunc CombineValidators(std::vector<ValidationFunc> validators);

// Command responses, only call these in command callbacks (except TerminatingCallback)
void Prompt(const PromptConfig& config);
void Prompt(std::vector<std::string> options);
void PromptText(const std::string& hint = "", HelpFunc help_func = nullptr, ValidationFunc validate_func = nullptr);
void PromptInt(const std::string& hint = "", HelpFunc help_func = nullptr, ValidationFunc validate_func = nullptr);
void PromptFloat(const std::string& hint = "", HelpFunc help_func = nullptr, ValidationFunc validate_func = nullptr);
void PromptWidget(std::function<bool()> widget_func, const std::string& help_text = "");

} // namespace ImCmd
