/**
 * GEDCOM 7.0 requires every tag that does not begin with an underscore
 * to be a standard tag. Any other tag XYZ remaining after the rest of
 * the conversion (such as 5.5's BLOB or 5.5.1's ANCI and DESI) is
 * changed to the extension tag _EXT_XYZ so its data is preserved.
 *
 * Record-level structures are also checked: a level-0 tag that is
 * standard but not a record type (for example a stray 0 NAME) is
 * renamed as well. Tags are not otherwise checked against their
 * superstructure.
 *
 * Should be after every filter that renames tags or creates structures.
 */

#include <stdlib.h>
#include <string.h>

/// all standard GEDCOM 7.0 tags, sorted for bsearch
static const char *const ged_exttags_standard[] = {
    "ABBR", "ADDR", "ADOP", "ADR1", "ADR2", "ADR3", "AGE", "AGNC",
    "ALIA", "ANUL", "ASSO", "AUTH", "BAPL", "BAPM", "BARM", "BASM",
    "BIRT", "BLES", "BURI", "CALN", "CAST", "CAUS", "CENS", "CHAN",
    "CHIL", "CHR", "CHRA", "CITY", "CONF", "CONL", "COPR", "CORP",
    "CREA", "CREM", "CROP", "CTRY", "DATA", "DATE", "DEAT", "DEST",
    "DIV", "DIVF", "DSCR", "EDUC", "EMAIL", "EMIG", "ENDL", "ENGA",
    "EVEN", "EXID", "FACT", "FAM", "FAMC", "FAMS", "FAX", "FCOM",
    "FILE", "FORM", "GEDC", "GIVN", "GRAD", "HEAD", "HEIGHT", "HUSB",
    "IDNO", "IMMI", "INDI", "INIL", "LANG", "LATI", "LEFT", "LONG",
    "MAP", "MARB", "MARC", "MARL", "MARR", "MARS", "MEDI", "MIME",
    "NAME", "NATI", "NATU", "NCHI", "NICK", "NMR", "NO", "NOTE",
    "NPFX", "NSFX", "OBJE", "OCCU", "ORDN", "PAGE", "PEDI", "PHON",
    "PHRASE", "PLAC", "POST", "PROB", "PROP", "PUBL", "QUAY", "REFN",
    "RELI", "REPO", "RESI", "RESN", "RETI", "ROLE", "SCHMA", "SDATE",
    "SEX", "SLGC", "SLGS", "SNOTE", "SOUR", "SPFX", "SSN", "STAE",
    "STAT", "SUBM", "SURN", "TAG", "TEMP", "TEXT", "TIME", "TITL",
    "TOP", "TRAN", "TRLR", "TYPE", "UID", "VERS", "WIDTH", "WIFE",
    "WILL", "WWW",
};

/// all standard GEDCOM 7.0 record tags, sorted for bsearch
static const char *const ged_exttags_records[] = {
    "FAM", "HEAD", "INDI", "OBJE", "REPO", "SNOTE", "SOUR", "SUBM", "TRLR",
};

static int ged_exttags_cmp(const void *key, const void *elem) {
    return strcmp((const char *)key, *(const char *const *)elem);
}

void ged_exttags(GedEvent *event, GedEmitterTemplate *emitter, void *rawstate) {
    long *level = (long *)rawstate;

    if (event->type == GED_START) {
        *level += 1;
        if (event->data[0] != '_' && !(event->flags & GED_TYPE_IS_URI)) {
            int known = (*level == 1)
                ? 0 != bsearch(event->data, ged_exttags_records,
                    sizeof(ged_exttags_records)/sizeof(*ged_exttags_records),
                    sizeof(*ged_exttags_records), ged_exttags_cmp)
                : 0 != bsearch(event->data, ged_exttags_standard,
                    sizeof(ged_exttags_standard)/sizeof(*ged_exttags_standard),
                    sizeof(*ged_exttags_standard), ged_exttags_cmp);
            if (!known) {
                char *tag = malloc(strlen(event->data) + 6);
                strcpy(tag, "_EXT_");
                strcat(tag, event->data);
                changePayloadToDynamic(event, tag);
            }
        }
    } else if (event->type == GED_END) {
        *level -= 1;
    }

    emitter->emit(emitter, *event);
}
