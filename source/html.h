#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// Lightweight HTML renderer: no CSS or JavaScript, just readable text with headings,
// lists, quotes, tables and tappable links. Built once (layout), drawn every frame.
typedef struct Doc Doc;

Doc        *html_layout(const char *html, float width, float scale);
void        html_free(Doc *d);
float       html_height(const Doc *d);
// Draws the part of the document between doc_y and doc_y + view_h at screen (ox, oy).
void        html_draw(const Doc *d, float doc_y, float view_h, float ox, float oy, int focus_link);
int         html_hit(const Doc *d, float x, float y);          // doc coords -> link or -1
int         html_nlinks(const Doc *d);
const char *html_link(const Doc *d, int i);
float       html_link_y(const Doc *d, int i);                  // top of the link's first line
// Next link whose top is >= doc_y (dir 1) or the previous one (dir -1); -1 if none.
int         html_link_near(const Doc *d, int from, int dir, float doc_y, float view_h);

// Plain text of an HTML fragment (tags removed, entities decoded, spaces collapsed).
void        html_to_text(const char *html, char *out, size_t n);

// ---------- pages and forms (browser) ----------
const char *html_title(const Doc *d);                   // <title>, "" if none
const char *html_base(const Doc *d);                    // <base href>, "" if none

typedef enum { HF_TEXT, HF_PASSWORD, HF_TEXTAREA, HF_HIDDEN, HF_CHECK, HF_RADIO, HF_SELECT, HF_SUBMIT, HF_BUTTON } HFType;

// Form controls are reachable as links whose href is "\x01<field index>".
int         html_link_field(const Doc *d, int link);    // field index, or -1 for a normal link
HFType      html_field_type(const Doc *d, int f);
const char *html_field_value(const Doc *d, int f);
const char *html_field_hint(const Doc *d, int f);       // placeholder or name
void        html_field_set(Doc *d, int f, const char *value);   // text fields
void        html_field_toggle(Doc *d, int f);           // checkbox, radio, select (next option)
int         html_field_form(const Doc *d, int f);
int         html_form_text_fields(const Doc *d, int form);
// Builds the submission of the form that owns field `submit` (a button or any field):
// fills url-encoded data, action and method. Returns false when there is no form.
bool        html_form_submit(const Doc *d, int submit, char *data, size_t n, const char **action, bool *post);
