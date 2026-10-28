#define _GNU_SOURCE
// URL helpers and text encoding fixes for the browser.
#include "browser.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Resolves href against base ("https://host/dir/page?x"), like a browser does.
void url_resolve(const char *base, const char *href, char *out, size_t n)
{
	while (*href == ' ' || *href == '\t' || *href == '\n' || *href == '\r') href++;
	if (!*href) { snprintf(out, n, "%s", base); return; }
	// already absolute (http:, https:, about:, mailto:...)
	const char *colon = strchr(href, ':');
	if (colon) {
		bool scheme = colon > href;
		for (const char *p = href; p < colon; p++) if (!isalnum((unsigned char)*p) && *p != '+' && *p != '-' && *p != '.') scheme = false;
		if (scheme) { snprintf(out, n, "%s", href); return; }
	}
	const char *sep = strstr(base, "://");
	if (!sep) { snprintf(out, n, "%s", href); return; }
	size_t scheme_len = sep - base;                     // "https"
	const char *host = sep + 3;
	const char *path = strchr(host, '/');
	size_t host_len = path ? (size_t)(path - host) : strlen(host);
	if (!path) path = "/";

	if (href[0] == '/' && href[1] == '/') { snprintf(out, n, "%.*s:%s", (int)scheme_len, base, href); return; }
	if (href[0] == '/') { snprintf(out, n, "%.*s://%.*s%s", (int)scheme_len, base, (int)host_len, host, href); return; }
	// base path without query/fragment
	char bpath[1024];
	size_t pl = strcspn(path, "?#");
	if (pl >= sizeof bpath) pl = sizeof bpath - 1;
	memcpy(bpath, path, pl);
	bpath[pl] = 0;
	if (href[0] == '#') { snprintf(out, n, "%.*s://%.*s%s%s", (int)scheme_len, base, (int)host_len, host, bpath, href); return; }
	if (href[0] == '?') { snprintf(out, n, "%.*s://%.*s%s%s", (int)scheme_len, base, (int)host_len, host, bpath, href); return; }
	// relative path: drop the last segment of the base, then handle ./ and ../
	char *slash = strrchr(bpath, '/');
	if (slash) slash[1] = 0;
	char joined[2048];
	snprintf(joined, sizeof joined, "%s%s", bpath, href);
	// normalize dot segments (only in the path part)
	char *q = joined + strcspn(joined, "?#");
	char tail[1024];
	snprintf(tail, sizeof tail, "%s", q);
	*q = 0;
	char outp[2048] = "";
	char *seg = joined + 1, *save;
	size_t ol = 0;
	char *tok = strtok_r(seg, "/", &save);
	char *segs[128];
	int ns = 0;
	bool trailing = joined[0] && strlen(href) && (href[strlen(href) - 1] == '/' || !strcmp(href, ".") || !strcmp(href, ".."));
	while (tok && ns < 128) {
		if (!strcmp(tok, ".")) { }
		else if (!strcmp(tok, "..")) { if (ns) ns--; }
		else segs[ns++] = tok;
		tok = strtok_r(NULL, "/", &save);
	}
	for (int i = 0; i < ns && ol + 2 < sizeof outp; i++) ol += snprintf(outp + ol, sizeof outp - ol, "/%s", segs[i]);
	if (!ns || trailing) { if (ol + 2 < sizeof outp) { outp[ol++] = '/'; outp[ol] = 0; } }
	snprintf(out, n, "%.*s://%.*s%s%s", (int)scheme_len, base, (int)host_len, host, outp, tail);
}

void url_host(const char *url, char *out, size_t n)
{
	const char *p = strstr(url, "://");
	p = p ? p + 3 : url;
	size_t l = strcspn(p, "/?#:");
	if (l >= n) l = n - 1;
	memcpy(out, p, l);
	out[l] = 0;
	if (!strncmp(out, "www.", 4)) memmove(out, out + 4, strlen(out + 4) + 1);
}

void url_encode_component(const char *s, char *out, size_t n)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t o = 0;
	for (; *s && o + 4 < n; s++) {
		unsigned char c = *s;
		if (isalnum(c) || strchr("-_.~", c)) out[o++] = c;
		else if (c == ' ') out[o++] = '+';
		else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
	}
	out[o] = 0;
}

// What the user typed in the address bar: a site or a search.
void url_from_input(const char *in, char *out, size_t n)
{
	while (*in == ' ') in++;
	char t[1024];
	snprintf(t, sizeof t, "%s", in);
	size_t l = strlen(t);
	while (l && t[l - 1] == ' ') t[--l] = 0;
	if (!strncmp(t, "http://", 7) || !strncmp(t, "https://", 8) || !strncmp(t, "about:", 6)) { snprintf(out, n, "%s", t); return; }
	bool looks_like_site = strchr(t, '.') && !strchr(t, ' ') && t[0] != '.' && t[l - 1] != '.';
	if (looks_like_site) { snprintf(out, n, "https://%s", t); return; }
	char q[900];
	url_encode_component(t, q, sizeof q);
	snprintf(out, n, SEARCH_URL "%s", q);
}

// ---------------------------------------------------------------- encodings

static const unsigned short cp1252[32] = {
	0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
	0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

static int put_cp(char *o, unsigned cp)
{
	if (cp < 0x80) { o[0] = cp; return 1; }
	if (cp < 0x800) { o[0] = 0xC0 | (cp >> 6); o[1] = 0x80 | (cp & 63); return 2; }
	o[0] = 0xE0 | (cp >> 12); o[1] = 0x80 | ((cp >> 6) & 63); o[2] = 0x80 | (cp & 63);
	return 3;
}

// Finds the page's charset: HTTP header first, then <meta charset> near the top.
static void find_charset(const char *ctype, const char *body, size_t len, char *cs, size_t n)
{
	cs[0] = 0;
	const char *p = ctype ? strcasestr(ctype, "charset=") : NULL;
	if (!p && body) {
		char head[3072];
		size_t l = len < sizeof head - 1 ? len : sizeof head - 1;
		memcpy(head, body, l);
		head[l] = 0;
		p = strcasestr(head, "charset=");
		if (p) {
			p += 8;
			while (*p == '"' || *p == '\'' || *p == ' ') p++;
			size_t k = 0;
			while (p[k] && (isalnum((unsigned char)p[k]) || p[k] == '-' || p[k] == '_') && k + 1 < n) { cs[k] = tolower((unsigned char)p[k]); k++; }
			cs[k] = 0;
			return;
		}
	}
	if (!p) return;
	p += 8;
	while (*p == '"' || *p == ' ') p++;
	size_t k = 0;
	while (p[k] && (isalnum((unsigned char)p[k]) || p[k] == '-' || p[k] == '_') && k + 1 < n) { cs[k] = tolower((unsigned char)p[k]); k++; }
	cs[k] = 0;
}

// Returns a malloc'd UTF-8 copy of the page body.
char *to_utf8(const char *body, size_t len, const char *ctype)
{
	char cs[40];
	find_charset(ctype, body, len, cs, sizeof cs);
	bool latin = strstr(cs, "8859") || strstr(cs, "1252") || !strcmp(cs, "latin1") || !strcmp(cs, "ascii");
	char *out = malloc(len * 3 + 1);
	if (!out) return NULL;
	size_t o = 0;
	const unsigned char *p = (const unsigned char *)body, *e = p + len;
	while (p < e) {
		unsigned char c = *p;
		if (c < 0x80) { out[o++] = c ? c : ' '; p++; continue; }
		if (!latin) {
			// valid UTF-8 sequence? copy it as is
			int need = c >= 0xF0 && c < 0xF8 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 && c < 0xE0 ? 1 : -1;
			if (need > 0 && p + need < e + 0) {
				bool ok = true;
				for (int k = 1; k <= need; k++) if ((p[k] & 0xC0) != 0x80) ok = false;
				if (ok) { memcpy(out + o, p, need + 1); o += need + 1; p += need + 1; continue; }
			}
		}
		// Latin-1 / Windows-1252 byte
		unsigned cp = c >= 0x80 && c < 0xA0 ? cp1252[c - 0x80] : c;
		if (!cp) cp = '?';
		o += put_cp(out + o, cp);
		p++;
	}
	out[o] = 0;
	return out;
}
