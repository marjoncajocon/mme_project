/*
** tfont_ed.c - a second tfont.c: the editor's font
**
** The code editor and the diff viewer have their own font (editor.fontFamily,
** editor.fontSize, VS Code's), the rest of the window mme.ui.fontSize's.
** tfont.c keeps one font in its statics; compiled again under other names
** it keeps a second one, with its own size and its own glyphs.
*/

#define font_add_dir	ed_font_add_dir
#define font_init	ed_font_init
#define font_set_px	ed_font_set_px
#define font_cell_w	ed_font_cell_w
#define font_cell_h	ed_font_cell_h
#define font_ascent	ed_font_ascent
#define font_name	ed_font_name
#define font_glyph	ed_font_glyph
#define font_emoji	ed_font_emoji
#define font_has_ligatures	ed_font_has_ligatures
#define font_shape	ed_font_shape
#define font_glyph_id	ed_font_glyph_id

#include "tfont.c"
