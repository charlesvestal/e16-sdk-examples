#ifndef MIRROR_PAGE_H
#define MIRROR_PAGE_H
#include <stddef.h>

/* tools/mirror/index.html, embedded at build time (tools/embed_file.py ->
 * mirror_page.c in the build directory); Disk mode's first boot and every
 * boot after keep a copy at MIRROR_PAGE_PATH (disk_mode.c). */
#define MIRROR_PAGE_PATH "/mirror.html"
extern const char mirror_page[];
extern const size_t mirror_page_len;

#endif
