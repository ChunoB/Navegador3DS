#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// Background HTTP worker. Requests run one at a time on a low-priority thread,
// reusing the HTTPS connection. The UI submits a job and polls it each frame.
typedef enum { JOB_FREE, JOB_QUEUED, JOB_RUNNING, JOB_DONE } JobState;

typedef struct {
	int    id;
	long   status;      // HTTP status (0 if no response)
	char  *body;        // response (NUL-terminated), owned by the job until taken
	size_t len;
	char   err[128];    // Portuguese error text when it failed
	float  progress;    // 0..1 for downloads
	char   final_url[1024];   // after redirects
	char   ctype[96];         // Content-Type header
} NetResult;

// cookie_file: where cookies are kept between runs (NULL = no cookies)
void  net_init(const char *ca_file, const char *user_agent, const char *accept, const char *cookie_file);
void  net_exit(void);
void  net_set_auth(const char *bearer_token);         // NULL = no Authorization header

// GET. pages > 1 follows Canvas "Link: rel=next" and merges the JSON arrays.
int   net_get(const char *url, int pages);
// POST with a form body (application/x-www-form-urlencoded).
int   net_post(const char *url, const char *form);
// Download to a file on the SD card.
int   net_download(const char *url, const char *path);

// Returns true once the job finished; fills *out (caller frees out->body with free()).
bool  net_poll(int id, NetResult *out);
float net_progress(int id);
void  net_forget(int id);                               // result no longer wanted
bool  net_busy(void);

// URL-encodes s into out.
void  url_encode(const char *s, char *out, size_t n);
