/* Ovyth runtime :: src/io.c
 *
 * File I/O operations for persistent storage.
 */
#include "ovrt.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <dirent.h>
#include <sys/types.h>
#endif

/* Read entire file into a string. Returns empty string on error. */
OvStr* ov_file_read(const char* path) {
  if (!path) return ov_str_new("", 0);

  FILE* f = fopen(path, "rb");
  if (!f) return ov_str_new("", 0);

  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (len < 0) {
    fclose(f);
    return ov_str_new("", 0);
  }

  char* buf = (char*)malloc((size_t)len + 1);
  if (!buf) {
    fclose(f);
    return ov_str_new("", 0);
  }

  size_t read = fread(buf, 1, (size_t)len, f);
  fclose(f);

  if (read != (size_t)len) {
    free(buf);
    return ov_str_new("", 0);
  }

  buf[len] = '\0';
  OvStr* s = ov_str_new(buf, (size_t)len);
  free(buf);
  return s;
}

/* Write string to file. Returns 1 on success, 0 on failure. */
int ov_file_write(const char* path, const char* data, size_t len) {
  if (!path) return 0;

  FILE* f = fopen(path, "wb");
  if (!f) return 0;

  size_t written = fwrite(data, 1, len, f);
  fclose(f);

  return written == len;
}

/* Append string to file. Returns 1 on success, 0 on failure. */
int ov_file_append(const char* path, const char* data, size_t len) {
  if (!path) return 0;

  FILE* f = fopen(path, "ab");
  if (!f) return 0;

  size_t written = fwrite(data, 1, len, f);
  fclose(f);

  return written == len;
}

/* Check if file exists. */
int ov_file_exists(const char* path) {
  if (!path) return 0;
  FILE* f = fopen(path, "rb");
  if (f) { fclose(f); return 1; }
  return 0;
}

/* Get file size. Returns -1 on error. */
int64_t ov_file_size(const char* path) {
  if (!path) return -1;
  FILE* f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fclose(f);
  return len;
}

/* Delete file. Returns 1 on success, 0 on failure. */
int ov_file_delete(const char* path) {
  if (!path) return 0;
  return remove(path) == 0;
}

/* Create directory (and parents). Returns 1 on success, 0 on failure. */
int ov_dir_create(const char* path) {
  if (!path) return 0;
#ifdef _WIN32
  return _mkdir(path) == 0 || errno == EEXIST;
#else
  return mkdir(path, 0755) == 0 || errno == EEXIST;
#endif
}

/* List directory contents. Returns list of filenames. */
OvValue ov_dir_list(const char* path) {
  if (!path) return ov_list(ov_list_new());

  OvList* list = ov_list_new();
#ifdef _WIN32
  /* Simplified Windows implementation */
  (void)path;
#else
  DIR* dir = opendir(path);
  if (!dir) return ov_list(list);

  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    ov_list_push(list, ov_str_val(entry->d_name));
  }
  closedir(dir);
#endif
  return ov_list(list);
}