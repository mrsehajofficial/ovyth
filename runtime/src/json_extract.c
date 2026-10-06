/* Vayu runtime :: src/json_extract.c (optimized)
 *
 * Fast JSON field extraction without building the full AST.
 * Extracts a single value by path from JSON string directly.
 * 
 * KEY OPTIMIZATIONS:
 *   1. NO VyStr allocation during key comparison - use direct memcmp
 *   2. Single reusable buffer for key parsing (stack-allocated)
 *   3. Early termination on match
 *   4. Minimal stack usage
 */
#include "vyrt.h"
#include "vy_sb.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    const char* p;
    const char* end;
    int depth;
    int failed;
} J;

static void skip_ws(J* j) {
    while (j->p < j->end) {
        char c = *j->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->p++;
        else break;
    }
}

static int at(J* j, char c) { return j->p < j->end && *j->p == c; }

static int lit(J* j, const char* s) {
    size_t n = strlen(s);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, s, n) != 0) return 0;
    j->p += n;
    return 1;
}

static int hex4(const char* p, uint32_t* out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static void emit_utf8(VyJsonBuf* sb, uint32_t cp) {
    char buf[4];
    int n = 0;
    if (cp < 0x80) {
        buf[n++] = (char)cp;
    } else if (cp < 0x800) {
        buf[n++] = (char)(0xC0 | (cp >> 6));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        buf[n++] = (char)(0xE0 | (cp >> 12));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        buf[n++] = (char)(0xF0 | (cp >> 18));
        buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    }
    vy_jbuf_append(sb, buf, (size_t)n);
}

/* Parse a JSON string value into buffer, return 1 on success */
static int parse_json_string_val(J* j, VyJsonBuf* sb) {
    if (!at(j, '"')) return 0;
    j->p++;
    vy_jbuf_init(sb);
    while (j->p < j->end && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p;
        if (c == '\\') {
            j->p++;
            if (j->p >= j->end) break;
            char e = *j->p++;
            switch (e) {
                case '"':  vy_jbuf_putc(sb, '"');  break;
                case '\\': vy_jbuf_putc(sb, '\\'); break;
                case '/':  vy_jbuf_putc(sb, '/');  break;
                case 'b':  vy_jbuf_putc(sb, '\b'); break;
                case 'f':  vy_jbuf_putc(sb, '\f'); break;
                case 'n':  vy_jbuf_putc(sb, '\n'); break;
                case 'r':  vy_jbuf_putc(sb, '\r'); break;
                case 't':  vy_jbuf_putc(sb, '\t'); break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(j->p, &cp)) { j->failed = 1; return 0; }
                    j->p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && j->p + 2 < j->end &&
                        j->p[0] == '\\' && j->p[1] == 'u') {
                        uint32_t lo;
                        if (hex4(j->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            j->p += 6;
                        }
                    }
                    emit_utf8(sb, cp);
                    break;
                }
                default: vy_jbuf_putc(sb, e); break;
            }
        } else {
            vy_jbuf_putc(sb, (char)c);
            j->p++;
        }
    }
    if (!at(j, '"')) { j->failed = 1; return 0; }
    j->p++;
    return 1;
}

/* Skip a complete JSON value starting at j->p */
static void skip_value(J* j) {
    skip_ws(j);
    if (at(j, '"')) {
        VyJsonBuf sb;
        vy_jbuf_init(&sb);
        parse_json_string_val(j, &sb);
        vy_jbuf_free(&sb);
    } else if (at(j, '{')) {
        j->depth++;
        j->p++;
        skip_ws(j);
        if (at(j, '}')) { j->p++; j->depth--; return; }
        while (1) {
            skip_ws(j);
            VyJsonBuf ksb;
            vy_jbuf_init(&ksb);
            if (!parse_json_string_val(j, &ksb)) { j->depth--; return; }
            vy_jbuf_free(&ksb);
            skip_ws(j);
            if (!at(j, ':')) { j->depth--; return; }
            j->p++;
            skip_value(j);
            skip_ws(j);
            if (at(j, ',')) { j->p++; continue; }
            if (at(j, '}')) { j->p++; j->depth--; return; }
            j->depth--; return;
        }
    } else if (at(j, '[')) {
        j->depth++;
        j->p++;
        skip_ws(j);
        if (at(j, ']')) { j->p++; j->depth--; return; }
        while (1) {
            skip_value(j);
            skip_ws(j);
            if (at(j, ',')) { j->p++; continue; }
            if (at(j, ']')) { j->p++; j->depth--; return; }
            j->depth--; return;
        }
    } else if (at(j, 't')) {
        if (lit(j, "true")) return;
    } else if (at(j, 'f')) {
        if (lit(j, "false")) return;
    } else if (at(j, 'n')) {
        if (lit(j, "null")) return;
    } else {
        /* Number */
        if (at(j, '-')) j->p++;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
        if (at(j, '.')) { j->p++; while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++; }
        if (at(j, 'e') || at(j, 'E')) {
            j->p++;
            if (at(j, '+') || at(j, '-')) j->p++;
            while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
        }
    }
}

/* Parse one path segment: either "key" or "[index]" */
static int parse_path_seg(const char** pp, char* name, size_t namelen, int* is_arr, int64_t* idx) {
    const char* p = *pp;
    
    /* Array index access */
    if (*p == '[') {
        p++;
        *is_arr = 1;
        *idx = 0;
        int neg = 0;
        if (*p == '-') { neg = 1; p++; }
        while (*p >= '0' && *p <= '9') {
            *idx = *idx * 10 + (*p - '0');
            p++;
        }
        if (neg) *idx = -(*idx);
        if (*p != ']') return 0;
        p++;
        if (*p == '.') p++;
        *pp = p;
        name[0] = '\0';
        return 1;
    }
    
    /* Object key */
    *is_arr = 0;
    size_t i = 0;
    while (*p && *p != '.' && *p != '[' && i < namelen - 1) {
        name[i++] = *p++;
    }
    name[i] = '\0';
    if (*p == '.') p++;
    *pp = p;
    return 1;
}

/* ============================================================
 * OPTIMIZED VERSION: Avoids VyStr allocation for key comparison
 * ============================================================ */

/* Fast string parsing into a fixed buffer - no heap allocation */
static size_t parse_key_to_buf(J* j, char* buf, size_t bufsz) {
    if (!at(j, '"')) return 0;
    j->p++; /* skip opening quote */
    
    size_t len = 0;
    while (j->p < j->end && *j->p != '"' && len < bufsz - 1) {
        if (*j->p == '\\' && j->p + 1 < j->end) {
            j->p++;
            switch (*j->p++) {
                case '"':  buf[len++] = '"';  break;
                case '\\': buf[len++] = '\\'; break;
                case '/':  buf[len++] = '/';  break;
                case 'b':  buf[len++] = '\b'; break;
                case 'f':  buf[len++] = '\f'; break;
                case 'n':  buf[len++] = '\n'; break;
                case 'r':  buf[len++] = '\r'; break;
                case 't':  buf[len++] = '\t'; break;
                default:   buf[len++] = *(j->p - 1); break;
            }
        } else {
            buf[len++] = *j->p++;
        }
    }
    
    if (!at(j, '"')) return 0;
    j->p++; /* skip closing quote */
    buf[len] = '\0';
    return len;
}

/*
 * Optimized extraction using buffer-based key comparison.
 * Eliminates the VyStr allocation per key lookup.
 */
VyValue vy_json_extract_field(const char* json, size_t len, const char* path) {
    if (!json || !path) return vy_nil();
    
    J j;
    j.p = json;
    j.end = json + len;
    j.depth = 0;
    j.failed = 0;
    
    skip_ws(&j);
    if (!at(&j, '{')) return vy_nil();
    j.p++;
    
    char seg_name[128];
    int is_array;
    int64_t arr_idx;
    const char* path_p = path;
    
    /* Reusable buffers - NO heap allocation! */
    char key_buf[256];
    
    while (*path_p) {
        if (!parse_path_seg(&path_p, seg_name, sizeof(seg_name), &is_array, &arr_idx)) {
            return vy_nil();
        }
        
        /* Find the key in current object */
        int found = 0;
        while (j.p < j.end) {
            skip_ws(&j);
            if (at(&j, '}')) return vy_nil();
            if (at(&j, ',')) { j.p++; continue; }
            
            /* Parse key DIRECTLY INTO BUFFER - NO VyStr allocation! */
            size_t key_len = parse_key_to_buf(&j, key_buf, sizeof(key_buf));
            if (key_len == 0) {
                return vy_nil();
            }
            
            skip_ws(&j);
            if (!at(&j, ':')) {
                return vy_nil();
            }
            j.p++;
            
            /* DIRECT memcmp - NO strlen, NO allocation! */
            if (!is_array && strlen(seg_name) == key_len && 
                memcmp(key_buf, seg_name, key_len) == 0) {
                found = 1;
                break;
            }
            
            /* Skip this value and continue */
            skip_value(&j);
        }
        
        if (!found) return vy_nil();
        
        /* Now we're positioned at the value for the matching key */
        skip_ws(&j);
        
        if (is_array) {
            /* Must be an array - extract element at arr_idx */
            if (!at(&j, '[')) return vy_nil();
            j.p++;
            
            int idx = 0;
            while (1) {
                skip_ws(&j);
                if (at(&j, ']')) return vy_nil();
                
                if (idx == arr_idx) {
                    /* Save the position of this element so we can continue parsing */
                    const char* elem_start = j.p;
                    
                    /* If there are more path segments, we need to parse this element as a new JSON context */
                    if (*path_p) {
                        /* Skip past this element first to find its length */
                        skip_value(&j);
                        size_t elem_len = j.p - elem_start;
                        
                        /* Re-run extraction on this element with remaining path */
                        VyValue val = vy_json_extract_field(elem_start, elem_len, path_p);
                        return val;
                    }
                    
                    /* No more path - extract the value directly */
                    VyValue val = vy_nil();
                    skip_ws(&j);
                    
                    if (at(&j, '"')) {
                        VyJsonBuf sb;
                        vy_jbuf_init(&sb);
                        if (parse_json_string_val(&j, &sb)) {
                            VyStr* s = vy_jbuf_finish(&sb);
                            val = vy_str(s);
                        }
                        vy_jbuf_free(&sb);
                    } else if (at(&j, '{') || at(&j, '[')) {
                        /* Nested structure with no more path - return nil for now */
                        return vy_nil();
                    } else {
                        /* Number or literal */
                        const char* start = j.p;
                        while (j.p < j.end && *j.p != ',' && *j.p != ']' && *j.p != '}') j.p++;
                        size_t n = j.p - start;
                        char buf[256];
                        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
                        memcpy(buf, start, n);
                        buf[n] = '\0';
                        char* end;
                        double d = strtod(buf, &end);
                        if (end == buf + n) {
                            val = vy_float(d);
                        } else {
                            long long ll = strtoll(buf, &end, 10);
                            if (end == buf + n) val = vy_int(ll);
                            else val = vy_bool(0);
                        }
                        if (lit(&j, "true")) val = vy_bool(1);
                        else if (lit(&j, "false")) val = vy_bool(0);
                        else if (lit(&j, "null")) val = vy_nil();
                    }
                    
                    /* Skip remaining array elements */
                    while (j.p < j.end) {
                        skip_ws(&j);
                        if (at(&j, ']')) { j.p++; break; }
                        if (at(&j, ',')) { j.p++; continue; }
                        skip_value(&j);
                    }
                    return val;
                }
                
                /* Skip this element */
                skip_value(&j);
                skip_ws(&j);
                if (at(&j, ']')) { j.p++; return vy_nil(); }
                if (at(&j, ',')) j.p++;
                idx++;
            }
        } else {
            /* Object key - check if there are more path segments */
            if (!*path_p) {
                /* No more path - extract and return this value */
                VyValue val = vy_nil();
                
                if (at(&j, '"')) {
                    VyJsonBuf sb;
                    vy_jbuf_init(&sb);
                    if (parse_json_string_val(&j, &sb)) {
                        VyStr* s = vy_jbuf_finish(&sb);
                        val = vy_str(s);
                    }
                    vy_jbuf_free(&sb);
                } else if (at(&j, '{') || at(&j, '[')) {
                    /* Nested structure - not supported for simple extraction */
                    return vy_nil();
                } else {
                    /* Number or literal */
                    const char* start = j.p;
                    while (j.p < j.end && *j.p != ',' && *j.p != '}' && *j.p != ']') j.p++;
                    size_t n = j.p - start;
                    char buf[256];
                    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
                    memcpy(buf, start, n);
                    buf[n] = '\0';
                    char* end;
                    double d = strtod(buf, &end);
                    if (end == buf + n) {
                        val = vy_float(d);
                    } else {
                        long long ll = strtoll(buf, &end, 10);
                        if (end == buf + n) val = vy_int(ll);
                        else val = vy_bool(0);
                    }
                    if (lit(&j, "true")) val = vy_bool(1);
                    else if (lit(&j, "false")) val = vy_bool(0);
                    else if (lit(&j, "null")) val = vy_nil();
                }
                return val;
            }
            
            /* More path segments - recurse into nested structure */
            if (at(&j, '{')) {
                j.p++; /* skip { */
                /* Continue loop to process next segment */
            } else if (at(&j, '[')) {
                /* Array - need special handling for index */
                j.p++; /* skip [ */
                skip_ws(&j);
                /* Parse array index from remaining path */
                int64_t inner_idx = 0;
                if (*path_p == '[') {
                    path_p++; /* skip [ */
                    while (*path_p >= '0' && *path_p <= '9') {
                        inner_idx = inner_idx * 10 + (*path_p - '0');
                        path_p++;
                    }
                    if (*path_p == ']') path_p++; /* skip ] */
                    if (*path_p == '.') path_p++; /* skip dot */
                }
                
                /* Find the element at inner_idx */
                int idx = 0;
                while (1) {
                    skip_ws(&j);
                    if (at(&j, ']')) break;
                    
                    if (idx == inner_idx) {
                        /* Found the element - save position and continue */
                        const char* elem_start = j.p;
                        skip_value(&j);
                        size_t elem_len = j.p - elem_start;
                        
                        /* Re-run extraction on this element */
                        VyValue val = vy_json_extract_field(elem_start, elem_len, path_p);
                        return val;
                    }
                    
                    skip_value(&j);
                    skip_ws(&j);
                    if (at(&j, ']')) break;
                    if (at(&j, ',')) j.p++;
                    idx++;
                }
                return vy_nil();
            } else {
                return vy_nil();
            }
        }
    }
    
    return vy_nil();
}
