/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The imported-level check on Windows (husk-tl-pvz2-level.m on Darwin, which parses the
 * file with Foundation's JSON reader). Windows has no JSON reader in its C library, so
 * this is a lighter test of the same shape: a JSON object, at most 16 MB, holding a
 * LevelDefinition with a StageModule and a Modules list. TODO: a real parse.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int tl_pvz2_level_valid(const char *path, char *err, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, n, "cannot stat level"); return -1; }
    _fseeki64(f, 0, SEEK_END); long long size = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
    if (size > 16 * 1024 * 1024) { fclose(f); snprintf(err, n, "level is too large to validate"); return -1; }
    char *b = malloc((size_t)size + 1);
    size_t got = b ? fread(b, 1, (size_t)size, f) : 0;
    fclose(f);
    if (!b || got != (size_t)size) { free(b); snprintf(err, n, "cannot read level"); return -1; }
    b[got] = 0;
    const char *p = b; while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || (unsigned char)*p == 0xEF || (unsigned char)*p == 0xBB || (unsigned char)*p == 0xBF) p++;
    int ok = *p == '{' && strstr(p, "\"LevelDefinition\"") &&
             ((strstr(p, "\"StageModule\"") && strstr(p, "\"Modules\"")) || strstr(p, "\"RandomLevelSet\""));
    if (!ok) snprintf(err, n, *p == '{' ? "not a level: no LevelDefinition with a StageModule and Modules" : "not a JSON level");
    free(b);
    return ok ? 1 : 0;
}
