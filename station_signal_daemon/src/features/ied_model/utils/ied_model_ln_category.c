#include <ctype.h>
#include <string.h>
#include "features/ied_model/utils/ied_model_ln_category.h"

/*
 * Group-letter -> category table. IEC 61850-7-4 (Ed2) groups every standard
 * LN class under one of sixteen letters: A, C, F, G, I, K, L, M, P, Q, R, S,
 * T, X, Y, Z - that grouping itself is the standard's own; which category
 * BUCKET each letter lands in below is this app's own curated judgment call,
 * not a spec fact, and is deliberately kept to this one small table so it's
 * trivially revisited without touching any call site.
 *
 * A letter absent from this table is not an error - IedModelLnCategory_forGroupLetter
 * returns OTHER for it, which is also where the domain-extension groups this
 * table deliberately doesn't enumerate land (D for DER per IEC 61850-7-420,
 * H for hydro per 7-410, W for wind per IEC 61400-25), along with any vendor
 * class whose first letter isn't a standard group at all.
 */
static const struct {
    char letter;
    LnCategory category;
} kGroupLetterTable[] = {
    { 'C', IED_MODEL_LN_CATEGORY_CONTROL },     /* supervisory control - CSWI, CILO, ... */
    { 'X', IED_MODEL_LN_CATEGORY_CONTROL },     /* switchgear - XCBR, XSWI */
    { 'A', IED_MODEL_LN_CATEGORY_CONTROL },     /* automatic control - ATCC, ... */
    { 'M', IED_MODEL_LN_CATEGORY_MEASUREMENT }, /* metering/measurement - MMXU, MMTR, MSQI, ... */
    { 'T', IED_MODEL_LN_CATEGORY_MEASUREMENT }, /* instrument transformers - TCTR, TVTR */
    { 'Q', IED_MODEL_LN_CATEGORY_MEASUREMENT }, /* power quality (Ed2) - QVVR, QVUB, QITR, QIUB, ...
                                                    measurements of supply quality, so they belong
                                                    with M/T rather than in the OTHER catch-all;
                                                    observed live in a real ABB station file */
    { 'P', IED_MODEL_LN_CATEGORY_PROTECTION },  /* protection - PDIS, PTOC, PDIF, ... */
    { 'R', IED_MODEL_LN_CATEGORY_PROTECTION },  /* protection-related - RREC, RDRE, RSYN, ... */
    { 'F', IED_MODEL_LN_CATEGORY_OTHER },       /* functional blocks (Ed2) - FCNT, FFIL, FLIM, FPID, ...
                                                    building blocks inside a function, not a
                                                    function a technician monitors on its own */
    { 'G', IED_MODEL_LN_CATEGORY_OTHER },       /* generic - GGIO, GAPC, GSAL */
    { 'K', IED_MODEL_LN_CATEGORY_OTHER },       /* mechanical/non-electrical primary equipment (Ed2) -
                                                    KFAN, KFIL, KPMP, KTNK, KVLV - auxiliary plant,
                                                    same posture as Y/Z */
    { 'I', IED_MODEL_LN_CATEGORY_OTHER },       /* interfacing/archiving - IHMI, IARC */
    { 'L', IED_MODEL_LN_CATEGORY_OTHER },       /* system - LLN0, LPHD, ... */
    { 'S', IED_MODEL_LN_CATEGORY_OTHER },       /* sensors/monitoring - SARC, SPDC, ... */
    { 'Y', IED_MODEL_LN_CATEGORY_OTHER },       /* power transformers - YPTR, YLTC, ... */
    { 'Z', IED_MODEL_LN_CATEGORY_OTHER },       /* further equipment - ZBAT, ZGEN, ... */
};
static const int kGroupLetterTableSize = (int) (sizeof(kGroupLetterTable) / sizeof(kGroupLetterTable[0]));

LnCategory
IedModelLnCategory_forGroupLetter(char groupLetter) {
    char upper = (char) toupper((unsigned char) groupLetter);
    for (int i = 0; i < kGroupLetterTableSize; i++) {
        if (kGroupLetterTable[i].letter == upper) return kGroupLetterTable[i].category;
    }
    return IED_MODEL_LN_CATEGORY_OTHER;
}

LnCategory
IedModelLnCategory_forLnClass(const char* lnClass) {
    if (!lnClass || lnClass[0] == '\0') return IED_MODEL_LN_CATEGORY_OTHER;
    return IedModelLnCategory_forGroupLetter(lnClass[0]);
}

/*
 * Standard IEC 61850-7-4 LN class name dictionary, used only by
 * IedModelLnCategory_forWireInstanceName's reverse-match (the no-SCL
 * online-discovery fallback path - see that function's own doc comment).
 * Best-effort, deliberately NOT claimed exhaustive against every edition of
 * the spec: an omitted real class name safely degrades that one LN to
 * IED_MODEL_LN_CATEGORY_OTHER (same as any other unrecognized name) rather
 * than mis-categorizing it, so completeness here is a quality-of-coverage
 * concern, not a correctness one. Each entry's own category is derived from
 * its own first letter via IedModelLnCategory_forGroupLetter, never
 * hand-duplicated, so a typo'd category here is structurally impossible.
 */
static const char* const kStandardLnClassNames[] = {
    /* A - automatic control */
    "ANCR", "ARCO", "ATCC", "AVCO",
    /* C - supervisory control */
    "CALH", "CCGR", "CILO", "CPOW", "CSWI", "CSYN",
    /* G - generic */
    "GAPC", "GGIO", "GSAL",
    /* I - interfacing/archiving */
    "IARC", "IHMI",
    /* L - system */
    "LLN0", "LPHD", "LCCH", "LGOS", "LPLD", "LSVS", "LTIM", "LTMS",
    /* M - metering/measurement */
    "MDIF", "MHAI", "MHAN", "MMTN", "MMTR", "MMXN", "MMXU", "MSQI", "MSTA",
    /* P - protection */
    "PDIF", "PDIR", "PDIS", "PDOP", "PFRC", "PHAR", "PHIZ", "PIOC", "PMRI",
    "PMSS", "POPF", "PPAM", "PPIO", "PRVC", "PSCH", "PSDE", "PTEF", "PTOC",
    "PTOF", "PTOV", "PTRC", "PTTR", "PTUC", "PTUV", "PUPF", "PVOC", "PVPH", "PZSU",
    /* R - protection related */
    "RADR", "RBDR", "RBRF", "RDIR", "RDRE", "RDRS", "RFLO", "RPSB", "RREC", "RSYN",
    /* S - sensors/monitoring */
    "SARC", "SIMG", "SIML", "SPDC",
    /* T - instrument transformers */
    "TCTR", "TVTR",
    /* X - switchgear */
    "XCBR", "XSWI",
    /* Y - power transformers */
    "YEFN", "YLTC", "YPSH", "YPTR",
    /* Z - further equipment */
    "ZAXN", "ZBAT", "ZBSH", "ZCAB", "ZCAP", "ZCON", "ZGEN", "ZGIL", "ZLIN",
    "ZMOT", "ZREA", "ZRES", "ZRRC", "ZSAR", "ZTCF", "ZTCR",
};
static const int kStandardLnClassNameCount =
        (int) (sizeof(kStandardLnClassNames) / sizeof(kStandardLnClassNames[0]));

/* SCL lnClass values (and this codebase's own IedModelUtils_buildLnName
 * output) are always canonical-case per spec - a plain case-sensitive
 * strncmp is correct here and avoids a non-ISO-C string.h dependency. */
static bool
endsWith(const char* str, size_t strLen, const char* suffix, size_t suffixLen) {
    if (suffixLen > strLen) return false;
    return strncmp(str + (strLen - suffixLen), suffix, suffixLen) == 0;
}

LnCategory
IedModelLnCategory_forWireInstanceName(const char* wireName) {
    if (!wireName || wireName[0] == '\0') return IED_MODEL_LN_CATEGORY_OTHER;

    /* Strip the trailing digit run (SCL's "inst" - always numeric, always
     * last, per IedModelUtils_buildLnName's own prefix+lnClass+inst order). */
    size_t len = strlen(wireName);
    size_t candidateLen = len;
    while (candidateLen > 0 && isdigit((unsigned char) wireName[candidateLen - 1])) {
        candidateLen--;
    }
    if (candidateLen == 0) return IED_MODEL_LN_CATEGORY_OTHER;

    /* Suffix-match against the dictionary, longest match wins - an arbitrary
     * vendor prefix (if any) is glued directly in front with no delimiter,
     * so the real class name is always the tail of the candidate string. */
    const char* bestMatch = NULL;
    size_t bestMatchLen = 0;
    for (int i = 0; i < kStandardLnClassNameCount; i++) {
        size_t nameLen = strlen(kStandardLnClassNames[i]);
        if (nameLen <= bestMatchLen) continue; /* can't improve on current best */
        if (endsWith(wireName, candidateLen, kStandardLnClassNames[i], nameLen)) {
            bestMatch = kStandardLnClassNames[i];
            bestMatchLen = nameLen;
        }
    }

    if (bestMatch) return IedModelLnCategory_forLnClass(bestMatch);

    /*
     * Structural fallback, tried only once the dictionary above has missed.
     * IEC 61850-6 fixes an LN class name at EXACTLY four characters
     * (tLNClassEnum, and the same 4-character rule the standard imposes on
     * extension classes), and IedModelUtils_buildLnName's own order is
     * prefix + lnClass + inst - so with the inst digits already stripped,
     * whatever the last four characters are IS the class name, whether or not
     * this file has ever heard of it.
     *
     * Why this is needed and not just a bigger dictionary: across three real
     * station files (two Siemens, one ABB) the dictionary misses 23 distinct
     * classes covering 4177 LN instances - LPDI (2492), LPDO (880), TGSN,
     * USER, SSVS, SBWI, SSYM, SPSQ, SSUM, SFFM, LTRK, SADC, ROPA, RSSR, CBAY,
     * RFUF, PSOF, PTUF, SCBR, PADM, QVVR, RDIF, QVUB - a mix of genuinely
     * standard classes this list simply omits (PTUF), Ed2 additions, and pure
     * vendor extensions. Enumeration loses to the next vendor file by
     * construction; this rule doesn't.
     *
     * Dictionary first, not this first: a dictionary hit is exact, so it
     * still wins wherever both would fire, and it remains the only thing that
     * can correctly classify a name whose real class is NOT four characters
     * (nothing standard, but a malformed input shouldn't be silently
     * re-interpreted). Under four characters left after stripping the inst,
     * there is no class to read and this returns OTHER like any other miss.
     */
    if (candidateLen < 4) return IED_MODEL_LN_CATEGORY_OTHER;
    return IedModelLnCategory_forGroupLetter(wireName[candidateLen - 4]);
}

/* The single spelling both always-include predicates below compare against -
 * kept as one constant so the SCL-side and wire-side checks can never drift
 * apart. */
static const char* const kAlwaysIncludedLnClass = "LLN0";

bool
IedModelLnCategory_isAlwaysIncludedLnClass(const char* lnClass) {
    if (!lnClass) return false;
    return strcmp(lnClass, kAlwaysIncludedLnClass) == 0;
}

bool
IedModelLnCategory_isAlwaysIncludedWireInstanceName(const char* wireName) {
    if (!wireName) return false;
    return strcmp(wireName, kAlwaysIncludedLnClass) == 0;
}

const char*
IedModelLnCategory_toString(LnCategory category) {
    switch (category) {
        case IED_MODEL_LN_CATEGORY_CONTROL: return "CONTROL";
        case IED_MODEL_LN_CATEGORY_MEASUREMENT: return "MEASUREMENT";
        case IED_MODEL_LN_CATEGORY_PROTECTION: return "PROTECTION";
        case IED_MODEL_LN_CATEGORY_OTHER: return "OTHER";
        default: return "UNKNOWN";
    }
}
