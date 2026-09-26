/*
** esettings.c - the Settings editor (Ctrl+,), VS Code's
**
** A tab with a search box on top, the table of contents on the left and
** every setting on the right: "Editor: Tab Size" with its description and
** its control - a checkbox, a list to pick from, or a box to type in. A
** change is written into settings.json at once (its comments kept) and
** applied; a setting not at its default has the blue bar on its left, and
** its gear offers Reset Setting. The {} icon opens settings.json itself.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** The settings there are
** ===================================================================
*/

enum { ST_BOOL, ST_NUM, ST_STR, ST_ENUM, ST_OBJ };

/* the table of contents; depth 1 is a part of the one above */
enum { C_COMMON, C_EDITOR, C_FORMAT, C_MINIMAP, C_SUGGEST, C_FILES, C_WORKBENCH, C_LOOK,
       C_FEATURES, C_EXPLORER, C_SEARCH, C_TERMINAL, C_SCM, C_TESTING, C_DEBUG, C_EXT, C_EMMET, C_HTML, C_VIM, TOC_N };

static const struct {
  const char *name;
  int depth;
} toc[TOC_N] = {
  {"Commonly Used", 0}, {"Text Editor", 0}, {"Formatting", 1}, {"Minimap", 1},
  {"Suggestions", 1}, {"Files", 1}, {"Workbench", 0}, {"Appearance", 1},
  {"Features", 0}, {"Explorer", 1}, {"Search", 1}, {"Terminal", 1}, {"Source Control", 1}, {"Testing", 1}, {"Debug", 1}, {"Extensions", 0}, {"Emmet", 1}, {"HTML", 1}, {"Vim", 1}
};

typedef struct Setting {
  const char *key;
  int toc, type, common;
  const char *def;	/* the default, as JSON */
  const char *values;	/* ST_ENUM: "off|on"; NULL for the themes */
  const char *desc;	/* VS Code's words; #key# is another setting */
} Setting;

static const Setting set[] = {
  {"editor.fontSize", C_EDITOR, ST_NUM, 1, "14", NULL,
   "Controls the font size in pixels of the code editor and the diff viewer (mme-sdl); the rest of the window has #mme.ui.fontSize#. In a terminal the terminal's font is used."},
  {"editor.fontFamily", C_EDITOR, ST_STR, 1, "\"\"", NULL,
   "Controls the font family of the code editor and the diff viewer (mme-sdl; the first of a comma list). Empty: JetBrains Mono. Any installed font works, or copy its .ttf/.otf files into mme-fonts. In a terminal the terminal's font is used."},
  {"editor.tabSize", C_EDITOR, ST_NUM, 1, "4", NULL,
   "The number of spaces a tab is equal to. This setting is overridden based on the file contents when #editor.detectIndentation# is on."},
  {"editor.insertSpaces", C_EDITOR, ST_BOOL, 1, "true", NULL,
   "Insert spaces when pressing Tab. This setting is overridden based on the file contents when #editor.detectIndentation# is on."},
  {"editor.detectIndentation", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether #editor.tabSize# and #editor.insertSpaces# will be automatically detected when a file is opened based on the file contents."},
  {"editor.lineNumbers", C_EDITOR, ST_ENUM, 0, "\"on\"", "on|off|relative|interval",
   "Controls the display of line numbers."},
  {"editor.renderWhitespace", C_EDITOR, ST_ENUM, 1, "\"selection\"", "none|boundary|selection|trailing|all",
   "Controls how the editor should render whitespace characters."},
  {"editor.wordWrap", C_EDITOR, ST_ENUM, 1, "\"off\"", "off|on",
   "Controls how lines should wrap."},
  {"editor.autoClosingBrackets", C_EDITOR, ST_ENUM, 0, "\"always\"", "always|languageDefined|beforeWhitespace|never",
   "Controls whether the editor should automatically close brackets after the user adds an opening bracket."},
  {"editor.bracketPairColorization.enabled", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether bracket pair colorization is enabled or not."},
  {"editor.formatOnSave", C_FORMAT, ST_BOOL, 1, "false", NULL,
   "Format a file on save. A formatter must be available and the editor must not be shutting down."},
  {"editor.formatOnPaste", C_FORMAT, ST_BOOL, 1, "false", NULL,
   "Controls whether the editor should automatically format the pasted content. A formatter must be available and the formatter should be able to format a range in a document."},
  {"editor.formatOnType", C_FORMAT, ST_BOOL, 1, "false", NULL,
   "Controls whether the editor should automatically format the line after typing."},
  {"editor.codeActionsOnSave", C_FORMAT, ST_OBJ, 0, "{}", NULL,
   "Run Code Actions for the editor on save. {\"source.organizeImports\": \"explicit\"} organizes the imports."},
  {"editor.linkedEditing", C_EDITOR, ST_BOOL, 0, "false", NULL,
   "Controls whether the editor has linked editing enabled: an HTML tag's closing tag changes with it."},
  {"editor.minimap.enabled", C_MINIMAP, ST_BOOL, 0, "true", NULL,
   "Controls whether the minimap is shown."},
  {"editor.minimap.side", C_MINIMAP, ST_ENUM, 0, "\"right\"", "left|right",
   "Controls the side where to render the minimap."},
  {"editor.minimap.showSlider", C_MINIMAP, ST_ENUM, 0, "\"mouseover\"", "always|mouseover",
   "Controls when the minimap slider is shown."},
  {"editor.minimap.renderCharacters", C_MINIMAP, ST_BOOL, 0, "true", NULL,
   "Render the actual characters on a line as opposed to color blocks."},
  {"editor.minimap.maxColumn", C_MINIMAP, ST_NUM, 0, "120", NULL,
   "Limit the width of the minimap to render at most a certain number of columns."},
  {"editor.minimap.scale", C_MINIMAP, ST_NUM, 0, "1", NULL,
   "Scale of content drawn in the minimap: 1, 2 or 3."},
  {"diffEditor.ignoreTrimWhitespace", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "When enabled, the diff editor ignores changes in leading or trailing whitespace."},
  {"diffEditor.hideUnchangedRegions.enabled", C_EDITOR, ST_BOOL, 0, "false", NULL,
   "Controls whether the diff editor shows unchanged regions."},
  {"diffEditor.renderSideBySide", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether the diff editor shows the diff side by side or inline."},
  {"editor.wordBasedSuggestions", C_SUGGEST, ST_BOOL, 0, "true", NULL,
   "Controls whether completions should be computed based on words in the document."},
  {"editor.snippetSuggestions", C_SUGGEST, ST_ENUM, 0, "\"inline\"", "top|bottom|inline|none",
   "Controls whether snippets are shown with other suggestions and how they are sorted."},
  {"files.trimTrailingWhitespace", C_FILES, ST_BOOL, 0, "false", NULL,
   "When enabled, will trim trailing whitespace when saving a file."},
  {"files.insertFinalNewline", C_FILES, ST_BOOL, 0, "false", NULL,
   "When enabled, insert a final new line at the end of the file when saving it."},
  {"workbench.editor.enablePreview", C_WORKBENCH, ST_BOOL, 1, "false", NULL,
   "Controls whether opened editors show as preview editors, which are reused until they are kept open (for example by double-clicking or editing). mme opens every file in its own tab by default."},
  {"workbench.startupEditor", C_WORKBENCH, ST_ENUM, 0, "\"welcomePage\"", "none|welcomePage",
   "Controls which editor is shown at startup, if none are restored from the previous session."},
  {"workbench.colorTheme", C_LOOK, ST_ENUM, 1, "\"Dark Modern\"", NULL,
   "Specifies the color theme used in the workbench."},
  {"explorer.confirmDragAndDrop", C_EXPLORER, ST_BOOL, 0, "true", NULL,
   "Controls whether the Explorer should ask for confirmation to move files and folders via drag and drop."},
  {"explorer.confirmDelete", C_EXPLORER, ST_BOOL, 0, "true", NULL,
   "Controls whether the Explorer should ask for confirmation when deleting a file via the trash."},
  {"explorer.decorations.colors", C_EXPLORER, ST_BOOL, 0, "true", NULL,
   "Controls whether file decorations should use colors."},
  {"explorer.decorations.badges", C_EXPLORER, ST_BOOL, 0, "true", NULL,
   "Controls whether file decorations should use badges."},
  {"explorer.compactFolders", C_EXPLORER, ST_BOOL, 1, "true", NULL,
   "Controls whether the Explorer should render folders in a compact form. In such a form, single child folders will be compressed in a combined tree element."},
  {"explorer.fileNesting.enabled", C_EXPLORER, ST_BOOL, 0, "false", NULL,
   "Controls whether file nesting is enabled in the Explorer. File nesting allows for related files in a directory to be visually grouped together under a single parent file."},
  {"explorer.fileNesting.expand", C_EXPLORER, ST_BOOL, 0, "true", NULL,
   "Controls whether file nests are automatically expanded."},
  {"explorer.sortOrder", C_EXPLORER, ST_ENUM, 0, "\"default\"", "default|mixed|filesFirst|type|modified",
   "Controls the property-based sorting of files and folders in the Explorer."},
  {"explorer.autoReveal", C_EXPLORER, ST_BOOL, 0, "true", NULL,
   "Controls whether the Explorer should automatically reveal and select files when opening them."},
  {"files.exclude", C_EXPLORER, ST_OBJ, 1, "{\"**/.git\": true, \"**/.svn\": true, \"**/.hg\": true, \"**/CVS\": true, \"**/.DS_Store\": true, \"**/Thumbs.db\": true}", NULL,
   "Configure glob patterns for excluding files and folders. For example, the file Explorer decides which files and folders to show or hide based on this setting."},
  {"search.searchOnType", C_SEARCH, ST_BOOL, 0, "true", NULL,
   "Search all files as you type."},
  {"search.searchEditor.defaultNumberOfContextLines", C_SEARCH, ST_NUM, 0, "1", NULL,
   "The default number of surrounding context lines to use when creating new Search Editors."},
  {"search.searchEditor.reusePriorSearchConfiguration", C_SEARCH, ST_BOOL, 0, "false", NULL,
   "When enabled, new Search Editors will reuse the includes, excludes, and flags of the previously opened Search Editor."},
  {"workbench.sideBar.visible", C_LOOK, ST_BOOL, 0, "true", NULL,
   "Controls the visibility of the primary side bar at startup."},
  {"terminal.integrated.shell", C_TERMINAL, ST_STR, 0, "\"\"", NULL,
   "The path of the shell the terminal uses. Empty: mmc-shell when it is in the PATH, else cmd or $SHELL."},
  {"terminal.integrated.shellIntegration.enabled", C_TERMINAL, ST_BOOL, 0, "true", NULL,
   "Determines whether or not shell integration is auto-injected to support features like enhanced command tracking and current working directory detection."},
  {"terminal.integrated.shellIntegration.decorationsEnabled", C_TERMINAL, ST_ENUM, 0, "\"both\"", "both|gutter|never",
   "When shell integration is enabled, adds a decoration for each command."},
  {"terminal.integrated.stickyScroll.enabled", C_TERMINAL, ST_BOOL, 0, "true", NULL,
   "Shows the current command at the top of the terminal."},
  {"terminal.integrated.copyOnSelection", C_TERMINAL, ST_BOOL, 0, "false", NULL,
   "Controls whether text selected in the terminal will be copied to the clipboard."},
  {"terminal.integrated.rightClickBehavior", C_TERMINAL, ST_ENUM, 0, "\"copyPaste\"", "default|copyPaste|paste|selectWord|nothing",
   "Controls how terminal reacts to right click."},
  {"terminal.integrated.enableMultiLinePasteWarning", C_TERMINAL, ST_ENUM, 0, "\"auto\"", "auto|always|never",
   "Controls whether to show a warning dialog when pasting multiple lines into the terminal."},
  {"terminal.integrated.cursorStyle", C_TERMINAL, ST_ENUM, 0, "\"block\"", "block|line|underline",
   "Controls the style of terminal cursor when the terminal is focused."},
  {"terminal.integrated.cursorBlinking", C_TERMINAL, ST_BOOL, 0, "false", NULL,
   "Controls whether the terminal cursor blinks."},
  {"terminal.integrated.scrollback", C_TERMINAL, ST_NUM, 0, "1000", NULL,
   "Controls the maximum number of lines the terminal keeps in its buffer."},
  {"terminal.integrated.cwd", C_TERMINAL, ST_STR, 0, "\"\"", NULL,
   "An explicit start path where the terminal will be launched."},
  {"terminal.integrated.confirmOnKill", C_TERMINAL, ST_ENUM, 0, "\"editor\"", "never|editor|panel|always",
   "Controls whether to confirm killing terminals when they have child processes."},
  {"terminal.integrated.confirmOnExit", C_TERMINAL, ST_ENUM, 0, "\"never\"", "never|always|hasChildProcesses",
   "Controls whether to confirm when the window closes if there are active terminal sessions."},
  {"editor.autoIndent", C_EDITOR, ST_ENUM, 0, "\"full\"", "none|keep|brackets|advanced|full",
   "Controls whether the editor should automatically adjust the indentation when users type, paste, move or indent lines."},
  {"editor.autoIndentOnPaste", C_EDITOR, ST_BOOL, 0, "false", NULL,
   "Controls whether the editor should automatically auto-indent the pasted content."},
  {"editor.multiCursorModifier", C_EDITOR, ST_ENUM, 0, "\"alt\"", "ctrlCmd|alt",
   "The modifier to be used to add multiple cursors with the mouse. The Go to Definition and Open Link mouse gestures will adapt such that they do not conflict with the multicursor modifier."},
  {"editor.multiCursorPaste", C_EDITOR, ST_ENUM, 0, "\"spread\"", "spread|full",
   "Controls pasting when the line count of the pasted text matches the cursor count: each cursor pastes a single line (spread), or each cursor pastes the full text."},
  {"editor.emptySelectionClipboard", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether copying without a selection copies the current line."},
  {"editor.renderControlCharacters", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether the editor should render control characters."},
  {"editor.unicodeHighlight.ambiguousCharacters", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether characters are highlighted that can be confused with basic ASCII characters, except those that are common in the current user locale."},
  {"editor.unicodeHighlight.invisibleCharacters", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether characters that just reserve space or have no width at all are highlighted."},
  {"editor.renderLineHighlight", C_EDITOR, ST_ENUM, 0, "\"line\"", "none|gutter|line|all",
   "Controls how the editor should render the current line highlight."},
  {"editor.cursorSurroundingLines", C_EDITOR, ST_NUM, 0, "0", NULL,
   "Controls the minimal number of visible leading lines (minimum 0) and trailing lines (minimum 1) surrounding the cursor."},
  {"editor.mouseWheelScrollSensitivity", C_EDITOR, ST_NUM, 0, "1", NULL,
   "A multiplier to be used on the deltaY of mouse wheel scroll events. 1 scrolls three lines a notch."},
  {"editor.scrollBeyondLastLine", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether the editor will scroll beyond the last line."},
  {"editor.wordSeparators", C_EDITOR, ST_STR, 0, "\"`~!@#$%^&*()-=+[{]}\\\\|;:'\\\",.<>/?\"", NULL,
   "Characters that will be used as word separators when doing word related navigations or operations."},
  {"editor.tabCompletion", C_SUGGEST, ST_ENUM, 0, "\"off\"", "on|off|onlySnippets",
   "Enables tab completions: a snippet's prefix, then Tab, inserts the snippet."},
  {"editor.acceptSuggestionOnEnter", C_SUGGEST, ST_ENUM, 0, "\"on\"", "on|smart|off",
   "Controls whether suggestions should be accepted on Enter, in addition to Tab. 'smart': only accept a suggestion with Enter when it makes a textual change."},
  {"editor.suggestSelection", C_SUGGEST, ST_ENUM, 0, "\"first\"", "first|recentlyUsed|recentlyUsedByPrefix",
   "Controls how suggestions are pre-selected when showing the suggest list."},
  {"editor.quickSuggestions", C_SUGGEST, ST_OBJ, 0, "{\"other\": \"on\", \"comments\": \"off\", \"strings\": \"off\"}", NULL,
   "Controls whether suggestions should automatically show up while typing: in other code, in comments, in strings."},
  {"editor.suggestOnTriggerCharacters", C_SUGGEST, ST_BOOL, 0, "true", NULL,
   "Controls whether suggestions should automatically show up when typing trigger characters."},
  {"editor.stickyScroll.enabled", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Shows the nested current scopes during the scroll at the top of the editor."},
  {"editor.stickyScroll.maxLineCount", C_EDITOR, ST_NUM, 0, "5", NULL,
   "Defines the maximum number of sticky lines to show."},
  {"editor.dragAndDrop", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether the editor should allow moving selections via drag and drop."},
  {"emmet.triggerExpansionOnTab", C_EMMET, ST_BOOL, 0, "true", NULL,
   "When enabled, Emmet abbreviations are expanded when pressing TAB, even when completions do not show up."},
  {"emmet.showExpandedAbbreviation", C_EMMET, ST_ENUM, 0, "\"always\"", "never|always|inMarkupAndStylesheetFilesOnly",
   "Shows expanded Emmet abbreviations as suggestions."},
  {"html.autoClosingTags", C_HTML, ST_BOOL, 0, "true", NULL,
   "Enable/disable autoclosing of HTML tags."},
  {"editor.hover.delay", C_EDITOR, ST_NUM, 0, "300", NULL,
   "Controls the delay in milliseconds after which the hover is shown."},
  {"editor.hover.sticky", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether the hover should remain visible when mouse is moved over it."},
  {"editor.occurrencesHighlight", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether the editor should highlight semantic symbol occurrences."},
  {"editor.guides.indentation", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether the editor should render indent guides."},
  {"editor.guides.highlightActiveIndentation", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether the editor should highlight the active indent guide."},
  {"editor.rulers", C_EDITOR, ST_OBJ, 0, "[]", NULL,
   "Render vertical rulers after a certain number of monospace characters. Use multiple values for multiple rulers. No rulers are drawn if array is empty."},
  {"editor.inlayHints.enabled", C_EDITOR, ST_ENUM, 0, "\"on\"", "on|off",
   "Enables the inlay hints in the editor."},
  {"editor.codeLens", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether the editor shows CodeLens."},
  {"editor.colorDecorators", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether the editor should render the inline color decorators and color picker."},
  {"editor.links", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether the editor should detect links and make them clickable."},
  {"editor.inlineSuggest.enabled", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether to automatically show inline suggestions in the editor."},
  {"editor.inlineSuggest.showToolbar", C_EDITOR, ST_ENUM, 0, "\"onHover\"", "onHover|always|never",
   "Controls when to show the inline suggestion toolbar."},
  {"github.copilot.nextEditSuggestions.enabled", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether to show next edit suggestions: the edit your change calls for elsewhere in the file, pointed at in the gutter."},
  {"editor.textmateGrammars", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Color the code with the TextMate grammars of VS Code and its extensions, exactly as VS Code does; off: mme's own simpler highlighter."},
  {"editor.maxTokenizationLineLength", C_EDITOR, ST_NUM, 0, "20000", NULL,
   "Lines above this length will not be tokenized for performance reasons."},
  {"editor.semanticHighlighting.enabled", C_EDITOR, ST_BOOL, 0, "true", NULL,
   "Controls whether the semanticHighlighting is shown for the languages that support it."},
  {"editor.lightbulb.enabled", C_EDITOR, ST_ENUM, 0, "\"onCode\"", "onCode|off",
   "Enables the Code Action lightbulb in the editor."},
  {"git.blame.editorDecoration.enabled", C_SCM, ST_BOOL, 0, "true", NULL,
   "Controls whether to show blame information in the editor using editor decorations."},
  {"git.blame.statusBarItem.enabled", C_SCM, ST_BOOL, 0, "true", NULL,
   "Controls whether to show blame information in the status bar."},
  {"git.enableSmartCommit", C_SCM, ST_BOOL, 0, "false", NULL,
   "Commit all changes when there are no staged changes."},
  {"git.suggestSmartCommit", C_SCM, ST_BOOL, 0, "true", NULL,
   "Suggests to enable smart commit (commit all changes when there are no staged changes)."},
  {"git.mergeEditor", C_SCM, ST_BOOL, 0, "true", NULL,
   "Open merge conflicts in the merge editor: Incoming and Current above, the Result below."},
  {"scm.diffDecorations", C_SCM, ST_ENUM, 0, "\"all\"", "all|gutter|none",
   "Controls diff decorations in the editor."},
  {"mme.extensions.useVSCodeExtensions", C_EXT, ST_BOOL, 0, "true", NULL,
   "Use the color themes, snippets and languages of the extensions VS Code has installed (~/.vscode/extensions); they are only read, never changed."},
  {"files.autoSave", C_FILES, ST_ENUM, 1, "\"off\"", "off|afterDelay|onFocusChange|onWindowChange",
   "Controls auto save of editors that have unsaved changes. afterDelay: saved after #files.autoSaveDelay#; onFocusChange: saved when the editor loses focus; onWindowChange: saved when the window loses focus."},
  {"files.autoSaveDelay", C_FILES, ST_NUM, 0, "1000", NULL,
   "Controls the delay in milliseconds after which an editor with unsaved changes is saved automatically. Only applies when #files.autoSave# is set to afterDelay."},
  {"files.hotExit", C_FILES, ST_ENUM, 0, "\"onExit\"", "off|onExit|onExitAndWindowClose",
   "Controls whether unsaved files are remembered between sessions, allowing the save prompt when exiting the editor to be skipped."},
  {"files.encoding", C_FILES, ST_ENUM, 0, "\"utf8\"", "utf8|utf8bom|utf16le|utf16be|windows1252|iso88591",
   "The default character set encoding to use when reading and writing files (a file's BOM wins)."},
  {"window.restoreWindows", C_WORKBENCH, ST_ENUM, 0, "\"all\"", "preserve|all|folders|one|none",
   "Controls how the editors of a folder are reopened after starting again: none starts empty, the others bring back the open files."},
  {"editor.cursorStyle", C_EDITOR, ST_ENUM, 0, "\"line\"", "line|block|underline|line-thin|block-outline|underline-thin",
   "Controls the cursor style."},
  {"editor.cursorBlinking", C_EDITOR, ST_ENUM, 0, "\"blink\"", "blink|smooth|phase|expand|solid",
   "Control the cursor animation style (a terminal can blink or not)."},
  {"debug.inlineValues", C_DEBUG, ST_ENUM, 0, "\"auto\"", "on|off|auto",
   "Show variable values inline in editor while debugging."},
  {"testing.gutterEnabled", C_TESTING, ST_BOOL, 0, "true", NULL,
   "Controls whether test decorations are shown in the editor gutter: run a test from its line, see how it went."},
  {"workbench.sideBar.location", C_LOOK, ST_ENUM, 0, "\"left\"", "left|right",
   "Controls the location of the primary side bar and activity bar. They can either show on the left or right of the workbench."},
  {"window.menuBarVisibility", C_LOOK, ST_ENUM, 0, "\"classic\"", "classic|visible|toggle|hidden|compact",
   "Control the visibility of the menu bar. A setting of 'toggle' or 'hidden' hides it (Alt+F, F10 still open the menus)."},
  {"mme.ui.fontSize", C_LOOK, ST_NUM, 0, "14", NULL,
   "Font size in pixels of the workbench (mme-sdl): the menus, the side bar, the tabs, the panel, the status bar. The code editor has #editor.fontSize#. Ctrl+= and Ctrl+- zoom both."},
  {"mme.extensions.run", C_EXT, ST_OBJ, 0, "[]", NULL,
   "The VS Code extensions whose code runs in mme's extension host (node), by id: [\"redhat.vscode-yaml\", \"golang.go\"]. Those installed with Install from VSIX run by themselves. An extension's themes, snippets and grammars work either way; webviews do not. Microsoft's own extensions (Pylance, C/C++, Python) are licensed for Microsoft's VS Code only."},
  {"mme.extensions.nodePath", C_EXT, ST_STR, 0, "\"\"", NULL,
   "The node program that runs the extensions' code. Empty: node from the PATH."},
  {"window.titleBarStyle", C_LOOK, ST_ENUM, 0, "\"custom\"", "custom|native",
   "Adjust the appearance of the window title bar (mme-sdl): 'custom' draws it with the menu bar, its own minimize, maximize and close buttons, no system frame; 'native' uses the system's frame. In a terminal the terminal has the frame."},
  {"workbench.statusBar.visible", C_LOOK, ST_BOOL, 0, "true", NULL,
   "Controls the visibility of the status bar at the bottom of the workbench."},
  {"workbench.activityBar.location", C_LOOK, ST_ENUM, 0, "\"default\"", "default|top|bottom|hidden",
   "Controls the location of the Activity Bar. It can either show to the side of the Primary Side Bar or be hidden."},
  {"window.commandCenter", C_LOOK, ST_BOOL, 0, "true", NULL,
   "Show command launcher together with the window title."},
  {"window.title", C_LOOK, ST_STR, 0, "\"\"", NULL,
   "Controls the window title based on the current context such as the opened workspace or active editor. Variables: ${activeEditorShort}, ${activeEditorMedium}, ${activeEditorLong}, ${activeFolderShort}, ${activeFolderMedium}, ${activeFolderLong}, ${folderName}, ${folderPath}, ${rootName}, ${rootPath}, ${appName}, ${dirty}, ${separator}. Empty: ${dirty}${activeEditorShort}${separator}${rootName}${separator}${appName}."},
  {"workbench.panel.defaultLocation", C_LOOK, ST_ENUM, 0, "\"bottom\"", "left|bottom|right",
   "Controls the default location of the panel (Terminal, Debug Console, Output, Problems)."},
  {"workbench.panel.alignment", C_LOOK, ST_ENUM, 0, "\"center\"", "left|right|center|justify",
   "Controls the alignment of the panel (Terminal, Debug Console, Output, Problems) when it is at the bottom: center under the editors, or justify across the whole width."},
  {"markdown.preview.scrollPreviewWithEditor", C_FEATURES, ST_BOOL, 0, "true", NULL,
   "When the Markdown preview is open beside its file, scrolling the file scrolls the preview to the same place."},
  {"markdown.preview.scrollEditorWithPreview", C_FEATURES, ST_BOOL, 0, "true", NULL,
   "And the other way round: scrolling the Markdown preview scrolls the file it was made from."},
  {"mme.chat.apiKey", C_EXT, ST_STR, 0, "\"\"", NULL,
   "The Anthropic API key Chat and Inline Chat use to ask Claude. When empty, the ANTHROPIC_API_KEY environment variable is used (or ANTHROPIC_AUTH_TOKEN, sent as a bearer token)."},
  {"mme.chat.baseUrl", C_EXT, ST_STR, 0, "\"\"", NULL,
   "The base URL of the Anthropic API, for a proxy or a gateway. When empty, the ANTHROPIC_BASE_URL environment variable is used, else https://api.anthropic.com."},
  {"mme.chat.model", C_EXT, ST_STR, 0, "\"claude-opus-5\"", NULL,
   "The Claude model that Chat and Inline Chat ask."},
  {"mme.chat.fallbacks", C_EXT, ST_BOOL, 0, "true", NULL,
   "Controls whether a request the model declines is answered by another model instead (the API's server-side fallback)."},
  {"mme.languageServers", C_EXT, ST_OBJ, 0, "{}", NULL,
   "The language server (IntelliSense) of each language: the command that starts it, by language id (\"c\", \"go\", \"python\" ...)."},
  {"vim.enable", C_VIM, ST_BOOL, 0, "false", NULL,
   "Enable Vim emulation: Normal, Insert and Visual modes, operators, registers, macros and : commands in the text editor. Vim: Toggle Vim Mode turns it on and off."},
  {"vim.useSystemClipboard", C_VIM, ST_BOOL, 0, "false", NULL,
   "Use system clipboard for unnamed register."},
  {"vim.useCtrlKeys", C_VIM, ST_BOOL, 0, "true", NULL,
   "Enable some Vim Ctrl key commands that override otherwise common operations, like Ctrl+F."},
  {"vim.handleKeys", C_VIM, ST_OBJ, 0, "{\"<C-d>\": true, \"<C-s>\": false, \"<C-z>\": false}", NULL,
   "Delegate certain key combinations back to VS Code to be handled natively."},
  {"vim.hlsearch", C_VIM, ST_BOOL, 0, "false", NULL,
   "Show all matches of the most recent search pattern."},
  {"vim.incsearch", C_VIM, ST_BOOL, 0, "true", NULL,
   "Show where a / or ? search matches as you type it."},
  {"vim.ignorecase", C_VIM, ST_BOOL, 0, "true", NULL,
   "Ignore case in search patterns."},
  {"vim.smartcase", C_VIM, ST_BOOL, 0, "true", NULL,
   "Override the 'ignorecase' option if the search pattern contains upper case characters."},
  {"vim.gdefault", C_VIM, ST_BOOL, 0, "false", NULL,
   "When on, the :substitute flag g is default on. This means that all matches in a line are substituted instead of one. When a g flag is given to a :substitute command, this will toggle the substitution of all or one match."},
  {"vim.startInInsertMode", C_VIM, ST_BOOL, 0, "false", NULL,
   "Start in Insert mode instead of Normal mode."}
};

#define NSET	((int)(sizeof(set) / sizeof(set[0])))


/* the ones mme reads that the page does not show: suggested and known in settings.json */
static const Setting more[] = {
  {"workbench.colorCustomizations", C_LOOK, ST_OBJ, 0, "{}", NULL,
   "Overrides colors from the currently selected color theme. A block named for one theme, \"[Dark+]\": { ... }, is only for it."},
  {"editor.tokenColorCustomizations", C_LOOK, ST_OBJ, 0, "{}", NULL,
   "Overrides editor syntax colors and font style from the currently selected color theme: \"comments\", \"strings\" ... or \"textMateRules\"."},
  {"workbench.editor.decorations.colors", C_LOOK, ST_BOOL, 0, "true", NULL,
   "Controls whether editor file decorations should use colors."},
  {"workbench.editor.decorations.badges", C_LOOK, ST_BOOL, 0, "true", NULL,
   "Controls whether editor file decorations should use badges."},
  {"terminal.integrated.defaultProfile.windows", C_TERMINAL, ST_STR, 0, "\"\"", NULL,
   "The default terminal profile on Windows."},
  {"terminal.integrated.defaultProfile.linux", C_TERMINAL, ST_STR, 0, "\"\"", NULL,
   "The default terminal profile on Linux."},
  {"terminal.integrated.defaultProfile.osx", C_TERMINAL, ST_STR, 0, "\"\"", NULL,
   "The default terminal profile on macOS."},
  {"mme.inlineCompletionServer", C_EXT, ST_STR, 0, "\"\"", NULL,
   "The command of a language server that gives inline suggestions (ghost text) in every language, a GitHub Copilot language server say."},
  {"screencastMode.keyboardOverlayTimeout", C_LOOK, ST_NUM, 0, "800", NULL,
   "Controls how long (in milliseconds) the keyboard overlay is shown in screencast mode."},
  {"screencastMode.onlyKeyboardShortcuts", C_LOOK, ST_BOOL, 0, "false", NULL,
   "Show only keyboard shortcuts in screencast mode (do not include action names)."},
  {"screencastMode.showCommands", C_LOOK, ST_BOOL, 0, "true", NULL,
   "Controls whether to show command names in the screencast mode overlay."},
  {"screencastMode.showKeys", C_LOOK, ST_BOOL, 0, "true", NULL,
   "Controls whether to show the keys pressed in the screencast mode overlay."},
  {"screencastMode.verticalOffset", C_LOOK, ST_NUM, 0, "20", NULL,
   "Controls the vertical offset of the screencast mode overlay from the bottom as a percentage of the window height."},
  {"security.workspace.trust.enabled", C_FEATURES, ST_BOOL, 0, "true", NULL,
   "Controls whether or not Workspace Trust is enabled: a folder not trusted opens in Restricted Mode (no tasks, debugging or tests; its settings that name programs are not applied). Read from the user's settings only."},
  {"security.workspace.trust.startupPrompt", C_FEATURES, ST_ENUM, 0, "\"once\"", "once|always|never",
   "Controls when the startup prompt to trust a folder is shown: once for each folder, always while it is not trusted, or never (it stays in Restricted Mode until trusted with Manage Workspace Trust)."},
  {"mme.debugAdapters", C_EXT, ST_OBJ, 0, "{}", NULL,
   "The debug adapters, by debug type: the command that starts each one."}
};

#define NMORE	((int)(sizeof(more) / sizeof(more[0])))

/* the running extensions' own settings (their contributes.configuration, from ehost.c), under Extensions */
#define MAX_XSET	512
static Setting g_xset[MAX_XSET];
static int g_nxset;
#define NALL	(NSET + g_nxset)

static const Setting *set_at (int i) {
  return i < NSET ? &set[i] : &g_xset[i - NSET];
}


void settings_ext_clear (void) {
  int i;
  for (i = 0; i < g_nxset; i++) {
    free((char *)g_xset[i].key);
    free((char *)g_xset[i].def);
    free((char *)g_xset[i].values);
    free((char *)g_xset[i].desc);
  }
  g_nxset = 0;
}


void settings_ext_add (const char *key, const char *type, const Json *def, const Json *en, const char *desc) {
  Setting *x;
  Buf b;
  size_t k;
  int i;
  if (g_nxset == MAX_XSET || key == NULL || !*key) return;
  for (i = 0; i < NSET; i++)	/* one of mme's own already */
    if (strcmp(set[i].key, key) == 0) return;
  for (i = 0; i < g_nxset; i++)
    if (strcmp(g_xset[i].key, key) == 0) return;
  x = &g_xset[g_nxset++];
  memset(x, 0, sizeof(*x));
  x->key = xstrdup(key);
  x->toc = C_EXT;
  x->type = strcmp(type, "boolean") == 0 ? ST_BOOL : (strcmp(type, "number") == 0 || strcmp(type, "integer") == 0) ? ST_NUM
          : strcmp(type, "string") == 0 ? ST_STR : ST_OBJ;
  buf_init(&b);
  if (def && def->type != J_NULL) json_write(&b, def);
  else buf_puts(&b, x->type == ST_BOOL ? "false" : x->type == ST_NUM ? "0" : x->type == ST_STR ? "\"\"" : "{}");
  buf_putc(&b, '\0');
  x->def = buf_take(&b);
  if (x->type == ST_STR && en && en->type == J_ARR && en->n > 0) {	/* a list to pick from */
    buf_init(&b);
    for (k = 0; k < en->n; k++) buf_printf(&b, "%s%s", k ? "|" : "", json_str(en->kid[k], ""));
    buf_putc(&b, '\0');
    x->values = buf_take(&b);
    x->type = ST_ENUM;
  }
  x->desc = xstrdup(desc ? desc : "");
}


/* the setting named key, of the page's or the others; NULL: none */
static const Setting *find_set (const char *key) {
  int i;
  for (i = 0; i < NALL; i++)
    if (strcmp(set_at(i)->key, key) == 0) return set_at(i);
  for (i = 0; i < NMORE; i++)
    if (strcmp(more[i].key, key) == 0) return &more[i];
  return NULL;
}


/* a setting mme has (for Import VS Code Settings, and settings.json's problems) */
int settings_known (const char *key) {
  return find_set(key) != NULL;
}


/* "editor.minimap.enabled" -> "Editor › Minimap: " and "Enabled" */
static void title_of (const char *key, char *cat, size_t ncat, char *name, size_t nname) {
  const char *last = strrchr(key, '.');
  size_t c = 0, n = 0;
  const char *p;
  int word = 1;
  cat[0] = name[0] = '\0';
  for (p = key; *p && c + 8 < ncat; p++) {
    if (p == last) break;
    if (*p == '.') {
      memcpy(cat + c, " \xE2\x80\xBA ", 5);	/* " › " */
      c += 5;
      word = 1;
      continue;
    }
    if (*p >= 'A' && *p <= 'Z' && !word) cat[c++] = ' ';
    cat[c++] = (char)(word && *p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    word = 0;
  }
  if (c + 3 < ncat) {
    cat[c++] = ':';
    cat[c++] = ' ';
  }
  cat[c] = '\0';
  word = 1;
  for (p = last ? last + 1 : key; *p && n + 3 < nname; p++) {
    if (*p >= 'A' && *p <= 'Z' && !word) name[n++] = ' ';
    name[n++] = (char)(word && *p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    word = 0;
  }
  name[n] = '\0';
}


/* the description as it shows: #editor.tabSize# is "Editor: Tab Size" */
static void desc_of (const Setting *s, char *out, size_t n) {
  const char *p = s->desc;
  size_t o = 0;
  while (*p && o + 1 < n) {
    if (*p == '#') {
      const char *e = strchr(p + 1, '#');
      if (e && e - p < 80) {
        char key[96], cat[128], name[96], both[256];
        size_t i;
        memcpy(key, p + 1, (size_t)(e - p - 1));
        key[e - p - 1] = '\0';
        title_of(key, cat, sizeof(cat), name, sizeof(name));
        snprintf(both, sizeof(both), "%s%s", cat, name);
        for (i = 0; both[i] && o + 1 < n; i++) out[o++] = both[i];
        p = e + 1;
        continue;
      }
    }
    out[o++] = *p++;
  }
  out[o] = '\0';
}


/* the value now, as text: "4", "true", "on" (a string without its quotes) */
static void value_of (const Setting *s, char *out, size_t n) {
  const Json *j = settings_value(s->key);
  if (s->type == ST_OBJ) {
    snprintf(out, n, "%s", j ? "set" : "");
    return;
  }
  if (j == NULL || (s->type == ST_BOOL && j->type != J_BOOL) || (s->type == ST_NUM && j->type != J_NUM) ||
      ((s->type == ST_STR || s->type == ST_ENUM) && j->type != J_STR)) {	/* the default */
    if (s->def[0] == '"') {
      snprintf(out, n, "%s", s->def + 1);
      if (strlen(out) > 0) out[strlen(out) - 1] = '\0';
    }
    else snprintf(out, n, "%s", s->def);
    return;
  }
  if (j->type == J_BOOL) snprintf(out, n, "%s", j->b ? "true" : "false");
  else if (j->type == J_NUM) snprintf(out, n, "%g", j->num);
  else snprintf(out, n, "%s", j->str);
}


static void def_of (const Setting *s, char *out, size_t n) {
  if (s->def[0] == '"') {
    snprintf(out, n, "%s", s->def + 1);
    if (strlen(out) > 0) out[strlen(out) - 1] = '\0';
  }
  else snprintf(out, n, "%s", s->def);
}


/* not at its default: the blue bar */
static int modified (const Setting *s) {
  char v[256], d[256];
  if (s->type == ST_OBJ) return settings_value(s->key) != NULL;
  value_of(s, v, sizeof(v));
  def_of(s, d, sizeof(d));
  return strcmp(v, d) != 0;
}


/* an enum's values: count, and the i-th into out */
static int enum_count (const Setting *s) {
  const char *p;
  int n = 1;
  if (s->values == NULL) return theme_count();
  for (p = s->values; *p; p++) n += *p == '|';
  return n;
}


static void enum_at (const Setting *s, int i, char *out, size_t n) {
  const char *p = s->values, *e;
  if (p == NULL) {
    snprintf(out, n, "%s", theme_name(i));
    return;
  }
  while (i-- > 0 && (p = strchr(p, '|')) != NULL) p++;
  if (p == NULL) {
    out[0] = '\0';
    return;
  }
  e = strchr(p, '|');
  snprintf(out, n, "%.*s", (int)(e ? (size_t)(e - p) : strlen(p)), p);
}


/* the setting takes value v (text: quoted for strings here) */
static void write_value (const Setting *s, const char *v, PageAct *a) {
  if (s->type == ST_STR || s->type == ST_ENUM) settings_put(s->key, v);
  else settings_put_json(s->key, v);
  a->what = PA_APPLY;
}

/* v into b for a snippet: in a choice (,|\ escaped) or a placeholder ($}\ escaped) */
static void snip_esc (Buf *b, const char *v, int choice) {
  for (; *v; v++) {
    if (*v == '\\' || (choice ? *v == ',' || *v == '|' : *v == '$' || *v == '}')) buf_putc(b, '\\');
    buf_putc(b, *v);
  }
}


/* the setting's value to type, as a snippet: the default first */
static void value_snippet (Buf *b, const Setting *s) {
  char d[256], e[256];
  int k;
  def_of(s, d, sizeof(d));
  if (s->type == ST_BOOL) buf_puts(b, strcmp(d, "true") == 0 ? "${1|true,false|}" : "${1|false,true|}");
  else if (s->type == ST_ENUM) {
    buf_puts(b, "\"${1|");
    snip_esc(b, d, 1);
    for (k = 0; k < enum_count(s); k++) {
      enum_at(s, k, e, sizeof(e));
      if (strcmp(e, d) == 0) continue;
      buf_putc(b, ',');
      snip_esc(b, e, 1);
    }
    buf_puts(b, "|}\"");
  }
  else if (s->type == ST_STR) {
    buf_puts(b, "\"${1:");
    snip_esc(b, d, 0);
    buf_puts(b, "}\"");
  }
  else {	/* a number, an object */
    buf_puts(b, "${1:");
    snip_esc(b, s->def, 0);
    buf_putc(b, '}');
  }
}


/* its description, and its default: the suggestion's and the hover's */
static char *doc_md (const Setting *s, int title) {
  char text[2048], cat[128], name[96];
  Buf b;
  desc_of(s, text, sizeof(text));
  buf_init(&b);
  if (title) {
    title_of(s->key, cat, sizeof(cat), name, sizeof(name));
    buf_printf(&b, "**%s%s**\n\n", cat, name);
  }
  buf_printf(&b, "%s\n\nDefault: `%s`", text, s->def);
  return buf_take(&b);
}


/*
** The settings as suggestions in settings.json, VS Code's: the key, and
** what goes in is "key": value with the value to pick or type (the
** default first). The ones in have[] (keys, nhave of them) are left out;
** lang: in a "[python]" block, only the ones a language can override.
*/
CompItem *settings_suggest (const char *const *have, size_t nhave, int lang, size_t *n) {
  CompItem *v = (CompItem *)xmalloc((NALL + NMORE + 1) * sizeof(CompItem));
  int i;
  size_t o = 0, h;
  for (i = 0; i < NALL + NMORE; i++) {
    const Setting *s = i < NALL ? set_at(i) : &more[i - NALL];
    CompItem *c;
    Buf b;
    for (h = 0; h < nhave && strcmp(have[h], s->key) != 0; h++) {}
    if (h < nhave || (lang && !settings_overridable(s->key))) continue;
    c = &v[o++];
    memset(c, 0, sizeof(*c));
    buf_init(&b);
    buf_printf(&b, "\"%s\": ", s->key);
    value_snippet(&b, s);
    c->label = xstrdup(s->key);
    c->detail = xstrdup("");
    c->insert = buf_take(&b);
    c->filter = xstrdup(s->key);
    c->sort = xstrdup(s->key);
    c->kind = 10;	/* property */
    c->snippet = 1;
    c->doc = doc_md(s, 0);
  }
  *n = o;
  return v;
}


/* a suggestion of a value: what it shows, what goes in */
static void value_item (CompItem *c, const char *label, const char *insert, int is_def, int kind) {
  char sort[16];
  static int order;
  memset(c, 0, sizeof(*c));
  c->label = xstrdup(label);
  c->detail = xstrdup(is_def ? "default" : "");
  c->insert = xstrdup(insert);
  c->filter = xstrdup(label);
  snprintf(sort, sizeof(sort), "%c%04d", is_def ? '0' : '1', order++ % 10000);
  c->sort = xstrdup(sort);
  c->kind = kind;
}


/* the values key can take, the default first; NULL (n 0): nothing to offer */
CompItem *settings_values (const char *key, size_t *n) {
  const Setting *s = find_set(key);
  CompItem *v;
  char d[256], e[256], q[300];
  int k, ne;
  size_t o = 0;
  *n = 0;
  if (s == NULL || s->type == ST_OBJ) return NULL;
  def_of(s, d, sizeof(d));
  ne = s->type == ST_ENUM ? enum_count(s) : 2;
  v = (CompItem *)xmalloc((size_t)(ne + 2) * sizeof(CompItem));
  if (s->type == ST_BOOL) {
    value_item(&v[o++], "true", "true", strcmp(d, "true") == 0, 12);
    value_item(&v[o++], "false", "false", strcmp(d, "false") == 0, 12);
  }
  else if (s->type == ST_ENUM)
    for (k = 0; k < ne; k++) {
      Buf b;
      enum_at(s, k, e, sizeof(e));
      buf_init(&b);
      json_put_str(&b, e, strlen(e));
      buf_putc(&b, '\0');
      snprintf(q, sizeof(q), "%s", b.s);
      buf_free(&b);
      value_item(&v[o++], e, q, strcmp(e, d) == 0, 12);
    }
  else value_item(&v[o++], d, s->def, 1, 12);	/* a number, a string: its default */
  *n = o;
  return v;
}


/* what the hover says of a setting (Markdown); NULL: not one of mme's */
char *settings_hover (const char *key) {
  const Setting *s = find_set(key);
  return s ? doc_md(s, 1) : NULL;
}


/*
** settings.json's problems, VS Code's words: a key mme does not have (1,
** a hint), a value of the wrong kind or not one of the list (2, a
** warning), a setting a "[lang]" block cannot hold (2). jtype: the
** value's J_*; str: a string's text. 0: nothing to say.
*/
/*
** Values VS Code takes that mme's lists do not have: no problem is said of
** them in settings.json (mme reads them as its nearest, or its default). A
** boolean's words are the strings it also takes.
*/
static const struct {
  const char *key, *values;
} vs_values[] = {
  {"editor.wordWrap", "wordWrapColumn|bounded"},
  {"editor.inlayHints.enabled", "onUnlessPressed|offUnlessPressed"},
  {"editor.lightbulb.enabled", "on"},
  {"workbench.startupEditor", "readme|newUntitledFile|welcomePageInEmptyWorkbench|terminal"},
  {"explorer.sortOrder", "foldersNestsFiles"},
  {"explorer.autoReveal", "focusNoScroll"},
  {"terminal.integrated.shellIntegration.decorationsEnabled", "overviewRuler"},
  {"scm.diffDecorations", "overview|minimap"},
  {"workbench.panel.defaultLocation", "top"},
  {"editor.wordBasedSuggestions", "off|currentDocument|matchingDocuments|allDocuments"},
  {"editor.occurrencesHighlight", "off|singleFile|multiFile"},
  {"editor.guides.highlightActiveIndentation", "always"},
  {"files.encoding",
   "iso88593|iso885915|macroman|cp437|windows1256|iso88596|windows1257|iso88594|iso885914|windows1250|"
   "iso88592|cp852|windows1251|cp866|cp1125|iso88595|koi8r|koi8u|iso885913|windows1253|iso88597|"
   "windows1255|iso88598|iso885910|iso885916|windows1254|iso88599|windows1258|gbk|gb18030|cp950|"
   "big5hkscs|shiftjis|eucjp|euckr|windows874|iso885911|koi8ru|koi8t|gb2312|cp865|cp850"}
};


static int vs_takes (const char *key, const char *v) {
  size_t i, n = strlen(v);
  for (i = 0; i < sizeof(vs_values) / sizeof(vs_values[0]); i++)
    if (strcmp(vs_values[i].key, key) == 0) {
      const char *p = vs_values[i].values;
      while (*p) {
        const char *e = strchr(p, '|');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && strncmp(p, v, n) == 0) return 1;
        p += len;
        if (*p) p++;
      }
    }
  return 0;
}


int settings_check (const char *key, int jtype, const char *str, int in_lang, char *msg, size_t n) {
  const Setting *s = find_set(key);
  char e[256];
  int k, ne;
  if (s == NULL) {
    snprintf(msg, n, "Unknown Configuration Setting");
    return 1;
  }
  if (in_lang && !settings_overridable(key)) {
    snprintf(msg, n, "This setting cannot be applied in this language.");
    return 2;
  }
  if (jtype == J_STR && vs_takes(key, str)) return 0;
  if (s->type == ST_ENUM && s->values == NULL) return 0;	/* a theme: VS Code's may be one mme does not have */
  if (s->type == ST_BOOL && jtype != J_BOOL) {
    snprintf(msg, n, "Incorrect type. Expected \"boolean\".");
    return 2;
  }
  if (s->type == ST_NUM && jtype != J_NUM) {
    snprintf(msg, n, "Incorrect type. Expected \"number\".");
    return 2;
  }
  if (s->type == ST_STR && jtype != J_STR) {
    snprintf(msg, n, "Incorrect type. Expected \"string\".");
    return 2;
  }
  if (s->type == ST_ENUM && jtype == J_STR) {
    size_t o;
    ne = enum_count(s);
    for (k = 0; k < ne; k++) {
      enum_at(s, k, e, sizeof(e));
      if (strcmp(e, str) == 0) return 0;
    }
    o = (size_t)snprintf(msg, n, "Value is not accepted. Valid values: ");
    for (k = 0; k < ne && o + 1 < n; k++) {
      enum_at(s, k, e, sizeof(e));
      o += (size_t)snprintf(msg + o, n - o, "%s\"%s\"", k ? ", " : "", e);
      if (o >= n) o = n - 1;
    }
    if (o + 1 < n) snprintf(msg + o, n - o, ".");
    return 2;
  }
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** The page
** ===================================================================
*/

enum { FOCUS_SEARCH, FOCUS_LIST };

typedef struct Entry {
  int head;	/* the toc entry it is under */
  int set;	/* a setting: its index; -1: the heading of head */
} Entry;

#define MAXHIT	64

static struct {
  char search[128];
  int focus;
  Entry e[(NSET + MAX_XSET) * 2 + TOC_N + 2];
  int ne, sel;	/* sel: the entry of the setting in focus */
  int top;	/* the list's first row shown */
  int editing;	/* a number or a string being typed */
  int fresh;	/* its text is selected: the first key typed takes its place */
  char text[256];
  size_t cur;
  int drop, drop_sel;	/* an enum's list is open */
  /* where things are, from the last picture: for the mouse */
  int x, y, w, h, list_x, list_w, body_y, body_h, toc_x, toc_w, json_x, json_y;
  int toc_row[TOC_N];
  struct {
    int entry, y0, y1, ctl_y, ctl_x0, ctl_x1, gear_x;
  } hit[MAXHIT];
  int nhit;
  int drop_x, drop_y, drop_w, drop_n, drop_top;
  int bar_x, drag_bar;	/* the scrollbar's column; its thumb held: 1 + where in it it was taken */
  int follow;	/* the setting in focus moved: the list scrolls to it (the wheel, the bar leave it) */
} S;


static void lower_to (const char *s, char *out, size_t n) {
  size_t i;
  for (i = 0; s[i] && i + 1 < n; i++) out[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]);
  out[i] = '\0';
}


/* does setting s have every word of the search in it? "@modified" too */
static int matches (const Setting *s) {
  char hay[1024], cat[128], name[96], d[600], q[128], *w, *save;
  if (S.search[0] == '\0') return 1;
  title_of(s->key, cat, sizeof(cat), name, sizeof(name));
  desc_of(s, d, sizeof(d));
  snprintf(hay, sizeof(hay), "%s %s%s %s", s->key, cat, name, d);
  lower_to(hay, hay, sizeof(hay));
  lower_to(S.search, q, sizeof(q));
  for (w = q; *w; w = save) {	/* each word */
    while (*w == ' ') w++;
    if (*w == '\0') break;
    for (save = w; *save && *save != ' '; save++) ;
    if (*save) *save++ = '\0';
    if (strcmp(w, "@modified") == 0) {
      if (!modified(s)) return 0;
    }
    else if (strstr(hay, w) == NULL) return 0;
  }
  return 1;
}


/* is setting i under toc entry t (Commonly Used: the common ones; a top one: its parts too)? */
static int under (int i, int t) {
  int p;
  if (t == C_COMMON) return set_at(i)->common;
  if (set_at(i)->toc == t) return 1;
  if (toc[t].depth != 0) return 0;
  for (p = set_at(i)->toc; p > 0 && toc[p].depth > 0; p--) ;	/* its top entry */
  return p == t;
}


/*
** The list again, as the search says: every entry of the table of contents
** with its heading and its settings; searching, the matches only (each once).
*/
static void build (void) {
  int t, i, keep = (S.sel >= 0 && S.sel < S.ne) ? S.e[S.sel].set : -1;
  S.ne = 0;
  for (t = 0; t < TOC_N; t++) {
    int any = 0;
    if (S.search[0] && t == C_COMMON) continue;
    for (i = 0; i < NALL; i++)
      if (under(i, t) && matches(set_at(i))) any = 1;
    if (!any) continue;
    if (!S.search[0]) {
      S.e[S.ne].head = t;
      S.e[S.ne++].set = -1;
    }
    for (i = 0; i < NALL; i++)
      if ((t == C_COMMON ? set_at(i)->common : set_at(i)->toc == t) && matches(set_at(i))) {
        S.e[S.ne].head = t;	/* for a setting: the entry it is under */
        S.e[S.ne++].set = i;
      }
  }
  S.sel = -1;
  for (i = 0; i < S.ne; i++)
    if (S.e[i].set >= 0) {
      if (S.sel < 0) S.sel = i;
      if (S.e[i].set == keep) {
        S.sel = i;
        break;
      }
    }
  if (S.top < 0) S.top = 0;
  S.follow = 1;
}


void sui_open (void) {
  S.focus = FOCUS_SEARCH;
  S.editing = S.drop = 0;
  build();
}


void sui_search (void) {
  S.focus = FOCUS_SEARCH;
  S.editing = S.drop = 0;
}


/* word-wrap s in width w: the lines' starts and lengths (at most max); their count */
static int wrap (const char *s, int w, const char **ls, int *ll, int max) {
  int n = 0;
  if (w < 8) w = 8;
  while (*s && n < max) {
    int len = (int)strlen(s), cut;
    if ((int)str_cols(s) <= w) cut = len;
    else {
      int i, cols = 0, last_sp = -1;
      for (i = 0; s[i] && cols < w; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) cols++;
        if (s[i] == ' ') last_sp = i;
      }
      cut = last_sp > 0 ? last_sp : i;
    }
    ls[n] = s;
    ll[n++] = cut;
    s += cut;
    while (*s == ' ') s++;
  }
  return n;
}


/* the rows a setting takes, in width w */
static int block_h (const Setting *s, int w) {
  char d[600];
  const char *ls[16];
  int ll[16];
  desc_of(s, d, sizeof(d));
  if (s->type == ST_BOOL) return 1 + wrap(d, w - 2, ls, ll, 16) + 1;
  return 1 + wrap(d, w, ls, ll, 16) + 1 + 1;
}


static int entry_h (int i, int w) {
  if (S.e[i].set < 0) return 2;	/* a heading, a blank row */
  return block_h(set_at(S.e[i].set), w);
}


/* the rows the whole list takes */
static int list_total (void) {
  int i, total = 0;
  for (i = 0; i < S.ne; i++) total += entry_h(i, S.list_w > 0 ? S.list_w : 60);
  return total;
}


/* not past the end */
static void clamp_top (void) {
  int most = list_total() - S.body_h;
  if (S.top > most) S.top = most;
  if (S.top < 0) S.top = 0;
}


/* the scrollbar's thumb: its first row (from S.body_y) and its length; 0 when all fits */
static int bar_thumb (int *at) {
  int total = list_total(), len;
  *at = 0;
  if (S.body_h < 2 || total <= S.body_h) return 0;
  len = (int)((double)S.body_h * S.body_h / total + 0.5);
  if (len < 1) len = 1;
  if (len > S.body_h - 1) len = S.body_h - 1;
  *at = (int)((double)S.top * (S.body_h - len) / (double)(total - S.body_h) + 0.5);
  if (*at < 0) *at = 0;
  if (*at > S.body_h - len) *at = S.body_h - len;
  return len;
}


/* the thumb's first row goes to row 'at' of the bar: the list follows */
static void bar_to (int at) {
  int len, now, total = list_total();
  len = bar_thumb(&now);
  if (len == 0) return;
  if (at < 0) at = 0;
  if (at > S.body_h - len) at = S.body_h - len;
  S.top = (int)((double)at * (total - S.body_h) / (double)(S.body_h - len) + 0.5);
  clamp_top();
}


/* the list scrolls so the setting in focus is in view */
static void keep_sel (void) {
  int i, y = 0, h;
  if (S.sel < 0 || S.list_w <= 0) return;
  for (i = 0; i < S.sel; i++) y += entry_h(i, S.list_w);
  h = entry_h(S.sel, S.list_w);
  if (S.sel > 0 && S.e[S.sel - 1].set < 0 && y - 2 < S.top) y -= 2;	/* its heading too */
  if (y < S.top) S.top = y;
  if (y + h > S.top + S.body_h) S.top = y + h - S.body_h;
  if (S.top < 0) S.top = 0;
}


/* text in bold, in the page's colors */
static int put_bold (int x, int y, int w, const char *s, int on) {
  uint32_t fg = ui_color(C_EDITOR_FG), bg = ui_color(on ? C_LINE_BG : C_EDITOR_BG);
  int c = 0;
  size_t i = 0, n = strlen(s), len;
  while (i < n && c < w) {
    uint32_t cp = utf8_decode(s + i, n - i, &len);
    c += scr_put_rgb(x + c, y, cp, fg, bg, RGB_BOLD);
    i += len;
  }
  return c;
}


static void draw_control (const Setting *s, int e, int x, int y, int w, int on) {
  char v[256];
  int bw = w < 42 ? w : 42;
  value_of(s, v, sizeof(v));
  if (S.nhit < MAXHIT) {
    S.hit[S.nhit].ctl_y = y;
    S.hit[S.nhit].ctl_x0 = x;
    S.hit[S.nhit].ctl_x1 = x + (s->type == ST_BOOL ? 1 : bw);
  }
  (void)e;
  if (s->type == ST_OBJ) {	/* a link */
    const char *t = "Edit in settings.json";
    size_t i;
    for (i = 0; t[i] && (int)i < w; i++)
      scr_put_rgb(x + (int)i, y, (unsigned char)t[i], ui_color(C_ACCENT), ui_color(on ? C_LINE_BG : C_EDITOR_BG), RGB_UNDER);
    if (S.nhit < MAXHIT) S.hit[S.nhit].ctl_x1 = x + (int)strlen(t);
    return;
  }
  if (s->type == ST_BOOL) {
    scr_put_rgb(x, y, strcmp(v, "true") == 0 ? 0xEAB2 : ' ', ui_color(C_INPUT_FG), ui_color(C_INPUT_BG), 0);
    return;
  }
  scr_fill(x, y, bw, S_INPUT);
  if (S.editing && on) {	/* typing into it; the text selected at first */
    size_t i, col = 0;
    scr_fill(x, y, bw, S_INPUT_ON);
    if (S.fresh && S.text[0]) {
      int tw = (int)str_cols(S.text) < bw - 2 ? (int)str_cols(S.text) : bw - 2;
      scr_putsw(x + 1, y, bw - 2, S.text, S_SEL);
      scr_restyle(x + 1, y, tw, S_SEL);
    }
    else scr_putsw(x + 1, y, bw - 2, S.text, S_INPUT_ON);
    for (i = 0; i < S.cur; i++)
      if (((unsigned char)S.text[i] & 0xC0) != 0x80) col++;
    if ((int)col < bw - 2) scr_cursor(x + 1 + (int)col, y);
    return;
  }
  scr_putsw(x + 1, y, bw - (s->type == ST_ENUM ? 4 : 2), v, S_INPUT);
  if (s->type == ST_ENUM) scr_put(x + bw - 2, y, 0xEAB4, S_INPUT);	/* chevron-down */
}


/* one setting at row y (maybe partly above the list: from row 'skip' of it) */
static void draw_block (int e, int y, int skip, int maxy) {
  const Setting *s = set_at(S.e[e].set);
  char cat[128], name[96], d[600];
  const char *ls[16];
  int ll[16], n, i, r = 0, on = e == S.sel && S.focus == FOCUS_LIST, x = S.list_x, w = S.list_w;
  int h = block_h(s, w), st = on ? S_LINE : S_TEXT;
  title_of(s->key, cat, sizeof(cat), name, sizeof(name));
  desc_of(s, d, sizeof(d));
  for (i = skip; i < h && y + i - skip < maxy; i++) scr_fill(x - 2, y + i - skip, w + 3, st);
  if (S.nhit < MAXHIT) {
    S.hit[S.nhit].entry = e;
    S.hit[S.nhit].y0 = y;
    S.hit[S.nhit].y1 = y + h - skip;
    S.hit[S.nhit].ctl_y = -1;
    S.hit[S.nhit].gear_x = -1;
  }
  if (modified(s))	/* the blue bar */
    for (i = skip; i < h - 1 && y + i - skip < maxy; i++)
      scr_put_rgb(x - 2, y + i - skip, 0x258E, ui_color(C_ACCENT), ui_color(on ? C_LINE_BG : C_EDITOR_BG), 0);
#define ROW(yy)	((yy) >= skip && y + (yy) - skip < maxy)
#define AT(yy)	(y + (yy) - skip)
  if (ROW(r)) {	/* Editor: Tab Size */
    int c = scr_puts(x, AT(r), cat, st);
    put_bold(x + c, AT(r), w - c, name, on);
    if (on) {
      scr_put(x - 1, AT(r), 0xEAF8, st);	/* the gear: its menu */
      if (S.nhit < MAXHIT) S.hit[S.nhit].gear_x = x - 1;
    }
  }
  r++;
  if (s->type == ST_BOOL) {	/* the checkbox, its description beside it */
    n = wrap(d, w - 2, ls, ll, 16);
    for (i = 0; i < n; i++, r++)
      if (ROW(r)) {
        if (i == 0) draw_control(s, e, x, AT(r), w, on);
        scr_putsw(x + 2, AT(r), ll[i] < w - 2 ? ll[i] : w - 2, ls[i], st);
      }
    if (n == 0 && ROW(r)) draw_control(s, e, x, AT(r), w, on), r++;
  }
  else {
    n = wrap(d, w, ls, ll, 16);
    for (i = 0; i < n; i++, r++)
      if (ROW(r)) {
        char line[600];
        snprintf(line, sizeof(line), "%.*s", ll[i], ls[i]);
        scr_putsw(x, AT(r), w, line, S_CRUMB);
      }
    if (ROW(r)) draw_control(s, e, x, AT(r), w, on);
    r++;
  }
#undef ROW
#undef AT
  if (S.nhit < MAXHIT) S.nhit++;
}


static void draw_drop (void) {
  const Setting *s;
  int i, n, h, x, y, w = 0;
  char v[128], now[256];
  if (!S.drop || S.sel < 0) return;
  s = set_at(S.e[S.sel].set);
  for (i = 0; i < S.nhit; i++)
    if (S.hit[i].entry == S.sel && S.hit[i].ctl_y >= 0) break;
  if (i == S.nhit) {
    S.drop = 0;
    return;
  }
  n = enum_count(s);
  for (i = 0; i < n; i++) {
    enum_at(s, i, v, sizeof(v));
    if ((int)str_cols(v) + 4 > w) w = (int)str_cols(v) + 4;
  }
  for (i = 0; i < S.nhit; i++)
    if (S.hit[i].entry == S.sel) break;
  x = S.hit[i].ctl_x0;
  if (w < S.hit[i].ctl_x1 - x) w = S.hit[i].ctl_x1 - x;
  y = S.hit[i].ctl_y + 1;
  h = n;
  if (y + n > S.y + S.h && S.hit[i].ctl_y - n >= S.body_y) y = S.hit[i].ctl_y - n;	/* no room under it: over it */
  else if (y + h > S.y + S.h) h = S.y + S.h - y;
  if (h < 1) h = 1;
  if (S.drop_sel < S.drop_top) S.drop_top = S.drop_sel;
  if (S.drop_sel >= S.drop_top + h) S.drop_top = S.drop_sel - h + 1;
  value_of(s, now, sizeof(now));
  S.drop_x = x;
  S.drop_y = y;
  S.drop_w = w;
  S.drop_n = h;
  for (i = 0; i < h && S.drop_top + i < n; i++) {
    int k = S.drop_top + i, st = k == S.drop_sel ? S_MENU_SEL : S_MENU;
    enum_at(s, k, v, sizeof(v));
    scr_fill(x, y + i, w, st);
    scr_putsw(x + 1, y + i, w - 3, v, st);
    if (strcmp(v, now) == 0) scr_put(x + w - 2, y + i, 0xEAB2, st);	/* check */
  }
}


void sui_draw (int x, int y, int w, int h, int focus) {
  int i, ly, t, cur_toc = -1, found = 0, sy;
  char count[64];
  S.x = x;
  S.y = y;
  S.w = w;
  S.h = h;
  S.nhit = 0;
  S.bar_x = 0;
  if (S.ne == 0 && S.search[0] == '\0') build();
  scr_box(x, y, w, h, S_TEXT);
  if (w < 30 || h < 8) return;
  /* the search box */
  scr_fill(x + 2, y + 1, w - 4, focus && S.focus == FOCUS_SEARCH ? S_INPUT_ON : S_INPUT);
  if (S.search[0]) scr_putsw(x + 3, y + 1, w - 26, S.search, focus && S.focus == FOCUS_SEARCH ? S_INPUT_ON : S_INPUT);
  else scr_putsw(x + 3, y + 1, w - 26, "Search settings", S_INPUT_HINT);
  for (i = 0; i < S.ne; i++) found += S.e[i].set >= 0;
  if (S.search[0]) {
    snprintf(count, sizeof(count), found == 1 ? "1 Setting Found" : found ? "%d Settings Found" : "No Settings Found", found);
    scr_puts(x + w - 3 - (int)strlen(count), y + 1, count, S_INPUT_HINT);
  }
  if (focus && S.focus == FOCUS_SEARCH) {
    size_t k, col = 0;
    for (k = 0; S.search[k]; k++)
      if (((unsigned char)S.search[k] & 0xC0) != 0x80) col++;
    scr_cursor(x + 3 + (int)col, y + 1);
  }
  /* "User", its line, and the {} that opens settings.json */
  put_bold(x + 3, y + 3, 4, "User", 0);
  for (i = x + 2; i < x + w - 2; i++) scr_put(i, y + 4, 0x2500, S_MENU_LINE);
  for (i = x + 2; i < x + 8; i++) scr_put_rgb(i, y + 4, 0x2501, ui_color(C_ACCENT), ui_color(C_EDITOR_BG), 0);
  S.json_x = x + w - 4;
  S.json_y = y + 3;
  scr_put(S.json_x, S.json_y, 0xEB0F, S_CRUMB);	/* codicon json: Open Settings (JSON) */
  /* the table of contents on the left, when there is room */
  S.body_y = y + 6;
  S.body_h = h - 6;
  S.toc_w = w >= 70 ? (w / 5 < 16 ? 16 : w / 5 > 26 ? 26 : w / 5) : 0;
  S.toc_x = x + 2;
  S.list_x = x + 2 + S.toc_w + (S.toc_w ? 3 : 0) + 2;
  S.list_w = x + w - 3 - S.list_x;
  if (S.list_w > 100) S.list_w = 100;
  if (S.list_w < 12) return;
  if (S.follow) keep_sel();
  S.follow = 0;
  clamp_top();
  /* the list */
  ly = 0;
  sy = S.body_y;
  for (i = 0; i < S.ne && sy < S.body_y + S.body_h; i++) {
    int eh = entry_h(i, S.list_w);
    if (ly + eh <= S.top) {
      ly += eh;
      continue;
    }
    {
      int skip = S.top > ly ? S.top - ly : 0;
      if (cur_toc < 0 || ly <= S.top) cur_toc = S.e[i].head;
      if (S.e[i].set < 0) {	/* a heading: the top ones bigger (bold), the parts bold */
        if (skip < 1) put_bold(S.list_x, sy, S.list_w, toc[S.e[i].head].name, 0);
      }
      else draw_block(i, sy, skip, S.body_y + S.body_h);
      sy += eh - skip;
    }
    ly += eh;
  }
  if (S.ne == 0) scr_puts(S.list_x, S.body_y, "No Settings Found", S_CRUMB);
  /* VS Code's scrollbar at the page's right edge */
  S.bar_x = x + w - 1;
  {
    int at, len = bar_thumb(&at), r;
    for (r = at; r < at + len; r++)	/* a half block, so it shows on any background */
      scr_put_rgb(S.bar_x, S.body_y + r, 0x2590, ui_color(C_THUMB), ui_color(C_EDITOR_BG), 0);
  }
  /* the table of contents */
  for (t = 0; t < TOC_N; t++) S.toc_row[t] = -1;
  if (S.toc_w) {
    int r = 0;
    for (t = 0; t < TOC_N && r < S.body_h; t++) {
      int n = 0, k, is_on;
      char line[64];
      for (k = 0; k < NALL; k++)
        if (under(k, t) && matches(set_at(k))) n++;
      if (S.search[0] && (n == 0 || t == C_COMMON)) continue;
      is_on = t == cur_toc;
      if (S.search[0]) snprintf(line, sizeof(line), "%*s%s (%d)", toc[t].depth * 2, "", toc[t].name, n);
      else snprintf(line, sizeof(line), "%*s%s", toc[t].depth * 2, "", toc[t].name);
      S.toc_row[t] = S.body_y + r;
      if (is_on) put_bold(S.toc_x, S.body_y + r, S.toc_w, line, 0);
      else scr_putsw(S.toc_x, S.body_y + r, S.toc_w, line, S_CRUMB);
      r++;
    }
    for (i = S.body_y; i < S.body_y + S.body_h; i++) scr_put(S.toc_x + S.toc_w + 1, i, 0x2502, S_MENU_LINE);
  }
  draw_drop();
}


/* the list scrolls to toc entry t: its heading, or its first match */
static void go_toc (int t) {
  int i, y = 0;
  for (i = 0; i < S.ne; i++) {
    if (S.e[i].head == t || (S.e[i].set >= 0 && under(S.e[i].set, t) && t != C_COMMON)) {
      S.top = y;
      for (; i < S.ne && S.e[i].set < 0; i++) ;
      if (i < S.ne) S.sel = i;
      S.focus = FOCUS_LIST;
      return;
    }
    y += entry_h(i, S.list_w > 0 ? S.list_w : 60);
  }
}


/* the setting in focus is used: toggled, its list opened, typed into, or its file opened */
static void activate (PageAct *a) {
  const Setting *s;
  char v[256];
  if (S.sel < 0) return;
  s = set_at(S.e[S.sel].set);
  value_of(s, v, sizeof(v));
  switch (s->type) {
    case ST_BOOL: write_value(s, strcmp(v, "true") == 0 ? "false" : "true", a); break;
    case ST_ENUM: {
      int i, n = enum_count(s);
      char e[128];
      S.drop = 1;
      S.drop_sel = S.drop_top = 0;
      for (i = 0; i < n; i++) {
        enum_at(s, i, e, sizeof(e));
        if (strcmp(e, v) == 0) S.drop_sel = i;
      }
      break;
    }
    case ST_OBJ:
      a->what = PA_CMD;
      a->cmd = CMD_SETTINGS_JSON;
      break;
    default:
      snprintf(S.text, sizeof(S.text), "%s", v);
      S.cur = strlen(S.text);
      S.editing = S.fresh = 1;
  }
}


/* what was typed goes in (a number must be one) */
static void edit_done (PageAct *a) {
  const Setting *s = set_at(S.e[S.sel].set);
  S.editing = 0;
  if (s->type == ST_NUM) {
    char *end;
    double d = strtod(S.text, &end);
    char num[64];
    while (*end == ' ') end++;
    if (S.text[0] == '\0' || *end) {
      toast(1, "Value must be a number.");
      return;
    }
    snprintf(num, sizeof(num), "%g", d);
    write_value(s, num, a);
  }
  else write_value(s, S.text, a);
}


/* a line of text typed into buf (size n) at *cur: 1 when k was for it */
static int edit_key (int k, char *buf, size_t n, size_t *cur) {
  int code = KEY_CODE(k);
  size_t len = strlen(buf);
  if (code == K_LEFT) {
    while (*cur > 0 && ((unsigned char)buf[--*cur] & 0xC0) == 0x80) ;
  }
  else if (code == K_RIGHT) {
    if (*cur < len) ++*cur;
    while (*cur < len && ((unsigned char)buf[*cur] & 0xC0) == 0x80) ++*cur;
  }
  else if (code == K_HOME) *cur = 0;
  else if (code == K_END) *cur = len;
  else if (code == K_BS) {
    size_t a = *cur;
    if (a == 0) return 1;
    while (a > 0 && ((unsigned char)buf[--a] & 0xC0) == 0x80) ;
    memmove(buf + a, buf + *cur, len - *cur + 1);
    *cur = a;
  }
  else if (code == K_DEL) {
    size_t b = *cur + 1;
    if (*cur >= len) return 1;
    while (b < len && ((unsigned char)buf[b] & 0xC0) == 0x80) b++;
    memmove(buf + *cur, buf + b, len - b + 1);
  }
  else if (IS_PASTE(k)) {
    Buf b;
    size_t i;
    buf_init(&b);
    paste_take(k, &b);
    for (i = 0; i < b.len && b.s[i] != '\n' && len + 1 < n; i++, len++) {
      memmove(buf + *cur + 1, buf + *cur, len - *cur + 1);
      buf[(*cur)++] = b.s[i];
    }
    buf_free(&b);
  }
  else if (IS_TEXT(k) && len + 5 < n) {
    char u[4];
    int m = utf8_encode((uint32_t)k, u);
    memmove(buf + *cur + m, buf + *cur, len - *cur + 1);
    memcpy(buf + *cur, u, (size_t)m);
    *cur += (size_t)m;
  }
  else return 0;
  return 1;
}


static size_t g_scur;	/* the search box's cursor */

static void move_sel (int d) {
  int i = S.sel;
  for (;;) {
    i += d;
    if (i < 0 || i >= S.ne) return;
    if (S.e[i].set >= 0) break;
  }
  S.sel = i;
}


/* the gear's menu: Reset Setting, Copy Setting ID, Copy Setting as JSON */
static void gear_menu (int x, int y, PageAct *a) {
  static const char *const label[] = {"Reset Setting", NULL, "Copy Setting ID", "Copy Setting as JSON"};
  static char copy[512];
  const Setting *s;
  char v[256];
  int r;
  if (S.sel < 0) return;
  s = set_at(S.e[S.sel].set);
  r = context_menu(x, y, label, NULL, 4);
  if (r == 0) {
    settings_reset(s->key);
    a->what = PA_APPLY;
  }
  else if (r == 2) {
    snprintf(copy, sizeof(copy), "%s", s->key);
    a->what = PA_COPY;
    a->text = copy;
  }
  else if (r == 3) {
    value_of(s, v, sizeof(v));
    if (s->type == ST_STR || s->type == ST_ENUM) snprintf(copy, sizeof(copy), "\"%s\": \"%s\"", s->key, v);
    else if (s->type == ST_OBJ) snprintf(copy, sizeof(copy), "\"%s\": {}", s->key);
    else snprintf(copy, sizeof(copy), "\"%s\": %s", s->key, v);
    a->what = PA_COPY;
    a->text = copy;
  }
}


void sui_key (int k, PageAct *a) {
  int code = KEY_CODE(k);
  a->what = PA_NONE;
  if (S.drop) {	/* the enum's list */
    const Setting *s = set_at(S.e[S.sel].set);
    int n = enum_count(s);
    if (code == K_UP && S.drop_sel > 0) S.drop_sel--;
    else if (code == K_DOWN && S.drop_sel + 1 < n) S.drop_sel++;
    else if (code == K_HOME) S.drop_sel = 0;
    else if (code == K_END) S.drop_sel = n - 1;
    else if (code == K_ENTER || code == ' ') {
      char v[128];
      enum_at(s, S.drop_sel, v, sizeof(v));
      S.drop = 0;
      write_value(s, v, a);
    }
    else if (code == K_ESC || code == K_TAB) S.drop = 0;
    return;
  }
  if (S.editing) {
    if (code == K_ENTER || code == K_TAB) edit_done(a);
    else if (code == K_ESC) S.editing = 0;
    else {
      if (S.fresh && (IS_TEXT(k) || code == K_BS || code == K_DEL || IS_PASTE(k))) {	/* it replaces */
        S.text[0] = '\0';
        S.cur = 0;
        if (code == K_BS || code == K_DEL) k = 0;
      }
      S.fresh = 0;
      if (k) edit_key(k, S.text, sizeof(S.text), &S.cur);
    }
    return;
  }
  if (S.focus == FOCUS_SEARCH) {
    if (code == K_DOWN || code == K_ENTER || code == K_TAB) {
      if (S.sel >= 0) S.focus = FOCUS_LIST;
    }
    else if (code == K_ESC) {
      S.search[0] = '\0';
      g_scur = 0;
      build();
    }
    else {
      size_t was = strlen(S.search);
      if (g_scur > was) g_scur = was;
      if (edit_key(k, S.search, sizeof(S.search), &g_scur)) {
        S.top = 0;
        build();
      }
    }
    return;
  }
  S.follow = 1;
  switch (code) {	/* the list */
    case K_UP: {
      int was = S.sel;
      move_sel(-1);
      if (S.sel == was) S.focus = FOCUS_SEARCH;	/* above the first: the search box */
      break;
    }
    case K_DOWN: move_sel(1); break;
    case K_PGUP: {
      int i;
      for (i = 0; i < 5; i++) move_sel(-1);
      break;
    }
    case K_PGDN: {
      int i;
      for (i = 0; i < 5; i++) move_sel(1);
      break;
    }
    case K_HOME: S.sel = -1, move_sel(1), S.top = 0; break;
    case K_END: S.sel = S.ne, move_sel(-1); break;
    case K_ENTER: case ' ': activate(a); break;
    case K_TAB: case K_ESC: S.focus = FOCUS_SEARCH; break;
    case K_F10:
      if (k & KM_SHIFT) {	/* Shift+F10: the gear's menu */
        int i;
        for (i = 0; i < S.nhit; i++)
          if (S.hit[i].entry == S.sel) gear_menu(S.list_x, S.hit[i].y0 + 1, a);
      }
      break;
    default:
      if (IS_TEXT(k)) {	/* typing: into the search */
        S.focus = FOCUS_SEARCH;
        g_scur = strlen(S.search);
        edit_key(k, S.search, sizeof(S.search), &g_scur);
        S.top = 0;
        build();
      }
  }
}


int sui_dragging (void) {
  return S.drag_bar != 0;
}


void sui_mouse (const Mouse *m, PageAct *a) {
  int i, press = m->button == 0 && m->press && !m->drag;
  a->what = PA_NONE;
  if (m->wheel) {
    if (S.drop) return;
    S.top += m->wheel * 3;
    clamp_top();
    return;
  }
  if (S.drag_bar) {	/* the thumb follows the mouse until the button comes up */
    if (m->button == 0 && m->drag) bar_to(m->y - S.body_y - (S.drag_bar - 1));
    if (!m->press) S.drag_bar = 0;
    return;
  }
  if (press && S.bar_x > 0 && m->x == S.bar_x && m->y >= S.body_y && m->y < S.body_y + S.body_h && !S.drop) {
    int at, len = bar_thumb(&at), ry = m->y - S.body_y;
    if (len > 0) {
      if (ry >= at && ry < at + len) S.drag_bar = 1 + (ry - at);	/* the thumb is taken */
      else {	/* elsewhere: the thumb jumps there, like VS Code */
        S.drag_bar = 1 + len / 2;
        bar_to(ry - len / 2);
      }
      return;
    }
  }
  if (!press && !(m->button == 2 && m->press)) return;
  if (S.drop) {	/* a value of the open list, or a click elsewhere closes it */
    if (press && m->x >= S.drop_x && m->x < S.drop_x + S.drop_w && m->y >= S.drop_y && m->y < S.drop_y + S.drop_n) {
      const Setting *s = set_at(S.e[S.sel].set);
      char v[128];
      enum_at(s, S.drop_top + m->y - S.drop_y, v, sizeof(v));
      write_value(s, v, a);
    }
    S.drop = 0;
    return;
  }
  if (S.editing) {
    PageAct b;
    b.what = PA_NONE;
    edit_done(&b);
    if (b.what != PA_NONE) *a = b;
  }
  if (m->y == S.y + 1) {	/* the search box */
    S.focus = FOCUS_SEARCH;
    return;
  }
  if (m->y == S.json_y && m->x >= S.json_x - 1 && m->x <= S.json_x + 1) {
    a->what = PA_CMD;
    a->cmd = CMD_SETTINGS_JSON;
    return;
  }
  if (S.toc_w && m->x >= S.toc_x && m->x < S.toc_x + S.toc_w) {	/* the table of contents */
    int t;
    for (t = 0; t < TOC_N; t++)
      if (S.toc_row[t] == m->y) go_toc(t);
    return;
  }
  for (i = 0; i < S.nhit; i++) {
    const Setting *s;
    if (m->y < S.hit[i].y0 || m->y >= S.hit[i].y1) continue;
    S.sel = S.hit[i].entry;
    S.focus = FOCUS_LIST;
    s = set_at(S.e[S.sel].set);
    if (m->button == 2 || (S.hit[i].gear_x >= 0 && m->x == S.hit[i].gear_x && m->y == S.hit[i].y0)) {
      gear_menu(m->x, m->y + 1, a);
      return;
    }
    if (m->y == S.hit[i].ctl_y && m->x >= S.hit[i].ctl_x0 && m->x < S.hit[i].ctl_x1 + (s->type == ST_BOOL ? 0 : 0))
      activate(a);
    else if (s->type == ST_BOOL && m->y > S.hit[i].y0 && m->y < S.hit[i].y1 - 1)
      activate(a);	/* its description is its label: a click toggles, like VS Code */
    return;
  }
}

/* }================================================================== */
