// Navegador 3DS: a lightweight web browser for simple sites (no JavaScript, no CSS).
// The page runs across both screens: the top shows the first part and the bottom
// continues it (touch there to follow links and fill in forms).
#include "browser.h"
#include "updater.h"
#include <curl/curl.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TOP_W 400
#define BOT_W 320
#define TOP_BAR 22
#define FOOT_Y 212
#define VIEW_TOP (240 - TOP_BAR)
#define VIEW_BOT FOOT_Y
#define VIEW_ALL (VIEW_TOP + VIEW_BOT)
#define DOC_W 308.0f
#define MAX_HIST 30
#define KEEP_HTML 4      // pages kept in memory for an instant "back"

typedef struct { char url[1024]; char title[160]; float scroll; char *html; } Hist;

static C3D_RenderTarget *t_top, *t_bot;
static Hist hist[MAX_HIST];
static int nhist, hpos = -1;
static Doc *doc;
static char cur_url[1024], cur_title[160];
static char *cur_html;
static float scroll, vel;
static int focus = -1;
static int job, dl_job;
static char job_url[1024];
static bool job_push;
static float job_restore = -1;
static char toast_msg[160];
static u64 toast_until, now_ms;
static int text_size = 1;      // 0 small, 1 normal, 2 large
static bool desktop_ua;
static bool upd_prompt, quit_app;

typedef struct { float x, y, w, h; int id; } Btn;
static Btn btns[16];
static int nbtns;
enum { B_BACK = 1, B_FWD, B_RELOAD, B_URL, B_FAV, B_HOME, B_UPD_YES, B_UPD_NO };

static bool touch_down, dragging;
static int touch_x0, touch_y0, last_ty;
static float scroll0;

static void navigate(const char *url, const char *post, bool push);

static void toast(const char *m)
{
	snprintf(toast_msg, sizeof toast_msg, "%s", m);
	toast_until = osGetTime() + 2500;
}

static float doc_scale(void) { return text_size == 0 ? 0.42f : text_size == 2 ? 0.56f : 0.48f; }

static bool keyboard(const char *hint, char *out, size_t n, bool multiline, bool password)
{
	SwkbdState kb;
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, (int)n - 1);
	swkbdSetHintText(&kb, hint);
	swkbdSetInitialText(&kb, out);
	if (multiline) swkbdSetFeatures(&kb, SWKBD_MULTILINE);
	if (password) swkbdSetPasswordMode(&kb, SWKBD_PASSWORD_HIDE_DELAY);
	swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, "Cancelar", false);
	swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "OK", true);
	static char buf[2048];
	if (n > sizeof buf) n = sizeof buf;
	if (swkbdInputText(&kb, buf, n) != SWKBD_BUTTON_RIGHT) return false;
	snprintf(out, n, "%s", buf);
	return true;
}

// ---------------------------------------------------------------- settings, favorites, history files

static void settings_load(void)
{
	FILE *f = fopen(APP_DIR "/config.ini", "r");
	if (!f) return;
	char l[128];
	while (fgets(l, sizeof l, f)) {
		if (!strncmp(l, "text_size=", 10)) text_size = atoi(l + 10);
		if (!strncmp(l, "desktop=", 8)) desktop_ua = atoi(l + 8) != 0;
	}
	fclose(f);
	if (text_size < 0 || text_size > 2) text_size = 1;
}

static void settings_save(void)
{
	FILE *f = fopen(APP_DIR "/config.ini", "w");
	if (!f) return;
	fprintf(f, "text_size=%d\ndesktop=%d\n", text_size, desktop_ua);
	fclose(f);
}

typedef struct { char title[128]; char url[512]; } Entry;

static int load_list(const char *path, Entry *e, int max)
{
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	char l[700];
	int n = 0;
	while (n < max && fgets(l, sizeof l, f)) {
		l[strcspn(l, "\r\n")] = 0;
		char *tab = strchr(l, '\t');
		if (!tab) continue;
		*tab = 0;
		snprintf(e[n].title, sizeof e[n].title, "%s", l);
		snprintf(e[n].url, sizeof e[n].url, "%s", tab + 1);
		n++;
	}
	fclose(f);
	return n;
}

static void save_list(const char *path, const Entry *e, int n)
{
	FILE *f = fopen(path, "w");
	if (!f) return;
	for (int i = 0; i < n; i++) fprintf(f, "%s\t%s\n", e[i].title, e[i].url);
	fclose(f);
}

static Entry favs[100];
static int nfavs;

static int fav_index(const char *url)
{
	for (int i = 0; i < nfavs; i++) if (!strcmp(favs[i].url, url)) return i;
	return -1;
}

static void toggle_fav(void)
{
	if (!strncmp(cur_url, "about:", 6)) { toast("Páginas do navegador não vão para os favoritos"); return; }
	int i = fav_index(cur_url);
	if (i >= 0) {
		memmove(&favs[i], &favs[i + 1], (nfavs - i - 1) * sizeof(Entry));
		nfavs--;
		toast("Tirado dos favoritos");
	} else if (nfavs < 100) {
		snprintf(favs[nfavs].title, sizeof favs[0].title, "%s", cur_title[0] ? cur_title : cur_url);
		snprintf(favs[nfavs].url, sizeof favs[0].url, "%s", cur_url);
		nfavs++;
		toast("Salvo nos favoritos");
	}
	save_list(APP_DIR "/favoritos.txt", favs, nfavs);
}

static void add_history_file(const char *title, const char *url)
{
	if (!strncmp(url, "about:", 6)) return;
	static Entry h[60];
	int n = load_list(APP_DIR "/historico.txt", h, 60);
	for (int i = 0; i < n; i++)
		if (!strcmp(h[i].url, url)) { memmove(&h[i], &h[i + 1], (n - i - 1) * sizeof(Entry)); n--; break; }
	if (n == 60) n--;
	memmove(&h[1], &h[0], n * sizeof(Entry));
	snprintf(h[0].title, sizeof h[0].title, "%s", title[0] ? title : url);
	snprintf(h[0].url, sizeof h[0].url, "%s", url);
	save_list(APP_DIR "/historico.txt", h, n + 1);
}

// ---------------------------------------------------------------- internal pages

typedef struct { char *s; size_t n, cap; } Buf;

static void bput(Buf *b, const char *s)
{
	size_t l = strlen(s);
	if (b->n + l + 1 > b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 4096;
		while (nc < b->n + l + 1) nc *= 2;
		char *ns = realloc(b->s, nc);
		if (!ns) return;
		b->s = ns;
		b->cap = nc;
	}
	memcpy(b->s + b->n, s, l + 1);
	b->n += l;
}

static void besc(Buf *b, const char *s)
{
	char t[2] = { 0, 0 };
	for (; *s; s++) {
		if (*s == '<') bput(b, "&lt;");
		else if (*s == '>') bput(b, "&gt;");
		else if (*s == '&') bput(b, "&amp;");
		else if (*s == '"') bput(b, "&quot;");
		else { t[0] = *s; bput(b, t); }
	}
}

static void link_list(Buf *b, const Entry *e, int n, const char *remove_prefix)
{
	bput(b, "<ul>");
	for (int i = 0; i < n; i++) {
		bput(b, "<li><a href=\"");
		besc(b, e[i].url);
		bput(b, "\">");
		besc(b, e[i].title);
		bput(b, "</a>");
		if (remove_prefix) {
			char enc[1600];
			url_encode_component(e[i].url, enc, sizeof enc);
			bput(b, " <small><a href=\"");
			bput(b, remove_prefix);
			bput(b, enc);
			bput(b, "\">[tirar]</a></small>");
		}
		bput(b, "</li>");
	}
	bput(b, "</ul>");
}

static char *page_start(void)
{
	Buf b = {0};
	bput(&b, "<h2>Navegador 3DS</h2>"
	         "<form action=\"about:ir\"><input type=\"text\" name=\"q\" placeholder=\"Pesquisar ou digitar um site\"> "
	         "<input type=\"submit\" value=\"Ir\"></form>");
	if (nfavs) {
		bput(&b, "<h3>Favoritos</h3>");
		link_list(&b, favs, nfavs < 12 ? nfavs : 12, NULL);
		if (nfavs > 12) bput(&b, "<p><a href=\"about:favoritos\">Ver todos os favoritos</a></p>");
	}
	bput(&b, "<h3>Sites que funcionam bem</h3><ul>"
	         "<li><a href=\"https://pt.m.wikipedia.org/\">Wikipédia</a></li>"
	         "<li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo (busca)</a></li>"
	         "<li><a href=\"https://text.npr.org/\">NPR em texto (notícias, inglês)</a></li>"
	         "<li><a href=\"https://lite.cnn.com/\">CNN Lite (notícias, inglês)</a></li>"
	         "<li><a href=\"http://68k.news/\">68k.news (notícias do mundo)</a></li>"
	         "<li><a href=\"https://news.ycombinator.com/\">Hacker News</a></li>"
	         "<li><a href=\"https://wiby.me/\">Wiby (busca de sites simples)</a></li>"
	         "<li><a href=\"http://frogfind.com/\">FrogFind (simplifica qualquer site)</a></li>"
	         "</ul>");
	static Entry h[8];
	int n = load_list(APP_DIR "/historico.txt", h, 8);
	if (n) {
		bput(&b, "<h3>Visitados há pouco</h3>");
		link_list(&b, h, n, NULL);
	}
	bput(&b, "<hr><p><a href=\"about:favoritos\">Favoritos</a> | <a href=\"about:historico\">Histórico</a> | "
	         "<a href=\"about:ajustes\">Ajustes</a> | <a href=\"about:ajuda\">Ajuda</a></p>");
	return b.s;
}

static char *page_settings(void)
{
	Buf b = {0};
	static const char *sizes[] = { "pequeno", "normal", "grande" };
	char t[200];
	bput(&b, "<h2>Ajustes</h2><h3>Tamanho do texto</h3><p>");
	for (int i = 0; i < 3; i++) {
		snprintf(t, sizeof t, "%s<a href=\"about:set?size=%d\">%s%s</a>", i ? " | " : "", i, sizes[i], i == text_size ? " (atual)" : "");
		bput(&b, t);
	}
	bput(&b, "</p><h3>Versão dos sites</h3><p>");
	bput(&b, desktop_ua ? "<a href=\"about:set?ua=0\">Celular</a> | Computador (atual)" : "Celular (atual) | <a href=\"about:set?ua=1\">Computador</a>");
	bput(&b, "</p><p><small>A versão de celular costuma ser mais leve e funcionar melhor.</small></p>");
	bput(&b, "<h3>Atualização</h3>");
	snprintf(t, sizeof t, "<p>Versão %s (%s). ", updater_version(), updater_is_cia() ? "CIA" : "3DSX");
	bput(&b, t);
	if (updater_state() != UPD_IDLE) { besc(&b, updater_message()); bput(&b, " "); }
	bput(&b, updater_state() == UPD_AVAILABLE ? "<a href=\"about:atualizar\">Instalar agora</a>" : "<a href=\"about:atualizar\">Procurar versão nova</a>");
	bput(&b, "</p><h3>Dados</h3><p><a href=\"about:limpar?o=historico\">Apagar o histórico</a> | "
	         "<a href=\"about:limpar?o=cookies\">Apagar os cookies (sai dos sites)</a></p>"
	         "<p><small>Arquivos baixados ficam em 3ds/Navegador3DS/downloads no cartão SD.</small></p>");
	return b.s;
}

static char *page_list(const char *title, const char *path, const char *remove_prefix)
{
	static Entry e[100];
	int n = load_list(path, e, 100);
	Buf b = {0};
	bput(&b, "<h2>");
	bput(&b, title);
	bput(&b, "</h2>");
	if (!n) bput(&b, "<p>Nada por aqui ainda.</p>");
	else link_list(&b, e, n, remove_prefix);
	return b.s;
}

static char *page_help(void)
{
	Buf b = {0};
	bput(&b, "<h2>Como usar</h2><ul>"
	         "<li><b>Direcional cima/baixo</b> ou <b>direcional circular</b>: rolar. <b>L / R</b>: uma tela inteira.</li>"
	         "<li><b>Direcional esquerda/direita</b>: escolher um link ou campo. <b>A</b>: abrir.</li>"
	         "<li>Na tela de baixo, <b>toque</b> nos links e campos e <b>arraste</b> para rolar.</li>"
	         "<li><b>B</b>: voltar. <b>X</b>: recarregar (ou parar). <b>Y</b>: digitar um endereço ou pesquisa.</li>"
	         "<li><b>SELECT</b>: salvar ou tirar dos favoritos. <b>START</b>: sair.</li></ul>"
	         "<h3>O que funciona</h3><p>Sites de texto: Wikipédia, notícias, blogs, fóruns, buscas e formulários simples. "
	         "O navegador não roda JavaScript, então sites como YouTube, Gmail, redes sociais e o Canvas não abrem "
	         "(para o Canvas, use o app Canvas 3DS). Imagens aparecem como [imagem].</p>"
	         "<p>Site pesado ou quebrado? Abra pelo <a href=\"http://frogfind.com/\">FrogFind</a>, que entrega uma versão simples.</p>");
	return b.s;
}

static char *page_message(const char *title, const char *text, const char *retry)
{
	Buf b = {0};
	bput(&b, "<h2>");
	besc(&b, title);
	bput(&b, "</h2><p>");
	besc(&b, text);
	bput(&b, "</p>");
	if (retry) { bput(&b, "<p><a href=\""); besc(&b, retry); bput(&b, "\">Tentar de novo</a></p>"); }
	bput(&b, "<p><a href=\"" START_URL "\">Página inicial</a></p>");
	return b.s;
}

// ---------------------------------------------------------------- showing a page

static void free_old_html(void)
{
	// keep only the most recent pages in memory
	for (int i = 0; i < nhist; i++)
		if (i < hpos - KEEP_HTML || i > hpos + KEEP_HTML) { free(hist[i].html); hist[i].html = NULL; }
}

// Takes ownership of html.
static void show_page(const char *url, char *html, bool push, float restore_scroll)
{
	if (hpos >= 0 && hpos < nhist) hist[hpos].scroll = scroll;
	html_free(doc);
	doc = html_layout(html, DOC_W, doc_scale());
	snprintf(cur_url, sizeof cur_url, "%s", url);
	const char *t = html_title(doc);
	if (t && *t) snprintf(cur_title, sizeof cur_title, "%s", t);
	else url_host(url, cur_title, sizeof cur_title);
	if (!strcmp(url, START_URL)) snprintf(cur_title, sizeof cur_title, "Página inicial");
	scroll = restore_scroll >= 0 ? restore_scroll : 0;
	vel = 0;
	focus = -1;
	cur_html = html;
	if (push) {
		// a new page drops the "forward" entries
		for (int i = hpos + 1; i < nhist; i++) { free(hist[i].html); hist[i].html = NULL; }
		nhist = hpos + 1;
		if (nhist == MAX_HIST) {
			free(hist[0].html);
			memmove(&hist[0], &hist[1], (MAX_HIST - 1) * sizeof(Hist));
			nhist--;
		}
		hpos = nhist++;
		memset(&hist[hpos], 0, sizeof(Hist));
		add_history_file(cur_title, url);
	}
	Hist *h = &hist[hpos];
	snprintf(h->url, sizeof h->url, "%s", url);
	snprintf(h->title, sizeof h->title, "%s", cur_title);
	if (h->html != html) { free(h->html); h->html = html; }
	free_old_html();
}

static void relayout(void)
{
	if (!cur_html) return;
	html_free(doc);
	doc = html_layout(cur_html, DOC_W, doc_scale());
	focus = -1;
}

static char *qparam(const char *url, const char *key)
{
	// value of key in "about:x?key=value&..." (decoded), malloc'd
	const char *q = strchr(url, '?');
	if (!q) return NULL;
	size_t kl = strlen(key);
	for (const char *p = q + 1; p && *p;) {
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			const char *v = p + kl + 1;
			size_t l = strcspn(v, "&");
			char *out = malloc(l + 1);
			if (!out) return NULL;
			size_t o = 0;
			for (size_t i = 0; i < l; i++) {
				if (v[i] == '+') out[o++] = ' ';
				else if (v[i] == '%' && i + 2 < l + 1 && v[i + 1] && v[i + 2]) {
					char hx[3] = { v[i + 1], v[i + 2], 0 };
					out[o++] = (char)strtol(hx, NULL, 16);
					i += 2;
				} else out[o++] = v[i];
			}
			out[o] = 0;
			return out;
		}
		p = strchr(p, '&');
		if (p) p++;
	}
	return NULL;
}

static void download_current(const char *url)
{
	mkdir(APP_DIR "/downloads", 0777);
	char name[128];
	const char *slash = strrchr(url, '/');
	snprintf(name, sizeof name, "%s", slash && slash[1] ? slash + 1 : "arquivo");
	name[strcspn(name, "?#")] = 0;
	for (char *p = name; *p; p++) if (strchr("\\/:*?\"<>|", *p)) *p = '_';
	char path[300];
	snprintf(path, sizeof path, APP_DIR "/downloads/%s", name[0] ? name : "arquivo");
	if (dl_job) net_forget(dl_job);
	dl_job = net_download(url, path);
	toast("Baixando...");
}

// about: pages
static bool internal(const char *url, bool push)
{
	if (strncmp(url, "about:", 6)) return false;
	char *html = NULL;
	if (!strcmp(url, START_URL) || !strcmp(url, "about:blank")) html = page_start();
	else if (!strcmp(url, "about:ajustes")) html = page_settings();
	else if (!strcmp(url, "about:ajuda")) html = page_help();
	else if (!strcmp(url, "about:favoritos")) html = page_list("Favoritos", APP_DIR "/favoritos.txt", "about:tirar-favorito?u=");
	else if (!strcmp(url, "about:historico")) html = page_list("Histórico", APP_DIR "/historico.txt", NULL);
	else if (!strncmp(url, "about:ir", 8)) {
		char *q = qparam(url, "q");
		if (q && *q) {
			char dest[1200];
			url_from_input(q, dest, sizeof dest);
			free(q);
			navigate(dest, NULL, true);
		} else free(q);
		return true;
	} else if (!strncmp(url, "about:set", 9)) {
		char *s = qparam(url, "size"), *ua = qparam(url, "ua");
		if (s) { text_size = atoi(s); free(s); }
		if (ua) { desktop_ua = atoi(ua) != 0; free(ua); toast("Vale a partir da próxima página"); }
		settings_save();
		html = page_settings();
		push = false;
	} else if (!strncmp(url, "about:tirar-favorito", 20)) {
		char *u = qparam(url, "u");
		if (u) { int i = fav_index(u); if (i >= 0) { memmove(&favs[i], &favs[i + 1], (nfavs - i - 1) * sizeof(Entry)); nfavs--; save_list(APP_DIR "/favoritos.txt", favs, nfavs); } free(u); }
		html = page_list("Favoritos", APP_DIR "/favoritos.txt", "about:tirar-favorito?u=");
		push = false;
		url = "about:favoritos";
	} else if (!strncmp(url, "about:limpar", 12)) {
		char *o = qparam(url, "o");
		if (o && !strcmp(o, "historico")) { remove(APP_DIR "/historico.txt"); toast("Histórico apagado"); }
		if (o && !strcmp(o, "cookies")) { remove(APP_DIR "/cookies.txt"); toast("Cookies apagados (vale ao abrir o app de novo)"); }
		free(o);
		html = page_settings();
		push = false;
		url = "about:ajustes";
	} else if (!strcmp(url, "about:atualizar")) {
		if (updater_state() == UPD_AVAILABLE) upd_prompt = true;
		else if (!updater_busy()) { updater_check(); toast("Procurando versão nova..."); }
		return true;
	} else if (!strncmp(url, "about:baixar", 12)) {
		char *u = qparam(url, "u");
		if (u) { download_current(u); free(u); }
		return true;
	} else html = page_message("Página não encontrada", url, NULL);
	if (!strncmp(url, "about:set", 9)) url = "about:ajustes";
	if (html) {
		if (!strncmp(url, "about:set", 9) || !strncmp(url, "about:ajustes", 13)) relayout();
		show_page(url, html, push, push ? -1 : scroll);
	}
	return true;
}

static void navigate(const char *url, const char *post, bool push)
{
	if (internal(url, push)) return;
	if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
		toast("Endereço não suportado");
		return;
	}
	if (job) net_forget(job);
	snprintf(job_url, sizeof job_url, "%s", url);
	job_push = push;
	job = post ? net_post(url, post) : net_get(url, 1);
}

static void finish_load(NetResult *r)
{
	const char *url = r->final_url[0] ? r->final_url : job_url;
	float restore = job_restore;
	job_restore = -1;
	if (!r->body || (!r->len && r->err[0])) {
		char *html = page_message("Não consegui abrir a página", r->err[0] ? r->err : "Erro desconhecido.", job_url);
		show_page(job_url, html, job_push, -1);
		return;
	}
	const char *ct = r->ctype;
	bool is_html = !ct[0] || strstr(ct, "html") || strstr(ct, "xml");
	bool is_text = strstr(ct, "text/plain") != NULL;
	if (!is_html && !is_text) {
		Buf b = {0};
		char sz[32];
		snprintf(sz, sizeof sz, "%.1f KB", r->len / 1024.0);
		bput(&b, "<h2>Arquivo</h2><p>Este endereço é um arquivo (");
		besc(&b, ct);
		bput(&b, ", ");
		bput(&b, sz);
		bput(&b, "), não uma página.</p><p><a href=\"about:baixar?u=");
		char enc[2400];
		url_encode_component(url, enc, sizeof enc);
		bput(&b, enc);
		bput(&b, "\">Baixar para o cartão SD</a></p>");
		show_page(url, b.s, job_push, -1);
		free(r->body);
		return;
	}
	char *utf = to_utf8(r->body, r->len, ct);
	free(r->body);
	if (!utf) { show_page(url, page_message("Sem memória", "A página é grande demais.", NULL), job_push, -1); return; }
	if (is_text) {
		Buf b = {0};
		bput(&b, "<pre>");
		besc(&b, utf);
		bput(&b, "</pre>");
		free(utf);
		utf = b.s;
	}
	show_page(url, utf, job_push, restore);
	if (r->err[0]) toast(r->err);   // e.g. a 404 page from the server
}

static void go_back(int dir)
{
	int t = hpos + dir;
	if (t < 0 || t >= nhist) return;
	hist[hpos].scroll = scroll;
	hpos = t;
	Hist *h = &hist[hpos];
	if (h->html) {
		char *html = h->html;
		h->html = NULL;   // show_page puts it back
		show_page(h->url, html, false, h->scroll);
		h->html = html;
		cur_html = html;
	} else {
		job_restore = h->scroll;
		navigate(h->url, NULL, false);
	}
}

// ---------------------------------------------------------------- links and forms

static const char *page_base(char *buf, size_t n)
{
	const char *b = html_base(doc);
	if (b && *b) { url_resolve(cur_url, b, buf, n); return buf; }
	return cur_url;
}

static void submit_form(int field)
{
	char data[4096];
	const char *action;
	bool post;
	if (!html_form_submit(doc, field, data, sizeof data, &action, &post)) return;
	char base[1024], target[2048];
	url_resolve(cur_url, action && *action ? action : cur_url, target, sizeof target);
	(void)base;
	if (!strncmp(target, "about:", 6)) {
		char u[3000];
		snprintf(u, sizeof u, "%s?%s", target, data);
		navigate(u, NULL, true);
		return;
	}
	if (post) { navigate(target, data, true); return; }
	target[strcspn(target, "?#")] = 0;
	char u[6200];
	snprintf(u, sizeof u, "%s?%s", target, data);
	navigate(u, NULL, true);
}

static void activate(int link)
{
	if (!doc || link < 0) return;
	int f = html_link_field(doc, link);
	if (f >= 0) {
		HFType t = html_field_type(doc, f);
		if (t == HF_CHECK || t == HF_RADIO || t == HF_SELECT) { html_field_toggle(doc, f); return; }
		if (t == HF_SUBMIT) { submit_form(f); return; }
		if (t == HF_BUTTON) { toast("Esse botão precisa de JavaScript"); return; }
		char v[2048];
		snprintf(v, sizeof v, "%s", html_field_value(doc, f));
		if (!keyboard(html_field_hint(doc, f), v, sizeof v, t == HF_TEXTAREA, t == HF_PASSWORD)) return;
		html_field_set(doc, f, v);
		// a form with a single text box (a search) goes on its own
		int form = html_field_form(doc, f);
		if (form >= 0 && t != HF_TEXTAREA && html_form_text_fields(doc, form) == 1) submit_form(f);
		return;
	}
	const char *href = html_link(doc, link);
	if (!href) return;
	if (href[0] == '#') { toast("Link para outra parte da página"); return; }
	char base[1024], abs[2048];
	url_resolve(page_base(base, sizeof base), href, abs, sizeof abs);
	if (!strncmp(abs, "javascript:", 11)) { toast("Esse link precisa de JavaScript"); return; }
	if (!strncmp(abs, "mailto:", 7) || !strncmp(abs, "tel:", 4)) { toast("Abra esse link no celular"); return; }
	navigate(abs, NULL, true);
}

static void ask_url(void)
{
	char t[1024] = "";
	if (strncmp(cur_url, "about:", 6)) snprintf(t, sizeof t, "%s", cur_url);
	if (!keyboard("Endereço ou pesquisa", t, sizeof t, false, false) || !t[0]) return;
	char u[1200];
	url_from_input(t, u, sizeof u);
	navigate(u, NULL, true);
}

// ---------------------------------------------------------------- drawing

static void clamp(void)
{
	float maxs = html_height(doc) - VIEW_ALL + 30;
	if (maxs < 0) maxs = 0;
	if (scroll > maxs) { scroll = maxs; vel = 0; }
	if (scroll < 0) { scroll = 0; vel = 0; }
}

static void spinner(float cx, float cy, u32 c)
{
	float a = (now_ms % 1000) / 1000.0f * 6.2832f;
	for (int i = 0; i < 8; i++) {
		float t = a + i * 0.785f;
		C2D_DrawCircleSolid(cx + cosf(t) * 6, cy + sinf(t) * 6, 0.5f, 1.6f, (c & 0x00FFFFFF) | ((u32)(40 + i * 26) << 24));
	}
}

static void draw_top(void)
{
	C2D_TargetClear(t_top, C_WHITE);
	C2D_SceneBegin(t_top);
	if (doc) html_draw(doc, scroll, VIEW_TOP, (TOP_W - DOC_W) / 2, TOP_BAR, focus);
	C2D_DrawRectSolid(0, 0, 0.5f, TOP_W, TOP_BAR, C_BAR);
	char host[128];
	url_host(cur_url, host, sizeof host);
	if (!strncmp(cur_url, "about:", 6)) snprintf(host, sizeof host, "Navegador 3DS");
	bool https = !strncmp(cur_url, "https://", 8);
	text_draw(cur_title, 8, 3, 0.47f, C_WHITE, 250, 0);
	text_draw(host, TOP_W - 8, 4, 0.42f, https ? C2D_Color32(150, 220, 160, 255) : C2D_Color32(200, 205, 215, 255), 120, 2);
	if (job) {
		spinner(262, 11, C_WHITE);
		float p = net_progress(job);
		C2D_DrawRectSolid(0, TOP_BAR - 2, 0.5f, TOP_W * (p > 0 ? p : 0.15f), 2, C_ACCENT);
	}
}

static void reg_btn(float x, float y, float w, float h, int id)
{
	if (nbtns < 16) btns[nbtns++] = (Btn){ x, y, w, h, id };
}

static void tri(float x1, float y1, float x2, float y2, float x3, float y3, u32 c)
{
	C2D_DrawTriangle(x1, y1, c, x2, y2, c, x3, y3, c, 0.5f);
}

// toolbar icons drawn with shapes (no font glyph needed)
static void icon(int id, float cx, float cy, u32 c)
{
	switch (id) {
	case B_BACK: tri(cx - 7, cy, cx + 5, cy - 7, cx + 5, cy + 7, c); break;
	case B_FWD: tri(cx + 7, cy, cx - 5, cy - 7, cx - 5, cy + 7, c); break;
	case B_RELOAD:
		if (job) { C2D_DrawRectSolid(cx - 6, cy - 6, 0.5f, 12, 12, c); break; }   // stop
		for (int i = 0; i < 20; i++) {
			float a = 0.6f + i * 0.25f;
			C2D_DrawCircleSolid(cx + cosf(a) * 6.5f, cy + sinf(a) * 6.5f, 0.5f, 1.4f, c);
		}
		tri(cx + 6, cy - 9, cx + 9, cy - 1, cx + 1, cy - 2, c);
		break;
	case B_HOME:
		tri(cx, cy - 8, cx - 9, cy, cx + 9, cy, c);
		C2D_DrawRectSolid(cx - 6, cy, 0.5f, 12, 7, c);
		break;
	case B_FAV: {
		bool on = fav_index(cur_url) >= 0;
		u32 col = on ? C2D_Color32(255, 200, 40, 255) : c;
		for (int i = 0; i < 5; i++) {
			float a0 = -1.5708f + i * 1.2566f, a1 = a0 + 0.6283f;
			tri(cx, cy, cx + cosf(a0) * 9, cy + sinf(a0) * 9, cx + cosf(a1) * 3.8f, cy + sinf(a1) * 3.8f, col);
			tri(cx, cy, cx + cosf(a1) * 3.8f, cy + sinf(a1) * 3.8f, cx + cosf(a0 + 1.2566f) * 9, cy + sinf(a0 + 1.2566f) * 9, col);
		}
		break;
	}
	}
}

static void draw_bottom(void)
{
	C2D_TargetClear(t_bot, C_WHITE);
	C2D_SceneBegin(t_bot);
	nbtns = 0;
	if (doc) html_draw(doc, scroll + VIEW_TOP, VIEW_BOT, (BOT_W - DOC_W) / 2, 0, focus);
	// scroll bar
	float total = html_height(doc);
	if (total > VIEW_ALL) {
		float h = VIEW_BOT * VIEW_ALL / total;
		if (h < 14) h = 14;
		float y = (VIEW_BOT - h) * (scroll / (total - VIEW_ALL + 30));
		C2D_DrawRectSolid(BOT_W - 3, y, 0.5f, 2, h, C2D_Color32(170, 175, 185, 200));
	}

	// toolbar
	C2D_DrawRectSolid(0, FOOT_Y, 0.5f, BOT_W, 240 - FOOT_Y, C_BAR);
	u32 on = C_WHITE, off = C2D_Color32(110, 120, 130, 255);
	struct { int id; float x; bool ok; } tb[] = {
		{ B_BACK, 20, hpos > 0 }, { B_FWD, 56, hpos < nhist - 1 }, { B_RELOAD, 92, true },
		{ B_FAV, 262, true }, { B_HOME, 298, true },
	};
	for (size_t i = 0; i < sizeof tb / sizeof *tb; i++) {
		icon(tb[i].id, tb[i].x, FOOT_Y + 14, tb[i].ok ? on : off);
		reg_btn(tb[i].x - 17, FOOT_Y, 34, 240 - FOOT_Y, tb[i].id);
	}
	// address box
	C2D_DrawRectSolid(114, FOOT_Y + 4, 0.5f, 128, 20, C_BAR2);
	char host[128];
	url_host(cur_url, host, sizeof host);
	text_draw(strncmp(cur_url, "about:", 6) ? host : "Pesquisar...", 178, FOOT_Y + 7, 0.42f, C_WHITE, 120, 1);
	reg_btn(114, FOOT_Y, 128, 240 - FOOT_Y, B_URL);

	if (upd_prompt) {
		nbtns = 0;
		C2D_DrawRectSolid(0, 0, 0.5f, BOT_W, 240, C2D_Color32(0, 0, 0, 140));
		C2D_DrawRectSolid(16, 36, 0.5f, 288, 168, C_CARD);
		C2D_DrawRectSolid(16, 36, 0.5f, 288, 22, C_BAR);
		text_draw("Atualização", 160, 39, 0.46f, C_WHITE, 0, 1);
		UpdState st = updater_state();
		char t[128];
		if (st == UPD_AVAILABLE) {
			snprintf(t, sizeof t, "Versão %s disponível", updater_latest());
			text_draw(t, 160, 70, 0.55f, C_TEXT, 270, 1);
			snprintf(t, sizeof t, "Você tem a %s. Baixa pelo Wi-Fi e instala sozinho.", updater_version());
			text_draw(t, 160, 96, 0.4f, C_SUB, 276, 1);
			C2D_DrawRectSolid(32, 124, 0.5f, 256, 30, C_ACCENT);
			text_draw("Atualizar agora (A)", 160, 131, 0.47f, C_WHITE, 0, 1);
			reg_btn(32, 124, 256, 30, B_UPD_YES);
			C2D_DrawRectSolid(32, 162, 0.5f, 256, 30, C_BAR2);
			text_draw("Depois (B)", 160, 169, 0.47f, C_WHITE, 0, 1);
			reg_btn(32, 162, 256, 30, B_UPD_NO);
		} else if (st == UPD_DOWNLOADING || st == UPD_INSTALLING || st == UPD_CHECKING) {
			text_draw(st == UPD_INSTALLING ? "Instalando..." : "Baixando...", 160, 74, 0.55f, C_TEXT, 270, 1);
			text_draw(updater_message(), 160, 100, 0.4f, C_SUB, 276, 1);
			float p = updater_progress();
			C2D_DrawRectSolid(40, 132, 0.5f, 240, 10, C_LINE);
			C2D_DrawRectSolid(40, 132, 0.5f, 240 * (p < 0 ? 0 : p > 1 ? 1 : p), 10, C_ACCENT);
		} else {
			bool done = st == UPD_DONE;
			text_draw(done ? "Pronto!" : "Não deu para atualizar", 160, 72, 0.55f, C_TEXT, 270, 1);
			text_draw(updater_message(), 160, 98, 0.4f, C_SUB, 276, 1);
			if (done) {
				C2D_DrawRectSolid(32, 124, 0.5f, 256, 30, C_ACCENT);
				text_draw("Fechar o app (A)", 160, 131, 0.47f, C_WHITE, 0, 1);
				reg_btn(32, 124, 256, 30, B_UPD_YES);
			}
			C2D_DrawRectSolid(32, 162, 0.5f, 256, 30, C_BAR2);
			text_draw(done ? "Continuar (B)" : "OK (B)", 160, 169, 0.47f, C_WHITE, 0, 1);
			reg_btn(32, 162, 256, 30, B_UPD_NO);
		}
	}

	if (now_ms < toast_until) {
		float w = text_width(toast_msg, 0.45f) + 20;
		if (w > 310) w = 310;
		C2D_DrawRectSolid(160 - w / 2, 176, 0.5f, w, 26, C2D_Color32(20, 24, 30, 230));
		text_draw(toast_msg, 160, 181, 0.45f, C_WHITE, 300, 1);
	}
}

// ---------------------------------------------------------------- input

static void update_answer(bool yes)
{
	UpdState st = updater_state();
	if (updater_busy()) return;
	if (yes && st == UPD_AVAILABLE) updater_install();
	else if (yes && st == UPD_DONE) quit_app = true;
	else if (!yes) upd_prompt = false;
}

static void press(int id)
{
	switch (id) {
	case B_BACK: go_back(-1); break;
	case B_FWD: go_back(1); break;
	case B_RELOAD:
		if (job) { net_forget(job); job = 0; toast("Parado"); }
		else { job_restore = scroll; navigate(cur_url, NULL, false); }
		break;
	case B_URL: ask_url(); break;
	case B_FAV: toggle_fav(); break;
	case B_HOME: navigate(START_URL, NULL, true); break;
	case B_UPD_YES: update_answer(true); break;
	case B_UPD_NO: update_answer(false); break;
	}
}

static void handle_touch(u32 down, u32 held, u32 up)
{
	touchPosition tp;
	hidTouchRead(&tp);
	if (down & KEY_TOUCH) {
		touch_down = true;
		dragging = false;
		touch_x0 = tp.px;
		touch_y0 = last_ty = tp.py;
		scroll0 = scroll;
		vel = 0;
		return;
	}
	if ((held & KEY_TOUCH) && touch_down) {
		if (abs(tp.py - touch_y0) > 6 && touch_y0 < FOOT_Y && !upd_prompt) dragging = true;
		if (dragging) {
			vel = (float)(last_ty - tp.py);
			scroll = scroll0 + (touch_y0 - tp.py);
			clamp();
		}
		last_ty = tp.py;
		return;
	}
	if ((up & KEY_TOUCH) && touch_down) {
		touch_down = false;
		if (dragging) return;
		for (int i = 0; i < nbtns; i++) {
			Btn *b = &btns[i];
			if (touch_x0 >= b->x && touch_x0 < b->x + b->w && touch_y0 >= b->y && touch_y0 < b->y + b->h) { press(b->id); return; }
		}
		if (upd_prompt || touch_y0 >= FOOT_Y || !doc) return;
		float dx = touch_x0 - (BOT_W - DOC_W) / 2, dy = touch_y0 + scroll + VIEW_TOP;
		int l = html_hit(doc, dx, dy);
		if (l >= 0) { focus = l; activate(l); }
	}
}

static void handle_keys(u32 down, u32 rep)
{
	if (upd_prompt) {
		if (down & KEY_A) update_answer(true);
		else if (down & KEY_B) update_answer(false);
		return;
	}
	if (down & KEY_B) { if (job) { net_forget(job); job = 0; } else go_back(-1); }
	if (down & KEY_X) press(B_RELOAD);
	if (down & KEY_Y) ask_url();
	if (down & KEY_SELECT) toggle_fav();
	if (down & KEY_A) activate(focus);
	if (rep & KEY_DUP) scroll -= 26;
	if (rep & KEY_DDOWN) scroll += 26;
	if (rep & KEY_R) scroll += VIEW_ALL - 40;
	if (rep & KEY_L) scroll -= VIEW_ALL - 40;
	circlePosition cp;
	hidCircleRead(&cp);
	if (abs(cp.dy) > 20) scroll -= cp.dy / 10.0f;
	if (rep & (KEY_DRIGHT | KEY_DLEFT)) {
		int dir = (rep & KEY_DRIGHT) ? 1 : -1;
		float fy = focus >= 0 ? html_link_y(doc, focus) : -1;
		bool visible = focus >= 0 && fy >= scroll && fy < scroll + VIEW_ALL;
		int n = html_link_near(doc, visible ? focus : -1, dir, scroll, VIEW_ALL);
		if (n >= 0) {
			focus = n;
			float y = html_link_y(doc, n);
			if (y < scroll || y > scroll + VIEW_ALL - 30) scroll = y - 40;
		}
	}
	clamp();
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv)
{
	updater_init("ChunoB/Navegador3DS", APP_VERSION, "Navegador3DS.3dsx", argc > 0 ? argv[0] : NULL);
	romfsInit();
	gfxInitDefault();
	osSetSpeedupEnable(true);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(8192);
	C2D_Prepare();
	t_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	t_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	text_init();
	hidSetRepeatParameters(16, 3);
	mkdir("sdmc:/3ds", 0777);
	mkdir(APP_DIR, 0777);
	settings_load();
	nfavs = load_list(APP_DIR "/favoritos.txt", favs, 100);

	u32 *socbuf = memalign(0x1000, 0x100000);
	bool net_ok = socbuf && R_SUCCEEDED(socInit(socbuf, 0x100000));
	curl_global_init(CURL_GLOBAL_DEFAULT);
	const char *ua = desktop_ua
		? "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36 Navegador3DS/" APP_VERSION
		: "Mozilla/5.0 (Linux; Android 4.4; Nintendo 3DS) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Mobile Safari/537.36 Navegador3DS/" APP_VERSION;
	net_init("romfs:/cacert.pem", ua, "text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.5", APP_DIR "/cookies.txt");

	navigate(START_URL, NULL, true);
	if (!net_ok) toast("Rede indisponível");
	u64 t_start = osGetTime();
	bool upd_checked = false, upd_told = false;

	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld(), up = hidKeysUp(), rep = hidKeysDownRepeat();
		now_ms = osGetTime();
		if ((down & KEY_START) && !upd_prompt) break;

		if (net_ok && !upd_checked && now_ms - t_start > 6000) { upd_checked = true; updater_check(); }
		if (!upd_told && updater_state() == UPD_AVAILABLE) { upd_told = true; upd_prompt = true; }
		if (quit_app) break;

		if (job) {
			NetResult r;
			if (net_poll(job, &r)) { job = 0; finish_load(&r); }
		}
		if (dl_job) {
			NetResult r;
			if (net_poll(dl_job, &r)) {
				dl_job = 0;
				toast(r.err[0] ? r.err : "Salvo em 3ds/Navegador3DS/downloads");
				free(r.body);
			}
		}

		handle_keys(down, rep);
		handle_touch(down, held, up);
		if (!touch_down && fabsf(vel) > 0.3f) { scroll += vel; vel *= 0.92f; clamp(); }

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		draw_top();
		draw_bottom();
		C3D_FrameEnd(0);
	}

	html_free(doc);
	for (int i = 0; i < nhist; i++) free(hist[i].html);
	updater_exit();
	net_exit();
	curl_global_cleanup();
	if (net_ok) socExit();
	text_exit();
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	romfsExit();
	return 0;
}
