/**
 * A minimal ZIP archive writer, sufficient for creating GEDZIP files.
 *
 * Only the "stored" (uncompressed) method is supported, which keeps
 * this zero-dependency. Entries are written sequentially, so the
 * output may be a non-seekable stream like stdout. ZIP64 is not
 * supported, so the archive must be smaller than 4GB.
 *
 * GedZip *z = gedzip_create(out);
 * gedzip_add_stream(z, "gedcom.ged", gedfile);
 * gedzip_add_bytes(z, "media/1.jpg", bytes, length);
 * gedzip_finish(z); // writes the central directory and frees z
 */

#pragma once

#include <stdio.h>  // FILE
#include <stddef.h> // size_t

typedef struct GedZip_t GedZip;

/// creates a new archive that will be written to `out`
GedZip *gedzip_create(FILE *out);

/// adds a file to the archive with the given contents; returns 0 on success
int gedzip_add_bytes(GedZip *z, const char *name, const unsigned char *data, size_t length);

/// adds a file to the archive with the contents of a seekable stream,
/// read from its beginning; returns 0 on success
int gedzip_add_stream(GedZip *z, const char *name, FILE *src);

/// writes the central directory and frees `z`; returns 0 on success
int gedzip_finish(GedZip *z);
