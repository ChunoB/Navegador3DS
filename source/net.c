#include "net.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_JOBS  24
#define MAX_BODY  (6 * 1024 * 1024)

typedef enum { M_GET, M_POST, M_FILE } Method;

typedef struct {
	JobState state;
	int      id;
	Method   method;
	char    *url;
	char    *form;
	char    *path;
	int      pages;
	bool     forgotten;
	NetResult res;
} Job;

static Job        g_jobs[MAX_JOBS];
static LightLock  g_lock;
static LightEvent g_wake;
static Thread     g_thread;
static volatile bool g_quit;
static int        g_next_id = 1;
static char       g_ca[256], g_ua[160], g_accept[160], g_cookies[256];
static char       g_auth[512];
static volatile int g_running_id;

// ---------------------------------------------------------------- curl callbacks

typedef struct { char *buf; size_t len, cap; bool over; } Mem;

static size_t on_body(char *p, size_t sz, size_t n, void *ud)
{
	Mem *m = ud;
	size_t k = sz * n;
	if (m->len + k + 1 > m->cap) {
		size_t nc = m->cap ? m->cap * 2 : 16384;
		while (nc < m->len + k + 1) nc *= 2;
		if (nc > MAX_BODY) { m->over = true; return 0; }
		char *nb = realloc(m->buf, nc);
		if (!nb) { m->over = true; return 0; }
		m->buf = nb;
		m->cap = nc;
	}
	memcpy(m->buf + m->len, p, k);
	m->len += k;
	m->buf[m->len] = 0;
	return k;
}

typedef struct { char next[1024]; } Hdr;

// Canvas paginates with: Link: <https://...&page=2>; rel="current", <...>; rel="next", ...
static size_t on_header(char *p, size_t sz, size_t n, void *ud)
{
	Hdr *h = ud;
	size_t k = sz * n;
	if (k > 5 && !strncasecmp(p, "link:", 5)) {
		char line[4096];
		size_t l = k < sizeof line - 1 ? k : sizeof line - 1;
		memcpy(line, p, l);
		line[l] = 0;
		char *r = strstr(line, "rel=\"next\"");
		if (r) {
			*r = 0;
			char *lt = strrchr(line, '<');
			char *gt = lt ? strchr(lt, '>') : NULL;
			if (lt && gt && (size_t)(gt - lt - 1) < sizeof h->next) {
				memcpy(h->next, lt + 1, gt - lt - 1);
				h->next[gt - lt - 1] = 0;
			}
		}
	}
	return k;
}

static int on_progress(void *ud, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un)
{
	Job *j = ud;
	if (total > 0) j->res.progress = (float)now / (float)total;
	return g_quit || j->forgotten ? 1 : 0;   // abort
}

static size_t on_file(char *p, size_t sz, size_t n, void *ud)
{
	return fwrite(p, sz, n, (FILE *)ud) * sz;
}

// ---------------------------------------------------------------- worker

static void setup(CURL *c, Job *j, const char *url, struct curl_slist **hdrs)
{
	curl_easy_reset(c);
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_USERAGENT, g_ua);
	if (g_cookies[0]) {
		// read the saved cookies once; after that the handle keeps them in memory
		static bool loaded;
		curl_easy_setopt(c, CURLOPT_COOKIEFILE, loaded ? "" : g_cookies);
		curl_easy_setopt(c, CURLOPT_COOKIEJAR, g_cookies);
		loaded = true;
	}
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(c, CURLOPT_CAINFO, g_ca);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 32L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 40L);
	curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 32 * 1024L);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
	curl_easy_setopt(c, CURLOPT_XFERINFODATA, j);
	char acc[200];
	snprintf(acc, sizeof acc, "Accept: %s", g_accept);
	*hdrs = curl_slist_append(NULL, acc);
	*hdrs = curl_slist_append(*hdrs, "Accept-Language: pt-BR,pt;q=0.9,en;q=0.6");
	// the token only goes to the Canvas server itself, never to file hosts after a redirect
	if (g_auth[0]) {
		char h[600];
		snprintf(h, sizeof h, "Authorization: Bearer %s", g_auth);
		*hdrs = curl_slist_append(*hdrs, h);
		curl_easy_setopt(c, CURLOPT_UNRESTRICTED_AUTH, 0L);
	}
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, *hdrs);
}

static void describe(Job *j, CURLcode r, long status)
{
	if (r == CURLE_OK && status >= 200 && status < 300) return;
	if (status == 401) snprintf(j->res.err, sizeof j->res.err, "Precisa de login (401).");
	else if (status == 403) snprintf(j->res.err, sizeof j->res.err, "Sem permissão para ver isso (403).");
	else if (status == 404) snprintf(j->res.err, sizeof j->res.err, "Não encontrado (404).");
	else if (status >= 400) snprintf(j->res.err, sizeof j->res.err, "O servidor respondeu com erro %ld.", status);
	else if (r == CURLE_COULDNT_RESOLVE_HOST) snprintf(j->res.err, sizeof j->res.err, "Sem internet (não achei o servidor).");
	else if (r == CURLE_OPERATION_TIMEDOUT) snprintf(j->res.err, sizeof j->res.err, "A conexão demorou demais.");
	else if (r == CURLE_WRITE_ERROR) snprintf(j->res.err, sizeof j->res.err, "Resposta grande demais ou erro ao gravar.");
	else snprintf(j->res.err, sizeof j->res.err, "Falha na conexão: %s", curl_easy_strerror(r));
}

// Appends JSON array text b to array text a: "[x,y]" + "[z]" -> "[x,y,z]".
static bool merge_arrays(Mem *a, const Mem *b)
{
	size_t ae = a->len, bs = 0;
	while (ae && a->buf[ae - 1] != ']') ae--;
	while (bs < b->len && b->buf[bs] != '[') bs++;
	if (!ae || bs >= b->len) return false;
	bool a_empty = true;
	for (size_t i = 0; i + 1 < ae; i++) if (a->buf[i] != '[' && a->buf[i] > ' ') { a_empty = false; break; }
	bool b_empty = true;
	for (size_t i = bs + 1; i < b->len; i++) {
		if (b->buf[i] == ']') break;
		if (b->buf[i] > ' ') { b_empty = false; break; }
	}
	if (b_empty) return true;
	a->len = ae - 1;   // drop ']'
	Mem tmp = *a;
	if (!a_empty) on_body(",", 1, 1, &tmp);
	on_body(b->buf + bs + 1, 1, b->len - bs - 1, &tmp);
	*a = tmp;
	return !a->over;
}

static void run(CURL *c, Job *j)
{
	struct curl_slist *h = NULL;
	CURLcode r;
	long status = 0;

	if (j->method == M_FILE) {
		char tmp[300];
		snprintf(tmp, sizeof tmp, "%s.part", j->path);
		FILE *f = fopen(tmp, "wb");
		if (!f) { snprintf(j->res.err, sizeof j->res.err, "Não consegui gravar no cartão SD."); return; }
		setup(c, j, j->url, &h);
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_file);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
		r = curl_easy_perform(c);
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
		curl_slist_free_all(h);
		fclose(f);
		j->res.status = status;
		describe(j, r, status);
		if (j->res.err[0]) { remove(tmp); return; }
		remove(j->path);
		rename(tmp, j->path);
		return;
	}

	Mem all = {0};
	char url[1024];
	snprintf(url, sizeof url, "%s", j->url);
	for (int page = 0; page < (j->pages > 0 ? j->pages : 1) && url[0]; page++) {
		Mem m = {0};
		Hdr hd = {{0}};
		setup(c, j, url, &h);
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
		curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
		curl_easy_setopt(c, CURLOPT_HEADERDATA, &hd);
		if (j->method == M_POST) {
			curl_easy_setopt(c, CURLOPT_POST, 1L);
			curl_easy_setopt(c, CURLOPT_POSTFIELDS, j->form ? j->form : "");
		}
		r = curl_easy_perform(c);
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
		curl_slist_free_all(h);
		h = NULL;
		if (m.over) r = CURLE_WRITE_ERROR;
		j->res.status = status;
		describe(j, r, status);
		char *eu = NULL, *ct = NULL;
		curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eu);
		curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
		snprintf(j->res.final_url, sizeof j->res.final_url, "%s", eu ? eu : url);
		snprintf(j->res.ctype, sizeof j->res.ctype, "%s", ct ? ct : "");
		if (g_cookies[0]) curl_easy_setopt(c, CURLOPT_COOKIELIST, "FLUSH");
		// an error page from the server still has a body worth showing
		if (j->res.err[0] && (r != CURLE_OK || !m.len)) { free(m.buf); break; }
		if (j->res.err[0]) { all = m; break; }
		if (page == 0) all = m;
		else { merge_arrays(&all, &m); free(m.buf); }
		snprintf(url, sizeof url, "%s", hd.next);
		if (g_quit || j->forgotten) break;
	}
	if (j->res.err[0] && !all.len) { free(all.buf); return; }
	j->res.body = all.buf ? all.buf : calloc(1, 1);
	j->res.len = all.len;
}

static void worker(void *arg)
{
	CURL *c = curl_easy_init();
	while (!g_quit) {
		Job *j = NULL;
		LightLock_Lock(&g_lock);
		for (int i = 0; i < MAX_JOBS; i++) {
			Job *k = &g_jobs[i];
			if (k->state == JOB_QUEUED && (!j || k->id < j->id)) j = k;
		}
		if (j) { j->state = JOB_RUNNING; g_running_id = j->id; }
		LightLock_Unlock(&g_lock);
		if (!j) { LightEvent_Wait(&g_wake); continue; }

		if (!j->forgotten && c) run(c, j);
		else if (!c) snprintf(j->res.err, sizeof j->res.err, "Sem memória para a rede.");

		LightLock_Lock(&g_lock);
		free(j->url); free(j->form); free(j->path);
		j->url = j->form = j->path = NULL;
		g_running_id = 0;
		if (j->forgotten) { free(j->res.body); memset(j, 0, sizeof *j); }
		else j->state = JOB_DONE;
		LightLock_Unlock(&g_lock);
	}
	if (c) curl_easy_cleanup(c);
}

// ---------------------------------------------------------------- public

void net_init(const char *ca_file, const char *user_agent, const char *accept, const char *cookie_file)
{
	snprintf(g_ca, sizeof g_ca, "%s", ca_file);
	snprintf(g_ua, sizeof g_ua, "%s", user_agent);
	snprintf(g_accept, sizeof g_accept, "%s", accept);
	snprintf(g_cookies, sizeof g_cookies, "%s", cookie_file ? cookie_file : "");
	LightLock_Init(&g_lock);
	LightEvent_Init(&g_wake, RESET_ONESHOT);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// below libctru's APT thread (0x31): the HOME button never waits on the network
	g_thread = threadCreate(worker, NULL, 96 * 1024, prio + 2, -2, false);
}

void net_exit(void)
{
	g_quit = true;
	LightEvent_Signal(&g_wake);
	if (g_thread) { threadJoin(g_thread, U64_MAX); threadFree(g_thread); }
	for (int i = 0; i < MAX_JOBS; i++) {
		free(g_jobs[i].url); free(g_jobs[i].form); free(g_jobs[i].path); free(g_jobs[i].res.body);
	}
	memset(g_jobs, 0, sizeof g_jobs);
}

void net_set_auth(const char *tok)
{
	snprintf(g_auth, sizeof g_auth, "%s", tok ? tok : "");
}

static int submit(Method m, const char *url, const char *form, const char *path, int pages)
{
	int id = -1;
	LightLock_Lock(&g_lock);
	for (int i = 0; i < MAX_JOBS; i++) {
		Job *j = &g_jobs[i];
		if (j->state != JOB_FREE) continue;
		memset(j, 0, sizeof *j);
		j->state = JOB_QUEUED;
		j->id = id = g_next_id++;
		j->method = m;
		j->url = strdup(url);
		j->form = form ? strdup(form) : NULL;
		j->path = path ? strdup(path) : NULL;
		j->pages = pages;
		break;
	}
	LightLock_Unlock(&g_lock);
	if (id > 0) LightEvent_Signal(&g_wake);
	return id;
}

int net_get(const char *url, int pages) { return submit(M_GET, url, NULL, NULL, pages); }
int net_post(const char *url, const char *form) { return submit(M_POST, url, form, NULL, 1); }
int net_download(const char *url, const char *path) { return submit(M_FILE, url, NULL, path, 1); }

bool net_poll(int id, NetResult *out)
{
	bool done = false;
	LightLock_Lock(&g_lock);
	for (int i = 0; i < MAX_JOBS; i++) {
		Job *j = &g_jobs[i];
		if (j->id != id) continue;
		if (j->state == JOB_DONE) {
			*out = j->res;
			out->id = id;
			memset(j, 0, sizeof *j);
			done = true;
		}
		break;
	}
	LightLock_Unlock(&g_lock);
	return done;
}

float net_progress(int id)
{
	float p = 0;
	LightLock_Lock(&g_lock);
	for (int i = 0; i < MAX_JOBS; i++) if (g_jobs[i].id == id) { p = g_jobs[i].res.progress; break; }
	LightLock_Unlock(&g_lock);
	return p;
}

void net_forget(int id)
{
	LightLock_Lock(&g_lock);
	for (int i = 0; i < MAX_JOBS; i++) {
		Job *j = &g_jobs[i];
		if (j->id != id) continue;
		if (j->state == JOB_RUNNING) j->forgotten = true;
		else { free(j->url); free(j->form); free(j->path); free(j->res.body); memset(j, 0, sizeof *j); }
		break;
	}
	LightLock_Unlock(&g_lock);
}

bool net_busy(void)
{
	bool b = false;
	LightLock_Lock(&g_lock);
	for (int i = 0; i < MAX_JOBS; i++) if (g_jobs[i].state == JOB_QUEUED || g_jobs[i].state == JOB_RUNNING) b = true;
	LightLock_Unlock(&g_lock);
	return b;
}

void url_encode(const char *s, char *out, size_t n)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t o = 0;
	for (; *s && o + 4 < n; s++) {
		unsigned char ch = *s;
		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || strchr("-_.~", ch)) out[o++] = ch;
		else { out[o++] = '%'; out[o++] = hex[ch >> 4]; out[o++] = hex[ch & 15]; }
	}
	out[o] = 0;
}
