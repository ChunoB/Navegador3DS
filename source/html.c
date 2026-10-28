#include "html.h"
#include "ui.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define F_BOLD   1
#define F_LINK   2
#define F_MUTED  4
#define F_CODE   8
#define F_FIELD  16    // form control drawn as a box
#define F_BUTTON 32

typedef struct {
	float x, y, w, h, scale;
	u32   color;
	int   text;     // offset into the string pool
	short link;
	u8    flags;
} Run;

typedef struct { float x, y, w, h; u32 color; } Rect;

typedef struct {
	int    form;          // -1 when outside a <form>
	HFType type;
	char  *name, *value, *placeholder;
	bool   checked;
	int    run;           // run that shows it (-1 if hidden)
	int    link;          // link index used for focus/taps
	char **opt_val, **opt_txt;
	int    nopt, sel;
} Field;

typedef struct { char *action; bool post; } Form;

struct Doc {
	Run   *runs;   int nruns, cruns;
	Rect  *rects;  int nrects, crects;
	char  *pool;   size_t npool, cpool;
	int   *links;  int nlinks, clinks;    // pool offsets of hrefs
	float *link_y;
	float  width, height;
	Field *fields; int nfields, cfields;
	Form  *forms;  int nforms, cforms;
	char   title[160];
	char   base[512];
};

// ---------------------------------------------------------------- growable arrays

static bool grow(void **p, int *cap, int need, size_t elem)
{
	if (need <= *cap) return true;
	int nc = *cap ? *cap * 2 : 64;
	while (nc < need) nc *= 2;
	void *np = realloc(*p, nc * elem);
	if (!np) return false;
	*p = np;
	*cap = nc;
	return true;
}

static int pool_add(Doc *d, const char *s, size_t n)
{
	if (d->npool + n + 1 > d->cpool) {
		size_t nc = d->cpool ? d->cpool * 2 : 8192;
		while (nc < d->npool + n + 1) nc *= 2;
		char *np = realloc(d->pool, nc);
		if (!np) return -1;
		d->pool = np;
		d->cpool = nc;
	}
	int off = (int)d->npool;
	memcpy(d->pool + off, s, n);
	d->pool[off + n] = 0;
	d->npool += n + 1;
	return off;
}

// ---------------------------------------------------------------- entities

typedef struct { const char *name; unsigned cp; } Ent;
static const Ent ents[] = {
	{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
	{"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026}, {"laquo", 0xAB}, {"raquo", 0xBB},
	{"ldquo", 0x201C}, {"rdquo", 0x201D}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"bull", 0x2022},
	{"middot", 0xB7}, {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"euro", 0x20AC},
	{"deg", 0xB0}, {"ordf", 0xAA}, {"ordm", 0xBA}, {"sect", 0xA7}, {"para", 0xB6}, {"times", 0xD7},
	{"divide", 0xF7}, {"plusmn", 0xB1}, {"frac12", 0xBD}, {"frac14", 0xBC}, {"sup2", 0xB2},
	{"sup3", 0xB3}, {"iexcl", 0xA1}, {"iquest", 0xBF}, {"rarr", 0x2192}, {"larr", 0x2190},
	{"shy", 0}, {"zwnj", 0}, {"zwj", 0}, {"thinsp", ' '}, {"ensp", ' '}, {"emsp", ' '},
	{"Agrave", 0xC0}, {"Aacute", 0xC1}, {"Acirc", 0xC2}, {"Atilde", 0xC3}, {"Auml", 0xC4},
	{"Ccedil", 0xC7}, {"Egrave", 0xC8}, {"Eacute", 0xC9}, {"Ecirc", 0xCA}, {"Euml", 0xCB},
	{"Igrave", 0xCC}, {"Iacute", 0xCD}, {"Icirc", 0xCE}, {"Iuml", 0xCF}, {"Ntilde", 0xD1},
	{"Ograve", 0xD2}, {"Oacute", 0xD3}, {"Ocirc", 0xD4}, {"Otilde", 0xD5}, {"Ouml", 0xD6},
	{"Ugrave", 0xD9}, {"Uacute", 0xDA}, {"Ucirc", 0xDB}, {"Uuml", 0xDC}, {"agrave", 0xE0},
	{"aacute", 0xE1}, {"acirc", 0xE2}, {"atilde", 0xE3}, {"auml", 0xE4}, {"ccedil", 0xE7},
	{"egrave", 0xE8}, {"eacute", 0xE9}, {"ecirc", 0xEA}, {"euml", 0xEB}, {"igrave", 0xEC},
	{"iacute", 0xED}, {"icirc", 0xEE}, {"iuml", 0xEF}, {"ntilde", 0xF1}, {"ograve", 0xF2},
	{"oacute", 0xF3}, {"ocirc", 0xF4}, {"otilde", 0xF5}, {"ouml", 0xF6}, {"ugrave", 0xF9},
	{"uacute", 0xFA}, {"ucirc", 0xFB}, {"uuml", 0xFC}, {"yacute", 0xFD}, {"szlig", 0xDF},
};

static int put_utf8(char *o, unsigned cp)
{
	if (cp < 0x80) { o[0] = cp; return 1; }
	if (cp < 0x800) { o[0] = 0xC0 | (cp >> 6); o[1] = 0x80 | (cp & 63); return 2; }
	if (cp < 0x10000) { o[0] = 0xE0 | (cp >> 12); o[1] = 0x80 | ((cp >> 6) & 63); o[2] = 0x80 | (cp & 63); return 3; }
	o[0] = 0xF0 | (cp >> 18); o[1] = 0x80 | ((cp >> 12) & 63); o[2] = 0x80 | ((cp >> 6) & 63); o[3] = 0x80 | (cp & 63);
	return 4;
}

// Decodes the entity at s ('&...'); writes UTF-8 to out, returns bytes consumed (0 = not an entity).
static int entity(const char *s, char *out, int *outn)
{
	const char *semi = memchr(s, ';', 12);
	if (!semi) return 0;
	unsigned cp;
	if (s[1] == '#') {
		cp = (s[2] == 'x' || s[2] == 'X') ? strtoul(s + 3, NULL, 16) : strtoul(s + 2, NULL, 10);
		if (cp == 0 || cp > 0x10FFFF) cp = '?';
		if (cp == 0xA0) cp = ' ';
	} else {
		size_t l = semi - s - 1;
		const Ent *e = NULL;
		for (size_t i = 0; i < sizeof ents / sizeof *ents; i++)
			if (strlen(ents[i].name) == l && !strncmp(ents[i].name, s + 1, l)) { e = &ents[i]; break; }
		if (!e) return 0;
		cp = e->cp;
	}
	*outn = cp ? put_utf8(out, cp) : 0;
	return (int)(semi - s + 1);
}

// ---------------------------------------------------------------- layout state

typedef struct { char name[12]; int skip_depth; } SkipT;

typedef struct {
	Doc  *d;
	float width, base;
	float x, y, indent, line_h;
	int   line_first;        // first run of the current line
	bool  space;             // a space is pending before the next word
	float margin;            // vertical space already added (margins collapse)
	int   bold, muted, code, pre, heading;
	int   link;
	// lists: type 0 = ul, 1 = ol, with their counters
	int   list_type[16], list_n[16], nlist;
	// skipped elements (script, style, hidden...)
	char  skip_name[16];
	int   skip_depth;
	// table state
	bool  cell_started;
	float pre_start;
	float quote_start[8]; int nquote;
	// current run being built
	char  buf[512];
	int   nbuf;
	float run_x, run_w;
	float scale;
	u32   color;
	u8    flags;
	// forms
	int   form;              // current <form> or -1
	int   sel_field;         // <select> being read, or -1
	bool  in_option, opt_selected, opt_has_val;
	char  opt_val[200];
	int   cap_field;         // <textarea>/<button> whose text is being captured, or -1
	char  cap[600];          // captured text (option, textarea, button, title)
	int   ncap;
	bool  in_title;
} L;

static float lh_for(float scale) { return 30.0f * scale + 3; }

static float cur_scale(L *l)
{
	static const float hs[7] = { 1, 1.45f, 1.28f, 1.14f, 1.06f, 1, 1 };
	return l->base * (l->heading ? hs[l->heading] : 1) * (l->code ? 0.95f : 1);
}

static void flush_run(L *l)
{
	if (!l->nbuf) return;
	Doc *d = l->d;
	if (!grow((void **)&d->runs, &d->cruns, d->nruns + 1, sizeof(Run))) { l->nbuf = 0; return; }
	int off = pool_add(d, l->buf, l->nbuf);
	if (off >= 0) {
		Run *r = &d->runs[d->nruns++];
		r->x = l->run_x;
		r->y = l->y;
		r->w = l->run_w;
		r->scale = l->scale;
		r->h = lh_for(l->scale);
		r->color = l->color;
		r->text = off;
		r->link = l->flags & F_LINK ? l->link : -1;
		r->flags = l->flags;
		if (r->h > l->line_h) l->line_h = r->h;
	}
	l->nbuf = 0;
}

static void style_now(L *l, float *scale, u32 *color, u8 *flags)
{
	*scale = cur_scale(l);
	*flags = (l->bold || l->heading ? F_BOLD : 0) | (l->link >= 0 ? F_LINK : 0) | (l->muted ? F_MUTED : 0) | (l->code ? F_CODE : 0);
	*color = l->link >= 0 ? C_LINK : l->muted ? C_SUB : l->code ? C2D_Color32(120, 40, 90, 255) : C_TEXT;
}

static void newline(L *l, bool force)
{
	flush_run(l);
	Doc *d = l->d;
	if (l->x <= l->indent + 0.1f && !force) { l->space = false; return; }
	float h = l->line_h > 0 ? l->line_h : lh_for(cur_scale(l));
	// align runs of this line on a common baseline
	for (int i = l->line_first; i < d->nruns; i++) d->runs[i].y = l->y + (h - d->runs[i].h) * 0.8f;
	l->y += h;
	l->x = l->indent;
	l->line_h = 0;
	l->line_first = d->nruns;
	l->space = false;
	l->margin = 0;
}

static void vspace(L *l, float px)
{
	newline(l, false);
	if (l->margin < px) { l->y += px - l->margin; l->margin = px; }
}

static void add_rect(L *l, float x, float y, float w, float h, u32 c)
{
	Doc *d = l->d;
	if (!grow((void **)&d->rects, &d->crects, d->nrects + 1, sizeof(Rect))) return;
	d->rects[d->nrects++] = (Rect){ x, y, w, h, c };
}

static float word_w(const char *s, int n, float scale)
{
	float w = 0;
	const char *e = s + n;
	while (s < e) {
		u32 cp;
		s += text_next_cp(s, &cp);
		w += text_cp_width(cp);
	}
	return w * scale;
}

// Places one word (no spaces) at the cursor, wrapping and splitting as needed.
static void place_word(L *l, const char *w, int n)
{
	float scale; u32 color; u8 flags;
	style_now(l, &scale, &color, &flags);
	if (l->nbuf && (scale != l->scale || color != l->color || flags != l->flags)) flush_run(l);
	float sp = l->space && l->x > l->indent ? text_cp_width(' ') * scale : 0;
	float ww = word_w(w, n, scale);
	float avail = l->width - l->x;
	if (sp + ww > avail && l->x > l->indent) {
		newline(l, false);
		sp = 0;
		avail = l->width - l->x;
	}
	if (ww > l->width - l->indent) {
		// a very long word (URL...): split it by characters
		int i = 0;
		while (i < n) {
			int j = i;
			float acc = 0;
			while (j < n) {
				u32 cp;
				int k = text_next_cp(w + j, &cp);
				float cw = text_cp_width(cp) * scale;
				if (acc + cw > l->width - l->x && j > i) break;
				acc += cw;
				j += k;
			}
			l->space = false;
			place_word(l, w + i, j - i);
			i = j;
			if (i < n) newline(l, false);
		}
		return;
	}
	float start = l->x + sp;
	if (l->nbuf && l->nbuf + n + 1 >= (int)sizeof l->buf - 1) flush_run(l);
	if (!l->nbuf) {
		// new run: the space becomes a gap before it
		l->run_x = start;
		l->run_w = 0;
		l->scale = scale;
		l->color = color;
		l->flags = flags;
	} else if (sp > 0) {
		// same run: the space is part of the text
		l->buf[l->nbuf++] = ' ';
		l->run_w += sp;
	}
	memcpy(l->buf + l->nbuf, w, n);
	l->nbuf += n;
	l->run_w += ww;
	l->x = start + ww;
	l->space = false;
	l->margin = 0;
}

// Collects text (with entities decoded and spaces collapsed) into l->cap.
static void capture(L *l, const char *s, int n)
{
	for (int i = 0; i < n && l->ncap < (int)sizeof l->cap - 5; i++) {
		char c = s[i];
		if (c == '\n' || c == '\r' || c == '\t') c = ' ';
		if (c == ' ' && (l->ncap == 0 || l->cap[l->ncap - 1] == ' ')) continue;
		if (c == '&') {
			char e[8]; int en = 0;
			int used = entity(s + i, e, &en);
			if (used) {
				for (int k = 0; k < en && l->ncap < (int)sizeof l->cap - 5; k++) l->cap[l->ncap++] = e[k];
				i += used - 1;
				continue;
			}
		}
		l->cap[l->ncap++] = c;
	}
	l->cap[l->ncap] = 0;
}

static void cap_trim(L *l)
{
	while (l->ncap && l->cap[l->ncap - 1] == ' ') l->cap[--l->ncap] = 0;
	l->cap[l->ncap] = 0;
}

static void put_text(L *l, const char *s, int n)
{
	if (l->in_title || l->in_option || l->cap_field >= 0) { capture(l, s, n); return; }
	if (l->sel_field >= 0) return;   // stray text inside <select>
	if (l->skip_depth) return;
	char word[512];
	int wn = 0;
	for (int i = 0; i <= n; i++) {
		char c = i < n ? s[i] : ' ';
		bool white = c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f';
		if (l->pre && c == '\n' && i < n) {
			if (wn) { place_word(l, word, wn); wn = 0; }
			newline(l, true);
			continue;
		}
		if (white) {
			if (wn) { place_word(l, word, wn); wn = 0; }
			if (i < n) {
				if (l->pre && c == ' ') { place_word(l, " ", 1); }
				else l->space = true;
			}
			continue;
		}
		if (c == '&') {
			char e[8];
			int en = 0;
			int used = entity(s + i, e, &en);
			if (used) {
				if (en == 1 && e[0] == ' ') {
					if (wn) { place_word(l, word, wn); wn = 0; }
					l->space = true;
				} else {
					for (int k = 0; k < en && wn < (int)sizeof word - 1; k++) word[wn++] = e[k];
				}
				i += used - 1;
				continue;
			}
		}
		if (wn < (int)sizeof word - 1) word[wn++] = c;
		else { place_word(l, word, wn); wn = 0; word[wn++] = c; }
	}
}

// ---------------------------------------------------------------- tags

static bool is_void(const char *t)
{
	static const char *v[] = { "br", "img", "hr", "input", "meta", "link", "source", "wbr", "area", "col", "embed", "param", "track", "base" };
	for (size_t i = 0; i < sizeof v / sizeof *v; i++) if (!strcmp(t, v[i])) return true;
	return false;
}

static bool is_block(const char *t)
{
	static const char *b[] = { "p", "div", "section", "article", "header", "footer", "main", "nav", "aside", "form",
		"figure", "figcaption", "dl", "dt", "dd", "address", "details", "summary", "center", "fieldset", "legend",
		"caption", "tbody", "thead", "tfoot", "table" };
	for (size_t i = 0; i < sizeof b / sizeof *b; i++) if (!strcmp(t, b[i])) return true;
	return false;
}

// Gets attribute value from the raw attribute text (case-insensitive name).
static bool attr(const char *a, int an, const char *name, char *out, size_t n)
{
	size_t nl = strlen(name);
	for (int i = 0; i + (int)nl <= an; i++) {
		if (strncasecmp(a + i, name, nl)) continue;
		if (i > 0 && !isspace((unsigned char)a[i - 1])) continue;
		int j = i + nl;
		while (j < an && isspace((unsigned char)a[j])) j++;
		if (j >= an || a[j] != '=') continue;
		j++;
		while (j < an && isspace((unsigned char)a[j])) j++;
		char q = j < an && (a[j] == '"' || a[j] == '\'') ? a[j++] : 0;
		size_t o = 0;
		while (j < an && o + 1 < n) {
			if (q ? a[j] == q : (isspace((unsigned char)a[j]) || a[j] == '>')) break;
			if (a[j] == '&') {
				char e[8]; int en = 0;
				int used = entity(a + j, e, &en);
				if (used) { for (int k = 0; k < en && o + 1 < n; k++) out[o++] = e[k]; j += used; continue; }
			}
			out[o++] = a[j++];
		}
		out[o] = 0;
		return true;
	}
	return false;
}

static int add_link(L *l, const char *href)
{
	Doc *d = l->d;
	if (!grow((void **)&d->links, &d->clinks, d->nlinks + 1, sizeof(int))) return -1;
	int off = pool_add(d, href, strlen(href));
	if (off < 0) return -1;
	d->links[d->nlinks] = off;
	return d->nlinks++;
}

// Boolean attribute (e.g. checked, selected, hidden), with or without a value.
static bool has_attr(const char *a, int an, const char *name)
{
	size_t nl = strlen(name);
	for (int i = 0; i + (int)nl <= an; i++) {
		if (strncasecmp(a + i, name, nl)) continue;
		if (i > 0 && !isspace((unsigned char)a[i - 1])) continue;
		char c = i + (int)nl < an ? a[i + nl] : ' ';
		if (c == ' ' || c == '=' || c == '/' || c == '>' || c == '\t' || c == '\n' || c == '\r') return true;
	}
	return false;
}

static char *sdup(const char *s) { return strdup(s ? s : ""); }

static int add_field(L *l, HFType t, const char *name, const char *value, const char *ph)
{
	Doc *d = l->d;
	if (!grow((void **)&d->fields, &d->cfields, d->nfields + 1, sizeof(Field))) return -1;
	Field *f = &d->fields[d->nfields];
	memset(f, 0, sizeof *f);
	f->form = l->form;
	f->type = t;
	f->name = sdup(name);
	f->value = sdup(value);
	f->placeholder = sdup(ph);
	f->run = f->link = -1;
	return d->nfields++;
}

// What the control shows: its value, the placeholder (muted), a check mark...
static void field_display(const Field *f, char *out, size_t n, bool *muted)
{
	*muted = false;
	switch (f->type) {
	case HF_CHECK: snprintf(out, n, "%s", f->checked ? "[x]" : "[  ]"); break;
	case HF_RADIO: snprintf(out, n, "%s", f->checked ? "(o)" : "(  )"); break;
	case HF_SELECT: snprintf(out, n, "%s  v", f->nopt ? f->opt_txt[f->sel] : ""); break;
	case HF_SUBMIT: case HF_BUTTON:
		snprintf(out, n, "%s", f->placeholder[0] ? f->placeholder : f->value[0] ? f->value : f->type == HF_SUBMIT ? "Enviar" : "Botão");
		break;
	case HF_PASSWORD:
		if (f->value[0]) { size_t k = strlen(f->value); size_t o = 0; while (k-- && o + 4 < n) { memcpy(out + o, "\xE2\x80\xA2", 3); o += 3; } out[o] = 0; }
		else { snprintf(out, n, "%s", f->placeholder[0] ? f->placeholder : "senha"); *muted = true; }
		break;
	default:
		if (f->value[0]) {
			// first line only
			size_t k = 0;
			while (f->value[k] && f->value[k] != '\n' && k + 1 < n) { out[k] = f->value[k]; k++; }
			out[k] = 0;
		} else { snprintf(out, n, "%s", f->placeholder); *muted = true; }
	}
}

static void place_field(L *l, int fi)
{
	Doc *d = l->d;
	if (fi < 0) return;
	flush_run(l);
	Field *f = &d->fields[fi];
	char disp[300], href[16];
	bool muted;
	field_display(f, disp, sizeof disp, &muted);
	float sc = l->base;
	float avail = l->width - l->indent;
	float w;
	bool button = f->type == HF_SUBMIT || f->type == HF_BUTTON;
	if (f->type == HF_CHECK || f->type == HF_RADIO) w = word_w(disp, strlen(disp), sc) + 4;
	else if (button) w = word_w(disp, strlen(disp), sc) + 18;
	else w = avail * 0.72f < 120 ? 120 : avail * 0.72f;
	if (w > avail) w = avail;
	float sp = l->space && l->x > l->indent ? text_cp_width(' ') * sc : 0;
	if (l->x + sp + w > l->width && l->x > l->indent) { newline(l, false); sp = 0; }
	snprintf(href, sizeof href, "\x01%d", fi);
	f->link = add_link(l, href);
	if (!grow((void **)&d->runs, &d->cruns, d->nruns + 1, sizeof(Run))) return;
	int off = pool_add(d, disp, strlen(disp));
	if (off < 0) return;
	Run *r = &d->runs[d->nruns];
	r->x = l->x + sp;
	r->y = l->y;
	r->w = w;
	r->h = lh_for(sc) + 2;
	r->scale = sc;
	r->color = button ? C_TEXT : muted ? C_SUB : C_TEXT;
	r->text = off;
	r->link = f->link;
	r->flags = F_FIELD | (button ? F_BUTTON : 0) | (muted ? F_MUTED : 0) | (f->type == HF_CHECK || f->type == HF_RADIO ? F_LINK : 0);
	f->run = d->nruns++;
	if (r->h > l->line_h) l->line_h = r->h;
	l->x += sp + w;
	l->space = true;
	l->margin = 0;
}

static void finish_option(L *l)
{
	if (!l->in_option || l->sel_field < 0) return;
	l->in_option = false;
	cap_trim(l);
	Field *f = &l->d->fields[l->sel_field];
	char **nv = realloc(f->opt_val, (f->nopt + 1) * sizeof(char *));
	if (!nv) return;
	f->opt_val = nv;
	char **nt = realloc(f->opt_txt, (f->nopt + 1) * sizeof(char *));
	if (!nt) return;
	f->opt_txt = nt;
	f->opt_val[f->nopt] = sdup(l->opt_has_val ? l->opt_val : l->cap);
	f->opt_txt[f->nopt] = sdup(l->cap);
	if (l->opt_selected) f->sel = f->nopt;
	f->nopt++;
	l->ncap = 0;
}

static void open_tag(L *l, const char *t, const char *a, int an, bool self_close)
{
	char v[1024];
	Doc *d = l->d;
	// page title and base URL live in <head>, which is otherwise skipped
	if (!strcmp(t, "title")) { if (!d->title[0]) { l->in_title = true; l->ncap = 0; } return; }
	if (!strcmp(t, "base")) { if (!d->base[0] && attr(a, an, "href", v, sizeof v)) snprintf(d->base, sizeof d->base, "%s", v); return; }
	// inside <select> only options matter; inside <textarea>/<button> only text
	if (l->sel_field >= 0) {
		if (!strcmp(t, "option")) {
			finish_option(l);
			l->in_option = true;
			l->ncap = 0;
			l->opt_has_val = attr(a, an, "value", l->opt_val, sizeof l->opt_val);
			l->opt_selected = has_attr(a, an, "selected");
		}
		return;
	}
	if (l->cap_field >= 0) {
		if (!strcmp(t, "img") && attr(a, an, "alt", v, sizeof v)) capture(l, v, strlen(v));
		return;
	}
	if (l->skip_depth) {
		if (!strcmp(t, l->skip_name) && !self_close && !is_void(t)) l->skip_depth++;
		return;
	}
	// things that are never shown
	bool hidden = !strcmp(t, "script") || !strcmp(t, "style") || !strcmp(t, "head") || !strcmp(t, "template") ||
	              !strcmp(t, "svg") || !strcmp(t, "math") || !strcmp(t, "object") || !strcmp(t, "dialog");
	if (!hidden && attr(a, an, "class", v, sizeof v) && (strstr(v, "screenreader-only") || strstr(v, "hidden-readable")))
		hidden = true;
	if (!hidden && attr(a, an, "style", v, sizeof v)) {
		char *s = strstr(v, "display");
		if (s) { s += 7; while (*s == ' ' || *s == ':') s++; if (!strncmp(s, "none", 4)) hidden = true; }
	}
	if (!hidden && has_attr(a, an, "hidden")) hidden = true;
	if (!hidden && attr(a, an, "aria-hidden", v, sizeof v) && !strcmp(v, "true") && strcmp(t, "a")) hidden = true;
	if (hidden && !self_close && !is_void(t)) {
		snprintf(l->skip_name, sizeof l->skip_name, "%s", t);
		l->skip_depth = 1;
		return;
	}
	if (hidden) return;

	float b = l->base;
	if (!strcmp(t, "br")) { flush_run(l); newline(l, true); return; }
	if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && !t[2]) { vspace(l, 10 * b * 2); l->heading = t[1] - '0'; return; }
	if (!strcmp(t, "p")) { vspace(l, 8 * b * 2); return; }
	if (!strcmp(t, "ul") || !strcmp(t, "ol")) {
		vspace(l, l->nlist ? 0 : 6 * b * 2);
		if (l->nlist < 16) { l->list_type[l->nlist] = t[0] == 'o'; l->list_n[l->nlist] = 0; l->nlist++; }
		l->indent += 16;
		l->x = l->indent;
		return;
	}
	if (!strcmp(t, "li")) {
		newline(l, false);
		char mark[16] = "\xE2\x80\xA2";   // bullet
		if (l->nlist && l->list_type[l->nlist - 1]) snprintf(mark, sizeof mark, "%d.", ++l->list_n[l->nlist - 1]);
		float sc = cur_scale(l);
		float mw = word_w(mark, strlen(mark), sc);
		float saved_x = l->x;
		l->x = l->indent - mw - 5;
		if (l->x < 0) l->x = 0;
		int muted = l->muted; l->muted = 1;
		place_word(l, mark, strlen(mark));
		flush_run(l);
		l->muted = muted;
		l->x = saved_x > l->indent ? saved_x : l->indent;
		l->space = false;
		return;
	}
	if (!strcmp(t, "blockquote")) {
		vspace(l, 6);
		if (l->nquote < 8) l->quote_start[l->nquote++] = l->y;
		l->indent += 12;
		l->x = l->indent;
		l->muted++;
		return;
	}
	if (!strcmp(t, "pre")) { vspace(l, 6); l->pre++; l->code++; l->pre_start = l->y; return; }
	if (!strcmp(t, "code") || !strcmp(t, "kbd") || !strcmp(t, "samp")) { l->code++; return; }
	if (!strcmp(t, "hr")) {
		vspace(l, 6);
		add_rect(l, l->indent, l->y + 2, l->width - l->indent, 1, C_LINE);
		l->y += 6;
		return;
	}
	if (!strcmp(t, "b") || !strcmp(t, "strong") || !strcmp(t, "th") || !strcmp(t, "dt")) l->bold++;
	if (!strcmp(t, "small") || !strcmp(t, "figcaption") || !strcmp(t, "caption")) l->muted++;
	if (!strcmp(t, "tr")) { newline(l, false); l->cell_started = false; return; }
	if (!strcmp(t, "td") || !strcmp(t, "th")) {
		if (l->cell_started) { int m = l->muted; l->muted = 1; l->space = true; place_word(l, "|", 1); l->muted = m; l->space = true; }
		l->cell_started = true;
		if (!strcmp(t, "th")) return;   // bold already counted
		return;
	}
	if (!strcmp(t, "a")) {
		if (attr(a, an, "href", v, sizeof v) && v[0] && strncmp(v, "javascript:", 11)) {
			if (l->space) { /* keep pending space before the link */ }
			l->link = add_link(l, v);
		}
		return;
	}
	if (!strcmp(t, "img")) {
		char alt[200] = "";
		attr(a, an, "alt", alt, sizeof alt);
		// decorative images are left out, unless they are the only thing in a link
		if (!alt[0] && l->link < 0) return;
		char label[240];
		snprintf(label, sizeof label, "[imagem%s%s]", alt[0] ? ": " : "", alt);
		int m = l->muted; l->muted = 1;
		l->space = true;
		put_text(l, label, strlen(label));
		l->muted = m;
		l->space = true;
		return;
	}
	if (!strcmp(t, "iframe") || !strcmp(t, "video") || !strcmp(t, "audio") || !strcmp(t, "embed")) {
		newline(l, false);
		int old = l->link;
		if (attr(a, an, "src", v, sizeof v) && v[0]) l->link = add_link(l, v);
		const char *lbl = !strcmp(t, "iframe") ? "[conteúdo incorporado]" : !strcmp(t, "audio") ? "[áudio]" : "[vídeo]";
		put_text(l, lbl, strlen(lbl));
		flush_run(l);
		l->link = old;
		newline(l, false);
		if (strcmp(t, "embed") && !self_close) {   // skip fallback content inside
			snprintf(l->skip_name, sizeof l->skip_name, "%s", t);
			l->skip_depth = 1;
		}
		return;
	}
	if (!strcmp(t, "form")) {
		newline(l, false);
		if (grow((void **)&d->forms, &d->cforms, d->nforms + 1, sizeof(Form))) {
			Form *fm = &d->forms[d->nforms];
			fm->action = sdup(attr(a, an, "action", v, sizeof v) ? v : "");
			fm->post = attr(a, an, "method", v, sizeof v) && !strcasecmp(v, "post");
			l->form = d->nforms++;
		}
		return;
	}
	if (!strcmp(t, "input")) {
		char type[24] = "text", name[128] = "", val[600] = "", ph[200] = "";
		attr(a, an, "type", type, sizeof type);
		for (char *p = type; *p; p++) *p = tolower((unsigned char)*p);
		attr(a, an, "name", name, sizeof name);
		attr(a, an, "value", val, sizeof val);
		if (!attr(a, an, "placeholder", ph, sizeof ph)) attr(a, an, "aria-label", ph, sizeof ph);
		HFType ft = !strcmp(type, "hidden") ? HF_HIDDEN : !strcmp(type, "password") ? HF_PASSWORD :
		            !strcmp(type, "checkbox") ? HF_CHECK : !strcmp(type, "radio") ? HF_RADIO :
		            !strcmp(type, "submit") || !strcmp(type, "image") ? HF_SUBMIT :
		            !strcmp(type, "button") || !strcmp(type, "reset") || !strcmp(type, "file") ? HF_BUTTON : HF_TEXT;
		if (!strcmp(type, "image") && !val[0]) attr(a, an, "alt", val, sizeof val);
		int fi = add_field(l, ft, name, ft == HF_CHECK || ft == HF_RADIO ? (val[0] ? val : "on") : val, ph);
		if (fi < 0) return;
		if (ft == HF_CHECK || ft == HF_RADIO) d->fields[fi].checked = has_attr(a, an, "checked");
		if (ft != HF_HIDDEN) place_field(l, fi);
		return;
	}
	if (!strcmp(t, "textarea") || !strcmp(t, "button")) {
		char name[128] = "", val[600] = "", ph[200] = "", type[24] = "submit";
		attr(a, an, "name", name, sizeof name);
		attr(a, an, "value", val, sizeof val);
		attr(a, an, "placeholder", ph, sizeof ph);
		attr(a, an, "type", type, sizeof type);
		HFType ft = t[0] == 't' ? HF_TEXTAREA : !strcasecmp(type, "submit") ? HF_SUBMIT : HF_BUTTON;
		int fi = add_field(l, ft, name, val, ph);
		if (self_close) { place_field(l, fi); return; }
		l->cap_field = fi;
		l->ncap = 0;
		return;
	}
	if (!strcmp(t, "select")) {
		char name[128] = "";
		attr(a, an, "name", name, sizeof name);
		l->sel_field = add_field(l, HF_SELECT, name, "", "");
		l->ncap = 0;
		return;
	}
	if (!strcmp(t, "dd")) { newline(l, false); l->indent += 12; l->x = l->indent; return; }
	if (is_block(t)) newline(l, false);
}

static void close_tag(L *l, const char *t)
{
	Doc *d = l->d;
	if (!strcmp(t, "title") && l->in_title) {
		cap_trim(l);
		snprintf(d->title, sizeof d->title, "%s", l->cap);
		l->in_title = false;
		l->ncap = 0;
		return;
	}
	if (l->sel_field >= 0) {
		if (!strcmp(t, "option")) finish_option(l);
		else if (!strcmp(t, "select")) {
			finish_option(l);
			int fi = l->sel_field;
			l->sel_field = -1;
			place_field(l, fi);
		}
		return;
	}
	if (l->cap_field >= 0) {
		if (!strcmp(t, "textarea") || !strcmp(t, "button")) {
			int fi = l->cap_field;
			l->cap_field = -1;
			cap_trim(l);
			Field *f = &d->fields[fi];
			if (f->type == HF_TEXTAREA) { free(f->value); f->value = sdup(l->cap); }
			else { free(f->placeholder); f->placeholder = sdup(l->cap); }   // button label
			l->ncap = 0;
			place_field(l, fi);
		}
		return;
	}
	if (!strcmp(t, "form")) { newline(l, false); l->form = -1; return; }
	if (l->skip_depth) {
		if (!strcmp(t, l->skip_name) && --l->skip_depth == 0) l->skip_name[0] = 0;
		return;
	}
	float b = l->base;
	if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && !t[2]) { flush_run(l); vspace(l, 6 * b * 2); l->heading = 0; return; }
	if (!strcmp(t, "p")) { vspace(l, 8 * b * 2); return; }
	if (!strcmp(t, "ul") || !strcmp(t, "ol")) {
		newline(l, false);
		if (l->nlist) l->nlist--;
		l->indent -= 16;
		if (l->indent < 0) l->indent = 0;
		l->x = l->indent;
		if (!l->nlist) vspace(l, 6 * b * 2);
		return;
	}
	if (!strcmp(t, "li")) { newline(l, false); return; }
	if (!strcmp(t, "blockquote")) {
		newline(l, false);
		l->indent -= 12;
		if (l->indent < 0) l->indent = 0;
		l->x = l->indent;
		if (l->muted) l->muted--;
		if (l->nquote) { float y0 = l->quote_start[--l->nquote]; add_rect(l, l->indent + 2, y0, 3, l->y - y0, C_LINE); }
		vspace(l, 6);
		return;
	}
	if (!strcmp(t, "pre")) {
		newline(l, false);
		if (l->pre) l->pre--;
		if (l->code) l->code--;
		add_rect(l, 0, l->pre_start - 2, l->width, l->y - l->pre_start + 4, C2D_Color32(240, 240, 244, 255));
		vspace(l, 6);
		return;
	}
	if (!strcmp(t, "code") || !strcmp(t, "kbd") || !strcmp(t, "samp")) { if (l->code) l->code--; return; }
	if (!strcmp(t, "b") || !strcmp(t, "strong") || !strcmp(t, "th") || !strcmp(t, "dt")) { if (l->bold) l->bold--; }
	if (!strcmp(t, "small") || !strcmp(t, "figcaption") || !strcmp(t, "caption")) { if (l->muted) l->muted--; }
	if (!strcmp(t, "a")) { flush_run(l); l->link = -1; return; }
	if (!strcmp(t, "dd")) { newline(l, false); l->indent -= 12; if (l->indent < 0) l->indent = 0; l->x = l->indent; return; }
	if (!strcmp(t, "tr") || !strcmp(t, "table")) { newline(l, false); if (!strcmp(t, "table")) vspace(l, 6); return; }
	if (is_block(t)) newline(l, false);
}

// ---------------------------------------------------------------- public

Doc *html_layout(const char *html, float width, float scale)
{
	Doc *d = calloc(1, sizeof(Doc));
	if (!d) return NULL;
	L *l = calloc(1, sizeof(L));
	if (!l) { free(d); return NULL; }
	l->d = d;
	l->width = d->width = width;
	l->base = scale;
	l->link = -1;
	l->form = l->sel_field = l->cap_field = -1;

	const char *p = html ? html : "";
	while (*p) {
		if (*p != '<') {
			const char *e = strchr(p, '<');
			if (!e) e = p + strlen(p);
			put_text(l, p, (int)(e - p));
			p = e;
			continue;
		}
		if (!strncmp(p, "<!--", 4)) {
			const char *e = strstr(p + 4, "-->");
			p = e ? e + 3 : p + strlen(p);
			continue;
		}
		if (p[1] == '!' || p[1] == '?') {
			const char *e = strchr(p, '>');
			p = e ? e + 1 : p + strlen(p);
			continue;
		}
		bool closing = p[1] == '/';
		const char *s = p + 1 + closing;
		if (!isalpha((unsigned char)*s)) {   // a lone '<' in the text
			put_text(l, "<", 1);
			p++;
			continue;
		}
		char name[16];
		int nn = 0;
		while (*s && (isalnum((unsigned char)*s) || *s == '-' || *s == ':') && nn < 15) name[nn++] = tolower((unsigned char)*s++);
		name[nn] = 0;
		while (*s && (isalnum((unsigned char)*s) || *s == '-' || *s == ':')) s++;
		// find the end of the tag, honoring quoted attribute values
		const char *e = s;
		char q = 0;
		while (*e && (q || *e != '>')) {
			if (q) { if (*e == q) q = 0; }
			else if (*e == '"' || *e == '\'') q = *e;
			e++;
		}
		bool self_close = e > s && e[-1] == '/';
		if (closing) close_tag(l, name);
		else {
			open_tag(l, name, s, (int)(e - s), self_close);
			// raw-text elements: jump straight to their end tag
			if (l->skip_depth && (!strcmp(name, "script") || !strcmp(name, "style"))) {
				char endtag[24];
				snprintf(endtag, sizeof endtag, "</%s", name);
				const char *z = *e ? e + 1 : e;
				const char *f = NULL;
				for (const char *k = z; *k; k++) if (*k == '<' && !strncasecmp(k, endtag, strlen(endtag))) { f = k; break; }
				l->skip_depth = 0;
				l->skip_name[0] = 0;
				if (f) { const char *g = strchr(f, '>'); p = g ? g + 1 : f + strlen(f); }
				else p = z + strlen(z);
				continue;
			}
		}
		p = *e ? e + 1 : e;
	}
	// controls left open by broken HTML
	if (l->sel_field >= 0) { finish_option(l); int fi = l->sel_field; l->sel_field = -1; place_field(l, fi); }
	if (l->cap_field >= 0) { int fi = l->cap_field; l->cap_field = -1; place_field(l, fi); }
	newline(l, false);
	d->height = l->y + 4;

	// first line of each link, for keyboard navigation
	d->link_y = calloc(d->nlinks ? d->nlinks : 1, sizeof(float));
	if (d->link_y) {
		for (int i = 0; i < d->nlinks; i++) d->link_y[i] = -1;
		for (int i = 0; i < d->nruns; i++) {
			int k = d->runs[i].link;
			if (k >= 0 && k < d->nlinks && d->link_y[k] < 0) d->link_y[k] = d->runs[i].y;
		}
	}
	free(l);
	return d;
}

void html_free(Doc *d)
{
	if (!d) return;
	for (int i = 0; i < d->nfields; i++) {
		Field *f = &d->fields[i];
		free(f->name); free(f->value); free(f->placeholder);
		for (int k = 0; k < f->nopt; k++) { free(f->opt_val[k]); free(f->opt_txt[k]); }
		free(f->opt_val); free(f->opt_txt);
	}
	for (int i = 0; i < d->nforms; i++) free(d->forms[i].action);
	free(d->fields); free(d->forms);
	free(d->runs); free(d->rects); free(d->pool); free(d->links); free(d->link_y);
	free(d);
}

float html_height(const Doc *d) { return d ? d->height : 0; }
int   html_nlinks(const Doc *d) { return d ? d->nlinks : 0; }
const char *html_link(const Doc *d, int i) { return d && i >= 0 && i < d->nlinks ? d->pool + d->links[i] : NULL; }
float html_link_y(const Doc *d, int i) { return d && d->link_y && i >= 0 && i < d->nlinks ? d->link_y[i] : 0; }

static int first_run_at(const Doc *d, float y)
{
	int lo = 0, hi = d->nruns;
	while (lo < hi) {
		int m = (lo + hi) / 2;
		if (d->runs[m].y + d->runs[m].h < y) lo = m + 1;
		else hi = m;
	}
	// runs on the same line may be slightly out of order after baseline alignment
	while (lo > 0 && d->runs[lo - 1].y + d->runs[lo - 1].h >= y - 40) lo--;
	return lo;
}

void html_draw(const Doc *d, float doc_y, float view_h, float ox, float oy, int focus)
{
	if (!d) return;
	float y1 = doc_y + view_h;
	for (int i = 0; i < d->nrects; i++) {
		const Rect *r = &d->rects[i];
		if (r->y > y1 || r->y + r->h < doc_y) continue;
		float top = r->y < doc_y ? doc_y : r->y;
		float bot = r->y + r->h > y1 ? y1 : r->y + r->h;
		C2D_DrawRectSolid(ox + r->x, oy + top - doc_y, 0.5f, r->w, bot - top, r->color);
	}
	for (int i = first_run_at(d, doc_y); i < d->nruns; i++) {
		const Run *r = &d->runs[i];
		if (r->y > y1) break;
		if (r->y + r->h < doc_y) continue;
		float x = ox + r->x, y = oy + r->y - doc_y;
		if (r->link >= 0 && r->link == focus)
			C2D_DrawRectSolid(x - 2, y, 0.5f, r->w + 4, r->h - 1, C2D_Color32(255, 226, 150, 255));
		const char *s = d->pool + r->text;
		if ((r->flags & F_FIELD) && !(r->flags & F_LINK)) {
			// text box or button
			bool btn = r->flags & F_BUTTON;
			u32 border = r->link == focus ? C2D_Color32(214, 69, 40, 255) : C2D_Color32(150, 156, 168, 255);
			C2D_DrawRectSolid(x, y + 1, 0.5f, r->w, r->h - 3, border);
			C2D_DrawRectSolid(x + 1, y + 2, 0.5f, r->w - 2, r->h - 5, btn ? C2D_Color32(232, 235, 240, 255) : C_WHITE);
			text_draw(s, btn ? x + r->w / 2 : x + 4, y + 1, r->scale, r->color, r->w - 8, btn ? 1 : 0);
			continue;
		}
		text_draw(s, x, y, r->scale, r->color, 0, 0);
		if (r->flags & F_BOLD) text_draw(s, x + 0.6f, y, r->scale, r->color, 0, 0);
		if ((r->flags & F_LINK) && !(r->flags & F_FIELD)) C2D_DrawRectSolid(x, y + r->h - 4, 0.5f, r->w, 1, C2D_Color32(120, 160, 230, 255));
	}
}

// ---------------------------------------------------------------- pages and forms

const char *html_title(const Doc *d) { return d ? d->title : ""; }
const char *html_base(const Doc *d) { return d ? d->base : ""; }

int html_link_field(const Doc *d, int link)
{
	const char *h = html_link(d, link);
	if (!h || h[0] != '\x01') return -1;
	int f = atoi(h + 1);
	return f >= 0 && f < d->nfields ? f : -1;
}

HFType html_field_type(const Doc *d, int f) { return d->fields[f].type; }
const char *html_field_value(const Doc *d, int f) { return d->fields[f].value; }
const char *html_field_hint(const Doc *d, int f)
{
	const Field *x = &d->fields[f];
	return x->placeholder[0] ? x->placeholder : x->name;
}
int html_field_form(const Doc *d, int f) { return d->fields[f].form; }

static void refresh_field(Doc *d, int fi)
{
	Field *f = &d->fields[fi];
	if (f->run < 0) return;
	char disp[300];
	bool muted;
	field_display(f, disp, sizeof disp, &muted);
	int off = pool_add(d, disp, strlen(disp));
	if (off < 0) return;
	Run *r = &d->runs[f->run];
	r->text = off;
	bool btn = f->type == HF_SUBMIT || f->type == HF_BUTTON;
	r->color = btn ? C_TEXT : muted ? C_SUB : C_TEXT;
	r->flags = (r->flags & ~F_MUTED) | (muted ? F_MUTED : 0);
}

void html_field_set(Doc *d, int fi, const char *value)
{
	Field *f = &d->fields[fi];
	free(f->value);
	f->value = sdup(value);
	refresh_field(d, fi);
}

void html_field_toggle(Doc *d, int fi)
{
	Field *f = &d->fields[fi];
	if (f->type == HF_CHECK) f->checked = !f->checked;
	else if (f->type == HF_RADIO) {
		for (int i = 0; i < d->nfields; i++) {
			Field *o = &d->fields[i];
			if (i != fi && o->type == HF_RADIO && o->form == f->form && !strcmp(o->name, f->name) && o->checked) {
				o->checked = false;
				refresh_field(d, i);
			}
		}
		f->checked = true;
	} else if (f->type == HF_SELECT && f->nopt) f->sel = (f->sel + 1) % f->nopt;
	refresh_field(d, fi);
}

int html_form_text_fields(const Doc *d, int form)
{
	int n = 0;
	for (int i = 0; i < d->nfields; i++)
		if (d->fields[i].form == form && (d->fields[i].type == HF_TEXT || d->fields[i].type == HF_PASSWORD || d->fields[i].type == HF_TEXTAREA)) n++;
	return n;
}

static void enc_add(char *out, size_t n, size_t *o, const char *s)
{
	static const char hex[] = "0123456789ABCDEF";
	for (; *s && *o + 4 < n; s++) {
		unsigned char c = *s;
		if (c == ' ') out[(*o)++] = '+';
		else if (isalnum(c) || strchr("-_.~*", c)) out[(*o)++] = c;
		else { out[(*o)++] = '%'; out[(*o)++] = hex[c >> 4]; out[(*o)++] = hex[c & 15]; }
	}
	out[*o] = 0;
}

static void pair(char *out, size_t n, size_t *o, const char *k, const char *v)
{
	if (*o && *o + 1 < n) out[(*o)++] = '&';
	enc_add(out, n, o, k);
	if (*o + 1 < n) out[(*o)++] = '=';
	enc_add(out, n, o, v);
}

bool html_form_submit(const Doc *d, int submit, char *data, size_t n, const char **action, bool *post)
{
	if (!d || submit < 0 || submit >= d->nfields) return false;
	int form = d->fields[submit].form;
	if (form < 0 || form >= d->nforms) return false;
	size_t o = 0;
	data[0] = 0;
	for (int i = 0; i < d->nfields; i++) {
		const Field *f = &d->fields[i];
		if (f->form != form || !f->name[0]) continue;
		switch (f->type) {
		case HF_TEXT: case HF_PASSWORD: case HF_TEXTAREA: case HF_HIDDEN: pair(data, n, &o, f->name, f->value); break;
		case HF_CHECK: case HF_RADIO: if (f->checked) pair(data, n, &o, f->name, f->value); break;
		case HF_SELECT: if (f->nopt) pair(data, n, &o, f->name, f->opt_val[f->sel]); break;
		case HF_SUBMIT: if (i == submit) pair(data, n, &o, f->name, f->value); break;
		default: break;
		}
	}
	*action = d->forms[form].action;
	*post = d->forms[form].post;
	return true;
}

int html_hit(const Doc *d, float x, float y)
{
	if (!d) return -1;
	int best = -1;
	float bd = 14;
	for (int i = first_run_at(d, y - 20); i < d->nruns; i++) {
		const Run *r = &d->runs[i];
		if (r->y > y + 20) break;
		if (r->link < 0) continue;
		// distance from the point to the run's box (generous for fingers)
		float dx = x < r->x ? r->x - x : x > r->x + r->w ? x - (r->x + r->w) : 0;
		float dy = y < r->y ? r->y - y : y > r->y + r->h ? y - (r->y + r->h) : 0;
		float dist = dx + dy;
		if (dist < bd) { bd = dist; best = r->link; }
	}
	return best;
}

int html_link_near(const Doc *d, int from, int dir, float doc_y, float view_h)
{
	if (!d || !d->nlinks || !d->link_y) return -1;
	if (from < 0) {
		// first link inside the view
		for (int i = 0; i < d->nlinks; i++) if (d->link_y[i] >= doc_y && d->link_y[i] < doc_y + view_h) return i;
		return -1;
	}
	int i = from + dir;
	while (i >= 0 && i < d->nlinks && d->link_y[i] < 0) i += dir;
	return i >= 0 && i < d->nlinks ? i : -1;
}

void html_to_text(const char *html, char *out, size_t n)
{
	size_t o = 0;
	bool space = false;
	const char *p = html ? html : "";
	while (*p && o + 5 < n) {
		if (*p == '<') {
			bool block = !strncasecmp(p, "<p", 2) || !strncasecmp(p, "<br", 3) || !strncasecmp(p, "<div", 4) ||
			             !strncasecmp(p, "<li", 3) || !strncasecmp(p, "</p", 3) || !strncasecmp(p, "<h", 2);
			if (!strncasecmp(p, "<script", 7) || !strncasecmp(p, "<style", 6)) {
				const char *e = strstr(p, !strncasecmp(p, "<script", 7) ? "</script" : "</style");
				if (!e) e = strstr(p, !strncasecmp(p, "<script", 7) ? "</SCRIPT" : "</STYLE");
				p = e ? e : p + strlen(p);
			}
			const char *e = strchr(p, '>');
			p = e ? e + 1 : p + strlen(p);
			if (block) space = true;
			continue;
		}
		if (*p == '&') {
			char e[8]; int en = 0;
			int used = entity(p, e, &en);
			if (used) {
				p += used;
				if (en == 1 && e[0] == ' ') { space = true; continue; }
				if (space && o) out[o++] = ' ';
				space = false;
				for (int k = 0; k < en && o + 1 < n; k++) out[o++] = e[k];
				continue;
			}
		}
		if (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') { space = true; p++; continue; }
		if (space && o) out[o++] = ' ';
		space = false;
		out[o++] = *p++;
	}
	out[o] = 0;
}
