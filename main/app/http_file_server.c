#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <unistd.h>

#include "midi_io.h"
#include "midi_parser.h"
#include "http_file_server.h"
#include "wifi_softap.h"

static const char *TAG4 = "HTTP_FS";
static httpd_handle_t server = NULL;

#define MOUNT_POINT "/sdcard"

// Embedded UI
static const char index_html[] = "<!doctype html><html><head><meta charset=\"utf-8\"><title>Glockenspiel SD Browser</title></head><body><h1>SD Card Browser</h1><input id=\"fileinput\" type=\"file\" /><button id=\"upload\">Upload</button><ul id=\"files\"></ul><script src=\"/app.js\"></script></body></html>";
static const char app_js[] = "async function listFiles(){const res=await fetch('/api/files');const files=await res.json();const ul=document.getElementById('files');ul.innerHTML='';files.forEach(name=>{const li=document.createElement('li');li.textContent=name;const play=document.createElement('button');play.textContent='Play';play.onclick=()=>fetch('/api/play?filename='+encodeURIComponent(name),{method:'POST'});const del=document.createElement('button');del.textContent='Delete';del.onclick=async()=>{await fetch('/api/file?filename='+encodeURIComponent(name),{method:'DELETE'});listFiles();};const dl=document.createElement('a');dl.textContent='Download';dl.href='/files/'+encodeURIComponent(name);li.appendChild(document.createTextNode(' '));li.appendChild(play);li.appendChild(document.createTextNode(' '));li.appendChild(del);li.appendChild(document.createTextNode(' '));li.appendChild(dl);ul.appendChild(li);});}document.getElementById('upload').onclick=async()=>{const f=document.getElementById('fileinput').files[0];if(!f) return alert('Choose file');const path=f.name;const data=await f.arrayBuffer();await fetch('/api/upload?filename='+encodeURIComponent(path),{method:'POST',body:data});listFiles();};listFiles();";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, index_html);
    return ESP_OK;
}

static esp_err_t appjs_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_sendstr(req, app_js);
    return ESP_OK;
}

// Helper: send 500
static esp_err_t send_500(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

// GET /api/files -> JSON array of filenames
static esp_err_t files_get_handler(httpd_req_t *req)
{
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) return send_500(req, "Failed to open SD mount");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");

    struct dirent *entry;
    bool first = true;
    while ((entry = readdir(dir)) != NULL) {
        if (!first) httpd_resp_sendstr_chunk(req, ",");
        first = false;
        // escape simple quotes
        char buf[512];
        snprintf(buf, sizeof(buf), "\"%s\"", entry->d_name);
        httpd_resp_sendstr_chunk(req, buf);
    }

    closedir(dir);
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// GET /files/<name> -> serve file
static esp_err_t file_get_handler(httpd_req_t *req)
{
    char filepath[512];
    const char *filename = req->uri + strlen("/files/");
    snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, filename);

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "Not found", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");

    char chunk[1024];
    size_t r;
    while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send(req, chunk, r) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }

    fclose(f);
    return ESP_OK;
}

// POST /api/upload?filename=NAME body=binary
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    char filepath[512];

    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return send_500(req, "Missing filename query");

    // parse filename param
    char param[128];
    if (httpd_query_key_value(query, "filename", param, sizeof(param)) != ESP_OK) {
        return send_500(req, "filename param required");
    }

    snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, param);

    FILE *f = fopen(filepath, "wb");
    if (!f) return send_500(req, "Unable to open target file for writing");

    char buf[1024];
    int ret;
    while ((ret = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, ret, f);
    }

    fclose(f);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// DELETE /api/file?filename=NAME
static esp_err_t delete_handler(httpd_req_t *req)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return send_500(req, "Missing query");

    char param[128];
    if (httpd_query_key_value(query, "filename", param, sizeof(param)) != ESP_OK) {
        return send_500(req, "filename param required");
    }

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, param);
    if (unlink(filepath) != 0) {
        return send_500(req, "Failed to delete file");
    }

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// POST /api/play?filename=NAME -> spawn playback task
static void play_task(void *arg)
{
    char *path = (char*)arg;
    // use existing read_midi_file which blocks playback
    read_midi_file(path, -1, -1);
    free(path);
    vTaskDelete(NULL);
}

static esp_err_t play_handler(httpd_req_t *req)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return send_500(req, "Missing query");

    char param[128];
    if (httpd_query_key_value(query, "filename", param, sizeof(param)) != ESP_OK) {
        return send_500(req, "filename param required");
    }

    char *filepath = malloc(512);
    if (!filepath) return send_500(req, "OOM");
    snprintf(filepath, 512, "%s/%s", MOUNT_POINT, param);

    // Spawn playback task
    if (xTaskCreate(play_task, "play_midi", 8 * 1024, filepath, 5, NULL) != pdPASS) {
        free(filepath);
        return send_500(req, "Failed to start playback task");
    }

    httpd_resp_sendstr(req, "PLAYING");
    return ESP_OK;
}

// POST /api/provision?ssid=...&pass=...&token=...
static esp_err_t provision_handler(httpd_req_t *req)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return send_500(req, "Missing query");

    char ssid[128];
    char pass[128];
    char token[64];
    if (httpd_query_key_value(query, "ssid", ssid, sizeof(ssid)) != ESP_OK) {
        return send_500(req, "ssid required");
    }
    // pass optional
    httpd_query_key_value(query, "pass", pass, sizeof(pass));
    if (httpd_query_key_value(query, "token", token, sizeof(token)) != ESP_OK) {
        return send_500(req, "token required");
    }

    esp_err_t r = wifi_provision(ssid, pass[0] ? pass : NULL, token);
    if (r != ESP_OK) {
        if (r == ESP_ERR_INVALID_ARG) {
            httpd_resp_set_status(req, "403 Forbidden");
            httpd_resp_sendstr(req, "Invalid token or args");
            return ESP_FAIL;
        }
        return send_500(req, "Provision failed");
    }

    httpd_resp_sendstr(req, "PROVISIONED");
    return ESP_OK;
}

esp_err_t start_file_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG4, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    // register embedded UI handlers
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL
    };
    httpd_uri_t appjs = {
        .uri = "/app.js",
        .method = HTTP_GET,
        .handler = appjs_get_handler,
        .user_ctx = NULL
    };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &appjs);

    // Register URI handlers
    httpd_uri_t files_get = {
        .uri = "/api/files",
        .method = HTTP_GET,
        .handler = files_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &files_get);

    httpd_uri_t upload_post = {
        .uri = "/api/upload",
        .method = HTTP_POST,
        .handler = upload_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &upload_post);

    httpd_uri_t delete_uri = {
        .uri = "/api/file",
        .method = HTTP_DELETE,
        .handler = delete_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &delete_uri);

    httpd_uri_t play_uri = {
        .uri = "/api/play",
        .method = HTTP_POST,
        .handler = play_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &play_uri);

    httpd_uri_t provision_uri = {
        .uri = "/api/provision",
        .method = HTTP_POST,
        .handler = provision_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &provision_uri);

    // Serve files under /files/
    httpd_uri_t file_get = {
        .uri = "/files/*",
        .method = HTTP_GET,
        .handler = file_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &file_get);

    ESP_LOGI(TAG4, "File server started");
    return ESP_OK;
}

void stop_file_server(void)
{
    if (server) httpd_stop(server);
}
