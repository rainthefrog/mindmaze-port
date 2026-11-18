/* A plain ZIP writer (zip_write.c, shared by the Windows and SDL layers) for "Export data for the phone app":
 * entries stored (the pictures and sounds hardly compress), UTF-8 names. The platform walks the data folder. */
#pragma once
#include <stddef.h>
#include <stdio.h>

typedef struct ZipW ZipW;
ZipW *zipw_open(FILE *f);                                                /* takes over f; NULL if out of memory */
void zipw_add(ZipW *z, const char *name, const void *data, size_t n);   /* name relative, '/'-separated */
void zipw_fail(ZipW *z);                                                 /* a file could not be read: the zip fails */
/* writes the central directory and closes the file; 1 = ok. files / bytes receive the totals */
int zipw_close(ZipW *z, int *files, double *bytes);
