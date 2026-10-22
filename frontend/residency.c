#define _GNU_SOURCE
#include "residency.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#define RESIDENCY_MAX_BYTES (4u * 1024u * 1024u)

bool residency_parse_map(const char *line, residency_map_t *m) {
    if (!line || !m) return false;
    unsigned long long lo, hi, offset, inode;
    unsigned major, minor;
    char perms[5] = {0}, dev[32] = {0}, path[512] = {0};
    int n = sscanf(line, "%llx-%llx %4s %llx %31s %llu %511[^\n]",
                   &lo, &hi, perms, &offset, dev, &inode, path);
    (void) offset; (void) inode; (void) major; (void) minor;
    if (n < 6 || hi <= lo || strlen(perms) < 3) return false;
    m->start = (uintptr_t) lo; m->end = (uintptr_t) hi;
    m->readable = perms[0] == 'r'; m->writable = perms[1] == 'w';
    m->executable = perms[2] == 'x';
    snprintf(m->path, sizeof(m->path), "%s", n == 7 ? path : "");
    while (m->path[0] == ' ') memmove(m->path, m->path + 1, strlen(m->path));
    return true;
}

bool residency_select_code_range(const residency_map_t *m, const char *exe,
                                 uintptr_t marker, uintptr_t etext, size_t page,
                                 uintptr_t *start, uintptr_t *end) {
    if (!m || !exe || !start || !end || !page || (page & (page - 1)) ||
        !m->readable || m->writable || !m->executable ||
        strcmp(m->path, exe) != 0 || marker < m->start || marker >= m->end ||
        etext <= m->start) return false;
    uintptr_t stop = m->end < etext ? m->end : etext;
    stop = (stop + page - 1) & ~(uintptr_t)(page - 1);
    if (stop > m->end) stop = m->end;
    uintptr_t begin = m->start & ~(uintptr_t)(page - 1);
    if (stop <= begin) return false;
    *start = begin; *end = stop;
    return true;
}

static size_t memory_budget(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    size_t budget = 0; /* No memory evidence: skip pinning rather than guess. */
    if (f) {
        char line[256]; unsigned long long kb;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
                size_t available = kb > SIZE_MAX / 1024 ? SIZE_MAX : (size_t) kb * 1024;
                budget = available / 8;
                if (budget > RESIDENCY_MAX_BYTES) budget = RESIDENCY_MAX_BYTES;
                break;
            }
        }
        fclose(f);
    }
    struct rlimit lim;
    if (getrlimit(RLIMIT_MEMLOCK, &lim) == 0 && lim.rlim_cur != RLIM_INFINITY) {
        size_t remaining = lim.rlim_cur > SIZE_MAX ? SIZE_MAX : (size_t) lim.rlim_cur;
        if (budget > remaining) budget = remaining;
    }
    return budget;
}

residency_summary_t residency_pin_code(const void *marker, const void *etext) {
    residency_summary_t s = {0};
    s.budget_bytes = memory_budget();
    if (!marker || !etext || s.budget_bytes == 0) return s;
    char exe[512]; ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len < 0) return s;
    exe[len] = '\0';
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return s;
    long p = sysconf(_SC_PAGESIZE);
    size_t page = p > 0 ? (size_t) p : 4096;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        residency_map_t m; uintptr_t a, b;
        if (!residency_parse_map(line, &m) ||
            !residency_select_code_range(&m, exe, (uintptr_t) marker,
                                         (uintptr_t) etext, page, &a, &b)) continue;
        size_t n = (size_t) (b - a);
        size_t left = s.budget_bytes - s.locked_bytes;
        n = n < left ? n : left;
        n -= n % page;
        if (!n) break;
        if (mlock((void *) a, n) == 0) { s.locked_bytes += n; s.locked_regions++; }
        else s.refused_regions++;
        if (s.locked_bytes >= s.budget_bytes) break;
    }
    fclose(f);
    fprintf(stderr, "memory: pinned %zu KiB of executable text (%u regions; %u refused; budget %zu KiB)\n",
            s.locked_bytes / 1024, s.locked_regions, s.refused_regions, s.budget_bytes / 1024);
    return s;
}
