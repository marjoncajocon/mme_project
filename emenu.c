/*
** emenu.c - VS Code's chrome: the menu bar and its menus, the quick
** input box (QuickPick) that pickers and questions use, the modal dialog,
** and the notification in the corner
**
** Each of them runs its own little key loop: ui_background draws the
** window under it, then it draws itself on top and waits for a key.
*/

#include "mme.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void (*ui_background) (void);
char ui_title[512];
char ui_cc[256];


/*
** {==================================================================
** Commands
** ===================================================================
*/

static const char *const names[CMD_N] = {
  "", "New File", "Open File...", "Open Folder...", "Open Project...",
  "Save", "Save As...", "Close Editor", "Exit",
  "Undo", "Redo", "Cut", "Copy", "Paste", "Find",
  "Find in Files", "Select All", "Move Line Up", "Move Line Down",
  "Explorer", "Search", "Source Control", "Toggle Primary Side Bar",
  "Go to Line/Column...", "Next Change", "Previous Change", "Go to File...",
  "Command Palette...", "About", "Toggle Terminal", "Kill Terminal", "Toggle Minimap",
  "Add Cursor Above", "Add Cursor Below", "Add Next Occurrence", "Select All Occurrences",
  "Settings", "New Terminal", "Split Editor", "Focus First Editor Group",
  "Focus Second Editor Group",
  "Go to Definition", "Trigger Suggest", "Next Problem", "Previous Problem",
  "Rename Symbol", "Quick Fix...", "Show Hover", "Problems", "Color Theme", "Zen Mode",
  "Word Wrap", "Replace", "Fold", "Unfold", "Fold All", "Unfold All", "Keyboard Shortcuts",
  "Toggle Line Comment", "Toggle Block Comment", "Copy Line Up", "Copy Line Down", "Delete Line",
  "Expand Line Selection", "Go Back", "Go Forward", "Go to Symbol in Editor...", "Format Document",
  "Insert Snippet...", "Insert Line Below", "Insert Line Above", "Indent Line", "Outdent Line",
  "Configure Snippets", "Extensions", "Install from VSIX...",
  "Git: Checkout to...", "Git: Create Branch...", "Git: Create Branch From...", "Git: Delete Branch...",
  "Git: Rename Branch...", "Git: Merge...", "Git: Pull", "Git: Push", "Git: Fetch", "Git: Sync",
  "Git: Stash", "Git: Pop Latest Stash", "Git: Apply Stash...", "Git: Undo Last Commit", "Git: Commit",
  "Git: Commit Staged (Amend)", "Git: View File History", "Git: Toggle Git Blame Editor Decoration",
  "Git: Refresh", "Toggle Inline View", "Source Control: More Actions...",
  "Go to References", "Go to Implementations", "Go to Type Definition", "Peek Definition",
  "Go to Symbol in Workspace...", "Transform to Uppercase", "Transform to Lowercase",
  "Transform to Title Case", "Sort Lines Ascending", "Sort Lines Descending", "Join Lines",
  "Trim Trailing Whitespace", "Delete Duplicate Lines", "Duplicate Selection", "Go to Bracket",
  "Select to Bracket", "Trigger Parameter Hints", "Reopen Closed Editor", "Change Language Mode",
  "Change End of Line Sequence", "Change Indentation...", "Indent Using Spaces", "Indent Using Tabs",
  "Convert Indentation to Spaces", "Convert Indentation to Tabs", "Detect Indentation from Content",
  "Toggle Render Whitespace", "Toggle Sticky Scroll", "Column Selection Mode",
  "Preferences: Open User Settings (JSON)", "Welcome", "Notifications: Show Notifications",
  "Explorer: New File...", "Explorer: New Folder...", "Explorer: Refresh", "Explorer: Collapse Folders",
  "Copy Path of Active File", "Copy Relative Path of Active File", "Reveal in File Explorer",
  "Open in Integrated Terminal",
  "Run and Debug", "Start Debugging", "Run Without Debugging", "Stop Debugging", "Restart Debugging",
  "Step Over", "Step Into", "Step Out", "Pause", "Toggle Breakpoint", "Debug Console",
  "Open Configurations", "Select and Start Debugging", "Run Task...", "Run Build Task...", "Configure Tasks",
  "Terminal: Split Terminal", "Terminal: Focus Previous Terminal in Terminal Group",
  "Terminal: Focus Next Terminal in Terminal Group", "Terminal: Find", "Terminal: Clear",
  "Terminal: Rename...", "Terminal: Select Default Profile", "Terminal: Create New Terminal (With Profile)",
  "View: Toggle Output", "Output: Show Output Channels...", "Output: Clear Output",
  "View: Toggle Maximized Panel",
  "Replace in Files", "View: Close Other Editors in Group", "View: Close Editors to the Right in Group",
  "View: Close Saved Editors in Group", "View: Close All Editors in Group", "View: Close All Editors",
  "View: Pin Editor", "View: Unpin Editor", "View: Keep Editor",
  "File: Reveal Active File in Explorer View", "View: Quick Open Previous Recently Used Editor in Group",
  "View: Show All Editors By Most Recently Used", "Local History: Find Entry to Restore...",
  "Markdown: Open Preview", "Markdown: Open Preview to the Side", "Explorer: Focus on Open Editors View",
  "Explorer: Focus on Timeline View", "Local History: Compare with File", "Focus and Select Breadcrumbs",
  "Auto Save", "File: Compare Active File with Saved", "File: Compare Active File With...",
  "File: Compare Active File with Clipboard", "Select for Compare", "Compare with Selected",
  "Change File Encoding", "File: Revert File", "View: Toggle Primary Side Bar Position",
  "Type", "Terminal: Send Custom Sequence to Terminal", "Preferences: Import VS Code Settings",
  "Open Workspace from File...", "Add Folder to Workspace...", "Save Workspace As...", "Close Workspace",
  "View: Split Editor Down", "View: Split Editor Left", "View: Split Editor Up",
  "View: Focus Third Editor Group", "View: Focus Fourth Editor Group", "View: Focus Left Editor Group",
  "View: Focus Right Editor Group", "View: Focus Editor Group Above", "View: Focus Editor Group Below",
  "View: Move Editor into Next Group", "View: Move Editor into Previous Group",
  "View: Join Editor Group with Next Group", "View: Join All Editor Groups", "View: Toggle Editor Group Sizes",
  "View: Reset Editor Group Sizes", "View: Single Column Editor Layout", "View: Two Columns Editor Layout",
  "View: Three Columns Editor Layout", "View: Two Rows Editor Layout", "View: Grid Editor Layout (2x2)",
  "View: Toggle Centered Layout", "View: Toggle Menu Bar", "View: Toggle Status Bar Visibility",
  "View: Toggle Activity Bar Visibility", "Editor Layout...", "Appearance...",
  "Format Selection", "Organize Imports", "Source Action...", "Expand Selection", "Shrink Selection",
  "Fold All Regions", "Unfold All Regions", "Fold All Block Comments", "Fold Level 1", "Fold Level 2",
  "Fold Level 3", "Fold Level 4", "Fold Level 5", "Fold Level 6", "Fold Level 7",
  "Show Call Hierarchy", "Show Outgoing Calls", "Show Supertypes", "Show Subtypes",
  "Show Next Change", "Show Previous Change", "Go to Next Change", "Go to Previous Change",
  "Git: Stage Change", "Git: Revert Change", "Git: Stage Selected Ranges", "Git: Unstage Selected Ranges",
  "Git: Revert Selected Ranges", "Merge Conflict: Accept Current", "Merge Conflict: Accept Incoming",
  "Merge Conflict: Accept Both", "Merge Conflict: Accept All Current", "Merge Conflict: Accept All Incoming",
  "Merge Conflict: Accept All Both", "Merge Conflict: Next Conflict", "Merge Conflict: Previous Conflict",
  "Merge Conflict: Compare Current Conflict", "Git: Clone", "Git: Initialize Repository", "Git: Publish Branch",
  "Testing: Focus on Test Explorer View", "Test: Run All Tests", "Test: Run Test at Cursor",
  "Test: Run Tests in Current File", "Test: Rerun Last Run", "Test: Debug Test at Cursor",
  "Test: Refresh Tests", "Test: Show Output", "Test: Cancel Test Run", "Test: Collapse All Tests",
  "Problems: Focus Filter", "Problems: Collapse All", "Problems: Toggle Show Active File Only",
  "Problems: Copy Message", "Outline: Toggle Follow Cursor", "Outline: Sort By...", "Outline: Collapse All",
  "Debug: Add Conditional Breakpoint...", "Debug: Add Logpoint...", "Debug: Edit Breakpoint",
  "Debug: Enable or Disable Breakpoint", "Debug: Remove All Breakpoints", "Debug: Enable All Breakpoints",
  "Debug: Disable All Breakpoints", "Debug: Run to Cursor", "Debug: Jump to Cursor", "Debug: Add to Watch",
  "Add Cursors to Line Ends", "Cursor Undo", "Cursor Redo", "Move Last Selection to Next Find Match",
  "Change All Occurrences", "Select All Occurrences of Find Match", "Reindent Lines", "Reindent Selected Lines",
  "Delete All Left", "Delete All Right", "Transpose Characters around the Cursor", "Cursor Word Part Left",
  "Cursor Word Part Right", "Delete Word Part Left", "Delete Word Part Right", "Toggle Tab Key Moves Focus",
  "Add Line Comment", "Remove Line Comment", "Scroll Page Up", "Scroll Page Down",
  "Terminal: Run Selected Text In Active Terminal", "Terminal: Run Active File In Active Terminal",
  "Terminal: Focus Terminal", "Terminal: Scroll To Previous Command", "Terminal: Scroll To Next Command",
  "Terminal: Select All", "Terminal: Copy Selection", "Terminal: Paste into Active Terminal",
  "Terminal: Run Recent Command...", "Terminal: Go to Recent Directory...", "Terminal: Change Color...",
  "Terminal: Change Icon...", "Developer: Inspect Editor Tokens and Scopes",
  "Explorer: Open to the Side", "Find in Folder...",
  "Help: Keyboard Shortcuts Reference", "Help: Tips and Tricks", "Help: Show All Commands",
  "Diff: Toggle Ignore Trim Whitespace", "Diff: Toggle Collapse Unchanged Regions",
  "View: Zoom In", "View: Zoom Out", "View: Reset Zoom",
  "mme: Restart Language Server", "mme: Show Language Status", "Notifications: Focus Notification Toast",
  "Notifications: Accept Notification Primary Action", "Notifications: Clear All Notifications",
  "Manage", "View: Move Panel Right", "View: Move Panel Left", "View: Move Panel To Bottom",
  "View: Set Panel Alignment to Center", "View: Set Panel Alignment to Justify", "View: Toggle Command Center",
  "View: Reopen Editor With Hex Editor", "Image Preview: Zoom In", "Image Preview: Zoom Out",
  "Image Preview: Reset Zoom",
  "Git: Open Merge Editor", "Merge Editor: Complete Merge", "Code Lens: Run...",
  "Trigger Inline Suggestion", "Accept Inline Suggestion", "Accept Next Word Of Inline Suggestion",
  "Hide Inline Suggestion", "Show Next Inline Suggestion", "Show Previous Inline Suggestion",
  "GitHub Copilot: Sign In", "GitHub Copilot: Sign Out", "GitHub Copilot: Show Status",
  "Jump to Next Edit Suggestion", "mme: Toggle Next Edit Suggestions",
  "Show or Focus Standalone Color Picker",
  "Chat: Open Chat", "Chat: New Chat", "Chat: Clear", "View: Toggle Secondary Side Bar Visibility", "Chat: Cancel",
  "Chat: Toggle Implicit Context", "Chat: Set API Key...", "Inline Chat: Start", "Inline Chat: Accept Changes",
  "Inline Chat: Discard",
  "Search Editor: New Search Editor", "Search Editor: Open Results in Editor", "Search Editor: Rerun Search",
  "Search Editor: Toggle Context Lines", "Search Editor: Increase Context Lines",
  "Search Editor: Decrease Context Lines", "Search Editor: Focus Search Editor Input",
  "Search Editor: Delete File Results",
  "Vim: Toggle Vim Mode", "File: Save All", "Git: View All Changes", "Git: View Staged Changes",
  "Test: Run All Tests with Coverage", "Test: Run Tests in Current File with Coverage",
  "Test: Run Test at Cursor with Coverage", "Test: Close Coverage", "Test: Toggle Inline Coverage",
  "Profiles: Switch Profile...", "Profiles: New Profile...", "Profiles: Rename Profile...", "Profiles: Delete Profile...",
  "Git: Create Worktree...", "Git: Open Worktree...", "Git: Delete Worktree...",
  "Workspaces: Manage Workspace Trust", "Developer: Toggle Screencast Mode",
  "New Window", "Workspaces: Duplicate As Workspace in New Window", "Close Window",
  "Notebook: Run All", "Notebook: Restart Kernel", "Notebook: Interrupt Kernel", "Notebook: Clear All Outputs",
  "Create: New Jupyter Notebook",
  "GitHub Pull Requests: Pull Requests", "GitHub Issues: Issues", "GitHub Pull Requests: Create Pull Request",
  "GitHub Issues: Create Issue", "GitHub: Sign In", "GitHub: Sign Out",
  "Remote-SSH: Connect to Host...",
  "Chat: Change Model...", "Chat: Toggle Agent Mode",
  "Settings Sync: Turn On...", "Settings Sync: Turn Off", "Settings Sync: Sync Now", "Settings Sync: Show Synced Data",
  "Ports: Forward a Port", "Ports: Focus on Ports View",
  "View: Move Editor into New Window", "View: Copy Editor into New Window",
  "Open Accessible View", "Help: Accessibility Help"
};

static const char *const keys[CMD_N] = {
  "", "Ctrl+N", "Ctrl+O", "", "Ctrl+R",
  "Ctrl+S", "Ctrl+Shift+S", "Ctrl+W", "Ctrl+Q",
  "Ctrl+Z", "Ctrl+Y", "Ctrl+X", "Ctrl+C", "Ctrl+V", "Ctrl+F",
  "Ctrl+Shift+F", "Ctrl+A", "Alt+Up", "Alt+Down",
  "Ctrl+Shift+E", "Ctrl+Shift+F", "Ctrl+Shift+G", "Ctrl+B",
  "Ctrl+G", "F7", "Shift+F7", "Ctrl+P",
  "Ctrl+Shift+P", "", "Ctrl+`", "", "",
  "Ctrl+Alt+Up", "Ctrl+Alt+Down", "Ctrl+D", "Ctrl+Shift+L",
  "Ctrl+,", "Ctrl+Shift+`", "Ctrl+\\", "Ctrl+1", "Ctrl+2",
  "F12", "Ctrl+Space", "F8", "Shift+F8",
  "F2", "Ctrl+.", "Ctrl+K Ctrl+I", "Ctrl+Shift+M", "Ctrl+K Ctrl+T", "Ctrl+K Z",
  "Alt+Z", "Ctrl+H", "Ctrl+Shift+[", "Ctrl+Shift+]", "Ctrl+K Ctrl+0", "Ctrl+K Ctrl+J",
  "Ctrl+K Ctrl+S",
  "Ctrl+/", "Shift+Alt+A", "Shift+Alt+Up", "Shift+Alt+Down", "Ctrl+Shift+K",
  "Ctrl+L", "Alt+Left", "Alt+Right", "Ctrl+Shift+O", "Shift+Alt+F",
  "", "Ctrl+Enter", "Ctrl+Shift+Enter", "Ctrl+]", "Ctrl+[",
  "", "Ctrl+Shift+X", "",
  "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
  "Shift+F12", "Ctrl+F12", "", "Alt+F12", "Ctrl+T", "", "", "", "", "", "", "Ctrl+K Ctrl+X", "", "",
  "Ctrl+Shift+\\", "", "Ctrl+Shift+Space", "Ctrl+Shift+T", "Ctrl+K M", "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "Shift+Alt+C", "Ctrl+K Ctrl+Shift+C", "Shift+Alt+R", "",
  "Ctrl+Shift+D", "F5", "Ctrl+F5", "Shift+F5", "Ctrl+Shift+F5",
  "F10", "F11", "Shift+F11", "F6", "F9", "Ctrl+Shift+Y",
  "", "", "", "Ctrl+Shift+B", "",
  "Ctrl+Shift+5", "Alt+Left", "Alt+Right", "Ctrl+F", "", "", "", "",
  "Ctrl+Shift+U", "", "", "",
  "Ctrl+Shift+H", "", "", "Ctrl+K U", "Ctrl+K W", "Ctrl+K Ctrl+W", "Ctrl+K Shift+Enter", "Ctrl+K Shift+Enter",
  "Ctrl+K Enter", "", "Ctrl+Tab", "", "", "Ctrl+Shift+V", "Ctrl+K V", "", "", "", "Ctrl+Shift+.",
  "", "Ctrl+K D", "", "Ctrl+K C", "", "", "", "", "",
  "", "", "", "", "", "", "",
  "Ctrl+K Ctrl+\\", "", "", "Ctrl+3", "Ctrl+4", "Ctrl+K Ctrl+Left", "Ctrl+K Ctrl+Right", "Ctrl+K Ctrl+Up",
  "Ctrl+K Ctrl+Down", "Ctrl+Alt+Right", "Ctrl+Alt+Left", "", "", "Ctrl+K Ctrl+M", "", "", "", "", "", "",
  "", "", "", "", "", "",
  "Ctrl+K Ctrl+F", "Shift+Alt+O", "", "Shift+Alt+Right", "Shift+Alt+Left",
  "Ctrl+K Ctrl+8", "Ctrl+K Ctrl+9", "Ctrl+K Ctrl+/", "Ctrl+K Ctrl+1", "Ctrl+K Ctrl+2",
  "Ctrl+K Ctrl+3", "Ctrl+K Ctrl+4", "Ctrl+K Ctrl+5", "Ctrl+K Ctrl+6", "Ctrl+K Ctrl+7",
  "Shift+Alt+H", "", "", "",
  "Alt+F3", "Shift+Alt+F3", "Alt+F5", "Shift+Alt+F5", "", "", "Ctrl+K Ctrl+Alt+S", "Ctrl+K Ctrl+N",
  "Ctrl+K Ctrl+R", "", "", "", "", "", "", "", "", "", "", "", "",
  "", "Ctrl+; A", "Ctrl+; C", "Ctrl+; F", "Ctrl+; L", "Ctrl+; Ctrl+C", "", "Ctrl+; Ctrl+O", "Ctrl+; Ctrl+X", "",
  "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "",
  "Shift+Alt+I", "Ctrl+U", "", "Ctrl+K Ctrl+D", "Ctrl+F2", "Alt+Enter", "", "", "", "", "", "", "", "", "",
  "Ctrl+M", "Ctrl+K Ctrl+C", "Ctrl+K Ctrl+U", "Alt+PageUp", "Alt+PageDown",
  "", "", "", "Ctrl+Up", "Ctrl+Down", "", "Ctrl+Shift+C", "Ctrl+Shift+V", "Ctrl+Alt+R", "Ctrl+G", "", "", "",
  "Ctrl+Enter", "Shift+Alt+F",
  "", "", "", "", "",
  "Ctrl+=", "Ctrl+-", "Ctrl+NumPad0",
  "", "", "", "Ctrl+Shift+A", "",
  "", "", "", "", "", "", "",
  "", "", "", "",
  "", "", "",
  "Alt+\\", "Tab", "Ctrl+Right", "Escape", "Alt+]", "Alt+[",
  "", "", "",
  "Tab", "",
  "",
  "Ctrl+Alt+I", "", "", "Ctrl+Alt+B", "", "", "", "Ctrl+I", "Ctrl+Enter", "Escape",
  "", "Alt+Enter", "Ctrl+Shift+R", "Alt+L", "Alt+=", "Alt+-", "Escape", "Ctrl+Shift+Backspace",
  "", "Ctrl+K S", "", "",
  "", "", "", "", "",
  "", "", "", "",
  "", "", "",
  "", "",
  "Ctrl+Shift+N", "", "Ctrl+Shift+W",
  "", "", "", "", "",
  "", "", "", "", "", "",
  "",
  "", "",
  "", "", "", "",
  "", "",
  "", "",
  "Alt+F2", "Alt+F1"
};

static const char *const ids[CMD_N] = {	/* VS Code's commands, for keybindings.json */
  "", "workbench.action.files.newUntitledFile", "workbench.action.files.openFile",
  "workbench.action.files.openFolder", "workbench.action.openRecent",
  "workbench.action.files.save", "workbench.action.files.saveAs",
  "workbench.action.closeActiveEditor", "workbench.action.quit",
  "undo", "redo", "editor.action.clipboardCutAction", "editor.action.clipboardCopyAction",
  "editor.action.clipboardPasteAction", "actions.find",
  "workbench.action.findInFiles", "editor.action.selectAll",
  "editor.action.moveLinesUpAction", "editor.action.moveLinesDownAction",
  "workbench.view.explorer", "workbench.view.search", "workbench.view.scm",
  "workbench.action.toggleSidebarVisibility",
  "workbench.action.gotoLine", "workbench.action.compareEditor.nextChange",
  "workbench.action.compareEditor.previousChange", "workbench.action.quickOpen",
  "workbench.action.showCommands", "workbench.action.showAboutDialog",
  "workbench.action.terminal.toggleTerminal", "workbench.action.terminal.kill",
  "editor.action.toggleMinimap",
  "editor.action.insertCursorAbove", "editor.action.insertCursorBelow",
  "editor.action.addSelectionToNextFindMatch", "editor.action.selectHighlights",
  "workbench.action.openSettings", "workbench.action.terminal.new",
  "workbench.action.splitEditor", "workbench.action.focusFirstEditorGroup",
  "workbench.action.focusSecondEditorGroup",
  "editor.action.revealDefinition", "editor.action.triggerSuggest",
  "editor.action.marker.next", "editor.action.marker.prev",
  "editor.action.rename", "editor.action.quickFix", "editor.action.showHover",
  "workbench.actions.view.problems", "workbench.action.selectTheme",
  "workbench.action.toggleZenMode", "editor.action.toggleWordWrap",
  "editor.action.startFindReplaceAction", "editor.fold", "editor.unfold",
  "editor.foldAll", "editor.unfoldAll", "workbench.action.openGlobalKeybindings",
  "editor.action.commentLine", "editor.action.blockComment", "editor.action.copyLinesUpAction",
  "editor.action.copyLinesDownAction", "editor.action.deleteLines", "expandLineSelection",
  "workbench.action.navigateBack", "workbench.action.navigateForward", "workbench.action.gotoSymbol",
  "editor.action.formatDocument", "editor.action.insertSnippet", "editor.action.insertLineAfter",
  "editor.action.insertLineBefore", "editor.action.indentLines", "editor.action.outdentLines",
  "workbench.action.openSnippets", "workbench.view.extensions",
  "workbench.extensions.action.installVSIX",
  "git.checkout", "git.branch", "git.branchFrom", "git.deleteBranch", "git.renameBranch", "git.merge",
  "git.pull", "git.push", "git.fetch", "git.sync", "git.stash", "git.stashPopLatest", "git.stashApply",
  "git.undoCommit", "git.commit", "git.commitStagedAmend", "git.viewFileHistory",
  "git.blame.toggleEditorDecoration", "git.refresh", "toggle.diff.renderSideBySide", "scm.moreActions",
  "editor.action.goToReferences", "editor.action.goToImplementation", "editor.action.goToTypeDefinition",
  "editor.action.peekDefinition", "workbench.action.showAllSymbols", "editor.action.transformToUppercase",
  "editor.action.transformToLowercase", "editor.action.transformToTitlecase",
  "editor.action.sortLinesAscending", "editor.action.sortLinesDescending", "editor.action.joinLines",
  "editor.action.trimTrailingWhitespace", "editor.action.removeDuplicateLines",
  "editor.action.duplicateSelection", "editor.action.jumpToBracket", "editor.action.selectToBracket",
  "editor.action.triggerParameterHints", "workbench.action.reopenClosedEditor",
  "workbench.action.editor.changeLanguageMode", "workbench.action.editor.changeEOL",
  "workbench.action.editor.changeIndentation", "editor.action.indentUsingSpaces",
  "editor.action.indentUsingTabs", "editor.action.indentationToSpaces", "editor.action.indentationToTabs",
  "editor.action.detectIndentation", "editor.action.toggleRenderWhitespace",
  "editor.action.toggleStickyScroll", "editor.action.toggleColumnSelection",
  "workbench.action.openSettingsJson", "workbench.action.showWelcomePage", "notifications.showList",
  "explorer.newFile", "explorer.newFolder", "workbench.files.action.refreshFilesExplorer",
  "workbench.files.action.collapseExplorerFolders", "copyFilePath", "copyRelativeFilePath",
  "revealFileInOS", "openInTerminal",
  "workbench.view.debug", "workbench.action.debug.start", "workbench.action.debug.run",
  "workbench.action.debug.stop", "workbench.action.debug.restart", "workbench.action.debug.stepOver",
  "workbench.action.debug.stepInto", "workbench.action.debug.stepOut", "workbench.action.debug.pause",
  "editor.debug.action.toggleBreakpoint", "workbench.debug.action.toggleRepl",
  "workbench.action.debug.configure", "workbench.action.debug.selectandstart",
  "workbench.action.tasks.runTask", "workbench.action.tasks.build", "workbench.action.tasks.configureTaskRunner",
  "workbench.action.terminal.split", "workbench.action.terminal.focusPreviousPane",
  "workbench.action.terminal.focusNextPane", "workbench.action.terminal.focusFind",
  "workbench.action.terminal.clear", "workbench.action.terminal.rename",
  "workbench.action.terminal.selectDefaultShell", "workbench.action.terminal.newWithProfile",
  "workbench.action.output.toggleOutput", "workbench.action.showOutputChannels",
  "workbench.output.action.clearOutput", "workbench.action.toggleMaximizedPanel",
  "workbench.action.replaceInFiles", "workbench.action.closeOtherEditors",
  "workbench.action.closeEditorsToTheRight", "workbench.action.closeUnmodifiedEditors",
  "workbench.action.closeEditorsInGroup", "workbench.action.closeAllEditors", "workbench.action.pinEditor",
  "workbench.action.unpinEditor", "workbench.action.keepEditor",
  "workbench.files.action.showActiveFileInExplorer",
  "workbench.action.quickOpenPreviousRecentlyUsedEditorInGroup",
  "workbench.action.showAllEditorsByMostRecentlyUsed", "workbench.action.localHistory.restoreViaPicker",
  "markdown.showPreview", "markdown.showPreviewToSide", "workbench.files.action.focusOpenEditorsView",
  "timeline.focus", "workbench.action.localHistory.compareWithFile", "breadcrumbs.focusAndSelect",
  "workbench.action.toggleAutoSave", "workbench.files.action.compareWithSaved",
  "workbench.files.action.compareFileWith", "workbench.files.action.compareWithClipboard",
  "selectForCompare", "compareFiles", "workbench.action.editor.changeEncoding",
  "workbench.action.files.revert", "workbench.action.toggleSidebarPosition",
  "type", "workbench.action.terminal.sendSequence", "mme.importVSCodeSettings",
  "workbench.action.openWorkspace", "workbench.action.addRootFolder", "workbench.action.saveWorkspaceAs",
  "workbench.action.closeFolder",
  "workbench.action.splitEditorDown", "workbench.action.splitEditorLeft", "workbench.action.splitEditorUp",
  "workbench.action.focusThirdEditorGroup", "workbench.action.focusFourthEditorGroup",
  "workbench.action.focusLeftGroup", "workbench.action.focusRightGroup", "workbench.action.focusAboveGroup",
  "workbench.action.focusBelowGroup", "workbench.action.moveEditorToNextGroup",
  "workbench.action.moveEditorToPreviousGroup", "workbench.action.joinTwoGroups",
  "workbench.action.joinAllGroups", "workbench.action.toggleEditorWidths", "workbench.action.evenEditorWidths",
  "workbench.action.editorLayoutSingle", "workbench.action.editorLayoutTwoColumns",
  "workbench.action.editorLayoutThreeColumns", "workbench.action.editorLayoutTwoRows",
  "workbench.action.editorLayoutTwoByTwoGrid", "workbench.action.toggleCenteredLayout",
  "workbench.action.toggleMenuBar", "workbench.action.toggleStatusbarVisibility",
  "workbench.action.toggleActivityBarVisibility", "mme.editorLayout", "mme.appearance",
  "editor.action.formatSelection", "editor.action.organizeImports", "editor.action.sourceAction",
  "editor.action.smartSelect.expand", "editor.action.smartSelect.shrink", "editor.foldAllMarkerRegions",
  "editor.unfoldAllMarkerRegions", "editor.foldAllBlockComments", "editor.foldLevel1", "editor.foldLevel2",
  "editor.foldLevel3", "editor.foldLevel4", "editor.foldLevel5", "editor.foldLevel6", "editor.foldLevel7",
  "references-view.showCallHierarchy", "references-view.showOutgoingCalls", "editor.showSupertypes",
  "editor.showSubtypes",
  "editor.action.dirtydiff.next", "editor.action.dirtydiff.previous", "workbench.action.editor.nextChange",
  "workbench.action.editor.previousChange", "git.stageChange", "git.revertChange", "git.stageSelectedRanges",
  "git.unstageSelectedRanges", "git.revertSelectedRanges", "merge-conflict.accept.current",
  "merge-conflict.accept.incoming", "merge-conflict.accept.both", "merge-conflict.accept.all-current",
  "merge-conflict.accept.all-incoming", "merge-conflict.accept.all-both", "merge-conflict.next",
  "merge-conflict.previous", "merge-conflict.compare", "git.clone", "git.init", "git.publish",
  "workbench.view.testing.focus", "testing.runAll", "testing.runAtCursor", "testing.runCurrentFile",
  "testing.reRunLastRun", "testing.debugAtCursor", "testing.refreshTests", "testing.showMostRecentOutput",
  "testing.cancelRun", "testing.collapseAll", "problems.action.focusFilter", "problems.action.collapseAll",
  "problems.action.toggleActiveFile", "problems.action.copyMessage", "outline.toggleFollowCursor",
  "outline.sortBy", "outline.collapse",
  "editor.debug.action.conditionalBreakpoint", "editor.debug.action.addLogPoint",
  "editor.debug.action.editBreakpoint", "editor.debug.action.toggleEnableBreakpoint",
  "workbench.debug.viewlet.action.removeAllBreakpoints", "workbench.debug.viewlet.action.enableAllBreakpoints",
  "workbench.debug.viewlet.action.disableAllBreakpoints", "editor.debug.action.runToCursor",
  "debug.jumpToCursor", "editor.debug.action.selectionToWatch",
  "editor.action.insertCursorAtEndOfEachLineSelected", "cursorUndo", "cursorRedo",
  "editor.action.moveSelectionToNextFindMatch", "editor.action.changeAll", "editor.action.selectAllMatches",
  "editor.action.reindentlines", "editor.action.reindentselectedlines", "deleteAllLeft", "deleteAllRight",
  "editor.action.transposeLetters", "cursorWordPartLeft", "cursorWordPartRight", "deleteWordPartLeft",
  "deleteWordPartRight", "editor.action.toggleTabFocusMode", "editor.action.addCommentLine",
  "editor.action.removeCommentLine", "scrollPageUp", "scrollPageDown",
  "workbench.action.terminal.runSelectedText", "workbench.action.terminal.runActiveFile",
  "workbench.action.terminal.focus", "workbench.action.terminal.scrollToPreviousCommand",
  "workbench.action.terminal.scrollToNextCommand", "workbench.action.terminal.selectAll",
  "workbench.action.terminal.copySelection", "workbench.action.terminal.paste",
  "workbench.action.terminal.runRecentCommand", "workbench.action.terminal.goToRecentDirectory",
  "workbench.action.terminal.changeColor", "workbench.action.terminal.changeIcon",
  "editor.action.inspectTMScopes",
  "explorer.openToSide", "filesExplorer.findInFolder",
  "workbench.action.keybindingsReference", "workbench.action.openTipsAndTricks", "workbench.action.showCommands.all",
  "toggle.diff.ignoreTrimWhitespace", "diffEditor.toggleCollapseUnchangedRegions",
  "workbench.action.zoomIn", "workbench.action.zoomOut", "workbench.action.zoomReset",
  "mme.restartLanguageServer", "mme.showLanguageStatus", "notifications.focusToasts",
  "notification.acceptPrimaryAction", "notifications.clearAll",
  "mme.manage", "workbench.action.positionPanelRight", "workbench.action.positionPanelLeft",
  "workbench.action.positionPanelBottom", "workbench.action.alignPanelCenter", "workbench.action.alignPanelJustify",
  "workbench.action.toggleCommandCenter",
  "workbench.action.reopenWithHexEditor", "imagePreview.zoomIn", "imagePreview.zoomOut",
  "imagePreview.resetZoom",
  "git.openMergeEditor", "mergeEditor.acceptMerge", "codelens.showLensesInCurrentLine",
  "editor.action.inlineSuggest.trigger", "editor.action.inlineSuggest.commit",
  "editor.action.inlineSuggest.acceptNextWord", "editor.action.inlineSuggest.hide",
  "editor.action.inlineSuggest.showNext", "editor.action.inlineSuggest.showPrevious",
  "github.copilot.signIn", "github.copilot.signOut", "github.copilot.status",
  "editor.action.inlineSuggest.jump", "mme.toggleNextEditSuggestions",
  "editor.action.showOrFocusStandaloneColorPicker",
  "workbench.action.chat.open", "workbench.action.chat.newChat", "workbench.action.chat.clear",
  "workbench.action.toggleAuxiliaryBar", "workbench.action.chat.cancel", "mme.chat.toggleImplicitContext",
  "mme.chat.setApiKey", "inlineChat.start", "inlineChat.acceptChanges", "inlineChat.discard",
  "search.action.openNewEditor", "search.action.openInEditor", "rerunSearchEditorSearch",
  "toggleSearchEditorContextLines", "increaseSearchEditorContextLines", "decreaseSearchEditorContextLines",
  "search.searchEditor.action.focusQueryEditorWidget", "search.searchEditor.action.deleteFileResults",
  "toggleVim", "workbench.action.files.saveAll", "git.viewChanges", "git.viewStagedChanges",
  "testing.coverageAll", "testing.coverageCurrentFile", "testing.coverageAtCursor",
  "testing.coverage.close", "testing.toggleInlineCoverage",
  "workbench.profiles.actions.switchProfile", "workbench.profiles.actions.createProfile",
  "workbench.profiles.actions.renameProfile", "workbench.profiles.actions.deleteProfile",
  "git.createWorktree", "git.openWorktree", "git.deleteWorktree",
  "workbench.trust.manage", "workbench.action.toggleScreencastMode",
  "workbench.action.newWindow", "workbench.action.duplicateWorkspaceInNewWindow", "workbench.action.closeWindow",
  "notebook.execute", "jupyter.restartkernel", "jupyter.interruptkernel", "notebook.clearAllOutputs",
  "ipynb.newUntitledIpynb",
  "github.pullRequests.list", "github.issues.list", "pr.create", "issue.createIssue", "github.signIn", "github.signOut",
  "opensshremotes.openEmptyWindow",
  "workbench.action.chat.changeModel", "workbench.action.chat.toggleAgentMode",
  "workbench.userDataSync.actions.turnOn", "workbench.userDataSync.actions.turnOff",
  "workbench.userDataSync.actions.syncNow", "workbench.userDataSync.actions.showSyncedData",
  "remote.tunnel.forwardCommandPalette", "~remote.forwardedPorts.focus",
  "workbench.action.moveEditorToNewWindow", "workbench.action.copyEditorToNewWindow",
  "editor.action.accessibleView", "editor.action.accessibilityHelp"
};

static char *user_keys[CMD_N];	/* keybindings.json's, over keys[] */


const char *cmd_name (int cmd) {
  if (cmd >= CMD_N) return ehost_cmd_title(cmd);	/* an extension's */
  return (cmd > 0 && cmd < CMD_N) ? names[cmd] : "";
}


const char *cmd_keys (int cmd) {
  if (cmd <= 0 || cmd >= CMD_N) return "";
  return user_keys[cmd] ? user_keys[cmd] : keys[cmd];
}


const char *cmd_default_keys (int cmd) {
  return (cmd > 0 && cmd < CMD_N) ? keys[cmd] : "";
}


void cmd_set_keys (int cmd, const char *k) {
  if (cmd <= 0 || cmd >= CMD_N) return;
  free(user_keys[cmd]);
  user_keys[cmd] = k ? xstrdup(k) : NULL;
}


const char *cmd_id (int cmd) {
  if (cmd >= CMD_N) return ehost_cmd_id(cmd);	/* an extension's */
  return (cmd > 0 && cmd < CMD_N) ? ids[cmd] : "";
}


int cmd_by_id (const char *id) {
  int c;
  for (c = 1; c < CMD_N; c++)
    if (strcmp(ids[c], id) == 0) return c;
  return ehost_cmd_by_id(id);	/* an extension's, or CMD_NONE */
}

/* }================================================================== */


/*
** {==================================================================
** The menu bar
** ===================================================================
*/

/* 0 is a line between groups, -1 the end */
static const int m_file[] = {CMD_NEW, CMD_NEW_WINDOW, 0, CMD_OPEN_FILE, CMD_OPEN_FOLDER, CMD_OPEN_WORKSPACE, CMD_OPEN_PROJECT,
                             0, CMD_ADD_FOLDER, CMD_SAVE_WORKSPACE, 0, CMD_SAVE, CMD_SAVE_AS, CMD_SAVE_ALL, 0, CMD_AUTO_SAVE, CMD_REVERT, 0, CMD_SETTINGS, CMD_SETTINGS_JSON, CMD_KEYS, CMD_SNIPPETS, CMD_IMPORT_VSCODE, 0, CMD_CLOSE, CMD_CLOSE_WORKSPACE, CMD_CLOSE_WINDOW, 0, CMD_QUIT, -1};
static const int m_edit[] = {CMD_UNDO, CMD_REDO, 0, CMD_CUT, CMD_COPY, CMD_PASTE,
                             0, CMD_FIND, CMD_REPLACE, CMD_FIND_FILES, 0, CMD_COMMENT, CMD_BLOCK_COMMENT, 0, CMD_UPPER, CMD_LOWER, CMD_SORT_ASC, CMD_JOIN, CMD_TRIM,
                             0, CMD_FORMAT, CMD_FORMAT_SEL, CMD_ORGANIZE_IMPORTS, CMD_SOURCE_ACTION, CMD_INSERT_SNIPPET, 0, CMD_SUGGEST, CMD_QUICKFIX, CMD_RENAME, -1};
static const int m_sel[] = {CMD_SELECT_ALL, CMD_SELECT_LINE, CMD_SELECT_BRACKET, CMD_EXPAND_SEL, CMD_SHRINK_SEL, CMD_CURSORS_LINE_ENDS, 0, CMD_DUP_SEL, CMD_COPY_UP, CMD_COPY_DOWN, CMD_LINE_UP,
                            CMD_LINE_DOWN, CMD_DELETE_LINE, 0,
                            CMD_CURSOR_UP, CMD_CURSOR_DOWN, CMD_NEXT_MATCH, CMD_ALL_MATCHES, 0, CMD_COLUMN_SELECT, -1};
static const int m_view[] = {CMD_PALETTE, 0, CMD_EXPLORER, CMD_SEARCH, CMD_GIT, CMD_EXTENSIONS, CMD_TEST_VIEW,
                             0, CMD_PROBLEMS, CMD_OUTPUT, CMD_DEBUG_CONSOLE, CMD_TERMINAL, CMD_PANEL_MAX, 0, CMD_SPLIT, CMD_SPLIT_DOWN, CMD_LAYOUT_MENU,
                             0, CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_ZOOM_RESET, 0, CMD_SIDEBAR, CMD_SIDEBAR_POS, CMD_MINIMAP, CMD_STICKY, CMD_RENDER_WS, CMD_WORDWRAP, CMD_ZEN,
                             CMD_APPEARANCE_MENU, 0, CMD_THEME, -1};
static const int m_go[] = {CMD_NAV_BACK, CMD_NAV_FORWARD, 0, CMD_QUICK_OPEN, CMD_GOTO_SYMBOL, CMD_WORKSPACE_SYMBOL, CMD_GOTO, CMD_GOTO_BRACKET, 0, CMD_DEFINITION, CMD_TYPE_DEF, CMD_IMPLEMENTATION, CMD_REFERENCES, CMD_CALL_HIERARCHY, CMD_PEEK_DEF, CMD_HOVER, 0, CMD_NEXT_PROBLEM,
                           CMD_PREV_PROBLEM, 0, CMD_NEXT_CHANGE, CMD_PREV_CHANGE, -1};
static const int m_run[] = {CMD_DEBUG_START, CMD_DEBUG_RUN, CMD_DEBUG_STOP, CMD_DEBUG_RESTART, 0,
                            CMD_DEBUG_CONFIG, CMD_DEBUG_SELECT, 0, CMD_DEBUG_STEP_OVER, CMD_DEBUG_STEP_INTO,
                            CMD_DEBUG_STEP_OUT, CMD_DEBUG_PAUSE, CMD_RUN_TO_CURSOR, 0, CMD_BREAKPOINT,
                            CMD_BP_CONDITIONAL, CMD_BP_LOG, CMD_BP_ENABLE_ALL, CMD_BP_DISABLE_ALL, CMD_BP_REMOVE_ALL, 0,
                            CMD_DEBUG_VIEW, CMD_DEBUG_CONSOLE, -1};
static const int m_term[] = {CMD_TERMINAL_NEW, CMD_TERMINAL_SPLIT, CMD_TERMINAL_NEW_PROFILE, 0, CMD_TASK_RUN,
                             CMD_TASK_BUILD, CMD_TERM_RUN_FILE, CMD_TERM_RUN_SEL, 0, CMD_TERMINAL, CMD_TERM_RECENT,
                             CMD_TERMINAL_FIND, CMD_TERMINAL_CLEAR, CMD_TERMINAL_RENAME, CMD_TERMINAL_KILL, 0,
                             CMD_TERMINAL_PROFILE, CMD_TASK_CONFIGURE, -1};
static const int m_help[] = {CMD_WELCOME, CMD_HELP_COMMANDS, 0, CMD_HELP_KEYS, CMD_HELP_TIPS, 0, CMD_NOTIFICATIONS, 0, CMD_ABOUT, -1};

static const struct {
  const char *name;
  const int *cmd;
} menus[] = {
  {"File", m_file}, {"Edit", m_edit}, {"Selection", m_sel},
  {"View", m_view}, {"Go", m_go}, {"Run", m_run}, {"Terminal", m_term}, {"Help", m_help}
};

#define NMENU	((int)(sizeof(menus) / sizeof(menus[0])))


static int title_x (int m) {
  int i, x = 3;	/* after the app's icon */
  for (i = 0; i < m; i++) x += (int)strlen(menus[i].name) + 2;
  return x;
}


int menubar_hit (int x) {
  int i;
  for (i = 0; i < NMENU; i++) {
    int x0 = title_x(i);
    if (x >= x0 && x < x0 + (int)strlen(menus[i].name) + 2) return i;
  }
  return -1;
}


/* the command center: where its arrows and its box were drawn */
static struct {
  int back, fwd, x0, x1;
} g_cc = {-1, -1, -1, -1};


int menubar_cc_hit (int x) {
  if (g_cc.x1 <= g_cc.x0) return 0;
  if (x == g_cc.back) return 1;
  if (x == g_cc.fwd) return 2;
  return x >= g_cc.x0 && x < g_cc.x1 ? 3 : 0;
}


/*
** VS Code's command center (window.commandCenter): in the middle of the
** title bar, "<- ->" and a box with the search icon and the folder's name
** that opens Go to File. 0: no room for it (the title shows instead).
*/
static int cc_draw (int end, int cols) {
  int bw = cols * 2 / 5, bx, tw;
  g_cc.back = g_cc.fwd = g_cc.x0 = g_cc.x1 = -1;
  if (!opt.command_center || ui_cc[0] == '\0') return 0;
  if (bw > 60) bw = 60;
  tw = (int)str_cols(ui_cc) + 4;
  if (bw < tw) bw = tw;
  if (bw > cols - end - 9) bw = cols - end - 9;	/* the room right of the menus */
  if (bw < 16) return 0;
  bx = (cols - bw) / 2;	/* in the middle, else just right of the menus */
  if (bx - 6 <= end) bx = end + 6;
  if (bx + bw >= cols - 1) return 0;
  g_cc.back = bx - 5;
  g_cc.fwd = bx - 3;
  g_cc.x0 = bx;
  g_cc.x1 = bx + bw;
  scr_put(g_cc.back, 0, 0xEA9B, S_MENUBAR);	/* codicon arrow-left: Go Back */
  scr_put(g_cc.fwd, 0, 0xEA9C, S_MENUBAR);	/* arrow-right: Go Forward */
  scr_fill(bx, 0, bw, S_INPUT);
  tw = (int)str_cols(ui_cc) + 2;	/* the icon, a space, the name: in the middle of the box */
  if (tw > bw - 2) tw = bw - 2;
  scr_put(bx + (bw - tw) / 2, 0, 0xEA6D, S_INPUT);	/* codicon search */
  scr_putsw(bx + (bw - tw) / 2 + 2, 0, tw - 2, ui_cc, S_INPUT);
  return 1;
}


void menubar_draw (int open) {
  int i, end = title_x(NMENU), cols = scr_cols(), w;
  scr_fill(0, 0, cols, S_MENUBAR);
  scr_put(1, 0, 0xF121, S_APPICON);	/* the app's icon: </> */
  for (i = 0; i < NMENU; i++) {
    int x = title_x(i), st = (i == open) ? S_MENUBAR_ON : S_MENUBAR;
    scr_fill(x, 0, (int)strlen(menus[i].name) + 2, st);
    scr_puts(x + 1, 0, menus[i].name, st);
  }
  if (cc_draw(end, cols)) return;	/* the command center takes the title's place */
  w = (int)str_cols(ui_title);	/* the title, in the middle when there is room */
  if (w > 0 && w < cols - 2 * end) scr_puts((cols - w) / 2, 0, ui_title, S_MENUBAR);
  else if (w > 0 && end + 2 + w < cols) scr_puts(cols - w - 1, 0, ui_title, S_MENUBAR);
}


static struct {
  int x, y, w, h;	/* the open menu on the screen */
  int top, vis;	/* its first item shown, how many show (a window too low for all of them) */
} g_drop;


/*
** A menu taller than the window, VS Code's way: as many of its rows as fit
** show, from row y (moved up when it can) to the row 'bottom' (excluded);
** the rest scroll. The rows it shows.
*/
static int menu_place (int n, int *y, int ymin, int bottom) {
  int h = n + 2;
  if (h > bottom - ymin) h = bottom - ymin;
  if (h < 3) h = 3;
  if (*y + h > bottom) *y = bottom - h;
  if (*y < ymin) *y = ymin;
  return h - 2;
}


/* the first item shown: follow keeps the selected one in view (the keys), else the wheel's place stays */
static void menu_scroll (int n, int vis, int sel, int follow, int *top) {
  if (follow && sel >= 0) {
    if (sel < *top) *top = sel;
    if (sel >= *top + vis) *top = sel - vis + 1;
  }
  if (*top > n - vis) *top = n - vis;
  if (*top < 0) *top = 0;
}


/* a slim scrollbar in the menu's right edge when not every item shows */
static void menu_bar (int x, int y, int w, int n, int vis, int top) {
  int len, at, i;
  if (vis >= n || vis < 1) return;
  len = vis * vis / n;
  if (len < 1) len = 1;
  at = top * (vis - len) / (n - vis);
  for (i = 0; i < len; i++)	/* a half block, like the panes' */
    scr_put_rgb(x + w - 1, y + 1 + at + i, 0x2590, ui_color(C_THUMB), ui_color(C_MENU_BG), 0);
}


static int count (const int *cmd) {
  int n = 0;
  while (cmd[n] >= 0) n++;
  return n;
}


static void drop_draw (int m, int sel, int follow) {
  const int *cmd = menus[m].cmd;
  int n = count(cmd), i, w = 24;
  for (i = 0; i < n; i++) {
    int need = (int)strlen(names[cmd[i]]) + (int)strlen(keys[cmd[i]]) + 8;
    if (cmd[i] && need > w) w = need;
  }
  g_drop.x = title_x(m);
  if (g_drop.x + w > scr_cols()) g_drop.x = scr_cols() - w;
  if (g_drop.x < 0) g_drop.x = 0;
  g_drop.y = 1;
  g_drop.w = w;
  g_drop.vis = menu_place(n, &g_drop.y, 1, scr_rows());
  g_drop.h = g_drop.vis + 2;
  menu_scroll(n, g_drop.vis, sel, follow, &g_drop.top);
  scr_box(g_drop.x, g_drop.y, w, g_drop.h, S_MENU);
  menu_bar(g_drop.x, g_drop.y, w, n, g_drop.vis, g_drop.top);
  for (i = g_drop.top; i < n && i < g_drop.top + g_drop.vis; i++) {
    int y = g_drop.y + 1 + i - g_drop.top, on = (i == sel);
    if (cmd[i] == 0) {
      int x;
      for (x = g_drop.x + 1; x < g_drop.x + w - 1; x++) scr_put(x, y, 0x2500, S_MENU_LINE);
      continue;
    }
    scr_fill(g_drop.x + 1, y, w - 2, on ? S_MENU_SEL : S_MENU);
    if (cmd_checked(cmd[i])) scr_put(g_drop.x + 1, y, 0xEAB2, on ? S_MENU_SEL : S_MENU);	/* codicon check */
    scr_puts(g_drop.x + 3, y, names[cmd[i]], on ? S_MENU_SEL : S_MENU);
    scr_puts(g_drop.x + w - 3 - (int)strlen(keys[cmd[i]]), y, keys[cmd[i]],
             on ? S_MENU_KEY_SEL : S_MENU_KEY);
  }
}


/* the next item that is not a line, from sel in direction d */
static int step (const int *cmd, int sel, int d) {
  int n = count(cmd), i;
  for (i = 0; i < n; i++) {
    sel = (sel + d + n) % n;
    if (cmd[sel] != 0) return sel;
  }
  return sel;
}


/*
** A menu at x, y (a right-click's): cmd lists CMD_s, 0 a line, -1 the
** end; label[i] (when not NULL) is what item i says instead of its
** command's name. The command picked, CMD_NONE for Esc or a click outside.
*/
/* the next item of a popup_list that can be picked, from sel in direction d; -1: none */
static int pl_step (const int *flags, int n, int sel, int d) {
  int i, k = sel;
  for (i = 0; i < n; i++) {
    k = k < 0 ? (d > 0 ? 0 : n - 1) : (k + d + n) % n;
    if (!(flags[k] & (MF_OFF | MF_LINE))) return k;
  }
  return -1;
}


int popup_width (const char *const *label, const int *flags, int n) {
  int i, w = 20;
  for (i = 0; i < n; i++)
    if (!(flags[i] & MF_LINE) && (int)str_cols(label[i]) + 8 > w) w = (int)str_cols(label[i]) + 8;
  return w;
}


/*
** A little menu of labels, like VS Code's context menus: a check before an
** item (MF_CHECK), dim ones (MF_OFF) cannot be picked, MF_LINE is a line,
** MF_SUB has the submenu's arrow (Right picks it too). The index picked, -1.
*/
int popup_list (int x0, int y0, const char *const *label, const int *flags, int n) {
  int w = popup_width(label, flags, n), h, sel = pl_step(flags, n, -1, 1), i, x, y, vis, top = 0, follow = 1;
  int said = -1;
  for (;;) {
    int k, code;
    if (sel >= 0 && sel != said) {	/* a screen reader's */
      acc_sayf("%s%s%s", label[sel], flags[sel] & MF_CHECK ? ", checked" : "", flags[sel] & MF_SUB ? ", submenu" : "");
      said = sel;
    }
    ui_background();
    x = x0;	/* placed again every time: the window may have changed its size */
    y = y0;
    if (x + w > scr_cols()) x = scr_cols() - w;
    if (x < 0) x = 0;
    vis = menu_place(n, &y, 0, scr_rows());
    h = vis + 2;
    menu_scroll(n, vis, sel, follow, &top);
    follow = 0;
    scr_box(x, y, w, h, S_MENU);
    menu_bar(x, y, w, n, vis, top);
    for (i = top; i < n && i < top + vis; i++) {
      int ry = y + 1 + i - top, on = (i == sel), cx, st = flags[i] & MF_OFF ? S_MENU_KEY : on ? S_MENU_SEL : S_MENU;
      if (flags[i] & MF_LINE) {
        for (cx = x + 1; cx < x + w - 1; cx++) scr_put(cx, ry, 0x2500, S_MENU_LINE);
        continue;
      }
      scr_fill(x + 1, ry, w - 2, on ? S_MENU_SEL : S_MENU);
      if (flags[i] & MF_CHECK) scr_put(x + 1, ry, 0xEAB2, st);	/* codicon check */
      scr_putsw(x + 3, ry, w - 6, label[i], st);
      if (flags[i] & MF_SUB) scr_put(x + w - 3, ry, 0xEAB6, st);	/* chevron-right */
    }
    scr_cursor(0, -1);
    scr_flush();
    k = term_key(200);
    if (k == K_NONE) continue;
    code = KEY_CODE(k);
    if (code == K_ESC || code == K_LEFT) return -1;
    if (code == K_UP && sel >= 0) sel = pl_step(flags, n, sel, -1), follow = 1;
    else if (code == K_DOWN && sel >= 0) sel = pl_step(flags, n, sel, 1), follow = 1;
    else if ((code == K_ENTER || code == ' ') && sel >= 0) return sel;
    else if (code == K_RIGHT && sel >= 0 && (flags[sel] & MF_SUB)) return sel;
    else if (code == K_MOUSE) {
      Mouse *mo = &term_mouse;
      int inside = mo->x >= x && mo->x < x + w && mo->y > y && mo->y < y + h - 1;
      if (mo->wheel) {	/* a menu too tall for the window scrolls */
        top += mo->wheel * 3;
        continue;
      }
      if (inside) {
        int it = top + mo->y - y - 1;
        if (flags[it] & (MF_OFF | MF_LINE)) continue;
        sel = it;
        if (mo->button == 0 && !mo->press && !mo->drag) return it;	/* the button came up here */
      }
      else if (mo->press && !mo->drag) return -1;	/* a click outside closes */
    }
  }
}


int menu_popup (int x0, int y0, const int *cmd, const char *const *label) {
  int n = count(cmd), i, w = 20, h, sel = step(cmd, -1, 1), x, y, vis, top = 0, follow = 1;
  for (i = 0; i < n; i++) {
    const char *s = label && label[i] ? label[i] : names[cmd[i]];
    int need = (int)str_cols(s) + (int)strlen(keys[cmd[i]]) + 8;
    if (cmd[i] && need > w) w = need;
  }
  int said = -1;
  for (;;) {
    int k, code;
    if (sel >= 0 && sel != said) {	/* a screen reader's */
      acc_sayf("%s%s%s", label && label[sel] ? label[sel] : names[cmd[sel]], *keys[cmd[sel]] ? ", " : "", keys[cmd[sel]]);
      said = sel;
    }
    ui_background();
    x = x0;	/* placed again every time: the window may have changed its size */
    y = y0;
    if (x + w > scr_cols()) x = scr_cols() - w;
    if (x < 0) x = 0;
    vis = menu_place(n, &y, 0, scr_rows());
    h = vis + 2;
    menu_scroll(n, vis, sel, follow, &top);
    follow = 0;
    scr_box(x, y, w, h, S_MENU);
    menu_bar(x, y, w, n, vis, top);
    for (i = top; i < n && i < top + vis; i++) {
      int ry = y + 1 + i - top, on = (i == sel), cx;
      const char *s = label && label[i] ? label[i] : names[cmd[i]];
      if (cmd[i] == 0) {
        for (cx = x + 1; cx < x + w - 1; cx++) scr_put(cx, ry, 0x2500, S_MENU_LINE);
        continue;
      }
      scr_fill(x + 1, ry, w - 2, on ? S_MENU_SEL : S_MENU);
      scr_puts(x + 3, ry, s, on ? S_MENU_SEL : S_MENU);
      scr_puts(x + w - 3 - (int)strlen(keys[cmd[i]]), ry, keys[cmd[i]], on ? S_MENU_KEY_SEL : S_MENU_KEY);
    }
    scr_cursor(0, -1);
    scr_pointer(PTR_POINTER);	/* every row of a menu, a picker or a dialog is clickable */
    scr_flush();
    k = term_key(200);
    if (k == K_NONE) continue;
    code = KEY_CODE(k);
    if (code == K_ESC) return CMD_NONE;
    if (code == K_UP) sel = step(cmd, sel, -1), follow = 1;
    else if (code == K_DOWN) sel = step(cmd, sel, 1), follow = 1;
    else if (code == K_ENTER || code == ' ') return cmd[sel];
    else if (code == K_MOUSE) {
      Mouse *mo = &term_mouse;
      int inside = mo->x >= x && mo->x < x + w && mo->y > y && mo->y < y + h - 1;
      if (mo->wheel) {	/* a menu too tall for the window scrolls */
        top += mo->wheel * 3;
        continue;
      }
      if (inside) {
        int it = top + mo->y - y - 1;
        if (cmd[it] == 0) continue;
        sel = it;
        if (mo->button == 0 && !mo->press && !mo->drag) return cmd[it];	/* the button came up here */
      }
      else if (mo->press && !mo->drag) return CMD_NONE;	/* a click outside closes */
    }
  }
}


int menu_run (int m) {
  int sel = step(menus[m].cmd, -1, 1), said_m = -1, said = -1, follow = 1;
  g_drop.top = 0;
  for (;;) {
    const int *cmd = menus[m].cmd;
    int k, code;
    if (sel >= 0 && (m != said_m || sel != said)) {	/* a screen reader's: the menu, the item and its keys */
      const char *kl = user_keys[cmd[sel]] ? user_keys[cmd[sel]] : keys[cmd[sel]];
      acc_sayf("%s%s%s%s%s", m != said_m ? menus[m].name : "", m != said_m ? " menu, " : "", names[cmd[sel]],
               kl && *kl ? ", " : "", kl ? kl : "");
      said_m = m;
      said = sel;
    }
    ui_background();
    menubar_draw(m);
    drop_draw(m, sel, follow);
    follow = 0;
    scr_cursor(0, -1);
    scr_pointer(PTR_POINTER);	/* every row of a menu, a picker or a dialog is clickable */
    scr_flush();
    k = term_key(200);
    if (k == K_NONE) continue;
    code = KEY_CODE(k);
    if (code == K_ESC || code == K_F10) return CMD_NONE;
    if (code == K_UP) sel = step(cmd, sel, -1), follow = 1;
    else if (code == K_DOWN) sel = step(cmd, sel, 1), follow = 1;
    else if (code == K_LEFT || code == K_RIGHT) {
      m = (m + (code == K_LEFT ? NMENU - 1 : 1)) % NMENU;
      sel = step(menus[m].cmd, -1, 1);
      g_drop.top = 0;
      follow = 1;
    }
    else if (code == K_ENTER || code == ' ') return cmd[sel];
    else if (code == K_MOUSE) {
      Mouse *mo = &term_mouse;
      int inside = mo->x >= g_drop.x && mo->x < g_drop.x + g_drop.w &&
                   mo->y > g_drop.y && mo->y < g_drop.y + g_drop.h - 1;
      if (mo->wheel) {	/* a menu too tall for the window scrolls */
        g_drop.top += mo->wheel * 3;
        continue;
      }
      if (mo->button != 0) continue;
      if (inside) {
        int i = g_drop.top + mo->y - g_drop.y - 1;
        if (cmd[i] == 0) continue;
        sel = i;
        if (!mo->press && !mo->drag) return cmd[i];	/* the button came up here */
      }
      else if (mo->y == 0 && mo->press && !mo->drag) {
        int hit = menubar_hit(mo->x);
        if (hit < 0 || hit == m) return CMD_NONE;
        m = hit;
        sel = step(menus[m].cmd, -1, 1);
        g_drop.top = 0;
        follow = 1;
      }
      else if (mo->press && !mo->drag) return CMD_NONE;	/* a click outside closes */
    }
  }
}

/*
** A context menu (the right button): at x, y (or as near as it fits);
** label[i] NULL is a line between groups, keys[] may be NULL. The item
** picked, or -1.
*/
int context_menu (int x0, int y0, const char *const *label, const char *const *keys, int n) {
  int sel = -1, i, w = 20, h, x, y, vis, top = 0, follow = 1;
  for (i = n - 1; i >= 0; i--)	/* the first item selected, for the keys */
    if (label[i]) sel = i;
  for (i = 0; i < n; i++)
    if (label[i]) {
      int need = (int)str_cols(label[i]) + (keys && keys[i] ? (int)strlen(keys[i]) : 0) + 8;
      if (need > w) w = need;
    }
  int said = -1;
  for (;;) {
    int k, code;
    if (sel >= 0 && sel != said && label[sel]) {	/* a screen reader's */
      acc_sayf("%s%s%s", label[sel], keys && keys[sel] && keys[sel][0] ? ", " : "", keys && keys[sel] ? keys[sel] : "");
      said = sel;
    }
    ui_background();
    x = x0;	/* placed again every time: the window may have changed its size */
    y = y0;
    if (x + w > scr_cols()) x = scr_cols() - w;
    if (x < 0) x = 0;
    vis = menu_place(n, &y, 1, scr_rows() - 1);	/* over the status bar only when it must */
    h = vis + 2;
    menu_scroll(n, vis, sel, follow, &top);
    follow = 0;
    scr_box(x, y, w, h, S_MENU);
    menu_bar(x, y, w, n, vis, top);
    for (i = top; i < n && i < top + vis; i++) {
      int ry = y + 1 + i - top, on = i == sel;
      if (label[i] == NULL) {
        int cx;
        for (cx = x + 1; cx < x + w - 1; cx++) scr_put(cx, ry, 0x2500, S_MENU_LINE);
        continue;
      }
      scr_fill(x + 1, ry, w - 2, on ? S_MENU_SEL : S_MENU);
      scr_puts(x + 3, ry, label[i], on ? S_MENU_SEL : S_MENU);
      if (keys && keys[i] && keys[i][0])
        scr_puts(x + w - 3 - (int)strlen(keys[i]), ry, keys[i], on ? S_MENU_KEY_SEL : S_MENU_KEY);
    }
    scr_cursor(0, -1);
    scr_pointer(PTR_POINTER);	/* every row of a menu, a picker or a dialog is clickable */
    scr_flush();
    k = term_key(200);
    if (k == K_NONE) continue;
    code = KEY_CODE(k);
    if (code == K_ESC) return -1;
    if (code == K_UP || code == K_DOWN) {
      int d = code == K_UP ? -1 : 1, t;
      for (t = 0; t < n; t++) {
        sel = sel < 0 ? (d > 0 ? 0 : n - 1) : (sel + d + n) % n;
        if (label[sel]) break;
      }
      follow = 1;
    }
    else if ((code == K_ENTER || code == ' ') && sel >= 0) return sel;
    else if (code == K_MOUSE) {
      Mouse *m = &term_mouse;
      int inside = m->x >= x && m->x < x + w && m->y > y && m->y < y + h - 1;
      if (m->wheel) {	/* a menu too tall for the window scrolls */
        top += m->wheel * 3;
        continue;
      }
      if (inside) {
        int at = top + m->y - y - 1;
        if (label[at] == NULL) continue;
        sel = at;
        if (!m->press && !m->drag && m->button != 3) return at;	/* let go here */
      }
      else if (m->button == 3) sel = -1;	/* moving outside */
      else if (m->press && !m->drag) return -1;	/* a click outside */
    }
  }
}

/* }================================================================== */


/*
** {==================================================================
** The quick input box
** ===================================================================
*/

void pick_init (Pick *p, const char *title) {
  memset(p, 0, sizeof(*p));
  p->title = title;
}


void pick_add (Pick *p, const char *label, const char *detail, int icon) {
  PickItem *it;
  if (p->n == p->cap) {
    p->cap = p->cap ? p->cap * 2 : 32;
    p->item = (PickItem *)xrealloc(p->item, p->cap * sizeof(PickItem));
  }
  it = &p->item[p->n++];
  it->label = xstrdup(label);
  it->detail = detail ? xstrdup(detail) : NULL;
  it->icon = icon;
  it->group = p->group;
}


void pick_clear (Pick *p) {
  size_t i;
  for (i = 0; i < p->n; i++) {
    free(p->item[i].label);
    free(p->item[i].detail);
  }
  p->n = 0;
}


void pick_free (Pick *p) {
  pick_clear(p);
  free(p->item);
  p->item = NULL;
  p->cap = 0;
}


static int lower (int c) {
  return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}


/*
** {==================================================================
** Fuzzy matching, like VS Code's quick input
** ===================================================================
*/

#define FZ_MAXN	512	/* longer texts: only their first bytes are scored */
#define FZ_MAXM	64
#define FZ_NONE	(-1000000)

static int fz_is_sep (int c) {
  return c == '/' || c == '\\' || c == '_' || c == '-' || c == '.' || c == ' ' || c == ':';
}


static int fz_is_alnum (int c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80;
}


/* what matching s[i] is worth by itself: more at the start and at word starts */
static int fz_char (const char *s, size_t i, int pc) {
  int c = (unsigned char)s[i], v = 1, prev;
  if (c == pc) v++;	/* the same case */
  if (i == 0) return v + 8;
  prev = (unsigned char)s[i - 1];
  if (fz_is_sep(prev)) return v + 7;	/* "src/main.c": m */
  if (prev >= 'a' && prev <= 'z' && c >= 'A' && c <= 'Z') return v + 6;	/* camelCase */
  if (!fz_is_alnum(prev) && fz_is_alnum(c)) return v + 5;
  return v;
}


/*
** The best alignment of pat in s[0..n) (the letters in order, not always
** side by side), scored as VS Code does in spirit: letters side by side and
** at word starts are worth more. FZ_NONE: no match. hit (n bytes, or NULL)
** gets the matched bytes.
*/
static int fz_score (const char *s, size_t n, const char *pat, size_t m, char *hit) {
  static int *M;	/* M[j * n + i]: the best with pat[j] at s[i] */
  static size_t cap;
  size_t i, j, k;
  int best = FZ_NONE;
  size_t bi = 0;
  if (n > FZ_MAXN) n = FZ_MAXN;
  if (hit) memset(hit, 0, n);
  if (m == 0) return 0;
  if (m > FZ_MAXM || m > n) return FZ_NONE;
  for (i = 0, j = 0; i < n && j < m; i++)	/* the letters in order at all? (fast) */
    if (lower((unsigned char)s[i]) == lower((unsigned char)pat[j])) j++;
  if (j < m) return FZ_NONE;
  if (cap < n * m) {
    cap = n * m;
    M = (int *)xrealloc(M, cap * sizeof(int));
  }
  for (j = 0; j < m; j++) {
    int run = FZ_NONE;	/* the best of the row above, left of i - 1 */
    int pc = (unsigned char)pat[j], lp = lower(pc);
    for (i = 0; i < n; i++) {
      int v = FZ_NONE;
      if (j > 0 && i >= 2 && M[(j - 1) * n + i - 2] > run) run = M[(j - 1) * n + i - 2];
      if (lower((unsigned char)s[i]) == lp) {
        int c = fz_char(s, i, pc);
        if (j == 0) v = c - (int)(i > 8 ? 8 : i);	/* earlier is better */
        else {
          int d = i > 0 ? M[(j - 1) * n + i - 1] : FZ_NONE;
          if (d > FZ_NONE) v = d + c + 5;	/* side by side */
          if (run > FZ_NONE && run + c > v) v = run + c;
        }
      }
      M[j * n + i] = v;
    }
  }
  for (i = 0; i < n; i++)
    if (M[(m - 1) * n + i] > best) {
      best = M[(m - 1) * n + i];
      bi = i;
    }
  if (best == FZ_NONE || hit == NULL) return best;
  hit[bi] = 1;	/* back along the best way */
  for (j = m - 1; j > 0; j--) {
    int c = fz_char(s, bi, (unsigned char)pat[j]), want = M[j * n + bi];
    size_t pick = 0;
    int found = 0;
    if (bi > 0 && M[(j - 1) * n + bi - 1] > FZ_NONE && M[(j - 1) * n + bi - 1] + c + 5 == want) {
      pick = bi - 1;
      found = 1;
    }
    for (k = 0; !found && k + 1 < bi; k++)
      if (M[(j - 1) * n + k] > FZ_NONE && M[(j - 1) * n + k] + c == want) {
        pick = k;
        found = 1;
      }
    if (!found) break;
    bi = pick;
    hit[bi] = 1;
  }
  return best;
}


/*
** What is typed that items are matched with: with p->line_suffix, VS Code's
** "main.c:12" or "main.c:12:5" is main.c, to be opened at line 12 (column
** 5). line, col (or NULL): those, 0 when not said.
*/
size_t pick_text_len (const Pick *p, long *line, long *col) {
  size_t n = strlen(p->text), i = n, c1, c2 = 0;
  long a = 0, b = 0;
  if (line) *line = 0;
  if (col) *col = 0;
  if (!p->line_suffix) return n;
  while (i > 0 && p->text[i - 1] >= '0' && p->text[i - 1] <= '9') i--;
  if (i == 0 || p->text[i - 1] != ':') return n;
  c1 = i - 1;	/* the ':' before the last number */
  if (c1 > 0) {	/* ":12:5": a line and a column */
    size_t j = c1;
    while (j > 0 && p->text[j - 1] >= '0' && p->text[j - 1] <= '9') j--;
    if (j < c1 && j > 0 && p->text[j - 1] == ':') c2 = j - 1;
  }
  if (c2 > 0) {
    a = strtol(p->text + c2 + 1, NULL, 10);
    b = strtol(p->text + c1 + 1, NULL, 10);
    c1 = c2;
  }
  else a = strtol(p->text + c1 + 1, NULL, 10);
  if (c1 == 0) return n;	/* ":12" alone is Go to Line's */
  if (line) *line = a;
  if (col) *col = b;
  return c1;
}


/*
** How well item matches what is typed: each word typed (split by spaces)
** must match. A word matches the label, better, or (items with a path,
** p->match_detail) the whole "detail/label"; a '/' in a word matches the
** whole path only. Higher is better; FZ_NONE: no. hl, hd (or NULL): the
** matched bytes of the label and of the detail.
*/
static int item_score (const Pick *p, const PickItem *it, char *hl, char *hd) {
  char typed[sizeof(p->text)];
  const char *t = typed;
  size_t ln = strlen(it->label), dn = it->detail ? strlen(it->detail) : 0;
  char full[FZ_MAXN + 1], fh[FZ_MAXN];
  size_t fn = 0;
  int total = 0, have_full = 0;
  if (hl) memset(hl, 0, ln + 1);
  if (hd) memset(hd, 0, dn + 1);
  snprintf(typed, sizeof(typed), "%.*s", (int)pick_text_len(p, NULL, NULL), p->text);
  while (*t == ' ') t++;
  if (*t == '\0') return 0;
  if (p->match_detail && it->detail && dn + ln + 1 <= FZ_MAXN) {
    size_t i;
    memcpy(full, it->detail, dn);
    full[dn] = '/';
    memcpy(full + dn + 1, it->label, ln);
    fn = dn + ln + 1;
    full[fn] = '\0';
    for (i = 0; i < fn; i++)
      if (full[i] == '\\') full[i] = '/';
    have_full = 1;
  }
  while (*t) {
    char w[FZ_MAXM + 1];
    size_t m = 0, i;
    int sc, slash = 0;
    while (*t && *t != ' ' && m < FZ_MAXM) {
      w[m] = *t == '\\' ? '/' : *t;
      if (w[m] == '/') slash = 1;
      m++;
      t++;
    }
    while (*t && *t != ' ') t++;	/* too long: the rest of the word is not looked at */
    while (*t == ' ') t++;
    w[m] = '\0';
    if (m == 0) continue;
    sc = slash ? FZ_NONE : fz_score(it->label, ln, w, m, hl ? fh : NULL);
    if (sc > FZ_NONE) {
      total += sc + 100 - (int)(ln > 60 ? 60 : ln) / 4;	/* the name matched: shorter names a little first */
      if (hl)
        for (i = 0; i < ln && i < FZ_MAXN; i++) hl[i] |= fh[i];
      continue;
    }
    if (!have_full || (sc = fz_score(full, fn, w, m, hd || hl ? fh : NULL)) == FZ_NONE) return FZ_NONE;
    total += sc - 20;
    for (i = 0; (hd || hl) && i < fn; i++) {
      if (!fh[i]) continue;
      if (i < dn) {
        if (hd) hd[i] = 1;
      }
      else if (i > dn && hl) hl[i - dn - 1] = 1;
    }
  }
  return total;
}

/* }================================================================== */


typedef struct Vis {
  size_t *v;
  int *score;
  size_t n;
} Vis;

static const int *g_scores;
static const PickItem *g_items;
static int g_by_name;	/* a tie goes by name (Go to File: the order files were found in is the threads') */

static int cmp_vis (const void *a, const void *b) {
  size_t x = *(const size_t *)a, y = *(const size_t *)b;
  int d = g_items[x].group - g_items[y].group;	/* recently opened, then the folder's files */
  if (d) return d;
  d = g_scores[x] < g_scores[y] ? 1 : g_scores[x] > g_scores[y] ? -1 : 0;	/* higher first */
  if (d) return d;
  if (g_by_name && (d = strcmp(g_items[x].label, g_items[y].label)) != 0) return d;	/* a tie: by name, then path */
  if (g_by_name && g_items[x].detail && g_items[y].detail && (d = strcmp(g_items[x].detail, g_items[y].detail)) != 0) return d;
  return x < y ? -1 : x > y;
}


/*
** The items that match, best first. narrow: what is typed only grew, so
** only the ones that matched before can match now (VS Code's quick open
** does the same: a big folder does not score every file at each key).
*/
static void filter (const Pick *p, Vis *vis, int narrow) {
  size_t i, n = narrow ? vis->n : p->n, k = 0;
  int typed = pick_text_len(p, NULL, NULL) > 0;
  for (i = 0; i < n; i++) {
    size_t it = narrow ? vis->v[i] : i;
    int sc = typed ? item_score(p, &p->item[it], NULL, NULL) : 0;
    if (sc == FZ_NONE || (!typed && p->typed_group > 0 && p->item[it].group >= p->typed_group)) continue;
    vis->score[it] = sc;
    vis->v[k++] = it;
  }
  vis->n = k;
  if (!p->keep_order && typed) {
    g_scores = vis->score;
    g_items = p->item;
    g_by_name = p->typed_group > 0;
    qsort(vis->v, vis->n, sizeof(size_t), cmp_vis);
  }
}


static struct {
  int x, y, w, rows;	/* the box on the screen; rows: of the list */
} g_box;


static void pick_draw (const Pick *p, const Vis *vis, size_t sel, size_t top) {
  int cols = scr_cols(), maxrows = scr_rows() - 6, i, x, cx;
  size_t hn = 256;
  char *hit = (char *)xmalloc(hn);
  g_box.w = cols - 4 < 90 ? cols - 4 : 90;
  if (g_box.w < 20) g_box.w = cols;
  g_box.x = (cols - g_box.w) / 2;
  g_box.y = 1;
  if (maxrows > 15) maxrows = 15;
  if (maxrows < 1) maxrows = 1;
  g_box.rows = (int)vis->n < maxrows ? (int)vis->n : maxrows;
  if (vis->n == 0 && p->hint) g_box.rows = 1;
  scr_box(g_box.x, g_box.y, g_box.w, g_box.rows > 0 ? g_box.rows + 4 : 3, S_BOX);
  /* the input */
  scr_fill(g_box.x + 1, g_box.y + 1, g_box.w - 2, S_INPUT);
  x = g_box.x + 2;
  if (p->prefix) {	/* a long path: its end, where the typing goes on */
    const char *s = p->prefix;
    int room = g_box.w / 2;
    if ((int)str_cols(s) > room) {
      while (*s && (int)str_cols(s) > room - 1) s++;
      while (((unsigned char)*s & 0xC0) == 0x80) s++;
      x += scr_put(x, g_box.y + 1, 0x2026, S_INPUT_HINT);
    }
    x += scr_putsw(x, g_box.y + 1, g_box.x + g_box.w - 3 - x, s, S_INPUT);
  }
  {
    int right = g_box.x + g_box.w - 3;	/* where the input ends: before the status */
    if (p->status) {	/* "Indexing... 1200 files", dim at the right end */
      int sw = (int)str_cols(p->status);
      if (sw < g_box.w / 2) {
        scr_putsw(right - sw, g_box.y + 1, sw, p->status, S_INPUT_HINT);
        right -= sw + 2;
      }
    }
    if (p->text[0] == '\0' && p->title && !p->prefix)
      scr_putsw(x, g_box.y + 1, right - x, p->title, S_INPUT_HINT);
    cx = x + scr_putsw(x, g_box.y + 1, right - x, p->text, S_INPUT_ON);
  }
  scr_cursor(cx, g_box.y + 1);
  /* the list */
  if (vis->n == 0 && p->hint)
    scr_putsw(g_box.x + 2, g_box.y + 3, g_box.w - 4, p->hint, S_BOX_DIM);
  for (i = 0; i < g_box.rows && vis->n > 0; i++) {
    size_t k = top + (size_t)i;
    const PickItem *it;
    int y = g_box.y + 3 + i, on, st, hst, sc, lx, right = g_box.x + g_box.w - 2;
    size_t b, n, dn;
    if (k >= vis->n) break;
    it = &p->item[vis->v[k]];
    on = (k == sel);
    st = on ? S_BOX_SEL : S_BOX;
    hst = on ? S_BOX_HIT_SEL : S_BOX_HIT;
    scr_fill(g_box.x + 1, y, g_box.w - 2, st);
    if (on) scr_round(g_box.x + 1, y, g_box.w - 2, 1, RC_ALL, RR_SMALL);
    lx = g_box.x + 2;
    if (it->icon) {
      scr_put(lx, y, (uint32_t)it->icon, st);
      lx += 2;
    }
    n = strlen(it->label);
    dn = it->detail ? strlen(it->detail) : 0;
    if (n + dn + 2 > hn) {
      hn = n + dn + 2;
      hit = (char *)xrealloc(hit, hn);
    }
    sc = p->text[0] ? item_score(p, it, hit, hit + n + 1) : 0;
    if (sc == FZ_NONE) memset(hit, 0, n + dn + 2);
    {	/* its group's name, at the right of the group's first item: "recently opened" */
      const char *gl = it->group >= 0 && it->group < 2 ? p->group_label[it->group] : NULL;
      if (gl && (k == 0 || p->item[vis->v[k - 1]].group != it->group)) {
        int gw = (int)str_cols(gl);
        if (gw + 20 < g_box.w) {
          scr_puts(g_box.x + g_box.w - 2 - gw, y, gl, on ? S_MENU_KEY_SEL : S_BOX_DIM);
          right = g_box.x + g_box.w - 4 - gw;
        }
      }
    }
    for (b = 0; b < n && lx < right;) {	/* the matched letters in blue */
      size_t len;
      uint32_t cp = utf8_decode(it->label + b, n - b, &len);
      lx += scr_put(lx, y, cp, (p->text[0] && hit[b]) ? hst : st);
      b += len;
    }
    if (it->detail && lx + 2 < right) {	/* the path, its matched letters in blue too */
      int dst = on ? S_MENU_KEY_SEL : S_BOX_DIM, dx = lx + 2, end = right - 1;
      const char *d = it->detail;
      for (b = 0; b < dn && dx < end;) {
        size_t len;
        uint32_t cp = utf8_decode(d + b, dn - b, &len);
        dx += scr_put(dx, y, cp, (p->text[0] && hit[n + 1 + b]) ? hst : dst);
        b += len;
      }
    }
  }
  free(hit);
}


int pick_run (Pick *p) {
  Vis vis;
  char matched[sizeof(p->text)];	/* what the items in vis were matched with */
  size_t sel = 0, top = 0;
  int r;
  int shown = -1, said = -1;	/* said: the item spoken (eaccess.c), -2 "No results" */
  size_t said_n = 0;	/* and how many there were */
  vis.v = (size_t *)xmalloc((p->n + 1) * sizeof(size_t));
  vis.score = (int *)xmalloc((p->n + 1) * sizeof(int));
  filter(p, &vis, 0);
  snprintf(matched, sizeof(matched), "%.*s", (int)pick_text_len(p, NULL, NULL), p->text);
  p->side = 0;
  if (p->start > 0 && (size_t)p->start < vis.n) sel = (size_t)p->start;
  for (;;) {
    int k, code, changed = 0;
    size_t len = strlen(p->text);
    if (sel >= vis.n) sel = vis.n ? vis.n - 1 : 0;
    if (p->on_move && vis.n && (int)vis.v[sel] != shown) {	/* the preview follows */
      shown = (int)vis.v[sel];
      p->on_move(shown);
    }
    if (sel < top) top = sel;
    if (vis.n && ((int)vis.v[sel] != said || vis.n != said_n)) {	/* a screen reader's: the item, where it is in the list */
      const PickItem *it = &p->item[vis.v[sel]];
      acc_sayf("%s%s%s%s%s, %lu of %lu", said == -1 && p->title ? p->title : "", said == -1 && p->title ? ": " : "",
               it->label ? it->label : "", it->detail && *it->detail ? ", " : "", it->detail ? it->detail : "",
               (unsigned long)sel + 1, (unsigned long)vis.n);
      said = (int)vis.v[sel];
      said_n = vis.n;
    }
    else if (!vis.n && said != -2) {
      acc_say(p->n ? "No results" : p->title ? p->title : "");
      said = -2;
    }
    ui_background();
    pick_draw(p, &vis, sel, top);
    if (g_box.rows > 0 && sel >= top + (size_t)g_box.rows) {
      top = sel - (size_t)g_box.rows + 1;
      continue;
    }
    scr_flush();
    k = term_key(p->on_tick ? 40 : 200);
    if (k == K_NONE) {
      int t = p->on_tick ? p->on_tick(p, 0) : 0;
      if (t == 2) {
        r = PICK_SWITCH;
        break;
      }
      if (t == 1) {	/* new items: the selection stays on top */
        free(vis.v);
        free(vis.score);
        vis.v = (size_t *)xmalloc((p->n + 1) * sizeof(size_t));
        vis.score = (int *)xmalloc((p->n + 1) * sizeof(int));
        filter(p, &vis, 0);
        matched[0] = '\0';
      }
      continue;
    }
    code = KEY_CODE(k);
    if (code == K_ESC) {
      r = PICK_CANCEL;
      break;
    }
    if (p->fresh && !IS_TEXT(k) && code != K_ENTER) p->fresh = 0;
    if (code == K_ENTER) {
      r = vis.n ? (int)vis.v[sel] : PICK_TEXT;
      p->side = (k & KM_CTRL) != 0;	/* Ctrl+Enter: to the side, where the caller can */
      break;
    }
    if (code == K_UP || (code == K_TAB && (k & KM_CTRL) && (k & KM_SHIFT)))
      sel = sel > 0 ? sel - 1 : (vis.n ? vis.n - 1 : 0);
    else if (code == K_DOWN || (code == K_TAB && (k & KM_CTRL))) sel = (sel + 1 < vis.n) ? sel + 1 : 0;
    else if (code == K_PGUP) sel = sel > (size_t)g_box.rows ? sel - (size_t)g_box.rows : 0;
    else if (code == K_PGDN) sel += (size_t)g_box.rows;
    else if (code == K_BS) {
      if (len == 0) {
        r = PICK_UP;
        break;
      }
      while (len > 0 && ((unsigned char)p->text[len - 1] & 0xC0) == 0x80) len--;
      if (len > 0) len--;
      p->text[len] = '\0';
      changed = 1;
    }
    else if (IS_PASTE(k)) {
      Buf b;
      size_t i;
      buf_init(&b);
      paste_take(k, &b);
      for (i = 0; i < b.len && b.s[i] != '\n' && len + 1 < sizeof(p->text); i++)
        p->text[len++] = b.s[i];
      p->text[len] = '\0';
      buf_free(&b);
      changed = 1;
    }
    else if (IS_TEXT(k) && len + 4 < sizeof(p->text)) {
      if (p->fresh) len = 0;	/* like a selected text: typing replaces it */
      len += (size_t)utf8_encode((uint32_t)k, p->text + len);
      p->text[len] = '\0';
      changed = 1;
    }
    else if (code == K_MOUSE) {
      Mouse *m = &term_mouse;
      int row = m->y - g_box.y - 3;
      int inside = m->x >= g_box.x && m->x < g_box.x + g_box.w &&
                   m->y >= g_box.y && m->y < g_box.y + g_box.rows + 4;
      if (m->wheel) {
        size_t st = (size_t)wheel_step(m->mods);
        if (m->wheel < 0) top = top > st ? top - st : 0;
        else if (vis.n > (size_t)g_box.rows) {
          top += st;
          if (top > vis.n - (size_t)g_box.rows) top = vis.n - (size_t)g_box.rows;
        }
        if (sel < top) sel = top;
        if (g_box.rows > 0 && sel >= top + (size_t)g_box.rows) sel = top + (size_t)g_box.rows - 1;
      }
      else if (m->button == 0 && m->press && !m->drag) {
        if (!inside) {
          r = PICK_CANCEL;
          break;
        }
        if (row >= 0 && row < g_box.rows && top + (size_t)row < vis.n) {
          r = (int)vis.v[top + (size_t)row];
          break;
        }
      }
    }
    if (p->on_tick) {
      int t = p->on_tick(p, changed);
      if (t == 2) {
        r = PICK_SWITCH;
        break;
      }
      if (t == 1) {
        free(vis.v);
        free(vis.score);
        vis.v = (size_t *)xmalloc((p->n + 1) * sizeof(size_t));
        vis.score = (int *)xmalloc((p->n + 1) * sizeof(int));
        changed = 1;
        matched[0] = '\0';	/* other items: every one is looked at */
      }
    }
    if (changed && p->modes && p->text[0] && strchr(p->modes, p->text[0])) {
      r = PICK_MODE;	/* ">" commands, "@" symbols ...: the caller shows those */
      break;
    }
    if (changed) {
      size_t ml = pick_text_len(p, NULL, NULL), ol = strlen(matched);
      p->fresh = 0;
      filter(p, &vis, ol > 0 && ml >= ol && strncmp(p->text, matched, ol) == 0);	/* it only grew: the ones left */
      snprintf(matched, sizeof(matched), "%.*s", (int)ml, p->text);
      sel = top = 0;
    }
  }
  free(vis.v);
  free(vis.score);
  return r;
}


char *ask_text (const char *title, const char *init) {
  Pick p;
  int r;
  pick_init(&p, title);
  p.hint = "Press 'Enter' to confirm or 'Escape' to cancel";
  if (init) snprintf(p.text, sizeof(p.text), "%s", init);
  p.fresh = init && *init;
  r = pick_run(&p);
  pick_free(&p);
  return r == PICK_TEXT ? xstrdup(p.text) : NULL;
}

/* }================================================================== */


/*
** {==================================================================
** The dialog
** ===================================================================
*/

/*
** VS Code's "Press desired key combination and then press ENTER": one key,
** or two for a chord (the third starts again). Esc: 0.
*/
int key_capture (const char *title, int *k2) {
  int got[2] = {0, 0}, n = 0;
  for (;;) {
    int cols = scr_cols(), rows = scr_rows(), w = 60, h = 6, x, y, k, code;
    char a[48], b[48], shown[100];
    if (w > cols - 2) w = cols - 2;
    x = (cols - w) / 2;
    y = rows / 3;
    scr_box(x, y, w, h, S_BOX);
    scr_putsw(x + 2, y + 1, w - 4, title, S_BOX_DIM);
    scr_putsw(x + 2, y + 2, w - 4, "Press desired key combination and then press ENTER.", S_BOX);
    scr_fill(x + 2, y + 4, w - 4, S_INPUT);
    shown[0] = '\0';
    if (n > 0) {
      key_name(got[0], 1, a, sizeof(a));
      if (n > 1) {
        key_name(got[1], 1, b, sizeof(b));
        snprintf(shown, sizeof(shown), "%s %s", a, b);
      }
      else snprintf(shown, sizeof(shown), "%s", a);
    }
    scr_putsw(x + 3, y + 4, w - 6, shown, S_INPUT_ON);
    scr_cursor(0, -1);
    scr_flush();
    k = term_key(-1);
    code = KEY_CODE(k);
    if (k == K_NONE || code == K_MOUSE || code == K_PASTE) continue;
    if (k == K_ESC) return 0;
    if (k == K_ENTER) {
      if (n == 0) continue;
      *k2 = n > 1 ? got[1] : 0;
      return got[0];
    }
    if (n == 2) n = 0;
    got[n++] = k;
  }
}


int dialog (const char *msg, const char *detail, const char *const *button, int n) {
  int sel = 0, i, said = -1;
  int bx[8], bw[8];
  if (n > 8) n = 8;
  for (;;) {
    int cols = scr_cols(), rows = scr_rows(), w, h = 7, x, y, k, code;
    int need = (int)str_cols(msg) + 8, bsum = 0;
    for (i = 0; i < n; i++) bsum += (int)strlen(button[i]) + 5;
    if (detail && (int)str_cols(detail) + 8 > need) need = (int)str_cols(detail) + 8;
    if (bsum + 4 > need) need = bsum + 4;
    w = need < cols - 2 ? need : cols - 2;
    if (w < 30 && cols > 32) w = 30;
    x = (cols - w) / 2;
    y = (rows - h) / 2;
    ui_background();
    scr_box(x, y, w, h, S_BOX);
    scr_put(x + 2, y + 1, 0xEA6C, S_TOAST_WARN);	/* codicon: warning */
    scr_putsw(x + 5, y + 1, w - 7, msg, S_BOX);
    if (detail) scr_putsw(x + 5, y + 2, w - 7, detail, S_BOX_DIM);
    {
      int bxx = x + w - 2;	/* the buttons, from the right */
      for (i = n - 1; i >= 0; i--) {
        bw[i] = (int)strlen(button[i]) + 4;
        bxx -= bw[i];
        bx[i] = bxx;
        bxx -= 1;
      }
      for (i = 0; i < n; i++) {
        int st = (i == sel) ? S_STATUS : S_INPUT;
        scr_fill(bx[i], y + 5, bw[i], st);
        if (st == S_STATUS) scr_round(bx[i], y + 5, bw[i], 1, RC_ALL, RR_SMALL);
        scr_puts(bx[i] + 2, y + 5, button[i], st);
      }
    }
    if (said != sel) {	/* a screen reader's: the question, then the button with the focus */
      if (said < 0) {
        size_t dl = detail ? strlen(detail) : 0;
        acc_sayf("%s %s%s %s button, %d of %d", msg, detail ? detail : "", dl && strchr(".?!", detail[dl - 1]) ? "" : ".",
                 button[sel], sel + 1, n);
      }
      else acc_sayf("%s button, %d of %d", button[sel], sel + 1, n);
      said = sel;
    }
    scr_cursor(0, -1);
    scr_pointer(PTR_POINTER);	/* every row of a menu, a picker or a dialog is clickable */
    scr_flush();
    k = term_key(200);
    if (k == K_NONE) continue;
    code = KEY_CODE(k);
    if (code == K_ESC) return -1;
    if (code == K_ENTER || code == ' ') return sel;
    if (code == K_LEFT || (code == K_TAB && (k & KM_SHIFT))) sel = (sel + n - 1) % n;
    else if (code == K_RIGHT || code == K_TAB) sel = (sel + 1) % n;
    else if (code == K_MOUSE && term_mouse.button == 0 && term_mouse.press) {
      for (i = 0; i < n; i++)
        if (term_mouse.y == y + 5 && term_mouse.x >= bx[i] && term_mouse.x < bx[i] + bw[i])
          return i;
    }
  }
}

/* }================================================================== */


/*
** {==================================================================
** Notifications
** ===================================================================
*/

#define TOAST_TIME	4000000	/* us: an information goes */
#define TOAST_ERR_TIME	10000000	/* an error stays longer */

/* the toast shown now (without actions) */
static struct {
  char msg[256];
  char src[32];	/* "gopls": the source line under it */
  int sev;	/* 0 information, 1 warning, 2 error */
  long long time;
} g_t;


/* the notifications that came, for the notification center: the last one last */
#define NOTE_MAX	50

static struct {
  char msg[NOTE_MAX][256];
  char src[NOTE_MAX][32];
  int sev[NOTE_MAX];
  int n, unread;
} g_note;


/* notifications with buttons (a language server's question): the first shows until it is answered */
#define ASK_MAX	8

typedef struct Ask {
  char msg[256];
  char src[32];
  int sev;
  char act[4][48];
  int nact;
  void (*done) (void *ud, int choice);	/* choice -1: closed without one */
  void *ud;
} Ask;

static Ask g_ask[ASK_MAX];
static int g_nask;
static int g_ask_focus, g_ask_sel;	/* the keys go to the first one's buttons */

/* where the toasts are drawn, for the mouse */
static struct {
  int x, y, w, h, close_x, gear_x, btn_y, bx0[4], bx1[4], nbtn;	/* the question */
  int tx, ty, tw, th, tclose_x;	/* the plain toast */
} TG;


static uint32_t sev_icon (int sev) {
  return sev >= 2 ? 0xEA87 : sev == 1 ? 0xEA6C : 0xEA74;	/* codicons error, warning, info */
}


static uint32_t sev_color (int sev) {
  return ui_color(sev >= 2 ? C_ERROR : sev == 1 ? C_WARNING : C_INFO);
}


static void note_add (const char *msg, int sev, const char *src) {
  if (msg[0] == '\0' || strstr(msg, "chord") || strstr(msg, "Chord")) return;	/* the Ctrl+K notice is not one */
  if (g_note.n > 0 && strcmp(g_note.msg[g_note.n - 1], msg) == 0) return;	/* the same again */
  if (g_note.n == NOTE_MAX) {
    memmove(g_note.msg, g_note.msg + 1, (NOTE_MAX - 1) * sizeof(g_note.msg[0]));
    memmove(g_note.src, g_note.src + 1, (NOTE_MAX - 1) * sizeof(g_note.src[0]));
    memmove(g_note.sev, g_note.sev + 1, (NOTE_MAX - 1) * sizeof(g_note.sev[0]));
    g_note.n--;
  }
  snprintf(g_note.msg[g_note.n], sizeof(g_note.msg[0]), "%s", msg);
  snprintf(g_note.src[g_note.n], sizeof(g_note.src[0]), "%s", src ? src : "");
  g_note.sev[g_note.n++] = sev;
  g_note.unread++;
}


static void note_del (int at) {
  int n = g_note.n;
  memmove(g_note.msg + at, g_note.msg + at + 1, (size_t)(n - at - 1) * sizeof(g_note.msg[0]));
  memmove(g_note.src + at, g_note.src + at + 1, (size_t)(n - at - 1) * sizeof(g_note.src[0]));
  memmove(g_note.sev + at, g_note.sev + at + 1, (size_t)(n - at - 1) * sizeof(g_note.sev[0]));
  g_note.n--;
}


static void toast_v (int sev, const char *src, const char *fmt, va_list ap) {
  vsnprintf(g_t.msg, sizeof(g_t.msg), fmt, ap);
  snprintf(g_t.src, sizeof(g_t.src), "%s", src ? src : "");
  g_t.sev = sev;
  g_t.time = os_now_us();
  note_add(g_t.msg, sev, src);
  acc_sayf("%s%s", sev == 2 ? "Error: " : sev == 1 ? "Warning: " : "", g_t.msg);
}


/* the last notification's text (Accessible View), NULL: none */
const char *note_last (void) {
  return g_note.n ? g_note.msg[g_note.n - 1] : NULL;
}


void toast (int warn, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  toast_v(warn ? 1 : 0, NULL, fmt, ap);
  va_end(ap);
}


/* a notification with its severity (0 info, 1 warning, 2 error) and its source ("gopls") */
void toast_src (int sev, const char *src, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  toast_v(sev, src, fmt, ap);
  va_end(ap);
}


/*
** A notification with buttons, like a language server's showMessageRequest:
** it shows until a button is clicked (or it is closed); done gets the
** button's index, -1 for none.
*/
void toast_ask (int sev, const char *src, const char *msg, const char *const *act, int n,
                void (*done) (void *ud, int choice), void *ud) {
  Ask *a;
  int i;
  if (g_nask == ASK_MAX) {	/* too many waiting: this one gets no answer */
    if (done) done(ud, -1);
    return;
  }
  a = &g_ask[g_nask++];
  memset(a, 0, sizeof(*a));
  snprintf(a->msg, sizeof(a->msg), "%s", msg);
  snprintf(a->src, sizeof(a->src), "%s", src ? src : "");
  a->sev = sev;
  for (i = 0; i < n && i < 4; i++) snprintf(a->act[i], sizeof(a->act[0]), "%s", act[i]);
  a->nact = i;
  a->done = done;
  a->ud = ud;
  note_add(msg, sev, src);
  acc_sayf("%s%s. Buttons: %s%s%s", sev == 2 ? "Error: " : sev == 1 ? "Warning: " : "", msg, a->act[0],
           a->nact > 1 ? ", " : "", a->nact > 1 ? a->act[1] : "");
}


/* question i answered with choice (-1: closed) */
static void ask_answer (int i, int choice) {
  Ask a;
  if (i < 0 || i >= g_nask) return;
  a = g_ask[i];
  memmove(g_ask + i, g_ask + i + 1, (size_t)(g_nask - i - 1) * sizeof(Ask));
  g_nask--;
  g_ask_sel = 0;
  if (g_nask == 0) g_ask_focus = 0;
  if (a.done) a.done(a.ud, choice >= a.nact ? -1 : choice);
}


int toast_unread (void) {
  return g_note.unread;
}


/* Notifications: Focus Notification Toast; 0: there is none with buttons */
int toast_focus (void) {
  if (g_nask == 0) return 0;
  g_ask_focus = 1;
  g_ask_sel = 0;
  return 1;
}


int toast_focused (void) {
  return g_ask_focus && g_nask > 0;
}


/* Notifications: Accept Notification Primary Action (Ctrl+Shift+A) */
int toast_accept (void) {
  if (g_nask == 0) return 0;
  ask_answer(0, 0);
  return 1;
}


/* Notifications: Clear All Notifications: the toasts and the questions go, the list too */
void toast_clear_all (void) {
  while (g_nask > 0) ask_answer(0, -1);
  g_t.msg[0] = '\0';
  g_note.n = g_note.unread = 0;
}


/* a key while the question has the keys: Left/Right/Tab pick a button, Enter it, Esc closes; 0: not its */
int toast_key (int k) {
  int code = KEY_CODE(k), n;
  if (!toast_focused()) return 0;
  n = g_ask[0].nact;
  if (code == K_ESC) ask_answer(0, -1);
  else if (code == K_ENTER) ask_answer(0, n ? g_ask_sel : -1);
  else if (code == K_RIGHT || (code == K_TAB && !(k & KM_SHIFT))) g_ask_sel = n ? (g_ask_sel + 1) % n : 0;
  else if (code == K_LEFT || code == K_TAB) g_ask_sel = n ? (g_ask_sel + n - 1) % n : 0;
  else {
    g_ask_focus = 0;	/* anything else: back to where the keys were */
    return 0;
  }
  return 1;
}


/* the message cut into lines of w columns at spaces: their starts and lengths; how many (max) */
static int wrap (const char *s, int w, const char **start, size_t *len, int max) {
  int n = 0;
  while (*s && n < max) {
    const char *p = s, *cut = NULL;
    int col = 0;
    while (*p && *p != '\n') {
      size_t l;
      uint32_t cp = utf8_decode(p, strlen(p), &l);
      int cw = uc_width(cp);
      if (col + cw > w) break;
      if (*p == ' ') cut = p;
      col += cw;
      p += l;
    }
    if (*p && *p != '\n' && cut && cut > s) p = cut;	/* at the last space */
    start[n] = s;
    len[n++] = (size_t)(p - s);
    s = p;
    while (*s == ' ' || *s == '\n') s++;
  }
  if (*s && n > 0) {	/* more than fits: the last line ends in ... */
    size_t l = len[n - 1];
    while (l > 0 && str_cols(start[n - 1]) > 0 && (int)l > w - 3) l--;
    len[n - 1] = l;
  }
  return n;
}


/* a message box of the corner (the toast's look): its height; y is its bottom row */
static int note_box (int bottom, int w, int sev, const char *src, const char *msg, const Ask *a, int focus, int sel,
                     int *top, int *close_x, int *gear_x) {
  const char *ls[4];
  size_t ll[4];
  int cols = scr_cols(), nl, h, x, y, i, tw = w - 10;
  char buf[260];
  nl = wrap(msg, tw > 10 ? tw : 10, ls, ll, a ? 4 : 3);
  if (nl == 0) nl = 1, ls[0] = msg, ll[0] = 0;
  h = 2 + nl + ((src && *src) || (a && a->nact) ? 1 : 0);
  x = cols - w - 1;
  y = bottom - h + 1;
  if (y < 1) return 0;
  scr_box(x, y, w, h, S_TOAST);
  if (focus) {	/* the keys are here: VS Code's focus border */
    for (i = 0; i < w; i++) {
      scr_put(x + i, y, 0x2500, S_TOAST);
      scr_set_fg(x + i, y, ui_color(C_ACCENT));
    }
  }
  scr_put(x + 2, y + 1, sev_icon(sev), S_TOAST);
  scr_set_fg(x + 2, y + 1, sev_color(sev));
  for (i = 0; i < nl; i++) {
    size_t n = ll[i] < sizeof(buf) - 4 ? ll[i] : sizeof(buf) - 4;
    memcpy(buf, ls[i], n);
    buf[n] = '\0';
    if (i == nl - 1 && ls[i] + ll[i] < msg + strlen(msg) && ls[i][ll[i]] != '\0' &&
        strlen(ls[i] + ll[i]) > 0 && i + 1 == (a ? 4 : 3))
      strcat(buf, "...");
    scr_putsw(x + 5, y + 1 + i, tw, buf, S_TOAST);
  }
  *close_x = x + w - 3;
  scr_put(*close_x, y + 1, 0xEA76, S_TOAST);	/* codicon close */
  *gear_x = -1;
  if (src && *src) {
    *gear_x = x + w - 5;
    scr_put(*gear_x, y + 1, 0xEAF8, S_TOAST);	/* gear: the notification center */
    snprintf(buf, sizeof(buf), "Source: %s", src);
    scr_putsw(x + 5, y + 1 + nl, w - 7, buf, S_BOX_DIM);
  }
  if (a) {	/* the buttons, on the right of the last row: the first is the primary one */
    int bx = x + w - 2;
    TG.nbtn = a->nact;
    TG.btn_y = y + 1 + nl;
    for (i = a->nact - 1; i >= 0; i--) {
      int bw = (int)str_cols(a->act[i]) + 2, st = i == 0 ? S_STATUS : S_INPUT;
      if (focus && i == sel) st = S_MENU_SEL;
      bx -= bw;
      if (bx < x + 2) {
        TG.bx0[i] = TG.bx1[i] = -1;
        continue;
      }
      scr_fill(bx, TG.btn_y, bw, st);
      scr_puts(bx + 1, TG.btn_y, a->act[i], st);
      TG.bx0[i] = bx;
      TG.bx1[i] = bx + bw;
      bx -= 1;
    }
  }
  *top = y;
  return h;
}


/* the spinner of a running progress: a Braille frame by the time */
static const char *spin (void) {
  static const char *const f[] = {"\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8", "\xE2\xA0\xBC",
                                  "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7", "\xE2\xA0\x87", "\xE2\xA0\x8F"};
  return f[(os_now_us() / 100000) % 10];
}


const char *ui_spinner (void) {
  return spin();
}


/*
** The notification center (the bell in the status bar): what is running
** (a language server's progress), the questions with their buttons, then
** every notification so far, the newest on top; the x of one clears it,
** "Clear All" every one. Esc or a click outside closes it.
*/
void note_center (void) {
  int sel = 0;
  g_note.unread = 0;
  g_t.msg[0] = '\0';	/* the toast goes: it is in the list */
  for (;;) {
    int cols = scr_cols(), rows = scr_rows(), w = cols < 72 ? cols - 2 : 70, x = cols - w - 1;
    int np = lsp_progress_count(), nq = g_nask, n = g_note.n, total, shown, h, y, i, k, code, clear_x, close_x;
    total = np + nq + n;
    shown = total ? total : 1;
    if (shown > rows - 8) shown = rows - 8;
    if (shown < 1) shown = 1;
    h = shown + 1;
    y = rows - 2 - h;
    if (y < 1 || w < 24) return;
    if (sel >= nq + n) sel = nq + n ? nq + n - 1 : 0;
    ui_background();
    scr_box(x, y, w, h, S_TOAST);
    scr_fill(x, y, w, S_BOX_TITLE);
    scr_puts(x + 2, y, total ? "NOTIFICATIONS" : "NO NEW NOTIFICATIONS", S_BOX_TITLE);
    clear_x = x + w - 5;
    close_x = x + w - 3;
    scr_put(clear_x, y, 0xEABF, S_BOX_TITLE);	/* codicon clear-all */
    scr_put(close_x, y, 0xEAB4, S_BOX_TITLE);	/* chevron-down: hide */
    for (i = 0; i < shown && i < total; i++) {
      int ry = y + 1 + i;
      char t[300];
      if (i < np) {	/* running: the spinner, and what it does */
        int pct = lsp_progress_text(i, t, sizeof(t));
        scr_fill(x, ry, w, S_TOAST);
        scr_puts(x + 2, ry, spin(), S_TOAST);
        scr_set_fg(x + 2, ry, ui_color(C_INFO));
        scr_putsw(x + 5, ry, w - 13, t, S_TOAST);
        if (pct >= 0) {	/* a bar of its percentage */
          int bw = 6, fill = pct * bw / 100, b;
          for (b = 0; b < bw; b++) scr_put(x + w - 8 + b, ry, b < fill ? 0x2588 : 0x2591, S_BOX_DIM);
        }
        continue;
      }
      k = i - np;	/* the row: questions, then notes */
      if (k < nq) {
        const Ask *a = &g_ask[k];
        int st = k == sel ? S_BOX_SEL : S_TOAST, bx = x + w - 5, b;
        scr_fill(x, ry, w, st);
        scr_put(x + 2, ry, sev_icon(a->sev), st);
        scr_set_fg(x + 2, ry, sev_color(a->sev));
        for (b = a->nact - 1; b >= 0; b--) bx -= (int)str_cols(a->act[b]) + 3;
        scr_putsw(x + 5, ry, bx - x - 6, a->msg, st);
        for (b = 0; b < a->nact; b++) {	/* its buttons, the first the primary one */
          int bw = (int)str_cols(a->act[b]) + 2;
          if (bx + bw >= x + w - 3) break;
          scr_fill(bx, ry, bw, b == 0 ? S_STATUS : S_INPUT);
          if (b == 0) scr_round(bx, ry, bw, 1, RC_ALL, RR_SMALL);
          scr_puts(bx + 1, ry, a->act[b], b == 0 ? S_STATUS : S_INPUT);
          bx += bw + 1;
        }
        if (k == sel) scr_put(x + w - 3, ry, 0xEA76, st);
        continue;
      }
      k -= nq;
      {
        int k2 = n - 1 - k, st = k + nq == sel ? S_BOX_SEL : S_TOAST;
        scr_fill(x, ry, w, st);
        scr_put(x + 2, ry, sev_icon(g_note.sev[k2]), st);
        scr_set_fg(x + 2, ry, sev_color(g_note.sev[k2]));
        if (g_note.src[k2][0]) snprintf(t, sizeof(t), "%s  (%s)", g_note.msg[k2], g_note.src[k2]);
        else snprintf(t, sizeof(t), "%s", g_note.msg[k2]);
        scr_putsw(x + 5, ry, w - 9, t, st);
        if (k + nq == sel) scr_put(x + w - 3, ry, 0xEA76, st);	/* its x */
      }
    }
    scr_cursor(0, -1);
    scr_pointer(PTR_POINTER);	/* every row is clickable */
    scr_flush();
    k = term_key(200);
    if (k == K_NONE) continue;
    code = KEY_CODE(k);
    if (code == K_ESC) return;
    if (code == K_ENTER) {
      if (sel < g_nask) ask_answer(sel, 0);	/* a question: its primary button */
      else return;
    }
    else if (code == K_UP && sel > 0) sel--;
    else if (code == K_DOWN && sel + 1 < nq + n) sel++;
    else if ((code == K_DEL || code == K_BS) && nq + n > 0) {	/* that one goes */
      if (sel < nq) ask_answer(sel, -1);
      else note_del(n - 1 - (sel - nq));
    }
    else if (code == K_MOUSE && term_mouse.press && !term_mouse.drag && !term_mouse.wheel) {
      Mouse *m = &term_mouse;
      if (m->x < x || m->x >= x + w || m->y < y || m->y >= y + h) return;	/* outside */
      if (m->y == y && m->x == clear_x) toast_clear_all();
      else if (m->y == y && m->x == close_x) return;
      else if (m->y > y && m->y - y - 1 >= np && m->y - y - 1 - np < nq + n) {
        int r = m->y - y - 1 - np;
        sel = r;
        if (r < nq) {	/* a question: which button */
          const Ask *a = &g_ask[r];
          int bx = x + w - 5, b;
          for (b = a->nact - 1; b >= 0; b--) bx -= (int)str_cols(a->act[b]) + 3;
          if (m->x == x + w - 3) ask_answer(r, -1);
          else
            for (b = 0; b < a->nact; b++) {
              int bw = (int)str_cols(a->act[b]) + 2;
              if (m->x >= bx && m->x < bx + bw) {
                ask_answer(r, b);
                break;
              }
              bx += bw + 1;
            }
        }
        else if (m->x == x + w - 3) note_del(n - 1 - (r - nq));	/* its x */
      }
    }
  }
}


void toast_draw (void) {
  int cols = scr_cols(), rows = scr_rows(), bottom = rows - 2, w;
  long long left = g_t.sev >= 2 ? TOAST_ERR_TIME : TOAST_TIME;
  TG.w = TG.tw = 0;
  if (g_nask > 0) {	/* the first question: until it is answered */
    const Ask *a = &g_ask[0];
    int top, h;
    w = cols - 2 < 64 ? cols - 2 : 64;
    h = note_box(bottom, w, a->sev, a->src, a->msg, a, toast_focused(), g_ask_sel, &top, &TG.close_x, &TG.gear_x);
    if (h > 0) {
      TG.x = cols - w - 1;
      TG.y = top;
      TG.w = w;
      TG.h = h;
      bottom = top - 1;
    }
  }
  if (g_t.msg[0] && os_now_us() - g_t.time <= left) {
    int top, h, gear;
    w = (int)str_cols(g_t.msg) + 11;
    if (g_t.src[0] && (int)strlen(g_t.src) + 20 > w) w = (int)strlen(g_t.src) + 20;
    if (w < 30) w = 30;
    if (w > 64) w = 64;
    if (w > cols - 2) w = cols - 2;
    h = note_box(bottom, w, g_t.sev, g_t.src, g_t.msg, NULL, 0, 0, &top, &TG.tclose_x, &gear);
    if (h > 0) {
      TG.tx = cols - w - 1;
      TG.ty = top;
      TG.tw = w;
      TG.th = h;
    }
  }
}


/* a click on a toast: its buttons, its x, its gear; 1: it was on one */
int toast_click (int x, int y) {
  if (TG.w > 0 && g_nask > 0 && x >= TG.x && x < TG.x + TG.w && y >= TG.y && y < TG.y + TG.h) {
    int i;
    if (y == TG.y + 1 && x == TG.close_x) ask_answer(0, -1);
    else if (y == TG.y + 1 && x == TG.gear_x) note_center();
    else if (y == TG.btn_y) {
      for (i = 0; i < TG.nbtn; i++)
        if (x >= TG.bx0[i] && x < TG.bx1[i]) {
          ask_answer(0, i);
          return 1;
        }
    }
    else toast_focus();	/* on its text: the keys go to it */
    return 1;
  }
  if (TG.tw > 0 && g_t.msg[0] && x >= TG.tx && x < TG.tx + TG.tw && y >= TG.ty && y < TG.ty + TG.th) {
    if (y == TG.ty + 1 && x == TG.tclose_x) g_t.msg[0] = '\0';
    else if (y == TG.ty + 1 && x == TG.tclose_x - 2 && g_t.src[0]) note_center();
    return 1;
  }
  return 0;
}

/* }================================================================== */
