/**
 * GEDCOM 5.5 allowed multimedia to be embedded in a MULTIMEDIA_RECORD
 * as an encoded BLOB, optionally split over several records:
 *
 *      0 @M1@ OBJE
 *      1 FORM bmp
 *      1 TITL A picture
 *      1 BLOB
 *      2 CONT .HM.......k.1..F.jwA.Dzzzzw............A....1.........0U.66..E.8
 *      1 OBJE @M2@
 *      0 @M2@ OBJE
 *      1 BLOB
 *      2 CONT ...
 *
 * When `ged_gedzip` is set, this decodes each such chain into a file
 * that will be placed in the GEDZIP archive and changes the head
 * record to refer to it:
 *
 *      0 @M1@ OBJE
 *      1 FILE media/blob1.bmp
 *      2 FORM image/bmp
 *      2 TITL A picture
 *
 * removing the continuation records entirely. The encoded text is
 * held in memory until `ged_blob_finish` decodes it into the archive.
 *
 * When `ged_gedzip` is not set the BLOB is left in place (and later
 * becomes an `_EXT_BLOB` extension) and a warning is printed.
 *
 * The encoding is documented in Appendix E of GEDCOM 5.5: each group
 * of 4 characters from [./0-9A-Za-z] encodes 3 bytes. Appendix E says
 * the bytes are in big-endian order with 0xFF padding, but the most
 * common real-world encoder (Ancestral Quest) reverses the order of the
 * 3 bytes and pads with 0x00. Both are accepted: the Ancestral Quest
 * order is used if it yields a known file signature, and the Appendix E
 * order otherwise (as in the GEDCOM 5.5 torture test files). Padding
 * bytes cannot be told apart from data, so they are kept.
 *
 * Pass 1 is event-based and finds the chained-to continuation records.
 * Pass 2 should be after `event2record` and before `objes2r`.
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "../strtrie.h"
#include "../gedzip.h"

struct ged_blob_chunk {
    char *text; // encoded BLOB payload, lines separated by '\n'
    char *next; // key of continuation record, or NULL
};

struct ged_blob_object {
    char *path; // name inside the archive
    struct ged_blob_chunk first;
};

struct ged_blob_state {
    trie continued; // pass 1: keys of continuation records (value is non-NULL)
    trie chunks;    // pass 2: key -> struct ged_blob_chunk * for continuations
    struct ged_blob_object *objects;
    size_t count, cap;
    int level;
    char inObje;
    char inChain;
    char warned;
};

/// malloced copy of an xref, case-normalized if requested
static char *ged_blob_key(const char *xref) {
    char *ans = malloc(strlen(xref) + 1);
    strcpy(ans, xref);
    if (ged_xref_case_insensitive)
        for(char *p = ans; *p; p += 1) *p = toupper(*p);
    return ans;
}

void ged_blob1(GedEvent *event, GedEmitterTemplate *emitter, void *rawstate) {
    struct ged_blob_state *state = (struct ged_blob_state *)rawstate;

    if (ged_gedzip) {
        if (event->type == GED_START) {
            state->level += 1;
            if (state->level == 1) state->inObje = !strcmp("OBJE", event->data);
            state->inChain = state->inObje && state->level == 2 && !strcmp("OBJE", event->data);
        } else if (event->type == GED_END) {
            state->level -= 1;
            state->inChain = 0;
        } else if (event->type == GED_POINTER && state->inChain) {
            char *key = ged_blob_key(event->data);
            if (trie_get(&state->continued, key)) free(key);
            else trie_put(&state->continued, key, key);
        }
    }

    emitter->emit(emitter, *event);
}

/// the value of a GEDCOM 5.5 BLOB character, or -1 if not one
static int ged_blob_value(char c) {
    if (c >= '.' && c <= '9') return c - 0x2E;
    if (c >= 'A' && c <= 'Z') return c - 0x35;
    if (c >= 'a' && c <= 'z') return c - 0x3B;
    return -1;
}

/**
 * Decodes the text of a BLOB, in Ancestral Quest byte order if `aq` is
 * set or Appendix E order otherwise. Each line is decoded separately;
 * an incomplete group of 2 or 3 characters at the end of a line (as
 * written by some encoders instead of padding) gives 1 or 2 bytes.
 * Returns a malloced buffer and sets `*length`.
 */
static unsigned char *ged_blob_decode(const char *text, int aq, size_t *length) {
    unsigned char *data = malloc(strlen(text) / 4 * 3 + 3);
    size_t n = 0;
    unsigned long group = 0;
    int have = 0;
    for(;; text += 1) {
        if (*text == '\n' || !*text) {
            if (have >= 2) { // partial group: most significant bits first
                group <<= 6 * (4 - have);
                data[n++] = (group >> 16) & 0xFF;
                if (have == 3) data[n++] = (group >> 8) & 0xFF;
            }
            group = 0;
            have = 0;
            if (!*text) break;
            continue;
        }
        int v = ged_blob_value(*text);
        if (v < 0) continue;
        group = (group << 6) | v;
        if (++have == 4) {
            data[n++] = (aq ? group : group >> 16) & 0xFF;
            data[n++] = (group >> 8) & 0xFF;
            data[n++] = (aq ? group >> 16 : group) & 0xFF;
            group = 0;
            have = 0;
        }
    }
    *length = n;
    return data;
}

/// a file extension if the data begins with a recognized signature
static const char *ged_blob_sniff(const unsigned char *d, size_t n) {
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return "bmp";
    if (n >= 4 && !memcmp(d, "GIF8", 4)) return "gif";
    // FF D8 FF is the same in both byte orders, so also require a marker
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF && d[3] >= 0xC0 && d[3] != 0xFF) return "jpeg";
    if (n >= 4 && (!memcmp(d, "II*\0", 4) || !memcmp(d, "MM\0*", 4))) return "tiff";
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d+8, "WAVE", 4)) return "wav";
    if (n >= 8 && !memcmp(d, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8)) return "ole";
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1A\n", 8)) return "png";
    if (n >= 3 && d[0] == 0x0A && d[1] <= 5 && d[2] == 1) return "pcx";
    // PICT has no signature, but its version opcode follows a 10-byte header
    if (n >= 14 && !memcmp(d+10, "\x00\x11\x02\xFF", 4)) return "pict";
    if (n >= 12 && d[10] == 0x11 && d[11] == 0x01) return "pict";
    return 0;
}

/// JPEG files usually have a JFIF or Exif segment first
static int ged_blob_jfif(const unsigned char *d, size_t n) {
    return n >= 10 && (!memcmp(d+6, "JFIF", 4) || !memcmp(d+6, "Exif", 4));
}

/**
 * 1 if the BLOB text should be decoded in Ancestral Quest order: that
 * order gives a known signature and Appendix E order does not give a
 * more convincing one. If `ext` is not NULL, it is set to the extension
 * of the recognized signature, or NULL if none was recognized.
 */
static int ged_blob_order(const char *text, const char **ext) {
    size_t n;
    unsigned char *d = ged_blob_decode(text, 1, &n);
    const char *found = ged_blob_sniff(d, n);
    int aq = found != 0;
    if (aq && !ged_blob_jfif(d, n)) {
        // a JPEG without JFIF might be Appendix E order with JFIF
        free(d);
        d = ged_blob_decode(text, 0, &n);
        if (ged_blob_sniff(d, n) && ged_blob_jfif(d, n)) aq = 0;
    }
    if (!found) {
        free(d);
        d = ged_blob_decode(text, 0, &n);
        found = ged_blob_sniff(d, n);
    }
    free(d);
    if (ext) *ext = found;
    return aq;
}

/// the 7.0 media type for a 5.5 MULTIMEDIA_FORMAT; see also `mediatype.c`
static char *ged_blob_mediatype(const char *form) {
    const char *known = 0;
    if (!strcmp("bmp", form)) known = "image/bmp";
    else if (!strcmp("gif", form)) known = "image/gif";
    else if (!strcmp("jpeg", form) || !strcmp("jpg", form)) known = "image/jpeg";
    else if (!strcmp("tiff", form) || !strcmp("tif", form)) known = "image/tiff";
    else if (!strcmp("png", form)) known = "image/png";
    else if (!strcmp("ole", form)) known = "application/x-oleobject";
    else if (!strcmp("pcx", form)) known = "image/vnd.zbrush.pcx";
    else if (!strcmp("pict", form) || !strcmp("pct", form)) known = "image/x-pict";
    else if (!strcmp("wav", form)) known = "audio/vnd.wave";

    char *ans;
    if (known) {
        ans = malloc(strlen(known) + 1);
        strcpy(ans, known);
    } else {
        ans = malloc(strlen(form) + 15);
        strcpy(ans, "application/x-");
        strcat(ans, form);
    }
    return ans;
}

/// a new structure with a constant tag and the given payload
static GedStructure *ged_blob_node(const char *tag, char *text) {
    GedStructure *s = calloc(1, sizeof(GedStructure));
    s->tag = (GedEvent){GED_START, 0, .data=(char *)tag};
    s->payload = (GedEvent){GED_TEXT, GED_OWNS_DATA, .data=text};
    return s;
}

/// unlinks and returns the first child with the given tag and payload type
static GedStructure *ged_blob_take(GedStructure *parent, const char *tag, GedEventType payload) {
    for(GedStructure **p = &(parent->child); *p; p = &((*p)->sibling)) {
        if (!strcmp(tag, (*p)->tag.data) && (*p)->payload.type == payload) {
            GedStructure *ans = *p;
            *p = ans->sibling;
            ans->sibling = 0;
            return ans;
        }
    }
    return 0;
}

void ged_blob2(GedEvent *event, GedEmitterTemplate *emitter, void *rawstate) {
    struct ged_blob_state *state = (struct ged_blob_state *)rawstate;

    if (event->type != GED_RECORD || strcmp("OBJE", event->record->tag.data)) {
        emitter->emit(emitter, *event);
        return;
    }
    GedStructure *rec = event->record;

    GedStructure *blob = 0;
    for(GedStructure *s = rec->child; s; s = s->sibling)
        if (!strcmp("BLOB", s->tag.data)) { blob = s; break; }
    if (!blob) {
        emitter->emit(emitter, *event);
        return;
    }
    if (!ged_gedzip) {
        if (!state->warned)
            fprintf(stderr, "WARNING: embedded BLOB multimedia kept as _EXT_BLOB; use --gedzip to extract it\n");
        state->warned = 1;
        emitter->emit(emitter, *event);
        return;
    }

    blob = ged_blob_take(rec, "BLOB", blob->payload.type);
    struct ged_blob_chunk chunk;
    if (blob->payload.type == GED_TEXT && (blob->payload.flags & GED_OWNS_DATA)) {
        chunk.text = blob->payload.data; // take ownership
        blob->payload.flags &= ~GED_OWNS_DATA;
    } else {
        const char *text = blob->payload.type == GED_TEXT ? blob->payload.data : "";
        chunk.text = malloc(strlen(text) + 1);
        strcpy(chunk.text, text);
    }
    ged_destroy_structure(blob);

    GedStructure *chain = ged_blob_take(rec, "OBJE", GED_POINTER);
    chunk.next = chain ? ged_blob_key(chain->payload.data) : 0;
    if (chain) ged_destroy_structure(chain);

    char *key = rec->anchor.type == GED_ANCHOR ? ged_blob_key(rec->anchor.data) : 0;
    if (key && trie_get(&state->continued, key)) {
        // continuation record: keep only its data
        if (trie_get(&state->chunks, key)) { // duplicate id; keep the first
            free(chunk.text);
            if (chunk.next) free(chunk.next);
            free(key);
        } else {
            struct ged_blob_chunk *stored = malloc(sizeof(struct ged_blob_chunk));
            *stored = chunk;
            trie_put(&state->chunks, key, stored);
        }
        ged_destroy_event(event);
        return;
    }
    if (key) free(key);

    // head record: pick a name and media type
    GedStructure *form = ged_blob_take(rec, "FORM", GED_TEXT);
    char *mediatype;
    char ext[9];
    if (form) {
        size_t n = 0;
        for(char *p = form->payload.data; *p && n < 8; p += 1)
            if (isalnum(*p)) ext[n++] = tolower(*p);
        ext[n] = 0;
        if (!n) strcpy(ext, "bin");
        mediatype = ged_blob_mediatype(ext);
        ged_destroy_structure(form);
    } else {
        const char *sniffed;
        ged_blob_order(chunk.text, &sniffed);
        strcpy(ext, sniffed ? sniffed : "bin");
        if (sniffed) mediatype = ged_blob_mediatype(ext);
        else {
            mediatype = malloc(25);
            strcpy(mediatype, "application/octet-stream");
        }
    }

    if (state->count >= state->cap) {
        state->cap = state->cap ? state->cap * 2 : 8;
        state->objects = realloc(state->objects, sizeof(struct ged_blob_object) * state->cap);
    }
    struct ged_blob_object *obj = state->objects + state->count;
    state->count += 1;
    obj->first = chunk;
    obj->path = malloc(strlen(ext) + 32);
    sprintf(obj->path, "media/blob%zu.%s", state->count, ext);

    // 7.0 structure: OBJE.FILE.FORM and OBJE.FILE.TITL
    char *path = malloc(strlen(obj->path) + 1);
    strcpy(path, obj->path);
    GedStructure *file = ged_blob_node("FILE", path);
    file->child = ged_blob_node("FORM", mediatype);
    file->child->sibling = ged_blob_take(rec, "TITL", GED_TEXT);
    file->sibling = rec->child;
    rec->child = file;

    emitter->emit(emitter, *event);
}

int ged_blob_finish(void *rawstate, GedZip *zip) {
    struct ged_blob_state *state = (struct ged_blob_state *)rawstate;
    int failed = 0;

    for(size_t i=0; i<state->count; i+=1) {
        struct ged_blob_object *obj = state->objects + i;
        char *text = obj->first.text;
        size_t length = strlen(text);

        // follow the chain; the step limit guards against cycles
        char *next = obj->first.next;
        for(size_t steps = 0; next && steps <= state->chunks.length; steps += 1) {
            struct ged_blob_chunk *c = trie_get(&state->chunks, next);
            if (!c) {
                fprintf(stderr, "WARNING: %s is missing continuation @%s@\n", obj->path, next);
                break;
            }
            if (!c->text) break; // already used: a cycle or shared tail
            size_t more = strlen(c->text);
            text = realloc(text, length + more + 2);
            text[length] = '\n'; // each record's lines are separate
            memcpy(text + length + 1, c->text, more + 1);
            length += more + 1;
            free(c->text);
            c->text = 0;
            next = c->next;
        }
        obj->first.text = text;

        size_t n;
        unsigned char *data = ged_blob_decode(text, ged_blob_order(text, 0), &n);
        if (gedzip_add_bytes(zip, obj->path, data, n)) failed = 1;
        free(data);
    }
    return failed;
}

void *ged_blob_maker(void) {
    return calloc(1, sizeof(struct ged_blob_state));
}

void ged_blob_freer(void *rawstate) {
    struct ged_blob_state *state = (struct ged_blob_state *)rawstate;

    // continued: key and value are the same malloced string
    for(size_t i=0; i<2*state->continued.length; i+=2)
        free(state->continued.kvpairs[i]);
    trie_free(&state->continued);

    for(size_t i=0; i<2*state->chunks.length; i+=2) {
        struct ged_blob_chunk *c = state->chunks.kvpairs[i+1];
        if (c->text) free(c->text);
        if (c->next) free(c->next);
        free(c);
        free(state->chunks.kvpairs[i]);
    }
    trie_free(&state->chunks);

    for(size_t i=0; i<state->count; i+=1) {
        free(state->objects[i].path);
        free(state->objects[i].first.text);
        if (state->objects[i].first.next) free(state->objects[i].first.next);
    }
    if (state->objects) free(state->objects);

    free(state);
}
