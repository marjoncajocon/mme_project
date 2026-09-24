/*
** etheme.c - color themes: VS Code's, by the names of its colors
**
** A theme gives every slot (editor.background, statusBar.background, a
** token's color ...) a color; the screen's styles are made from the slots.
** Dark+ has them all; the others say only where they differ from it.
** Ctrl+K Ctrl+T picks one; settings.json keeps it (workbench.colorTheme).
** Extensions add theirs (theme_add): read when picked, over Dark+ or Light+.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct Color {
  int slot;
  uint32_t rgb;
} Color;

/* Dark+: VS Code's dark theme, the base of the others */
static const uint32_t dark_plus[C_N] = {
  [C_EDITOR_BG] = 0x1E1E1E, [C_EDITOR_FG] = 0xD4D4D4, [C_LINE_BG] = 0x282828,
  [C_SEL_BG] = 0x264F78, [C_MATCH_BG] = 0x613314, [C_GUTTER] = 0x858585,
  [C_GUTTER_ON] = 0xC6C6C6, [C_CTRL] = 0x6A6A6A, [C_WS] = 0x464646,
  [C_STATUS_BG] = 0x007ACC, [C_STATUS_FG] = 0xFFFFFF, [C_STATUS_ITEM] = 0x1F8AD2,
  [C_TITLE_BG] = 0x3C3C3C, [C_TITLE_FG] = 0xCCCCCC, [C_TITLE_ON] = 0x505050,
  [C_MENU_BG] = 0x252526, [C_MENU_FG] = 0xCCCCCC, [C_MENU_SEL_BG] = 0x094771,
  [C_MENU_SEL_FG] = 0xFFFFFF, [C_MENU_DIM] = 0x969696, [C_MENU_SEP] = 0x454545,
  [C_ACT_BG] = 0x333333, [C_ACT_FG] = 0xFFFFFF, [C_ACT_DIM] = 0x858585,
  [C_SIDE_BG] = 0x252526, [C_SIDE_FG] = 0xCCCCCC, [C_SIDE_HEAD] = 0xBBBBBB,
  [C_LIST_SEL_BG] = 0x04395E, [C_LIST_SEL_FG] = 0xFFFFFF, [C_LIST_CUR_BG] = 0x37373D,
  [C_DIM] = 0x8B8B8B, [C_BORDER] = 0x454545,
  [C_INPUT_BG] = 0x3C3C3C, [C_INPUT_FG] = 0xCCCCCC, [C_INPUT_HINT] = 0x8B8B8B,
  [C_TOGGLE_BG] = 0x245F8F, [C_HIT] = 0x2AAAFF, [C_TOAST_BG] = 0x2D2D30,
  [C_ERROR] = 0xF14C4C, [C_WARNING] = 0xCCA700, [C_INFO] = 0x3794FF,
  [C_GIT_M] = 0xE2C08D, [C_GIT_A] = 0x81B88B, [C_GIT_D] = 0xC74E39, [C_GIT_U] = 0x73C991,
  [C_ICON_BLUE] = 0x519ABA, [C_ICON_YELLOW] = 0xCBCB41, [C_ICON_GREEN] = 0x8DC149,
  [C_ICON_RED] = 0xCC3E44, [C_ICON_PURPLE] = 0xA074C4, [C_ICON_ORANGE] = 0xE37933,
  [C_ICON_GREY] = 0x6D8086,
  [C_DIFF_ADD] = 0x253A1E, [C_DIFF_ADD_HI] = 0x3E6426, [C_DIFF_DEL] = 0x481E20,
  [C_DIFF_DEL_HI] = 0x7D2628,
  [C_TABS_BG] = 0x252526, [C_TAB_BG] = 0x2D2D2D, [C_TAB_FG] = 0x969696,
  [C_TAB_ON_BG] = 0x1E1E1E, [C_TAB_ON_FG] = 0xFFFFFF, [C_CRUMB] = 0xA9A9A9,
  [C_ACCENT] = 0x007ACC, [C_MCURSOR] = 0xAEAFAD, [C_LIGHTBULB] = 0xFFCC00,
  [C_MINIMAP_SLIDER] = 0x333333, [C_THUMB] = 0x434343, [C_THUMB_ON] = 0x5A5A5A,
  [C_BRACKET1] = 0xFFD700, [C_BRACKET2] = 0xDA70D6, [C_BRACKET3] = 0x179FFF,
  [C_BRACKET_MATCH] = 0x2E4A2E,
  [C_GUIDE] = 0x404040, [C_GUIDE_ON] = 0x707070, [C_RULER] = 0x5A5A5A,
  [C_INLAY_FG] = 0x969696, [C_INLAY_BG] = 0x2A2A2A, [C_LENS] = 0x999999, [C_GHOST] = 0x6B6B6B,
  [C_TERM_FG] = 0xCCCCCC, [C_TERM_BG] = 0x1E1E1E,
  [C_ANSI + 0] = 0x000000, [C_ANSI + 1] = 0xCD3131, [C_ANSI + 2] = 0x0DBC79,
  [C_ANSI + 3] = 0xE5E510, [C_ANSI + 4] = 0x2472C8, [C_ANSI + 5] = 0xBC3FBC,
  [C_ANSI + 6] = 0x11A8CD, [C_ANSI + 7] = 0xE5E5E5, [C_ANSI + 8] = 0x666666,
  [C_ANSI + 9] = 0xF14C4C, [C_ANSI + 10] = 0x23D18B, [C_ANSI + 11] = 0xF5F543,
  [C_ANSI + 12] = 0x3B8EEA, [C_ANSI + 13] = 0xD670D6, [C_ANSI + 14] = 0x29B8DB,
  [C_ANSI + 15] = 0xE5E5E5,
  [C_TOK + T_TEXT] = 0xD4D4D4, [C_TOK + T_KEYWORD] = 0xC586C0, [C_TOK + T_STORAGE] = 0x569CD6,
  [C_TOK + T_TYPE] = 0x4EC9B0, [C_TOK + T_FUNC] = 0xDCDCAA, [C_TOK + T_STRING] = 0xCE9178,
  [C_TOK + T_NUMBER] = 0xB5CEA8, [C_TOK + T_COMMENT] = 0x6A9955, [C_TOK + T_VAR] = 0x9CDCFE,
  [C_TOK + T_CONST] = 0x4FC1FF, [C_TOK + T_ESCAPE] = 0xD7BA7D, [C_TOK + T_HEADING] = 0x569CD6,
  [C_TOK + T_WS] = 0x464646
};

/* Dark Modern: VS Code's default now; Dark+ with darker, quieter chrome */
static const Color dark_modern[] = {
  {C_EDITOR_BG, 0x1F1F1F}, {C_EDITOR_FG, 0xCCCCCC}, {C_LINE_BG, 0x262626},
  {C_GUTTER, 0x6E7681}, {C_GUTTER_ON, 0xCCCCCC},
  {C_STATUS_BG, 0x181818}, {C_STATUS_FG, 0xCCCCCC}, {C_STATUS_ITEM, 0x1F1F1F},
  {C_TITLE_BG, 0x181818}, {C_TITLE_ON, 0x2A2A2A},
  {C_MENU_BG, 0x1F1F1F}, {C_MENU_SEL_BG, 0x0078D4}, {C_MENU_SEP, 0x454545},
  {C_ACT_BG, 0x181818}, {C_ACT_FG, 0xD7D7D7}, {C_ACT_DIM, 0x868686},
  {C_SIDE_BG, 0x181818}, {C_LIST_CUR_BG, 0x2A2D2E}, {C_BORDER, 0x2B2B2B},
  {C_INPUT_BG, 0x313131}, {C_INPUT_HINT, 0x989898}, {C_TOGGLE_BG, 0x0078D4},
  {C_TOAST_BG, 0x1F1F1F},
  {C_TABS_BG, 0x181818}, {C_TAB_BG, 0x181818}, {C_TAB_FG, 0x9D9D9D},
  {C_TAB_ON_BG, 0x1F1F1F}, {C_CRUMB, 0xA9A9A9}, {C_ACCENT, 0x0078D4},
  {C_MINIMAP_SLIDER, 0x323232}, {C_TERM_BG, 0x181818}, {C_TERM_FG, 0xCCCCCC},
  {C_INLAY_BG, 0x262626},
  {C_TOK + T_TEXT, 0xCCCCCC}
};

/* Light+: VS Code's light theme */
static const Color light_plus[] = {
  {C_EDITOR_BG, 0xFFFFFF}, {C_EDITOR_FG, 0x000000}, {C_LINE_BG, 0xF3F3F3},
  {C_SEL_BG, 0xADD6FF}, {C_MATCH_BG, 0xF6C4A7}, {C_GUTTER, 0x237893},
  {C_GUTTER_ON, 0x0B216F}, {C_CTRL, 0xBBBBBB}, {C_WS, 0xD3D3D3},
  {C_TITLE_BG, 0xDDDDDD}, {C_TITLE_FG, 0x333333}, {C_TITLE_ON, 0xC8C8C8},
  {C_MENU_BG, 0xF3F3F3}, {C_MENU_FG, 0x333333}, {C_MENU_SEL_BG, 0x0060C0},
  {C_MENU_DIM, 0x717171}, {C_MENU_SEP, 0xD4D4D4},
  {C_ACT_BG, 0x2C2C2C}, {C_ACT_DIM, 0x8B8B8B},
  {C_SIDE_BG, 0xF3F3F3}, {C_SIDE_FG, 0x616161}, {C_SIDE_HEAD, 0x6F6F6F},
  {C_LIST_SEL_BG, 0x0060C0}, {C_LIST_CUR_BG, 0xE4E6F1}, {C_DIM, 0x8B8B8B},
  {C_BORDER, 0xD4D4D4}, {C_INPUT_BG, 0xFFFFFF}, {C_INPUT_FG, 0x616161},
  {C_INPUT_HINT, 0x767676}, {C_TOGGLE_BG, 0xCCE4F5}, {C_HIT, 0x0066BF},
  {C_TOAST_BG, 0xF3F3F3}, {C_ERROR, 0xE51400}, {C_WARNING, 0xBF8803}, {C_INFO, 0x1A85FF},
  {C_GIT_M, 0x895503}, {C_GIT_A, 0x587C0C}, {C_GIT_D, 0xAD0707}, {C_GIT_U, 0x007100},
  {C_DIFF_ADD, 0xEBF1DD}, {C_DIFF_ADD_HI, 0xD5E8B6}, {C_DIFF_DEL, 0xFFDCDC},
  {C_DIFF_DEL_HI, 0xFFB4B4},
  {C_TABS_BG, 0xF3F3F3}, {C_TAB_BG, 0xECECEC}, {C_TAB_FG, 0x707070},
  {C_TAB_ON_BG, 0xFFFFFF}, {C_TAB_ON_FG, 0x333333}, {C_CRUMB, 0x616161},
  {C_MCURSOR, 0x707070}, {C_LIGHTBULB, 0xDDB100},
  {C_MINIMAP_SLIDER, 0xE0E0E0}, {C_THUMB, 0xC1C1C1}, {C_THUMB_ON, 0x929292},
  {C_BRACKET1, 0x0431FA}, {C_BRACKET2, 0x319331}, {C_BRACKET3, 0x7B3814},
  {C_BRACKET_MATCH, 0xD9EBD9},
  {C_GUIDE, 0xD3D3D3}, {C_GUIDE_ON, 0x939393}, {C_RULER, 0xD3D3D3},
  {C_INLAY_FG, 0x969696}, {C_INLAY_BG, 0xF0F0F0}, {C_LENS, 0x919191}, {C_GHOST, 0xA6A6A6},
  {C_TERM_FG, 0x333333}, {C_TERM_BG, 0xFFFFFF},
  {C_ANSI + 2, 0x00BC00}, {C_ANSI + 3, 0x949800}, {C_ANSI + 4, 0x0451A5},
  {C_ANSI + 5, 0xBC05BC}, {C_ANSI + 6, 0x0598BC}, {C_ANSI + 7, 0x555555},
  {C_ANSI + 10, 0x14CE14}, {C_ANSI + 11, 0xB5BA00}, {C_ANSI + 12, 0x0451A5},
  {C_ANSI + 13, 0xBC05BC}, {C_ANSI + 14, 0x0598BC}, {C_ANSI + 15, 0xA5A5A5},
  {C_TOK + T_TEXT, 0x000000}, {C_TOK + T_KEYWORD, 0xAF00DB}, {C_TOK + T_STORAGE, 0x0000FF},
  {C_TOK + T_TYPE, 0x267F99}, {C_TOK + T_FUNC, 0x795E26}, {C_TOK + T_STRING, 0xA31515},
  {C_TOK + T_NUMBER, 0x098658}, {C_TOK + T_COMMENT, 0x008000}, {C_TOK + T_VAR, 0x001080},
  {C_TOK + T_CONST, 0x0070C1}, {C_TOK + T_ESCAPE, 0xEE0000}, {C_TOK + T_HEADING, 0x800000},
  {C_TOK + T_WS, 0xD3D3D3}
};

/* Monokai */
static const Color monokai[] = {
  {C_EDITOR_BG, 0x272822}, {C_EDITOR_FG, 0xF8F8F2}, {C_LINE_BG, 0x3E3D32},
  {C_SEL_BG, 0x49483E}, {C_MATCH_BG, 0x5F5A36}, {C_GUTTER, 0x90908A},
  {C_GUTTER_ON, 0xC2C2BF}, {C_WS, 0x464741},
  {C_STATUS_BG, 0x414339}, {C_STATUS_ITEM, 0x5A5C4E},
  {C_TITLE_BG, 0x1E1F1C}, {C_TITLE_ON, 0x414339},
  {C_MENU_BG, 0x1E1F1C}, {C_MENU_SEL_BG, 0x75715E},
  {C_ACT_BG, 0x272822}, {C_ACT_FG, 0xF8F8F2}, {C_ACT_DIM, 0x75715E},
  {C_SIDE_BG, 0x1E1F1C}, {C_LIST_SEL_BG, 0x75715E}, {C_LIST_CUR_BG, 0x414339},
  {C_DIM, 0x75715E}, {C_BORDER, 0x414339}, {C_INPUT_BG, 0x414339},
  {C_TOGGLE_BG, 0x75715E}, {C_HIT, 0xA6E22E}, {C_TOAST_BG, 0x1E1F1C},
  {C_TABS_BG, 0x1E1F1C}, {C_TAB_BG, 0x34352F}, {C_TAB_FG, 0xCCCCC7},
  {C_TAB_ON_BG, 0x272822}, {C_ACCENT, 0xA6E22E}, {C_MCURSOR, 0xF8F8F0},
  {C_MINIMAP_SLIDER, 0x3E3D32}, {C_THUMB, 0x49483E}, {C_THUMB_ON, 0x75715E},
  {C_BRACKET_MATCH, 0x4A4A3E},
  {C_GUIDE, 0x464741}, {C_GUIDE_ON, 0x767771}, {C_RULER, 0x464741},
  {C_INLAY_FG, 0x90908A}, {C_INLAY_BG, 0x32332C}, {C_LENS, 0x90908A}, {C_GHOST, 0x75715E},
  {C_TERM_BG, 0x272822}, {C_TERM_FG, 0xF8F8F2},
  {C_TOK + T_TEXT, 0xF8F8F2}, {C_TOK + T_KEYWORD, 0xF92672}, {C_TOK + T_STORAGE, 0x66D9EF},
  {C_TOK + T_TYPE, 0xA6E22E}, {C_TOK + T_FUNC, 0xA6E22E}, {C_TOK + T_STRING, 0xE6DB74},
  {C_TOK + T_NUMBER, 0xAE81FF}, {C_TOK + T_COMMENT, 0x88846F}, {C_TOK + T_VAR, 0xF8F8F2},
  {C_TOK + T_CONST, 0xAE81FF}, {C_TOK + T_ESCAPE, 0xAE81FF}, {C_TOK + T_HEADING, 0xA6E22E},
  {C_TOK + T_WS, 0x464741}
};

/* Solarized Dark */
static const Color solarized[] = {
  {C_EDITOR_BG, 0x002B36}, {C_EDITOR_FG, 0x839496}, {C_LINE_BG, 0x073642},
  {C_SEL_BG, 0x274642}, {C_MATCH_BG, 0x584C27}, {C_GUTTER, 0x586E75},
  {C_GUTTER_ON, 0x93A1A1}, {C_WS, 0x0E4B58}, {C_CTRL, 0x586E75},
  {C_STATUS_BG, 0x00212B}, {C_STATUS_FG, 0x93A1A1}, {C_STATUS_ITEM, 0x003847},
  {C_TITLE_BG, 0x002C39}, {C_TITLE_FG, 0x93A1A1}, {C_TITLE_ON, 0x005A6F},
  {C_MENU_BG, 0x00212B}, {C_MENU_FG, 0x93A1A1}, {C_MENU_SEL_BG, 0x005A6F},
  {C_ACT_BG, 0x003847}, {C_ACT_FG, 0x93A1A1}, {C_ACT_DIM, 0x586E75},
  {C_SIDE_BG, 0x00212B}, {C_SIDE_FG, 0x93A1A1}, {C_LIST_SEL_BG, 0x005A6F},
  {C_LIST_CUR_BG, 0x004454}, {C_DIM, 0x586E75}, {C_BORDER, 0x073642},
  {C_INPUT_BG, 0x003847}, {C_INPUT_FG, 0x93A1A1}, {C_TOGGLE_BG, 0x005A6F},
  {C_HIT, 0x2AA198}, {C_TOAST_BG, 0x00212B},
  {C_TABS_BG, 0x004052}, {C_TAB_BG, 0x004052}, {C_TAB_FG, 0x93A1A1},
  {C_TAB_ON_BG, 0x002B36}, {C_CRUMB, 0x93A1A1}, {C_ACCENT, 0x2AA198}, {C_MCURSOR, 0xD30102},
  {C_MINIMAP_SLIDER, 0x073642}, {C_THUMB, 0x0E4B58}, {C_THUMB_ON, 0x2AA198},
  {C_BRACKET_MATCH, 0x0E4B58},
  {C_GUIDE, 0x0E4B58}, {C_GUIDE_ON, 0x586E75}, {C_RULER, 0x0E4B58},
  {C_INLAY_FG, 0x657B83}, {C_INLAY_BG, 0x073642}, {C_LENS, 0x657B83}, {C_GHOST, 0x586E75},
  {C_TERM_BG, 0x002B36}, {C_TERM_FG, 0x839496},
  {C_TOK + T_TEXT, 0x839496}, {C_TOK + T_KEYWORD, 0x859900}, {C_TOK + T_STORAGE, 0x93A1A1},
  {C_TOK + T_TYPE, 0xCB4B16}, {C_TOK + T_FUNC, 0x268BD2}, {C_TOK + T_STRING, 0x2AA198},
  {C_TOK + T_NUMBER, 0xD33682}, {C_TOK + T_COMMENT, 0x586E75}, {C_TOK + T_VAR, 0x268BD2},
  {C_TOK + T_CONST, 0xCB4B16}, {C_TOK + T_ESCAPE, 0xDC322F}, {C_TOK + T_HEADING, 0x268BD2},
  {C_TOK + T_WS, 0x0E4B58}
};

static const struct {
  const char *name;
  const Color *over;
  size_t n;
} themes[] = {
  {"Dark Modern", dark_modern, sizeof(dark_modern) / sizeof(Color)},
  {"Dark+", NULL, 0},
  {"Light+", light_plus, sizeof(light_plus) / sizeof(Color)},
  {"Monokai", monokai, sizeof(monokai) / sizeof(Color)},
  {"Solarized Dark", solarized, sizeof(solarized) / sizeof(Color)}
};

#define NTHEME	((int)(sizeof(themes) / sizeof(themes[0])))


/* the styles: each a foreground and a background slot, and attributes */
static const struct {
  unsigned char fg, bg;
  const char *attr;
} style[S_N] = {
  [S_TEXT] = {C_EDITOR_FG, C_EDITOR_BG, ""}, [S_LINE] = {C_EDITOR_FG, C_LINE_BG, ""},
  [S_GUTTER] = {C_GUTTER, C_EDITOR_BG, ""}, [S_GUTTER_CUR] = {C_GUTTER_ON, C_EDITOR_BG, ""},
  [S_SEL] = {C_EDITOR_FG, C_SEL_BG, ""}, [S_MATCH] = {C_EDITOR_FG, C_MATCH_BG, ""},
  [S_CTRL] = {C_CTRL, C_EDITOR_BG, ""},
  [S_STATUS] = {C_STATUS_FG, C_STATUS_BG, ""}, [S_STATUS_ITEM] = {C_STATUS_FG, C_STATUS_ITEM, ""},
  [S_STATUS_DIM] = {C_STATUS_FG, C_STATUS_BG, "2;"},	/* an item that is there but does nothing */
  [S_MENUBAR] = {C_TITLE_FG, C_TITLE_BG, ""}, [S_MENUBAR_ON] = {C_TITLE_FG, C_TITLE_ON, ""},
  [S_MENU] = {C_MENU_FG, C_MENU_BG, ""}, [S_MENU_SEL] = {C_MENU_SEL_FG, C_MENU_SEL_BG, ""},
  [S_MENU_KEY] = {C_MENU_DIM, C_MENU_BG, ""}, [S_MENU_KEY_SEL] = {C_MENU_SEL_FG, C_MENU_SEL_BG, ""},
  [S_MENU_LINE] = {C_MENU_SEP, C_MENU_BG, ""},
  [S_ACT] = {C_ACT_DIM, C_ACT_BG, ""}, [S_ACT_ON] = {C_ACT_FG, C_ACT_BG, ""},
  [S_ACT_BAR] = {C_ACT_FG, C_ACT_BG, ""},
  [S_SIDE] = {C_SIDE_FG, C_SIDE_BG, ""}, [S_SIDE_HEAD] = {C_SIDE_HEAD, C_SIDE_BG, ""},
  [S_SIDE_TITLE] = {C_SIDE_FG, C_SIDE_BG, "1;"}, [S_SIDE_DIR] = {C_SIDE_FG, C_SIDE_BG, ""},
  [S_SIDE_SEL] = {C_LIST_SEL_FG, C_LIST_SEL_BG, ""}, [S_SIDE_CUR] = {C_SIDE_FG, C_LIST_CUR_BG, ""},
  [S_SIDE_ACTIVE] = {C_SIDE_FG, C_LIST_CUR_BG, "1;"}, [S_SIDE_DIM] = {C_DIM, C_SIDE_BG, ""},
  [S_SIDE_HIT] = {C_EDITOR_FG, C_MATCH_BG, ""}, [S_BORDER] = {C_BORDER, C_SIDE_BG, ""},
  [S_INPUT] = {C_INPUT_FG, C_INPUT_BG, ""}, [S_INPUT_ON] = {C_INPUT_FG, C_INPUT_BG, ""},
  [S_INPUT_HINT] = {C_INPUT_HINT, C_INPUT_BG, ""}, [S_TOGGLE_ON] = {C_INPUT_FG, C_TOGGLE_BG, ""},
  [S_BOX] = {C_MENU_FG, C_MENU_BG, ""}, [S_BOX_TITLE] = {C_INPUT_FG, C_INPUT_BG, ""},
  [S_BOX_SEL] = {C_LIST_SEL_FG, C_LIST_SEL_BG, ""}, [S_BOX_DIM] = {C_DIM, C_MENU_BG, ""},
  [S_BOX_HIT] = {C_HIT, C_MENU_BG, "1;"}, [S_BOX_HIT_SEL] = {C_HIT, C_LIST_SEL_BG, "1;"},
  [S_TOAST] = {C_MENU_FG, C_TOAST_BG, ""}, [S_TOAST_WARN] = {C_WARNING, C_TOAST_BG, ""},
  [S_GIT_M] = {C_GIT_M, C_SIDE_BG, ""}, [S_GIT_A] = {C_GIT_A, C_SIDE_BG, ""},
  [S_GIT_D] = {C_GIT_D, C_SIDE_BG, ""}, [S_GIT_U] = {C_GIT_U, C_SIDE_BG, ""},
  [S_ICON_BLUE] = {C_ICON_BLUE, C_SIDE_BG, ""}, [S_ICON_YELLOW] = {C_ICON_YELLOW, C_SIDE_BG, ""},
  [S_ICON_GREEN] = {C_ICON_GREEN, C_SIDE_BG, ""}, [S_ICON_RED] = {C_ICON_RED, C_SIDE_BG, ""},
  [S_ICON_PURPLE] = {C_ICON_PURPLE, C_SIDE_BG, ""}, [S_ICON_ORANGE] = {C_ICON_ORANGE, C_SIDE_BG, ""},
  [S_ICON_GREY] = {C_ICON_GREY, C_SIDE_BG, ""},
  [S_DIFF_ADD] = {C_EDITOR_FG, C_DIFF_ADD, ""}, [S_DIFF_ADD_HI] = {C_EDITOR_FG, C_DIFF_ADD_HI, ""},
  [S_DIFF_DEL] = {C_EDITOR_FG, C_DIFF_DEL, ""}, [S_DIFF_DEL_HI] = {C_EDITOR_FG, C_DIFF_DEL_HI, ""},
  [S_DIFF_FILL] = {C_BORDER, C_EDITOR_BG, ""}, [S_DIFF_HUNK] = {C_GUTTER, C_SIDE_BG, ""},
  [S_DIFF_NUM] = {C_GUTTER, C_EDITOR_BG, ""},
  [S_TABS] = {C_TAB_FG, C_TABS_BG, ""}, [S_TAB] = {C_TAB_FG, C_TAB_BG, ""},
  [S_TAB_ON] = {C_TAB_ON_FG, C_TAB_ON_BG, ""}, [S_CRUMB] = {C_CRUMB, C_EDITOR_BG, ""},
  [S_TAB_PREVIEW] = {C_TAB_FG, C_TAB_BG, "3;"}, [S_APPICON] = {C_ACCENT, C_TITLE_BG, ""},
  [S_PANEL] = {C_EDITOR_FG, C_EDITOR_BG, ""}, [S_PANEL_TAB] = {C_DIM, C_EDITOR_BG, ""},
  [S_PANEL_TAB_ON] = {C_TAB_ON_FG, C_EDITOR_BG, "4;"}, [S_MCURSOR] = {C_EDITOR_BG, C_MCURSOR, ""}
};

/* the token backgrounds, B_* */
static const unsigned char tok_bg[B_N] = {
  C_EDITOR_BG, C_LINE_BG, C_SEL_BG, C_MATCH_BG, C_DIFF_ADD, C_DIFF_ADD_HI, C_DIFF_DEL, C_DIFF_DEL_HI
};

static uint32_t g_color[C_N];
static char g_sgr[S_N][72];
static char g_tok[T_N * B_N][72];
static int g_cur = -1;
static char g_curname[128];	/* its name: the index changes when extensions come and go */

typedef struct Added {	/* an extension's theme */
  char *name;
  int light;
  ThemeLoad load;
  void *arg;
} Added;

static Added *g_added;
static int g_nadded, g_capadded;


void theme_add (const char *name, int light, ThemeLoad load, void *arg) {
  int i;
  for (i = 0; i < NTHEME; i++)
    if (m_stricmp(themes[i].name, name) == 0) return;
  for (i = 0; i < g_nadded; i++)
    if (m_stricmp(g_added[i].name, name) == 0) return;
  if (g_nadded == g_capadded) {
    g_capadded = g_capadded ? g_capadded * 2 : 16;
    g_added = (Added *)xrealloc(g_added, (size_t)g_capadded * sizeof(Added));
  }
  g_added[g_nadded].name = xstrdup(name);
  g_added[g_nadded].light = light;
  g_added[g_nadded].load = load;
  g_added[g_nadded].arg = arg;
  g_nadded++;
}


void theme_clear_added (void) {
  int i;
  for (i = 0; i < g_nadded; i++) free(g_added[i].name);
  g_nadded = 0;
}


uint32_t ui_color (int slot) {
  return g_color[slot];
}


static void make_sgr (char *out, size_t n, const char *attr, uint32_t fg, uint32_t bg) {
  snprintf(out, n, "\033[0;%s38;2;%u;%u;%u;48;2;%u;%u;%um", attr,
           (unsigned)(fg >> 16), (unsigned)((fg >> 8) & 255), (unsigned)(fg & 255),
           (unsigned)(bg >> 16), (unsigned)((bg >> 8) & 255), (unsigned)(bg & 255));
}


/* the theme by its name (or the first one); the screen is sent again */
int theme_set (const char *name) {
  int t, i;
  for (t = 0; t < NTHEME + g_nadded; t++)
    if (name && m_stricmp(theme_name(t), name) == 0) break;
  if (t == NTHEME + g_nadded) t = 0;
  memcpy(g_color, dark_plus, sizeof(g_color));
  if (t < NTHEME)
    for (i = 0; i < (int)themes[t].n; i++) g_color[themes[t].over[i].slot] = themes[t].over[i].rgb;
  else {	/* an extension's: over Dark+ or Light+ */
    const Added *a = &g_added[t - NTHEME];
    if (a->light)
      for (i = 0; i < (int)(sizeof(light_plus) / sizeof(Color)); i++) g_color[light_plus[i].slot] = light_plus[i].rgb;
    a->load(a->arg, g_color);
  }
  theme_customize(g_color, theme_name(t));	/* workbench.colorCustomizations has the last word */
  snprintf(g_curname, sizeof(g_curname), "%s", theme_name(t));
  for (i = 0; i < S_N; i++)
    make_sgr(g_sgr[i], sizeof(g_sgr[i]), style[i].attr, g_color[style[i].fg], g_color[style[i].bg]);
  for (i = 0; i < T_N * B_N; i++)
    make_sgr(g_tok[i], sizeof(g_tok[i]), i / B_N == T_HEADING ? "1;" : "",
             g_color[C_TOK + i / B_N], g_color[tok_bg[i % B_N]]);
  g_cur = t;
  scr_redraw();
  return t;
}


const char *theme_sgr (int st) {
  if (g_cur < 0) theme_set(NULL);
  return st < S_N ? g_sgr[st] : g_tok[st - S_N];
}


/* a token style's colors, for cells that get their own (squiggles) */
void theme_tok (int st, uint32_t *fg, uint32_t *bg) {
  st -= S_N;
  *fg = g_color[C_TOK + st / B_N];
  *bg = g_color[tok_bg[st % B_N]];
}


int theme_count (void) {
  return NTHEME + g_nadded;
}


const char *theme_name (int i) {
  if (i >= 0 && i < NTHEME) return themes[i].name;
  if (i >= NTHEME && i < NTHEME + g_nadded) return g_added[i - NTHEME].name;
  return "";
}


int theme_current (void) {
  int t;
  for (t = 0; t < NTHEME + g_nadded; t++)
    if (strcmp(theme_name(t), g_curname) == 0) return t;
  return g_cur;
}
