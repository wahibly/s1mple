// src/static.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "static.h"
#include "http.h"
#include "server_tcp.h"
#include "logger.h"


typedef struct {
    const char *ext;
    const char *mime;
} mime_entry_t;

static const mime_entry_t mime_table[] = {
    // text
    { ".html",  "text/html; charset=utf-8"           },
    { ".htm",   "text/html; charset=utf-8"           },
    { ".css",   "text/css"                            },
    { ".js",    "application/javascript"              },
    { ".mjs",   "application/javascript"              },
    { ".json",  "application/json"                    },
    { ".xml",   "application/xml"                     },
    { ".txt",   "text/plain; charset=utf-8"           },
    { ".md",    "text/markdown"                       },
    // image
    { ".png",   "image/png"                           },
    { ".jpg",   "image/jpeg"                          },
    { ".jpeg",  "image/jpeg"                          },
    { ".gif",   "image/gif"                           },
    { ".webp",  "image/webp"                          },
    { ".svg",   "image/svg+xml"                       },
    { ".ico",   "image/x-icon"                        },
    { ".bmp",   "image/bmp"                           },
    // font
    { ".woff",  "font/woff"                           },
    { ".woff2", "font/woff2"                          },
    { ".ttf",   "font/ttf"                            },
    { ".otf",   "font/otf"                            },
    // video/audio
    { ".mp4",   "video/mp4"                           },
    { ".webm",  "video/webm"                          },
    { ".ogg",   "video/ogg"                           },
    { ".mp3",   "audio/mpeg"                          },
    { ".wav",   "audio/wav"                           },
    // documents/data
    { ".pdf",   "application/pdf"                     },
    { ".zip",   "application/zip"                     },
    { ".gz",    "application/gzip"                    },
    { ".wasm",  "application/wasm"                    },
    { NULL,     NULL                                  } 
};

// get type
const char *static_mime_type(const char *filepath) {
    const char *ext = strrchr(filepath, '.'); //to extract the extension
    if (!ext) return "application/octet-stream";

    for (int i = 0; mime_table[i].ext != NULL; i++) {
        if (strcasecmp(ext, mime_table[i].ext) == 0) {
            return mime_table[i].mime;
        }
    }

    return "application/octet-stream";
}

// build etag
// ETag = hex string of file size XOR modification time. (fast just metadata not whole file)
static void build_etag(const struct stat *st, char *out, int out_size) {
    snprintf(out, out_size, "\"%lx-%lx\"", (unsigned long)st->st_size, (unsigned long)st->st_mtime);
}

// parse bytes=start-end from a range header value
static int parse_range(const char *range_header, long file_size, long *out_start, long *out_end) {
    if (!range_header) return 0;
    if (strncmp(range_header, "bytes=", 6) != 0) return 0;

    const char *spec = range_header + 6;
    long start = -1, end = -1;

    if (spec[0] == '-') {
        // bytes=-x means last x byte
        long suffix = atol(spec + 1);
        if (suffix <= 0) return 0;
        start = file_size - suffix;
        end   = file_size - 1;
    } else {
        char *dash = strchr(spec, '-');
        if (!dash) return 0;

        start = atol(spec);
        if (*(dash + 1) == '\0') {
            // bytes=x- means from x to end
            end = file_size - 1;
        } else {
            end = atol(dash + 1);
        }
    }

    if (start < 0 || end < 0 || start > end || end >= file_size) return 0;

    *out_start = start;
    *out_end   = end;
    return 1;
}

void static_serve(int conn_fd, http_request_t *req, const char *filepath) {
    printf("ENTERED static_serve()\n");
    fflush(stdout);

    // stat() give us size and modification time without open the file
    struct stat st;
    if (stat(filepath, &st) < 0) {
        http_respond_error(conn_fd, "404 Not Found", "The requested file was not found.");
        return;
    }

    // reject directories
    if (S_ISDIR(st.st_mode)) {
        http_respond_error(conn_fd, "403 Forbidden", "Directory listing is not allowed.");
        return;
    }

    long file_size = (long)st.st_size;

    char etag[64];
    build_etag(&st, etag, sizeof(etag));

    const char *if_none_match = http_get_header(req, "If-None-Match");
    if (if_none_match && strcmp(if_none_match, etag) == 0) {
        // client has the current version so send 304(no body)
        char headers[512];
        int  hlen = snprintf(headers, sizeof(headers),
            "HTTP/1.1 304 Not Modified\r\n"
            "ETag: %s\r\n"
            "Connection: %s\r\n"
            "\r\n",
            etag,
            req->keep_alive ? "keep-alive" : "close");
        write(conn_fd, headers, hlen);
        log_info("static", "304 %s (ETag match)", filepath);
        return;
    }

    const char *ims = http_get_header(req, "If-Modified-Since");
    if (ims) {
        struct tm tm_ims;
        memset(&tm_ims, 0, sizeof(tm_ims));
        // HTTP date format: "Mon, 26 Jun 2005 00:00:00 GMT"
        if (strptime(ims, "%a, %d %b %Y %H:%M:%S GMT", &tm_ims)) {
            time_t req_time = timegm(&tm_ims);
            if (req_time >= st.st_mtime) {
                char headers[512];
                int  hlen = snprintf(headers, sizeof(headers),
                    "HTTP/1.1 304 Not Modified\r\n"
                    "ETag: %s\r\n"
                    "Connection: %s\r\n"
                    "\r\n",
                    etag,
                    req->keep_alive ? "keep-alive" : "close");
                write(conn_fd, headers, hlen);
                log_info("static", "304 %s (If-Modified-Since)", filepath);
                return;
            }
        }
    }

    const char *mime = static_mime_type(filepath);

  //check range request
    const char *range_header = http_get_header(req, "Range");
    long range_start = 0;
    long range_end   = file_size - 1;
    int  is_range    = 0;

    printf("Range header = [%s]\n",
       range_header ? range_header : "NULL");

    if (range_header) {
        if (parse_range(range_header, file_size, &range_start, &range_end)) {
            is_range = 1;
        } else {
            // invalid range , return 416 Range Not Satisfiable
            char headers[256];
            int  hlen = snprintf(headers, sizeof(headers),
                "HTTP/1.1 416 Range Not Satisfiable\r\n"
                "Content-Range: bytes */%ld\r\n"
                "Connection: close\r\n"
                "\r\n",
                file_size);
            write(conn_fd, headers, hlen);
            return;
        }
    }

    long send_len = range_end - range_start + 1;

    //open and seek to position
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        http_respond_error(conn_fd, "500 Internal Server Error", "Could not open file.");
        return;
    }

    if (range_start > 0) {
        fseek(f, range_start, SEEK_SET);
    }

    // build last modified date string with HTTP format
    char last_modified[64];
    struct tm *gmt = gmtime(&st.st_mtime);
    strftime(last_modified, sizeof(last_modified), "%a, %d %b %Y %H:%M:%S GMT", gmt);

    char headers[1024];
    int  hlen;

    if (is_range) {
        hlen = snprintf(headers, sizeof(headers),
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n"
            "Content-Range: bytes %ld-%ld/%ld\r\n"
            "ETag: %s\r\n"
            "Last-Modified: %s\r\n"
            "Accept-Ranges: bytes\r\n"
            "Connection: %s\r\n"
            "\r\n",
            mime, send_len,
            range_start, range_end, file_size,
            etag, last_modified,
            req->keep_alive ? "keep-alive" : "close");
        log_info("static", "206 %s bytes=%ld-%ld",
                 filepath, range_start, range_end);
    } else {
        hlen = snprintf(headers, sizeof(headers),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n"
            "ETag: %s\r\n"
            "Last-Modified: %s\r\n"
            "Accept-Ranges: bytes\r\n"
            "Connection: %s\r\n"
            "\r\n",
            mime, send_len,
            etag, last_modified,
            req->keep_alive ? "keep-alive" : "close");
        log_info("static", "200 %s (%ld bytes, %s)",
                 filepath, send_len, mime);
    }

    write(conn_fd, headers, hlen);

    // don't read the whole file into memory , stream it in 64KB chunks
    char    chunk[65536];
    long    remaining = send_len;
    size_t  to_read;
    size_t  bytes_read;

    while (remaining > 0) {
        to_read    = (remaining > (long)sizeof(chunk)) ? sizeof(chunk) : (size_t)remaining;
        bytes_read = fread(chunk, 1, to_read, f);
        if (bytes_read <= 0) break;

        ssize_t written = write(conn_fd, chunk, bytes_read);
        if (written < 0) break; 

        remaining -= bytes_read;
    }

    fclose(f);
}