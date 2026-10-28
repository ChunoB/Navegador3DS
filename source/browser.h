#pragma once
#include "ui.h"
#include "html.h"
#include "net.h"

#define SEARCH_URL "https://lite.duckduckgo.com/lite/?q="
#define START_URL  "about:inicio"

// url.c
void  url_resolve(const char *base, const char *href, char *out, size_t n);
void  url_host(const char *url, char *out, size_t n);
void  url_encode_component(const char *s, char *out, size_t n);
void  url_from_input(const char *in, char *out, size_t n);   // typed text -> site or search
char *to_utf8(const char *body, size_t len, const char *ctype);  // malloc'd
