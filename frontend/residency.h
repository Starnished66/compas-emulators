#ifndef FRONTEND_RESIDENCY_H
#define FRONTEND_RESIDENCY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uintptr_t start, end;
    bool readable, writable, executable;
    char path[512];
} residency_map_t;

typedef struct {
    size_t locked_bytes;
    size_t budget_bytes;
    unsigned locked_regions;
    unsigned refused_regions;
} residency_summary_t;

/* Parse one /proc/PID/maps line. Safe for host-side tests. */
bool residency_parse_map(const char *line, residency_map_t *map);
/* Select a page-aligned range only for a read-only executable mapping of the
 * main executable that contains marker, clipped at etext. */
bool residency_select_code_range(const residency_map_t *map, const char *exe_path,
                                 uintptr_t marker, uintptr_t etext, size_t page_size,
                                 uintptr_t *start, uintptr_t *end);
/* Best-effort pinning of this static executable's text. Never pins more than
 * min(4 MiB, available physical memory / 8, remaining RLIMIT_MEMLOCK). */
residency_summary_t residency_pin_code(const void *marker, const void *etext);

#endif
