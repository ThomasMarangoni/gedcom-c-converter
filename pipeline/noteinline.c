/**
 * GEDCOM 5.5.1 had only one kind of note record, which was often used
 * just to hold the text of a single note. 7.0 distinguishes shared
 * notes (SNOTE) from nested NOTE structures. `note2snote` changes every
 * note record into a SNOTE; this filter, which runs before it, instead
 * inlines note records that appear to have been meant as a nested NOTE:
 *
 *      1 NOTE @N1@
 *      0 @N1@ NOTE The text
 *      1 SOUR @S1@
 *
 * becomes
 *
 *      1 NOTE The text
 *      2 SOUR @S1@
 *
 * The heuristic only inlines a note record when that loses nothing:
 *
 * - exactly one pointer anywhere in the file points to it, and that
 *   pointer is the payload of a NOTE substructure;
 * - it has a text payload; and
 * - its only substructures are SOUR citations that contain no NOTE
 *   pointers (so that inlining cannot create cycles or lose any data
 *   like REFN, RIN, or CHAN that a nested NOTE cannot hold).
 *
 * Pass 1 counts pointers and keeps a copy of each candidate record.
 * Pass 2 removes the inlined records and replaces each such pointer
 * with the record's copied payload and substructures. The copy is made
 * from the stream as it is at this filter, so this should be early in
 * the pipeline (after `ged_merge` and `ged_tagcase`) for the inserted
 * events to receive every later conversion. It must be before
 * `note2snote` and `fixid`.
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "../strtrie.h"

struct ged_noteinline_note {
    GedEvent *events; // payload and substructures, not START/ANCHOR/END
    size_t length, cap;
};

struct ged_noteinline_refs {
    long total; // pointers of any kind to this xref
    long notes; // of those, how many are payloads of NOTE substructures
    long defs;  // note records with this xref
    int inlined; // set in pass 2 if the record is being inlined
};

struct ged_noteinline_state {
    trie refs;  // key -> struct ged_noteinline_refs *
    trie notes; // key -> struct ged_noteinline_note *, or NULL if not inlinable

    // pass 1
    struct ged_noteinline_note *building; // copy of current candidate
    char *buildingKey;
    int candidate; // current record is still a candidate

    // both passes
    int level;
    int inNote;    // current structure is NOTE
    int finalized; // pass 2 has begun

    // pass 2
    GedEvent held; // a NOTE start, held until the next event is seen
    int skip;      // inside a record being removed
};

/// malloced copy of an xref, case-normalized if requested
static char *ged_noteinline_key(const char *xref) {
    char *ans = malloc(strlen(xref) + 1);
    strcpy(ans, xref);
    if (ged_xref_case_insensitive)
        for(char *p = ans; *p; p += 1) *p = toupper(*p);
    return ans;
}

static void ged_noteinline_note_free(struct ged_noteinline_note *n) {
    for(size_t i=0; i<n->length; i+=1) ged_destroy_event(n->events + i);
    if (n->events) free(n->events);
    free(n);
}

/// appends a copy of `e` that owns its own data
static void ged_noteinline_copy(struct ged_noteinline_note *n, const GedEvent *e) {
    if (n->length >= n->cap) {
        n->cap = n->cap ? n->cap * 2 : 8;
        n->events = realloc(n->events, sizeof(GedEvent) * n->cap);
    }
    GedEvent *c = n->events + n->length;
    n->length += 1;
    *c = *e;
    if (e->data) {
        c->data = malloc(strlen(e->data) + 1);
        strcpy(c->data, e->data);
        c->flags |= GED_OWNS_DATA;
    } else {
        c->flags &= ~GED_OWNS_DATA;
    }
}

/// the reference counts for a key, created if needed
static struct ged_noteinline_refs *ged_noteinline_refs_for(struct ged_noteinline_state *state, const char *key) {
    struct ged_noteinline_refs *r = trie_get(&state->refs, key);
    if (!r) {
        char *copy = malloc(strlen(key) + 1);
        strcpy(copy, key);
        r = calloc(1, sizeof(struct ged_noteinline_refs));
        trie_put(&state->refs, copy, r);
    }
    return r;
}

/// ends the current pass 1 note record, keeping it if a candidate
static void ged_noteinline_close(struct ged_noteinline_state *state) {
    if (!state->building) return;
    if (state->buildingKey) {
        ged_noteinline_refs_for(state, state->buildingKey)->defs += 1;
        if (state->candidate && state->building->length
        && state->building->events[0].type == GED_TEXT
        && !trie_get(&state->notes, state->buildingKey)) {
            trie_put(&state->notes, state->buildingKey, state->building);
            state->buildingKey = 0;
            state->building = 0;
        }
    }
    if (state->building) ged_noteinline_note_free(state->building);
    if (state->buildingKey) free(state->buildingKey);
    state->building = 0;
    state->buildingKey = 0;
}

void ged_noteinline1(GedEvent *event, GedEmitterTemplate *emitter, void *rawstate) {
    struct ged_noteinline_state *state = (struct ged_noteinline_state *)rawstate;

    switch(event->type) {
        case GED_START: {
            state->level += 1;
            state->inNote = !strcmp("NOTE", event->data);
            if (state->level == 1) {
                if (state->inNote) {
                    state->building = calloc(1, sizeof(struct ged_noteinline_note));
                    state->candidate = 1;
                }
            } else if (state->building) {
                if (state->level == 2 && strcmp("SOUR", event->data))
                    state->candidate = 0;
                ged_noteinline_copy(state->building, event);
            }
        } break;
        case GED_END: {
            state->level -= 1;
            state->inNote = 0;
            if (state->level == 0) ged_noteinline_close(state);
            else if (state->building) ged_noteinline_copy(state->building, event);
        } break;
        case GED_ANCHOR: {
            if (state->building && state->level == 1) {
                if (state->buildingKey) state->candidate = 0;
                else state->buildingKey = ged_noteinline_key(event->data);
            } else if (state->building) {
                ged_noteinline_copy(state->building, event);
            }
        } break;
        case GED_POINTER: {
            if (state->level > 1) {
                char *key = ged_noteinline_key(event->data);
                struct ged_noteinline_refs *r = ged_noteinline_refs_for(state, key);
                free(key);
                r->total += 1;
                if (state->inNote) r->notes += 1;
            }
            if (state->building) {
                if (state->level == 1 || state->inNote) state->candidate = 0;
                else ged_noteinline_copy(state->building, event);
            }
        } break;
        case GED_TEXT: case GED_LINEBREAK: {
            if (state->building) ged_noteinline_copy(state->building, event);
        } break;
        default: break;
    }

    emitter->emit(emitter, *event);
}

/// discards stored notes that do not meet the reference requirement
static void ged_noteinline_finalize(struct ged_noteinline_state *state) {
    ged_noteinline_close(state); // in case of truncated input
    for(size_t i=0; i<2*state->notes.length; i+=2) {
        struct ged_noteinline_note *n = state->notes.kvpairs[i+1];
        struct ged_noteinline_refs *r = trie_get(&state->refs, state->notes.kvpairs[i]);
        if (n && !(r && r->total == 1 && r->notes == 1 && r->defs == 1)) {
            ged_noteinline_note_free(n);
            trie_put(&state->notes, state->notes.kvpairs[i], 0);
        } else if (n) {
            r->inlined = 1;
        }
    }
    state->level = 0;
    state->inNote = 0;
    state->finalized = 1;
}

/// 1 if the note record with this xref is being inlined, else 0
static int ged_noteinline_isinlined(struct ged_noteinline_state *state, const char *xref) {
    char *key = ged_noteinline_key(xref);
    struct ged_noteinline_refs *r = trie_get(&state->refs, key);
    free(key);
    return r && r->inlined;
}

void ged_noteinline2(GedEvent *event, GedEmitterTemplate *emitter, void *rawstate) {
    struct ged_noteinline_state *state = (struct ged_noteinline_state *)rawstate;

    if (!state->finalized) ged_noteinline_finalize(state);

    if (state->skip) { // removing an inlined record
        if (event->type == GED_START) state->level += 1;
        if (event->type == GED_END) state->level -= 1;
        if (state->level == 0) state->skip = 0;
        ged_destroy_event(event);
        return;
    }

    if (state->held.type == GED_START) {
        GedEvent held = state->held;
        state->held = (GedEvent){GED_UNUSED, 0, .data=0};

        if (state->level == 1 && event->type == GED_ANCHOR
        && ged_noteinline_isinlined(state, event->data)) {
            // the inlined record itself; drop all of it
            ged_destroy_event(&held);
            ged_destroy_event(event);
            state->skip = 1;
            return;
        }

        emitter->emit(emitter, held);

        if (state->level > 1 && event->type == GED_POINTER) {
            char *key = ged_noteinline_key(event->data);
            struct ged_noteinline_note *n = trie_get(&state->notes, key);
            if (n) {
                // n's events are emitted as-is, transferring ownership
                for(size_t i=0; i<n->length; i+=1)
                    emitter->emit(emitter, n->events[i]);
                n->length = 0;
                ged_noteinline_note_free(n);
                trie_put(&state->notes, key, 0);
                free(key);
                ged_destroy_event(event);
                return;
            }
            free(key);
        }
    }

    if (event->type == GED_START) {
        state->level += 1;
        if (!strcmp("NOTE", event->data)) {
            state->held = *event;
            return;
        }
    } else if (event->type == GED_END) {
        state->level -= 1;
    }

    emitter->emit(emitter, *event);
}

void *ged_noteinline_maker(void) {
    return calloc(1, sizeof(struct ged_noteinline_state));
}

void ged_noteinline_freer(void *rawstate) {
    struct ged_noteinline_state *state = (struct ged_noteinline_state *)rawstate;

    for(size_t i=0; i<2*state->refs.length; i+=2) {
        free(state->refs.kvpairs[i]);
        free(state->refs.kvpairs[i+1]);
    }
    trie_free(&state->refs);

    for(size_t i=0; i<2*state->notes.length; i+=2) {
        free(state->notes.kvpairs[i]);
        if (state->notes.kvpairs[i+1]) ged_noteinline_note_free(state->notes.kvpairs[i+1]);
    }
    trie_free(&state->notes);

    if (state->building) ged_noteinline_note_free(state->building);
    if (state->buildingKey) free(state->buildingKey);
    ged_destroy_event(&state->held);

    free(state);
}
