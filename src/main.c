#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <json-c/json.h>

#include <chiaki/session.h>
#include <chiaki/discovery.h>
#include <chiaki/log.h>
#include <chiaki/base64.h>
#include <curl/curl.h>
// For PSN Remote Play session nonces (data1/data2)
#include <openssl/rand.h>
#include <openssl/evp.h>
// chiaki/remote/holepunch.h no longer needed — PSN wakeup is done
// with direct curl calls to avoid the library's broken SSL paths.

#include <ss4s.h>

#include "config.h"
#include "video.h"
#include "audio.h"
#include "input.h"
#include "ui.h"
#include "stats.h"
#include "stream_health.h"
#include "config_import.h"
#include "app_id.h"
#include "root_feedback.h"
#include "webos_keys.h"

#define CONFIG_PATH CHIAKI_APP_DIR "/config.json"
#define LOG_PATH    "/tmp/chiaki.log"

#ifndef CHIAKI_NG_REVISION
#define CHIAKI_NG_REVISION "unknown"
#endif
#ifndef SS4S_REVISION
#define SS4S_REVISION "unknown"
#endif

// ── Global state ──────────────────────────────────────────────────────────────

// Session callbacks run on Chiaki threads. Keep their shared state atomic; the
// signal handler only touches sig_atomic_t and is observed by the main thread.
static atomic_bool g_should_exit   = ATOMIC_VAR_INIT(false);
static atomic_bool g_session_ended = ATOMIC_VAR_INIT(false);
static atomic_int  g_quit_reason   = ATOMIC_VAR_INIT(CHIAKI_QUIT_REASON_NONE);
static atomic_bool g_ps5_sleeping  = ATOMIC_VAR_INIT(false);
static volatile sig_atomic_t g_signal_exit = 0;

static ChiakiSession  g_session;
static SDL_Window    *g_window   = NULL;
static SDL_Renderer  *g_renderer = NULL;

// Global log file — accessible via app_log() from audio.c / video.c
FILE *g_log_file   = NULL;
int   g_log_level  = 0x1c;  // default: INFO|WARNING|ERROR until config loads

static bool app_exit_requested(void)
{
    return g_signal_exit != 0 ||
           atomic_load_explicit(&g_should_exit, memory_order_acquire);
}

static void app_request_exit(void)
{
    atomic_store_explicit(&g_should_exit, true, memory_order_release);
    atomic_store_explicit(&g_session_ended, true, memory_order_release);
}

// ── Shared log helpers ────────────────────────────────────────────────────────

// app_log_always: unconditional — bypasses log_level gate entirely.
// Use for wakeup/probe status, session lifecycle, and anything that must
// appear regardless of what level the user configured.
void app_log_always(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_log_file) {
        va_start(ap, fmt);
        vfprintf(g_log_file, fmt, ap);
        va_end(ap);
        fflush(g_log_file);
    }
}

// app_log: INFO-gated — for high-volume operational chatter.
void app_log(const char *fmt, ...)
{
    if (!(g_log_level & 0x04)) return;  // INFO bit not set — suppress
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_log_file) {
        va_start(ap, fmt);
        vfprintf(g_log_file, fmt, ap);
        va_end(ap);
        fflush(g_log_file);
    }
}

// ── Logging ───────────────────────────────────────────────────────────────────

// ChiakiLogLevel is a BITMASK: DEBUG=1, VERBOSE=2, INFO=4, WARNING=8, ERROR=16.
static const char *chiaki_level_str(ChiakiLogLevel level)
{
    switch (level) {
    case CHIAKI_LOG_DEBUG:   return "DEBUG";
    case CHIAKI_LOG_VERBOSE: return "VERBOSE";
    case CHIAKI_LOG_INFO:    return "INFO";
    case CHIAKI_LOG_WARNING: return "WARNING";
    case CHIAKI_LOG_ERROR:   return "ERROR";
    default:                 return "LOG";
    }
}

static void log_cb(ChiakiLogLevel level, const char *msg, void *user)
{
    const char *lvl = chiaki_level_str(level);
    fprintf(stderr, "[CHIAKI][%s] %s\n", lvl, msg);
    if (g_log_file) {
        fprintf(g_log_file, "[CHIAKI][%s] %s\n", lvl, msg);
        fflush(g_log_file);
    }
}

// ── Session callbacks ─────────────────────────────────────────────────────────

static void session_event_cb(ChiakiEvent *event, void *user)
{
    InputContext *input_ctx = user;
    input_handle_session_event(input_ctx, event);

    switch (event->type)
    {
    case CHIAKI_EVENT_CONNECTED:
        app_log("[APP] Connected to PS5\n");
        break;
    case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
        app_log("[APP] PIN requested — register first using chiaki-ng on PC\n");
        chiaki_session_set_login_pin(&g_session,
            (const uint8_t *)"0000", 4);
        break;
    case CHIAKI_EVENT_RUMBLE:
    case CHIAKI_EVENT_TRIGGER_EFFECTS:
    case CHIAKI_EVENT_LED_COLOR:
    case CHIAKI_EVENT_PLAYER_INDEX:
    case CHIAKI_EVENT_HAPTIC_INTENSITY:
    case CHIAKI_EVENT_TRIGGER_INTENSITY:
        /* Applied asynchronously by input_handle_session_event(). */
        break;
    case CHIAKI_EVENT_VIDEO_FEC_FAILURE:
        atomic_fetch_add_explicit(&g_stream_stats.video_fec_failures, 1,
                                  memory_order_relaxed);
        if (event->video_fec_failure.idr_request_sent)
            atomic_fetch_add_explicit(&g_stream_stats.video_idr_requests, 1,
                                      memory_order_relaxed);
        app_log("[VIDEO] FEC failure frame=%d automatic_idr=%d\n",
                event->video_fec_failure.frame_index,
                event->video_fec_failure.idr_request_sent ? 1 : 0);
        break;
    case CHIAKI_EVENT_QUIT:
        // Log the reason so we can diagnose what triggered the disconnect.
        app_log_always("[APP] Session quit: reason=%d (%s) reason_str=%s\n",
                event->quit.reason,
                chiaki_quit_reason_string(event->quit.reason),
                event->quit.reason_str ? event->quit.reason_str : "(none)");
        atomic_store_explicit(&g_quit_reason, event->quit.reason,
                              memory_order_release);
        atomic_store_explicit(&g_session_ended, true, memory_order_release);
        // Detect PS5 Rest Mode via reason_str (no dedicated enum in this chiaki-ng version).
        // When the PS5 enters Rest Mode mid-stream it sends "Server shutting down".
        // Set flags so the event loop exits and we do not reconnect.
        if (event->quit.reason == CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN ||
            (event->quit.reason_str &&
             (strstr(event->quit.reason_str, "sleep") ||
              strstr(event->quit.reason_str, "standby") ||
              strstr(event->quit.reason_str, "shutting down"))))
        {
            app_log("[APP] PS5 entered Rest Mode (%s) -- exiting\n",
                    event->quit.reason_str);
            atomic_store_explicit(&g_ps5_sleeping, true, memory_order_release);
            atomic_store_explicit(&g_should_exit, true, memory_order_release);
        }
        break;
    default:
        break;
    }
}

// ── Signal handler ────────────────────────────────────────────────────────────
// SIGTERM is sent by webOS when the user force-closes the app.
// Treat it as a deliberate exit — no reconnect.
static void sig_handler(int sig)
{
    (void)sig;
    g_signal_exit = 1;
}

static void ss4s_log_cb(SS4S_LogLevel level, const char *tag, const char *fmt, ...)
{
    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (level <= SS4S_LogLevelWarn)
        app_log_always("[SS4S][%s][%d] %s\n", tag ? tag : "?", level, message);
    else
        app_log("[SS4S][%s][%d] %s\n", tag ? tag : "?", level, message);
}

// ── Wakeup ────────────────────────────────────────────────────────────────────

static bool host_port_ready(const char *host, uint16_t port, int timeout_ms)
{
    if (!host || !host[0] || timeout_ms < 0)
        return false;

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1)
        return false;

    int probe = socket(AF_INET, SOCK_STREAM, 0);
    if (probe < 0)
        return false;

    int flags = fcntl(probe, F_GETFL, 0);
    if (flags < 0 || fcntl(probe, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(probe);
        return false;
    }

    int rc = connect(probe, (struct sockaddr *)&address, sizeof(address));
    if (rc == 0) {
        close(probe);
        return true;
    }
    if (errno != EINPROGRESS) {
        close(probe);
        return false;
    }

    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(probe, &write_fds);
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    rc = select(probe + 1, NULL, &write_fds, NULL, &timeout);
    if (rc <= 0) {
        close(probe);
        return false;
    }

    int socket_error = 0;
    socklen_t error_len = sizeof(socket_error);
    bool ready = getsockopt(probe, SOL_SOCKET, SO_ERROR,
                            &socket_error, &error_len) == 0 &&
                 socket_error == 0;
    close(probe);
    return ready;
}

static void do_wakeup(AppConfig *cfg, ChiakiLog *log)
{
    uint8_t account_id[8];
    size_t decoded_len = sizeof(account_id);
    if (chiaki_base64_decode(cfg->psn_account_id_b64,
                             strlen(cfg->psn_account_id_b64),
                             account_id, &decoded_len) != CHIAKI_ERR_SUCCESS)
    {
        app_log_always("[WAKEUP] Failed to decode psn_account_id — check config\n");
        return;
    }
    uint64_t credential = *(uint64_t *)account_id;

    // Send unicast to the PS5's IP address.
    app_log("[WAKEUP] Sending packet (unicast) to %s\n", cfg->host);
    ChiakiErrorCode err = chiaki_discovery_wakeup(log, NULL, cfg->host,
                                                  credential, cfg->ps5);
    if (err != CHIAKI_ERR_SUCCESS)
        app_log_always("[WAKEUP] Unicast failed: %s\n", chiaki_error_string(err));

    // Also send to the subnet broadcast address (x.x.x.255).
    // Some PS5 firmware/router combinations drop unicast UDP 987 in rest mode.
    char broadcast[32];
    strncpy(broadcast, cfg->host, sizeof(broadcast) - 1);
    broadcast[sizeof(broadcast) - 1] = '\0';
    char *last_dot = strrchr(broadcast, '.');
    if (last_dot)
    {
        strcpy(last_dot + 1, "255");
        app_log("[WAKEUP] Sending packet (broadcast) to %s\n", broadcast);
        err = chiaki_discovery_wakeup(log, NULL, broadcast, credential, cfg->ps5);
        if (err != CHIAKI_ERR_SUCCESS)
            app_log_always("[WAKEUP] Broadcast failed: %s\n", chiaki_error_string(err));
    }
}


// ── SSL / CA certificate setup ───────────────────────────────────────────────
static void setup_ssl_ca_bundle(void)
{
    static const char *ca_paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/cert.pem",
        "/usr/share/ca-certificates/ca-certificates.crt",
        CHIAKI_APP_DIR "/cacert.pem",
        NULL
    };
    const char *existing = getenv("CURL_CA_BUNDLE");
    if (existing && existing[0] && access(existing, R_OK) == 0) {
        app_log_always("[SSL] Using CURL_CA_BUNDLE=%s (preset)\n", existing);
        return;
    }
    for (int i = 0; ca_paths[i]; i++) {
        if (access(ca_paths[i], R_OK) == 0) {
            setenv("CURL_CA_BUNDLE", ca_paths[i], 1);
            setenv("SSL_CERT_FILE",  ca_paths[i], 1);
            app_log_always("[SSL] Found CA bundle: %s\n", ca_paths[i]);
            return;
        }
    }
    app_log_always("[SSL] WARNING: No CA bundle found\n");
}

// ── PSN cloud wakeup (direct HTTP — no chiaki holepunch library) ─────────────
// The chiaki-ng holepunch library creates its own internal curl handles with
// SSL verification pointed at build-time CA paths that don't exist on webOS.
// We can't fix the library's handles, so we implement the PSN wakeup API
// calls ourselves using curl handles we control.
//
// PSN OAuth credentials — public, baked into all chiaki-ng releases.
#define PSN_CLIENT_ID     "ba495a24-818c-472b-b12d-ff231c1b5745"
#define PSN_CLIENT_SECRET "mvaiZkRsAsI1IBkY"
#define PSN_TOKEN_URL     "https://ca.account.sony.com/api/authz/v3/oauth/token"
#define PSN_SCOPE \
    "psn:clientapp " \
    "referenceDataService:countryConfig.read " \
    "pushNotification:webSocket.desktop.connect " \
    "sessionManager:remotePlaySession.system.update"

// PSN API endpoints
#define PSN_DEVICES_URL \
    "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/clients" \
    "?platform=PS5&includeFields=device&limit=10&offset=0"
#define PSN_SESSION_URL \
    "https://web.np.playstation.com/api/sessionManager/v1/remotePlaySessions"
#define PSN_COMMAND_URL \
    "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/commands"


typedef struct { char *buf; size_t len; } CurlBuf;

// Generate a 16-byte random nonce and base64-encode it into out (NUL-terminated).
// Output length will be 24 chars + NUL.
static bool psn_make_b64_nonce(char out[32])
{
    unsigned char raw[16];
    if (RAND_bytes(raw, (int)sizeof(raw)) != 1)
        return false;
    int n = EVP_EncodeBlock((unsigned char *)out, raw, (int)sizeof(raw));
    if (n <= 0 || n >= 31)
        return false;
    out[n] = '\0';
    return true;
}

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    CurlBuf *b = (CurlBuf *)userdata;
    size_t total = size * nmemb;
    char *tmp = realloc(b->buf, b->len + total + 1);
    if (!tmp) return 0;
    b->buf = tmp;
    memcpy(b->buf + b->len, ptr, total);
    b->len += total;
    b->buf[b->len] = '\0';
    return total;
}

// Helper: set common curl options for PSN HTTPS calls
static void psn_curl_common(CURL *curl, CurlBuf *resp)
{
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       15L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
}

// ── Token refresh ────────────────────────────────────────────────────────────
static char *psn_refresh_access_token(const char *refresh_token)
{
    CURL *curl = curl_easy_init();
    if (!curl) { app_log_always("[PSN] curl_easy_init failed\n"); return NULL; }

    const char *token = refresh_token;
    if (strncmp(token, "v3.", 3) == 0) {
        token += 3;
        app_log_always("[PSN] Stripped 'v3.' prefix from refresh token\n");
    }
    app_log_always("[PSN] Refresh token available (len=%zu)\n", strlen(token));

    char *enc_token = curl_easy_escape(curl, token, 0);
    char *enc_scope = curl_easy_escape(curl, PSN_SCOPE, 0);
    char body[4096];
    snprintf(body, sizeof(body),
        "grant_type=refresh_token&refresh_token=%s&scope=%s", enc_token, enc_scope);
    curl_free(enc_token);
    curl_free(enc_scope);

    CurlBuf resp = {0};
    struct curl_slist *hdrs = curl_slist_append(NULL,
        "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(curl, CURLOPT_URL,       PSN_TOKEN_URL);
    curl_easy_setopt(curl, CURLOPT_POST,      1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_USERNAME,  PSN_CLIENT_ID);
    curl_easy_setopt(curl, CURLOPT_PASSWORD,  PSN_CLIENT_SECRET);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    psn_curl_common(curl, &resp);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        app_log_always("[PSN] Token refresh curl error: %s\n", curl_easy_strerror(rc));
        free(resp.buf);
        return NULL;
    }
    app_log_always("[PSN] Token endpoint HTTP %ld (%zu bytes)\n", http_code, resp.len);
    if (http_code != 200) {
        app_log_always("[PSN] Token refresh failed (HTTP %ld)\n", http_code);
        free(resp.buf);
        return NULL;
    }

    char *access_token = NULL;
    if (resp.buf) {
        struct json_object *root = json_tokener_parse(resp.buf);
        if (root) {
            struct json_object *tok;
            if (json_object_object_get_ex(root, "access_token", &tok))
                access_token = strdup(json_object_get_string(tok));
            json_object_put(root);
        }
        free(resp.buf);
    }
    if (access_token)
        app_log_always("[PSN] Token refresh OK (len=%zu)\n", strlen(access_token));
    else
        app_log_always("[PSN] Could not parse access_token\n");
    return access_token;
}

// ── List PSN devices ─────────────────────────────────────────────────────────
// Returns the duid of the first PS5 with Remote Play enabled, or NULL.
static char *psn_list_devices(const char *access_token)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    char auth_hdr[512];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", access_token);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth_hdr);

    CurlBuf resp = {0};
    curl_easy_setopt(curl, CURLOPT_URL, PSN_DEVICES_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    psn_curl_common(curl, &resp);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        app_log_always("[PSN] list_devices curl error: %s\n", curl_easy_strerror(rc));
        free(resp.buf);
        return NULL;
    }
    app_log_always("[PSN] list_devices HTTP %ld (%zu bytes)\n",
            http_code, resp.len);

    if (http_code != 200 || !resp.buf) {
        free(resp.buf);
        return NULL;
    }

    // Parse device list — structure from API:
    // {"clients":[{"device":{"name":"PS5-616","enabledFeatures":["remotePlay"],...},"duid":"...","platform":"PS5"}]}
    char *duid = NULL;
    struct json_object *root = json_tokener_parse(resp.buf);
    if (root) {
        struct json_object *clients;
        if (json_object_object_get_ex(root, "clients", &clients)) {
            int n = json_object_array_length(clients);
            app_log_always("[PSN] Found %d device(s)\n", n);
            for (int i = 0; i < n; i++) {
                struct json_object *client = json_object_array_get_idx(clients, i);
                struct json_object *device_obj, *duid_obj;

                // Name is nested: client.device.name
                const char *dname = "?";
                bool rp_enabled = false;
                if (json_object_object_get_ex(client, "device", &device_obj)) {
                    struct json_object *name_obj;
                    if (json_object_object_get_ex(device_obj, "name", &name_obj))
                        dname = json_object_get_string(name_obj);

                    // Check enabledFeatures array for "remotePlay"
                    struct json_object *features;
                    if (json_object_object_get_ex(device_obj, "enabledFeatures", &features)) {
                        int nf = json_object_array_length(features);
                        for (int j = 0; j < nf; j++) {
                            const char *feat = json_object_get_string(
                                json_object_array_get_idx(features, j));
                            if (feat && strcmp(feat, "remotePlay") == 0) {
                                rp_enabled = true;
                                break;
                            }
                        }
                    }
                }

                app_log_always("[PSN]   [%d] name=%s  remotePlay=%d\n", i, dname, rp_enabled);

                if (rp_enabled && !duid) {
                    if (json_object_object_get_ex(client, "duid", &duid_obj))
                        duid = strdup(json_object_get_string(duid_obj));
                }
            }
        } else {
            app_log_always("[PSN] No 'clients' key — top-level keys:");
            json_object_object_foreach(root, key, val) {
                (void)val;
                app_log_always(" %s", key);
            }
            app_log_always("\n");
        }
        json_object_put(root);
    }
    free(resp.buf);

    if (duid)
        app_log_always("[PSN] Selected a Remote Play-enabled device\n");
    else
        app_log_always("[PSN] No suitable device found\n");
    return duid;
}

// ── Send wakeup via PSN session manager ──────────────────────────────────────
// ── Decode base64 PSN account ID to numeric string ───────────────────────────
// PSN stores account IDs as 8-byte big-endian values, base64-encoded.
// The API wants the decimal string representation.
static bool psn_decode_account_id(const char *b64, char *out, size_t out_sz)
{
    // base64 decode (simple inline — input is always 12 chars for 8 bytes)
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t buf[8];
    size_t b64_len = strlen(b64);
    size_t pad = 0;
    if (b64_len > 0 && b64[b64_len-1] == '=') pad++;
    if (b64_len > 1 && b64[b64_len-2] == '=') pad++;

    size_t out_len = 0;
    uint32_t accum = 0;
    int bits = 0;
    for (size_t i = 0; i < b64_len && b64[i] != '='; i++) {
        const char *p = strchr(t, b64[i]);
        if (!p) continue;
        accum = (accum << 6) | (uint32_t)(p - t);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_len < sizeof(buf))
                buf[out_len++] = (uint8_t)(accum >> bits);
            accum &= (1u << bits) - 1;
        }
    }
    if (out_len != 8) {
        app_log_always("[PSN] account_id base64 decode: expected 8 bytes, got %zu\n", out_len);
        return false;
    }

    // Little-endian uint64 (PSN stores account ID as LE bytes in base64)
    uint64_t id = 0;
    for (int i = 7; i >= 0; i--)
        id = (id << 8) | buf[i];

    snprintf(out, out_sz, "%" PRIu64, id);
    return true;
}

// ── Generate a client device UID ─────────────────────────────────────────────
// chiaki-ng client DUIDs are 48 hex chars starting with "0000000700410080".
// We generate a deterministic one from a seed so it's stable across runs.
static void psn_make_client_duid(char *out, size_t out_sz)
{
    // Simple hash of our app identity for the random portion
    const char *seed = CHIAKI_APP_ID "-webos-client-001";
    uint8_t hash[16] = {0};
    for (size_t i = 0; seed[i]; i++)
        hash[i % 16] ^= (uint8_t)seed[i];
    // Standard prefix (0000000700410080) + 32 hex chars from hash
    snprintf(out, out_sz,
        "0000000700410080"
        "%02x%02x%02x%02x%02x%02x%02x%02x"
        "%02x%02x%02x%02x%02x%02x%02x%02x",
        hash[0], hash[1], hash[2], hash[3], hash[4], hash[5],
        hash[6], hash[7], hash[8], hash[9], hash[10], hash[11],
        hash[12], hash[13], hash[14], hash[15]);
}

// ── Step 1: Create a Remote Play session on PSN ──────────────────────────────
// Returns the sessionId on success, or NULL on failure.
// Creating the session triggers a PSN notification to the PS5.
static char *psn_create_session(const char *access_token, const char *account_id,
                                const char *client_duid, char **out_member_device_uid)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    if (out_member_device_uid)
        *out_member_device_uid = NULL;

    char auth_hdr[512];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", access_token);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth_hdr);
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    // Match the exact body format from chiaki-ng logs:
    // {"remotePlaySessions":[{"members":[{"accountId":"me","deviceUniqueId":"me","platform":"me","pushContexts":[{"pushContextId":"<uuid>"}]}]}]}
    // Note: accountId/deviceUniqueId/platform are all "me" — server resolves from bearer token.
    // pushContexts is REQUIRED — we generate a random UUID since we don't have a WebSocket.
    // The session creation itself triggers PSN to send an SQS wakeup to the PS5.
    char push_uuid[64];
    snprintf(push_uuid, sizeof(push_uuid),
        "%08x-%04x-%04x-%04x-%08x%04x",
        (unsigned)time(NULL), (unsigned)(rand() & 0xFFFF),
        0x4000 | (rand() & 0x0FFF),
        0x8000 | (rand() & 0x3FFF),
        (unsigned)rand(), (unsigned)(rand() & 0xFFFF));

    char body[2048];
    snprintf(body, sizeof(body),
        "{\"remotePlaySessions\":[{\"members\":[{"
            "\"accountId\":\"me\","
            "\"deviceUniqueId\":\"me\","
            "\"platform\":\"me\","
            "\"pushContexts\":[{\"pushContextId\":\"%s\"}]"
        "}]}]}",
        push_uuid);

    CurlBuf resp = {0};
    curl_easy_setopt(curl, CURLOPT_URL, PSN_SESSION_URL);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    psn_curl_common(curl, &resp);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        app_log_always("[PSN] Session create curl error: %s\n", curl_easy_strerror(rc));
        free(resp.buf);
        return NULL;
    }
    app_log_always("[PSN] Session create HTTP %ld (%zu bytes)\n",
            http_code, resp.len);

    if (http_code < 200 || http_code >= 300 || !resp.buf) {
        free(resp.buf);
        return NULL;
    }

    // Parse sessionId from response:
    // {"remotePlaySessions":[{"sessionId":"...", ...}]}
    char *session_id = NULL;
    struct json_object *root = json_tokener_parse(resp.buf);
    if (root) {
        struct json_object *sessions;
        if (json_object_object_get_ex(root, "remotePlaySessions", &sessions) &&
            json_object_array_length(sessions) > 0)
        {
            struct json_object *first = json_object_array_get_idx(sessions, 0);
            struct json_object *sid;
            if (json_object_object_get_ex(first, "sessionId", &sid))
                session_id = strdup(json_object_get_string(sid));
            if (out_member_device_uid) {
                struct json_object *members;
                if (json_object_object_get_ex(first, "members", &members) &&
                    json_object_array_length(members) > 0)
                {
                    struct json_object *m0 = json_object_array_get_idx(members, 0);
                    struct json_object *mdu;
                    if (json_object_object_get_ex(m0, "deviceUniqueId", &mdu))
                        *out_member_device_uid = strdup(json_object_get_string(mdu));
                }
            }
        }
        json_object_put(root);
    }
    free(resp.buf);

    if (session_id) {
        app_log_always("[PSN] Session created\n");
        if (out_member_device_uid && *out_member_device_uid)
            app_log_always("[PSN] Session member device ID received\n");
    } else {
        app_log_always("[PSN] Could not parse sessionId from response\n");
    }
    return session_id;
}

// ── Step 2: Start session — sends SQS wakeup to the PS5 ─────────────────────
static bool psn_start_session(const char *access_token,
                              const char *duid,
                              const char *account_id,
                              const char *session_id)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return false;

    // IMPORTANT:
    // The "start session / wake" request is sent to the CAN (cloudAssistedNavigation) commands
    // endpoint, NOT to the sessionManager sessionMessage endpoint.
    //
    // chiaki-ng uses:
    //   https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/commands
    //
    // Posting to /remotePlaySessions/<id>/sessionMessage expects a message envelope (to/payload)
    // and will return 4xx/202 but won't reliably wake the console.

    const char *url = PSN_COMMAND_URL;
    app_log_always("[PSN] Session command URL: %s\n", url);

    char auth_hdr[512];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", access_token);

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth_hdr);
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    // Build initialParams exactly like chiaki-ng (JSON-as-string with escaped quotes)
    char data1_b64[32], data2_b64[32];
    if (!psn_make_b64_nonce(data1_b64) || !psn_make_b64_nonce(data2_b64))
    {
        app_log_always("[PSN] Failed to generate random nonces for initialParams\n");
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        return false;
    }
    char initial_params[1024];
    snprintf(initial_params, sizeof(initial_params),
            "{\\\"accountId\\\":%s,"
            "\\\"roomId\\\":0,"
            "\\\"sessionId\\\":\\\"%s\\\","
            "\\\"clientType\\\":\\\"Windows\\\","
            "\\\"data1\\\":\\\"%s\\\","
            "\\\"data2\\\":\\\"%s\\\"}",
            account_id, session_id, data1_b64, data2_b64);

    // Command envelope (this is what yields HTTP 200 + {"commandId":...} in chiaki-ng logs)
    char body[2048];
    snprintf(body, sizeof(body),
            "{"
              "\"commandDetail\":{"
                "\"commandType\":\"remotePlay\","
                "\"duid\":\"%s\","
                "\"messageDestination\":\"SQS\","
                "\"parameters\":{"
                  "\"initialParams\":\"%s\""
                "},"
                "\"platform\":\"PS5\""
              "}"
            "}",
            duid, initial_params);

    CurlBuf resp = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    psn_curl_common(curl, &resp);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (rc != CURLE_OK)
        app_log_always("[PSN] Session command curl error: %s\n", curl_easy_strerror(rc));

    if (resp.buf && resp.len)
        app_log_always("[PSN] Session command HTTP %ld (%zu bytes)\n",
                       http_code, resp.len);
    else
        app_log_always("[PSN] Session command HTTP %ld (0 bytes)\n", http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    free(resp.buf);

    return (rc == CURLE_OK) && (http_code >= 200 && http_code < 300);
}


// ── Cleanup: delete session ──────────────────────────────────────────────────
static void psn_delete_session(const char *access_token, const char *session_id)
{
    CURL *curl = curl_easy_init();
    if (!curl) return;

    char url[1024];
    snprintf(url, sizeof(url), "%s/%s/members/me", PSN_SESSION_URL, session_id);

    char auth_hdr[512];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", access_token);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth_hdr);

    CurlBuf resp = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    psn_curl_common(curl, &resp);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    app_log_always("[PSN] Session delete HTTP %ld\n", http_code);
    free(resp.buf);
}

// ── PSN wakeup orchestrator ──────────────────────────────────────────────────
static void do_psn_wakeup(AppConfig *cfg, ChiakiLog *log)
{
    app_log_always("[PSN] Starting PSN cloud wakeup sequence\n");

    // Step 0: Decode PSN account ID from base64 to numeric string
    char account_id[32];
    if (!cfg->psn_account_id_b64 || !cfg->psn_account_id_b64[0] ||
        !psn_decode_account_id(cfg->psn_account_id_b64, account_id, sizeof(account_id)))
    {
        app_log_always("[PSN] No valid psn_account_id — falling back to UDP wakeup\n");
        do_wakeup(cfg, log);
        return;
    }
    app_log_always("[PSN] Account ID decoded\n");

    // Step 1: Refresh access token
    char *access_token = psn_refresh_access_token(cfg->psn_refresh_token);
    if (!access_token) {
        app_log_always("[PSN] No access_token — falling back to UDP wakeup\n");
        do_wakeup(cfg, log);
        return;
    }

    // Step 2: List devices to find PS5's duid
    char *duid = psn_list_devices(access_token);
    if (!duid) {
        app_log_always("[PSN] Could not find PS5 device — falling back to UDP wakeup\n");
        free(access_token);
        do_wakeup(cfg, log);
        return;
    }

    // Step 3: Generate our client device UID
    char client_duid[128];
    psn_make_client_duid(client_duid, sizeof(client_duid));
    app_log_always("[PSN] Client device ID generated\n");

    // Step 4: Create Remote Play session on PSN
    char *session_id = psn_create_session(access_token, account_id, client_duid, NULL);
    if (!session_id) {
        app_log_always("[PSN] Session creation failed — falling back to UDP wakeup\n");
        free(duid);
        free(access_token);
        do_wakeup(cfg, log);
        return;
    }

    // Step 5: Start session — sends SQS wakeup push to PS5
    bool ok = psn_start_session(access_token, duid, account_id, session_id);

    // Step 6: Clean up the session (best-effort)
    psn_delete_session(access_token, session_id);
    free(session_id);
    free(duid);
    free(access_token);

    if (ok) {
        app_log_always("[PSN] Cloud wakeup sent successfully — waiting for PS5\n");
    } else {
        app_log_always("[PSN] Cloud wakeup command failed — falling back to UDP wakeup\n");
        do_wakeup(cfg, log);
    }
}

// ── webOS version detection ───────────────────────────────────────────────────
// Reads /var/run/nyx/os_info.json (always present on webOS) and returns the
// correct SS4S module name for the detected webOS major version:
//   webOS 5+  →  "ndl-webos5"  (NDL_DirectMedia_DL_Initialize / dlopen-based API)
//   webOS 4.x →  "ndl-webos4"  (NDL_DirectMediaInit / direct-link API)
//
// Falls back through two secondary checks if the primary file is unreadable,
// then defaults to "ndl-webos5" (the most common case for recent LG TVs).
static const char *detect_webos_ss4s_module(void)
{
    // ── Primary: /var/run/nyx/os_info.json ───────────────────────────────────
    // Present on every webOS device. Contains "core_os_release":"X.Y.Z".
    static const char *nyx_paths[] = {
        "/var/run/nyx/os_info.json",
        "/etc/nyx/os_info.json",
        NULL
    };
    for (int pi = 0; nyx_paths[pi]; pi++) {
        FILE *f = fopen(nyx_paths[pi], "r");
        if (!f) continue;
        char buf[512];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';

        // Look for "core_os_release":"X..." or "webos_release":"X..."
        static const char *keys[] = {
            "core_os_release", "webos_release", "release", NULL
        };
        for (int ki = 0; keys[ki]; ki++) {
            char *p = strstr(buf, keys[ki]);
            if (!p) continue;
            p = strchr(p, ':');
            if (!p) continue;
            // Skip : whitespace "
            while (*p && (*p == ':' || *p == ' ' || *p == '"')) p++;
            int major = atoi(p);
            if (major >= 5) {
                app_log_always("[AUTO] webOS %d detected via %s — using ndl-webos5\n",
                               major, nyx_paths[pi]);
                return "ndl-webos5";
            }
            if (major == 4) {
                app_log_always("[AUTO] webOS %d detected via %s — using ndl-webos4\n",
                               major, nyx_paths[pi]);
                return "ndl-webos4";
            }
            if (major > 0) {
                // webOS 3 or unknown old version — ndl-webos4 is the safer bet
                // (esplayer backend is not supported in this build)
                app_log_always("[AUTO] webOS %d detected via %s — defaulting ndl-webos4\n",
                               major, nyx_paths[pi]);
                return "ndl-webos4";
            }
        }
        app_log_always("[AUTO] Could not parse version from %s\n", nyx_paths[pi]);
    }

    // ── Secondary: probe NDL shared library version ──────────────────────────
    // webOS 5+ ships libndl-directmedia2.so; webOS 4 has libndl-directmedia.so only.
    static const char *ndl2_paths[] = {
        "/usr/lib/libndl-directmedia2.so",
        "/usr/lib/libndl-directmedia2.so.1",
        "/usr/local/lib/libndl-directmedia2.so",
        NULL
    };
    for (int i = 0; ndl2_paths[i]; i++) {
        if (access(ndl2_paths[i], F_OK) == 0) {
            app_log_always("[AUTO] Found %s — using ndl-webos5\n", ndl2_paths[i]);
            return "ndl-webos5";
        }
    }
    static const char *ndl1_paths[] = {
        "/usr/lib/libndl-directmedia.so",
        "/usr/lib/libndl-directmedia.so.1",
        NULL
    };
    for (int i = 0; ndl1_paths[i]; i++) {
        if (access(ndl1_paths[i], F_OK) == 0) {
            app_log_always("[AUTO] Found %s (no v2) — using ndl-webos4\n", ndl1_paths[i]);
            return "ndl-webos4";
        }
    }

    // ── Final fallback ───────────────────────────────────────────────────────
    app_log_always("[AUTO] webOS version unknown — defaulting to ndl-webos5\n");
    return "ndl-webos5";
}

// ── Reconnect decision ────────────────────────────────────────────────────────
// Returns true if the session should be retried based on the quit reason.
// We reconnect on transient network errors, not on deliberate user/PS5 quits.
static bool should_reconnect(ChiakiQuitReason reason)
{
    if (atomic_load_explicit(&g_ps5_sleeping, memory_order_acquire))
        return false;

    switch (reason) {
    case CHIAKI_QUIT_REASON_STOPPED:
    case CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN:
    case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE:
    case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_CRASH:
    case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_VERSION_MISMATCH:
    case CHIAKI_QUIT_REASON_PSN_REGIST_FAILED:
        return false;
    default:
        return true;
    }
}

// Session-request failures happen before streaming starts. Return to the
// launcher so the user can correct the host or console state instead of making
// the process disappear or retrying forever.
static const char *session_request_failure_message(ChiakiQuitReason reason)
{
    switch (reason) {
    case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE:
        return "Remote Play is already in use on the PS5.";
    case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_CRASH:
        return "The PS5 Remote Play service reported an error.";
    case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_VERSION_MISMATCH:
        return "PS5 protocol mismatch. Update the app and retry.";
    case CHIAKI_QUIT_REASON_PSN_REGIST_FAILED:
        return "Registration was rejected. Import the console config again.";
    default:
        return NULL;
    }
}

typedef struct StreamBackend
{
    SS4S_Player *player;
    VideoContext *video;
    AudioContext *audio;
} StreamBackend;

static const char *stream_codec_name(int codec)
{
    switch (codec) {
    case CHIAKI_CODEC_H265_HDR: return "H.265 HDR";
    case CHIAKI_CODEC_H265: return "H.265";
    case CHIAKI_CODEC_H264: return "H.264";
    default: return "unknown";
    }
}

static bool select_stream_profile(const AppConfig *cfg,
                                  const SS4S_VideoCapabilities *caps,
                                  int *codec_out, int *bitrate_out,
                                  char *error, size_t error_size)
{
    const bool has_h264 = (caps->codecs & SS4S_VIDEO_H264) != 0;
    const bool has_h265 = (caps->codecs & SS4S_VIDEO_H265) != 0;
    const bool force_h264 = cfg->video_codec && !strcmp(cfg->video_codec, "h264");
    const bool want_hdr = cfg->video_codec && !strcmp(cfg->video_codec, "h265_hdr");
    int codec = CHIAKI_CODEC_H264;

    if (!cfg->ps5) {
        if (!has_h264) {
            snprintf(error, error_size, "The SS4S backend does not support PS4 H.264 video.");
            return false;
        }
    } else if (want_hdr && has_h265 && caps->hdr) {
        codec = CHIAKI_CODEC_H265_HDR;
    } else if (!force_h264 && has_h265) {
        codec = CHIAKI_CODEC_H265;
        if (want_hdr)
            app_log_always("[STREAM] HDR unavailable; falling back to H.265 SDR\n");
    } else if (has_h264) {
        codec = CHIAKI_CODEC_H264;
        if (!force_h264)
            app_log_always("[STREAM] HEVC unavailable; falling back to H.264\n");
    } else if (has_h265) {
        codec = CHIAKI_CODEC_H265;
        app_log_always("[STREAM] H.264 unavailable; using H.265\n");
    } else {
        snprintf(error, error_size, "The SS4S backend supports neither H.264 nor H.265.");
        return false;
    }

    int bitrate = cfg->video_bitrate;
    if (caps->maxBitrate > 0 &&
        (unsigned int)bitrate > caps->maxBitrate) {
        app_log_always("[STREAM] Clamping bitrate from %d to backend maximum %u kbps\n",
                       bitrate, caps->maxBitrate);
        bitrate = (int)caps->maxBitrate;
    }

    *codec_out = codec;
    *bitrate_out = bitrate;
    app_log_always("[STREAM] Effective profile: %dx%d@%d %s %d kbps\n",
                   cfg->video_width, cfg->video_height, cfg->video_fps,
                   stream_codec_name(codec), bitrate);
    return true;
}

static bool stream_backend_open(StreamBackend *backend, const AppConfig *cfg,
                                int codec, VideoInitResult *video_result)
{
    memset(backend, 0, sizeof(*backend));
    if (video_result)
        *video_result = VIDEO_INIT_ERROR;

    backend->player = SS4S_PlayerOpen();
    if (!backend->player) {
        app_log_always("[SS4S] PlayerOpen failed\n");
        return false;
    }

    SS4S_PlayerSetWaitAudioVideoReady(backend->player, true);
    SS4S_PlayerSetViewportSize(backend->player, cfg->video_width, cfg->video_height);

    backend->video = video_init(backend->player, cfg->video_width,
                                cfg->video_height, cfg->video_fps,
                                codec, video_result);
    if (!backend->video)
        return false;

    backend->audio = audio_init(backend->player);
    if (!backend->audio) {
        app_log_always("[AUDIO] Could not allocate audio context\n");
        return false;
    }

    return true;
}

static void stream_backend_close(StreamBackend *backend)
{
    if (!backend)
        return;
    audio_fini(backend->audio);
    video_fini(backend->video);
    if (backend->player)
        SS4S_PlayerClose(backend->player);
    memset(backend, 0, sizeof(*backend));
}

static void send_idr_request(const char *reason)
{
    atomic_fetch_add_explicit(&g_stream_stats.video_idr_requests, 1,
                              memory_order_relaxed);
    ChiakiErrorCode err = chiaki_session_request_idr(&g_session);
    app_log_always("[RECOVERY] IDR request (%s): %s\n", reason,
                   chiaki_error_string(err));
}

static void log_stream_summary(int attempt, ChiakiQuitReason reason)
{
    app_log_always(
        "[STREAM] Attempt %d summary: reason=%s samples=%llu frames=%llu "
        "lost=%llu recovered=%llu fec=%llu video_not_ready=%llu "
        "video_keyframe=%llu video_errors=%llu audio_not_ready=%llu "
        "audio_overflow=%llu audio_errors=%llu idr=%llu reconnects=%d\n",
        attempt, chiaki_quit_reason_string(reason),
        (unsigned long long)atomic_load_explicit(&g_stream_stats.video_samples, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_stream_stats.video_frames, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_stream_stats.video_frames_lost, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_stream_stats.video_frames_recovered, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_stream_stats.video_fec_failures, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_stream_stats.video_feed_not_ready, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_stream_stats.video_feed_keyframe_requests, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_stream_stats.video_feed_errors, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_stream_stats.audio_feed_not_ready, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_stream_stats.audio_feed_overflows, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_stream_stats.audio_feed_errors, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_stream_stats.video_idr_requests, memory_order_relaxed),
        attempt > 0 ? attempt - 1 : 0);
}

static bool wait_for_reconnect(SDL_Renderer *renderer, uint32_t delay_ms)
{
    const uint64_t deadline = stats_monotonic_ms() + delay_ms;
    while (!app_exit_requested())
    {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_KEYDOWN && webos_event_is_back_or_red(&event.key)))
            {
                app_request_exit();
                return false;
            }
        }

        uint64_t now = stats_monotonic_ms();
        if (now >= deadline)
            return true;

        unsigned int seconds = (unsigned int)((deadline - now + 999u) / 1000u);
        char message[96];
        snprintf(message, sizeof(message), "Reconnecting in %u s (Back/Red cancels)", seconds);
        ui_render_loading(renderer, message);
        SDL_RenderPresent(renderer);
        SDL_Delay(50);
    }
    return false;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    // Disable core dumps — a crashing 1080p stream process can produce a
    // 400MB core file and quickly fill the TV's limited storage.
    { struct rlimit rl = {0, 0}; setrlimit(RLIMIT_CORE, &rl); }

    /* ===== REQUIRED WEBOS SS4S ENV SETUP ===== */
    chdir(CHIAKI_APP_DIR);
    setenv("SS4S_CONFIG_FILE", "./lib/ss4s_modules.ini", 1);
    // SS4S_MODULE is set after config load — auto-detection picks ndl-webos4 or ndl-webos5.
    // The pinned SS4S modules read APPID; retain SS4S_APP_ID for compatibility
    // with older builds that used the prefixed name.
    setenv("APPID",             CHIAKI_APP_ID,    1);
    setenv("SS4S_APP_ID",      CHIAKI_APP_ID,    1);
    /* ========================================= */

    // Open log unconditionally first — once cfg is loaded we'll close it
    // if logging=false. This lets us capture any early startup errors.
    g_log_file = fopen(LOG_PATH, "w");
    if (!g_log_file)
        g_log_file = fopen("/tmp/chiaki-debug.log", "w");

    // Stamp each launch clearly so we can count restarts in the log.
    {
        time_t t = time(NULL);
        char tbuf[64];
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&t));
        app_log("\n\n========== NEW LAUNCH %s ==========\n", tbuf);
    }

    const char *ss4s_module_env = getenv("SS4S_MODULE");
    const char *ss4s_config_env = getenv("SS4S_CONFIG_FILE");
    const char *app_id_env = getenv("APPID");
    const char *ss4s_app_id_env = getenv("SS4S_APP_ID");
    app_log("[APP] SS4S_MODULE=%s\n",
            ss4s_module_env ? ss4s_module_env : "(unset)");
    app_log("[APP] SS4S_CONFIG_FILE=%s\n",
            ss4s_config_env ? ss4s_config_env : "(unset)");
    app_log("[APP] APPID=%s\n",
            app_id_env ? app_id_env : "(unset)");
    app_log("[APP] SS4S_APP_ID=%s\n",
            ss4s_app_id_env ? ss4s_app_id_env : "(unset)");

    const char *config_path = CONFIG_PATH;
    if (argc > 1 && argv[1][0] != '{' && argv[1][0] != '\0')
        config_path = argv[1];

    app_log("[APP] config_path = %s\n", config_path);

    /* Rooted TVs can activate the bundled, strictly compatibility-gated
     * DualSense driver and old-webOS Bluetooth correction through Homebrew. */
    root_feedback_bootstrap();

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);


    // ── Load config ──────────────────────────────────────────────────────────
    AppConfig cfg;
    if (config_load(&cfg, config_path) != 0)
    {
        app_log("[APP] Failed to load config from %s\n", config_path);
        return 1;
    }
    // Honour log_level=0 (off): close the log file we opened early for startup errors.
    g_log_level = cfg.log_level;
    if (!cfg.log_level && g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }

    app_log_always("[APP] Connecting to %s  ps5=%d  %dx%d@%dfps\n",
            cfg.host, cfg.ps5, cfg.video_width, cfg.video_height, cfg.video_fps);

    // ── SSL / cURL init ──────────────────────────────────────────────────────
    curl_global_init(CURL_GLOBAL_DEFAULT);
    setup_ssl_ca_bundle();

    // ── SDL2 init ────────────────────────────────────────────────────────────
    // webosbrew/SDL-webOS latches these access-policy hints when the window is
    // created. They keep Back/Exit/Guide usable inside the app and prevent the
    // launcher ribbon from covering the stream.
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_EXIT", "true");
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_GUIDE", "true");
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_RIBBON", "false");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    {
        app_log("[APP] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // NDL renders video on a hardware plane BELOW the app's GL surface.
    // To make the video visible we need our GL surface to be transparent.
    //
    // Strategy:
    //   1. Request an EGL surface with an 8-bit alpha channel BEFORE creating
    //      the window, so the webOS compositor allocates a compositable surface.
    //   2. Create a fullscreen OpenGL window (not SOFTWARE — that's always
    //      opaque on webOS because there is no compositor blending path).
    //   3. Each frame: clear to RGBA(0,0,0,0) and call SDL_RenderPresent so
    //      the compositor sees fresh transparent frames and keeps the app alive.
    //
    // Do NOT call SDL_HideWindow — webOS kills hidden-window processes.
    // Do NOT use SDL_RENDERER_SOFTWARE — can't composite-transparent on webOS.
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  8);

    g_window = SDL_CreateWindow(
        "Chiaki",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        1920, 1080,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);

    if (!g_window)
    {
        // Fallback: try without FULLSCREEN_DESKTOP in case the webOS SDL
        // build doesn't support that flag in combination with OpenGL.
        app_log("[APP] Fullscreen+GL window failed (%s), trying borderless\n",
                SDL_GetError());
        g_window = SDL_CreateWindow(
            "Chiaki",
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            1920, 1080,
            SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS | SDL_WINDOW_SHOWN);
    }

    if (!g_window)
    {
        app_log("[APP] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    app_log("[APP] SDL window created OK\n");

    // Keep Magic Remote keyboard focus attached to the fullscreen app.
    SDL_SetWindowGrab(g_window, SDL_TRUE);

    // Accelerated renderer — uses OpenGL ES on webOS, which respects the
    // EGL surface alpha channel we requested above.
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer)
    {
        app_log("[APP] Accelerated renderer failed (%s), trying software\n",
                SDL_GetError());
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!g_renderer)
    {
        app_log("[APP] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }

    // Clear to fully transparent black immediately so the NDL plane is
    // visible as soon as the first frame is presented.
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 0);
    SDL_RenderClear(g_renderer);
    SDL_RenderPresent(g_renderer);

    InputContext *input_ctx = input_init();
    if (!input_ctx)
        app_log_always("[INPUT] Initialization failed; continuing without a controller\n");

    SDL_ShowCursor(SDL_ENABLE);

    // ── Stats overlay state (toggled at runtime) ─────────────────────────────
    StatsOverlay stats_overlay;
    stats_overlay_init(&stats_overlay);


    // ── Chiaki logging ───────────────────────────────────────────────────────
    // Pass the configured bitmask so chiaki-ng itself filters at the source.
    ChiakiLog chiaki_log;
    chiaki_log_init(&chiaki_log, cfg.log_level, log_cb, NULL);

    // Discovery packets contain the wake credential in their verbose hex dump.
    // Use a restricted logger for wakeup while retaining the selected level for
    // the actual streaming session.
    ChiakiLog wakeup_log;
    chiaki_log_init(&wakeup_log,
                    CHIAKI_LOG_WARNING | CHIAKI_LOG_ERROR,
                    log_cb, NULL);

    char launcher_message[256] = "";

    // ── Launcher UI (shown on every launch) ───────────────────────────────

show_launcher:
    atomic_store_explicit(&g_session_ended, false, memory_order_release);
    atomic_store_explicit(&g_quit_reason, CHIAKI_QUIT_REASON_NONE, memory_order_release);
    atomic_store_explicit(&g_ps5_sleeping, false, memory_order_release);

    app_log("[APP] Showing launcher UI\n");
    SDL_ShowCursor(SDL_ENABLE);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);  // opaque for UI
    SDL_RenderClear(g_renderer);
    SDL_RenderPresent(g_renderer);
    SDL_SetWindowSize(g_window, cfg.video_width, cfg.video_height);
    UIResult ui_result = ui_run_registration(
        g_renderer, &cfg, config_path, &chiaki_log, launcher_message, input_ctx);
    launcher_message[0] = '\0';

    // Back to transparent so NDL plane shows through during stream
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 0);
    SDL_RenderClear(g_renderer);
    SDL_RenderPresent(g_renderer);

    if (ui_result != UI_RESULT_CONNECT)
    {
        app_log("[APP] Launcher dismissed\n");
        goto cleanup_sdl;
    }

    if (app_exit_requested())
        goto cleanup_sdl;

    SDL_ShowCursor(SDL_DISABLE);

    // The launcher can update logging settings and reload cfg in place.
    chiaki_log_init(&chiaki_log, cfg.log_level, log_cb, NULL);

    // The launcher can also import a config that restores "auto", so resolve
    // the module after every launcher pass and before SS4S sees the value.
    if (cfg.ss4s_module && strcmp(cfg.ss4s_module, "auto") == 0) {
        free(cfg.ss4s_module);
        cfg.ss4s_module = strdup(detect_webos_ss4s_module());
    }
    if (!cfg.ss4s_module) {
        snprintf(launcher_message, sizeof(launcher_message),
                 "Could not select a media backend.");
        goto show_launcher;
    }
    setenv("SS4S_MODULE", cfg.ss4s_module, 1);
    app_log_always("[APP] SS4S module: %s\n", cfg.ss4s_module);

    bool return_to_launcher = false;

    // ── SS4S init and capability negotiation ─────────────────────────────────
    app_log_always("[APP] Streaming revisions: chiaki-ng=%s SS4S=%s\n",
                   CHIAKI_NG_REVISION, SS4S_REVISION);
    app_log("[APP] Initialising SS4S (module: %s)\n", cfg.ss4s_module);
    SS4S_Config ss4s_cfg = {
        .audioDriver = cfg.ss4s_module,
        .videoDriver = cfg.ss4s_module,
        .loggingFunction = ss4s_log_cb,
    };
    bool ss4s_initialized = false;
    int ss4s_rc = SS4S_Init(argc, argv, &ss4s_cfg);
    ss4s_initialized = true;
    if (ss4s_rc != 0 || SS4S_PostInit(argc, argv) != 0 ||
        !SS4S_GetVideoModuleName() || !SS4S_GetAudioModuleName())
    {
        app_log_always("[SS4S] Backend initialization failed (rc=%d video=%s audio=%s)\n",
                       ss4s_rc,
                       SS4S_GetVideoModuleName() ? SS4S_GetVideoModuleName() : "missing",
                       SS4S_GetAudioModuleName() ? SS4S_GetAudioModuleName() : "missing");
        snprintf(launcher_message, sizeof(launcher_message),
                 "Could not initialize the %s media backend.", cfg.ss4s_module);
        return_to_launcher = true;
        goto cleanup_ss4s;
    }

    SS4S_SetLogLevel((cfg.log_level & CHIAKI_LOG_INFO)
        ? SS4S_LogLevelInfo : SS4S_LogLevelWarn);

    SS4S_VideoCapabilities video_caps;
    SS4S_AudioCapabilities audio_caps;
    memset(&video_caps, 0, sizeof(video_caps));
    memset(&audio_caps, 0, sizeof(audio_caps));
    if (!SS4S_GetVideoCapabilities(&video_caps))
    {
        snprintf(launcher_message, sizeof(launcher_message),
                 "The media backend did not report video capabilities.");
        return_to_launcher = true;
        goto cleanup_ss4s;
    }
    if (!SS4S_GetAudioCapabilitiesByCodecs(&audio_caps, SS4S_AUDIO_OPUS) ||
        !(audio_caps.codecs & SS4S_AUDIO_OPUS))
    {
        snprintf(launcher_message, sizeof(launcher_message),
                 "The media backend does not support Remote Play Opus audio.");
        return_to_launcher = true;
        goto cleanup_ss4s;
    }

    app_log_always("[SS4S] Capabilities: codecs=0x%x hdr=%d max=%ux%u@%u "
                   "max_bitrate=%u suggested=%u audio=0x%x channels=%u\n",
                   (unsigned int)video_caps.codecs, video_caps.hdr,
                   video_caps.maxWidth, video_caps.maxHeight, video_caps.maxFps,
                   video_caps.maxBitrate, video_caps.suggestedBitrate,
                   (unsigned int)audio_caps.codecs, audio_caps.maxChannels);

    int requested_codec = CHIAKI_CODEC_H264;
    int effective_bitrate = cfg.video_bitrate;
    if (!select_stream_profile(&cfg, &video_caps, &requested_codec,
                               &effective_bitrate,
                               launcher_message, sizeof(launcher_message)))
    {
        return_to_launcher = true;
        goto cleanup_ss4s;
    }

    // ── Exported window (webOS 5+) ─────────────────────────────────────────
    // On webOS 5+, NDL requires the app to pass its SDL window's native webOS
    // window ID to the NDL backend so that UMS (User Media Service) grants
    // foreground video-plane access.  Without this the video plane is either
    // not shown or gets background priority (black screen).
    //
    // SDL_GetWindowWMInfo() returns a webOS-specific SDL_SysWMinfo struct.
    // On the webosbrew SDL2 port, info.info.dummy (or a webOS-specific field)
    // may contain the exported window ID.  We pass it via the SS4S env var
    // that the NDL module reads, OR via a direct SS4S API if available.
    {
        SDL_SysWMinfo wm_info;
        SDL_VERSION(&wm_info.version);
        if (SDL_GetWindowWMInfo(g_window, &wm_info)) {
            app_log("[APP] SDL_GetWindowWMInfo OK — subsystem=%d\n",
                    (int)wm_info.subsystem);
            // On the webOS SDL2 build, the exported window ID is stored in
            // wm_info as a string pointer.  The field name varies by SDL build;
            // we log the raw bytes so we can identify it.
            app_log("[APP] WM info bytes (first 64): ");
            const unsigned char *raw = (const unsigned char *)&wm_info.info;
            for (int _i = 0; _i < 64 && _i < (int)sizeof(wm_info.info); _i++)
                app_log("%02x ", raw[_i]);
            app_log("\n");
        } else {
            app_log("[APP] SDL_GetWindowWMInfo FAILED: %s\n", SDL_GetError());
        }
    }
    // ── Build connect info (done once, reused across reconnects) ──────────────
    ChiakiConnectInfo info;
    memset(&info, 0, sizeof(info));
    info.host = cfg.host;
    info.ps5  = cfg.ps5;
    info.enable_dualsense = cfg.ps5;

    size_t decoded_len;

    decoded_len = sizeof(info.regist_key);
    if (chiaki_base64_decode(cfg.registered_key_b64,
                             strlen(cfg.registered_key_b64),
                             (uint8_t *)info.regist_key,
                             &decoded_len) != CHIAKI_ERR_SUCCESS)
    {
        app_log("[APP] Failed to decode registered_key\n");
        snprintf(launcher_message, sizeof(launcher_message),
                 "Registration key is invalid. Import config again.");
        return_to_launcher = true;
        goto cleanup_ss4s;
    }
    decoded_len = sizeof(info.morning);
    if (chiaki_base64_decode(cfg.rp_key_b64,
                             strlen(cfg.rp_key_b64),
                             info.morning, &decoded_len) != CHIAKI_ERR_SUCCESS)
    {
        app_log("[APP] Failed to decode rp_key\n");
        snprintf(launcher_message, sizeof(launcher_message),
                 "Remote Play key is invalid. Import config again.");
        return_to_launcher = true;
        goto cleanup_ss4s;
    }
    uint8_t account_id_raw[8];
    decoded_len = sizeof(account_id_raw);
    if (chiaki_base64_decode(cfg.psn_account_id_b64,
                             strlen(cfg.psn_account_id_b64),
                             account_id_raw, &decoded_len) != CHIAKI_ERR_SUCCESS)
    {
        app_log("[APP] Failed to decode psn_account_id\n");
        snprintf(launcher_message, sizeof(launcher_message),
                 "PSN account ID is invalid. Import config again.");
        return_to_launcher = true;
        goto cleanup_ss4s;
    }
    memcpy(&info.psn_account_id, account_id_raw, sizeof(info.psn_account_id));

    info.video_profile.width   = cfg.video_width;
    info.video_profile.height  = cfg.video_height;
    info.video_profile.max_fps = cfg.video_fps;
    info.video_profile.bitrate = effective_bitrate;

    info.video_profile.codec = requested_codec;
    info.video_profile_auto_downgrade = true;
    info.packet_loss_max = cfg.packet_loss_max;
    info.enable_idr_on_fec_failure = cfg.idr_on_fec_failure;

    // ── Session / reconnect loop ──────────────────────────────────────────────
    int attempt = 0;
    unsigned int consecutive_failures = 0;
    unsigned int decoder_open_failures = 0;
    bool watchdog_recovery_active = false;
    unsigned int watchdog_recovery_retries = 0;
    while (!app_exit_requested())
    {
        attempt++;
        app_log_always("[APP] Session attempt %d — connecting to %s\n", attempt, cfg.host);

        atomic_store_explicit(&g_session_ended, false, memory_order_release);
        atomic_store_explicit(&g_quit_reason, CHIAKI_QUIT_REASON_NONE, memory_order_release);
        atomic_store_explicit(&g_ps5_sleeping, false, memory_order_release);

        stats_reset(&g_stream_stats);
        bool overlay_enabled = stats_overlay.enabled;
        stats_overlay_init(&stats_overlay);
        stats_overlay.enabled = overlay_enabled;
        stats_set_video_format(&g_stream_stats, cfg.video_width, cfg.video_height, cfg.video_fps,
                             info.video_profile.codec);
        atomic_store_explicit(&g_stream_stats.reconnects,
                              attempt > 0 ? (uint64_t)(attempt - 1) : 0,
                              memory_order_relaxed);

        // Show a minimal loading screen immediately.
        ui_render_loading(g_renderer, "Connecting");
        SDL_RenderPresent(g_renderer);

        // ── Wakeup ───────────────────────────────────────────────────────────
        // Send wakeup on the first attempt only. Check port 9295 first so an
        // already-awake console does not receive a competing PSN cloud Remote
        // Play command immediately before the local Chiaki session. If it is
        // asleep, resend the local wake packet every 5s until it is reachable.
        if (cfg.wakeup && attempt == 1)
        {
            bool ps5_ready = host_port_ready(cfg.host, 9295, 250);
            if (ps5_ready) {
                app_log_always("[WAKEUP] PS5 is already ready; skipping wakeup\n");
            } else {
                if (cfg.psn_refresh_token && cfg.psn_refresh_token[0])
                    do_psn_wakeup(&cfg, &wakeup_log);
                else
                    do_wakeup(&cfg, &wakeup_log);

                Uint32 deadline = SDL_GetTicks() + (Uint32)cfg.wakeup_delay_ms;
                Uint32 next_wakeup = SDL_GetTicks() + 5000;
                app_log_always("[WAKEUP] Waiting up to %dms for PS5 to become ready...\n",
                        cfg.wakeup_delay_ms);
                SDL_Event ev;

                while (!app_exit_requested() && SDL_GetTicks() < deadline)
                {
                    while (SDL_PollEvent(&ev))
                        if (ev.type == SDL_QUIT) { app_request_exit(); break; }
                    if (app_exit_requested()) break;

                    ui_render_loading(g_renderer, "Waking console");
                    SDL_RenderPresent(g_renderer);

                    if (host_port_ready(cfg.host, 9295, 1000)) {
                        app_log_always("[WAKEUP] PS5 is ready (port 9295 open)\n");
                        ps5_ready = true;
                        break;
                    }

                    if (SDL_GetTicks() >= next_wakeup)
                    {
                        do_wakeup(&cfg, &wakeup_log);
                        next_wakeup = SDL_GetTicks() + 5000;
                    }
                    SDL_Delay(250);
                }
            }

            if (!ps5_ready && !app_exit_requested()) {
                app_log_always("[WAKEUP] PS5 did not become reachable; returning to launcher\n");
                snprintf(launcher_message, sizeof(launcher_message),
                         "PS5 did not wake at %s. Turn it on or check Rest Mode network settings.",
                         cfg.host);
                return_to_launcher = true;
                break;
            }
        }

        if (app_exit_requested())
            break;

        StreamBackend backend;
        VideoInitResult video_result = VIDEO_INIT_ERROR;
        if (!stream_backend_open(&backend, &cfg, requested_codec, &video_result))
        {
            stream_backend_close(&backend);
            decoder_open_failures++;
            if (video_result == VIDEO_INIT_UNSUPPORTED_CODEC ||
                decoder_open_failures >= 3)
            {
                snprintf(launcher_message, sizeof(launcher_message),
                         video_result == VIDEO_INIT_UNSUPPORTED_CODEC
                            ? "The media backend rejected the selected video codec."
                            : "The TV video decoder failed to open three times.");
                return_to_launcher = true;
                break;
            }

            uint32_t delay_ms = stream_retry_delay_ms(consecutive_failures++);
            app_log_always("[RECOVERY] Decoder open failed (%u/3); retrying in %u ms\n",
                           decoder_open_failures, delay_ms);
            if (!wait_for_reconnect(g_renderer, delay_ms))
                break;
            continue;
        }
        decoder_open_failures = 0;

        ChiakiErrorCode err = chiaki_session_init(&g_session, &info, &chiaki_log);
        if (err != CHIAKI_ERR_SUCCESS)
        {
            app_log("[APP] chiaki_session_init failed: %s\n", chiaki_error_string(err));
            snprintf(launcher_message, sizeof(launcher_message),
                     "Could not initialize stream: %s", chiaki_error_string(err));
            return_to_launcher = true;
            stream_backend_close(&backend);
            break;
        }

        chiaki_session_set_event_cb(&g_session, session_event_cb, input_ctx);
        chiaki_session_set_video_sample_cb(&g_session,
            video_sample_cb, backend.video);
        app_log("[APP] Video callback registered\n");

        ChiakiAudioSink audio_sink = audio_make_sink(backend.audio);
        chiaki_session_set_audio_sink(&g_session, &audio_sink);
        app_log("[APP] Audio sink registered: header_cb=%p frame_cb=%p user=%p\n",
                (void *)audio_sink.header_cb,
                (void *)audio_sink.frame_cb,
                audio_sink.user);

        ChiakiAudioSink haptics_sink = input_make_haptics_sink(input_ctx);
        chiaki_session_set_haptics_sink(&g_session, &haptics_sink);
        app_log("[APP] Haptics sink registered\n");

        err = chiaki_session_start(&g_session);
        if (err != CHIAKI_ERR_SUCCESS)
        {
            app_log("[APP] chiaki_session_start failed: %s\n", chiaki_error_string(err));
            chiaki_session_fini(&g_session);
            stream_backend_close(&backend);
            snprintf(launcher_message, sizeof(launcher_message),
                     "Could not start stream: %s", chiaki_error_string(err));
            return_to_launcher = true;
            break;
        }

        // Attach the SDL controller state/feedback path to this session.
        input_set_session(input_ctx, &g_session);
        app_log_always("[APP] Session started — entering event loop\n");

        // ── Per-session event loop ────────────────────────────────────────────
        SDL_Event ev;
        uint32_t stream_start_ms = 0;
        uint64_t stable_stream_started_ms = 0;
        uint64_t last_latency_query_ms = 0;
        uint64_t decoder_keyframe_requests_seen = 0;
        int current_latency_ms = -1;
        bool current_latency_valid = false;
        bool watchdog_reconnect = false;
        const char *watchdog_reason = NULL;
        StreamHealth health;
        stream_health_init(&health, stats_monotonic_ms());

        while (!atomic_load_explicit(&g_session_ended, memory_order_acquire) &&
               !app_exit_requested())
        {
            bool have_video_frame =
                atomic_load_explicit(&g_stream_stats.video_frames,
                                     memory_order_relaxed) > 0;
            while (SDL_PollEvent(&ev))
            {
                if (ev.type == SDL_QUIT)
                {
                    // SDL_QUIT arrives when the user presses Home on webOS.
                    // Treat it as a deliberate exit — no reconnect.
                    app_log("[APP] SDL_QUIT received (Home button?) — exiting\n");
                    app_request_exit();
                    break;
                }
                // TV remote Back or Red → disconnect/exit (not forwarded to PS5).
                if (ev.type == SDL_KEYDOWN &&
                    webos_event_is_back_or_red(&ev.key))
                {
                    app_log("[APP] Remote Back/Red — disconnecting\n");
                    app_request_exit();
                    break;
                }
                // ── Block TV remote navigation keys during streaming ─────────
                // During an active stream, the TV remote should not send any
                // input to the PS5 — only the SDL GameController path
                // controls the console.  Remote keys we handle here:
                //   UP (short press) → toggle stats overlay
                //   All other nav keys → swallowed silently
                // Volume, Power, Mute are handled by webOS at the system level
                // and never arrive as SDL events.  Back and Home are handled
                // above before this block.
                if (have_video_frame && (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP))
                {
                    SDL_Keycode k = ev.key.keysym.sym;
                    bool is_up = (k == SDLK_UP || (int)k == 1073741906);    // WEBOS_KEY_UP

                    // Block ALL remote directional / navigation keys from
                    // reaching the PS5. Gamepad D-pad uses SDL controller events.
                    bool is_remote_nav = is_up
                        || k == SDLK_DOWN   || (int)k == 1073741905   // WEBOS_KEY_DOWN
                        || k == SDLK_LEFT   || (int)k == 1073741904   // WEBOS_KEY_LEFT
                        || k == SDLK_RIGHT  || (int)k == 1073741903   // WEBOS_KEY_RIGHT
                        || k == SDLK_RETURN || k == SDLK_KP_ENTER
                        || (int)k == 1073742089                        // WEBOS_KEY_GREEN
                        || (int)k == 1073742090                        // WEBOS_KEY_YELLOW
                        || (int)k == 1073742091;                       // WEBOS_KEY_BLUE

                    if (is_remote_nav)
                    {
                        // Toggle stats overlay on short press of UP (KEYDOWN, no repeat)
                        if (is_up && ev.type == SDL_KEYDOWN && !ev.key.repeat)
                        {
                            stats_overlay_toggle(&stats_overlay);
                            app_log_always("[UI] Stats overlay %s (UP to toggle)\n",
                                           stats_overlay.enabled ? "ON" : "OFF");
                        }
                        // Swallow all remote nav keys — don't forward to PS5
                        continue;
                    }
                }
                input_handle_event(input_ctx, &ev);
            }

            input_pump(input_ctx);

            const uint64_t health_now_ms = stats_monotonic_ms();
            have_video_frame =
                atomic_load_explicit(&g_stream_stats.video_frames,
                                     memory_order_acquire) > 0;
            uint64_t last_video_frame_ms =
                atomic_load_explicit(&g_stream_stats.video_last_frame_ms,
                                     memory_order_acquire);
            if (have_video_frame && last_video_frame_ms == 0)
                last_video_frame_ms = health_now_ms;

            if (have_video_frame && stable_stream_started_ms == 0)
            {
                stable_stream_started_ms = health_now_ms;
                if (watchdog_recovery_active)
                {
                    watchdog_recovery_active = false;
                    watchdog_recovery_retries = 0;
                    app_log_always("[RECOVERY] Video resumed; watchdog recovery complete\n");
                }
            }

            if (health_now_ms - last_latency_query_ms >=
                STREAM_HEALTH_LATENCY_SAMPLE_MS)
            {
                int latency_us = 0;
                current_latency_valid = SS4S_PlayerGetVideoLatency(
                    backend.player, 1000000, &latency_us);
                current_latency_ms = current_latency_valid
                    ? (latency_us + 500) / 1000 : -1;
                atomic_store_explicit(&g_stream_stats.video_latency_ms,
                                      current_latency_ms, memory_order_relaxed);
                last_latency_query_ms = health_now_ms;
            }

            uint64_t decoder_keyframe_requests = atomic_load_explicit(
                &g_stream_stats.video_feed_keyframe_requests,
                memory_order_relaxed);
            if (decoder_keyframe_requests > decoder_keyframe_requests_seen)
            {
                decoder_keyframe_requests_seen = decoder_keyframe_requests;
                if (stream_health_allow_idr(&health, health_now_ms))
                    send_idr_request("decoder requested keyframe");
            }

            StreamHealthAction health_action = stream_health_update(
                &health, health_now_ms, have_video_frame, last_video_frame_ms,
                current_latency_valid, current_latency_ms);
            if (health_action == STREAM_HEALTH_ACTION_REQUEST_IDR)
            {
                send_idr_request(have_video_frame ? "video stalled" : "startup timeout");
            }
            else if (health_action != STREAM_HEALTH_ACTION_NONE)
            {
                watchdog_reconnect = true;
                if (!watchdog_recovery_active)
                    watchdog_recovery_retries = 0;
                watchdog_recovery_active = true;
                if (health_action == STREAM_HEALTH_ACTION_RECONNECT_STARTUP)
                    watchdog_reason = "no accepted video frame for 15 seconds";
                else if (health_action == STREAM_HEALTH_ACTION_RECONNECT_STALL)
                    watchdog_reason = "video feed stalled for 5 seconds";
                else
                    watchdog_reason = "decoder latency exceeded 750 ms for 5 samples";
                app_log_always("[RECOVERY] Reconnecting: %s\n", watchdog_reason);
                atomic_store_explicit(&g_session_ended, true, memory_order_release);
                break;
            }

            // While we're waiting for the first video sample, show an opaque
            // loading screen. Once video starts, switch back to transparent
            // frames so the NDL plane underneath shows through.
            if (!have_video_frame)
            {
                ui_render_loading(g_renderer, "Starting stream");
                SDL_RenderPresent(g_renderer);
                SDL_Delay(16);
            }
            else
            {
                // Capture the moment the first video frame appeared.
                if (stream_start_ms == 0)
                    stream_start_ms = SDL_GetTicks();

                // Present a transparent frame each tick.
                // This does two things: (1) keeps the webOS compositor seeing
                // active frame production so it won't terminate the app for
                // inactivity, and (2) ensures the NDL video plane underneath
                // is composited and visible on screen.
                SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
                SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 0);
                SDL_RenderClear(g_renderer);

                stats_overlay_update(&stats_overlay, &g_stream_stats, health_now_ms);
                if (stats_overlay.enabled)
                    ui_render_stats_overlay(g_renderer, stats_overlay.text);

                // Show a brief startup hint for 6 seconds (with 1s fade-out)
                // so users know they can press UP for the stats overlay.
                // Skip the hint if the stats overlay is already visible.
                if (!stats_overlay.enabled && stream_start_ms != 0)
                {
                    const uint32_t hint_duration_ms = 6000;
                    const uint32_t fade_ms = 1000;
                    uint32_t elapsed = (uint32_t)(SDL_GetTicks() - stream_start_ms);
                    if (elapsed < hint_duration_ms)
                    {
                        float opacity = 1.0f;
                        if (elapsed > hint_duration_ms - fade_ms)
                            opacity = (float)(hint_duration_ms - elapsed) / (float)fade_ms;
                        ui_render_hint(g_renderer, "Press UP on remote for stream stats", opacity);
                    }
                }

                SDL_RenderPresent(g_renderer);
                SDL_Delay(4);   // ~250Hz loop; keeps compositor alive without adding input latency
            }
        }

        ChiakiQuitReason session_reason = (ChiakiQuitReason)
            atomic_load_explicit(&g_quit_reason, memory_order_acquire);

        if (g_signal_exit)
            app_log_always("[APP] Termination signal received — exiting\n");

        // Only send the sleep command if the user/application is exiting. A
        // watchdog reconnect must never put the console into Rest Mode.
        if (cfg.sleep_on_exit && app_exit_requested() &&
            !atomic_load_explicit(&g_ps5_sleeping, memory_order_acquire))
        {
            app_log("[APP] Sending PS5 to rest mode\n");
            chiaki_session_goto_bed(&g_session);
            SDL_Delay(500);
        }

        app_log_always("[APP] Stopping session (reason=%d watchdog=%s)\n",
                       (int)session_reason,
                       watchdog_reason ? watchdog_reason : "none");
        chiaki_session_stop(&g_session);
        chiaki_session_join(&g_session);
        input_set_session(input_ctx, NULL);
        if (session_reason == CHIAKI_QUIT_REASON_NONE)
            session_reason = (ChiakiQuitReason)
                atomic_load_explicit(&g_quit_reason, memory_order_acquire);
        chiaki_session_fini(&g_session);
        log_stream_summary(attempt, session_reason);
        stream_backend_close(&backend);

        if (stable_stream_started_ms > 0 &&
            stats_monotonic_ms() - stable_stream_started_ms >=
                STREAM_HEALTH_STABLE_RESET_MS)
        {
            consecutive_failures = 0;
            app_log_always("[RECOVERY] Stable 60-second stream; retry backoff reset\n");
        }

        // ── Reconnect decision ────────────────────────────────────────────────
        const bool watchdog_session_in_use_retry =
            session_reason == CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE &&
            stream_watchdog_can_retry_session_in_use(
                watchdog_recovery_active, watchdog_recovery_retries);
        const char *request_failure =
            (watchdog_reconnect || watchdog_session_in_use_retry)
                ? NULL : session_request_failure_message(session_reason);
        if (watchdog_session_in_use_retry)
            app_log_always(
                "[RECOVERY] Console is still releasing the previous Remote Play session "
                "(retry %u/%u)\n",
                watchdog_recovery_retries + 1u,
                STREAM_HEALTH_WATCHDOG_RP_IN_USE_RETRIES);
        if (!app_exit_requested() && request_failure)
        {
            snprintf(launcher_message, sizeof(launcher_message),
                     "%s", request_failure);
            return_to_launcher = true;
            app_log_always("[APP] Session request failed — returning to launcher\n");
            break;
        }
        else if (!app_exit_requested() &&
                 (watchdog_reconnect || watchdog_session_in_use_retry ||
                  should_reconnect(session_reason)))
        {
            const bool watchdog_retry = watchdog_recovery_active;
            uint32_t delay_ms = watchdog_retry
                ? stream_watchdog_retry_delay_ms(watchdog_recovery_retries++)
                : stream_retry_delay_ms(consecutive_failures);
            consecutive_failures++;
            if (watchdog_retry)
            {
                app_log_always(
                    "[RECOVERY] Retrying stream in %u ms "
                    "(watchdog recovery retry %u, failure streak %u)\n",
                    delay_ms, watchdog_recovery_retries, consecutive_failures);
            }
            else
            {
                app_log_always(
                    "[RECOVERY] Retrying stream in %u ms (failure streak %u)\n",
                    delay_ms, consecutive_failures);
            }
            if (!wait_for_reconnect(g_renderer, delay_ms))
                break;
        }
        else
        {
            // Deliberate exit or console shutdown — leave the loop.
            break;
        }
    }

cleanup_ss4s:
    if (ss4s_initialized)
        SS4S_Quit();

    if (return_to_launcher && !app_exit_requested())
        goto show_launcher;

cleanup_sdl:
    input_fini(input_ctx);
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    SDL_Quit();

    config_free(&cfg);
    curl_global_cleanup();
    if (g_log_file) fclose(g_log_file);
    return 0;
}
