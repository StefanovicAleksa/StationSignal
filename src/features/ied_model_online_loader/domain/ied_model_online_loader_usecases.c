#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/ied_model_online_loader/domain/ied_model_online_loader_usecases.h"

/* Removes any "(...)" array-index annotation from one dot-separated path
 * segment (e.g. "item(1)component" -> "itemcomponent") - see this function's
 * caller's own doc comment for why this is a deliberate drop, not a
 * preserve-and-forward. */
static char*
stripArrayIndexAnnotation(const char* token) {
    char* result = malloc(strlen(token) + 1);
    if (!result) return NULL;

    char* out = result;
    const char* p = token;
    while (*p) {
        if (*p == '(') {
            while (*p && *p != ')') p++;
            if (*p == ')') p++;
            continue;
        }
        *out++ = *p++;
    }
    *out = '\0';
    return result;
}

char*
IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef(const char* acsiRef) {
    if (!acsiRef) return NULL;

    char* copy = strdup(acsiRef);
    if (!copy) return NULL;

    /* Trailing "[FC]" must be the very last thing in the string. */
    char* openBracket = strrchr(copy, '[');
    char* closeBracket = strrchr(copy, ']');
    if (!openBracket || !closeBracket || closeBracket < openBracket || closeBracket[1] != '\0') {
        free(copy);
        return NULL;
    }
    *closeBracket = '\0';
    const char* fc = openBracket + 1;
    *openBracket = '\0';

    /* First "." after the "LD/LN" prefix marks the start of the DO/SDO/DA
     * chain - LD and LN names never themselves contain a "." (only the
     * FCDA path segments after them can, via nested SDOs). */
    char* dot = strchr(copy, '.');
    if (!dot) {
        free(copy);
        return NULL;
    }
    *dot = '\0';
    const char* ldLn = copy;
    char* chain = dot + 1;

    char* joined = NULL;
    char* tok = strtok(chain, ".");
    while (tok) {
        char* clean = stripArrayIndexAnnotation(tok);
        if (!clean) {
            free(joined);
            free(copy);
            return NULL;
        }

        size_t joinedLen = joined ? strlen(joined) : 0;
        size_t newLen = joinedLen + (joined ? 1 : 0) + strlen(clean) + 1;
        char* next = malloc(newLen);
        if (!next) {
            free(clean);
            free(joined);
            free(copy);
            return NULL;
        }
        if (joined) snprintf(next, newLen, "%s$%s", joined, clean);
        else snprintf(next, newLen, "%s", clean);

        free(clean);
        free(joined);
        joined = next;
        tok = strtok(NULL, ".");
    }

    if (!joined) {
        /* Only an "LD/LN" prefix, nothing after - malformed for this purpose. */
        free(copy);
        return NULL;
    }

    size_t outLen = strlen(ldLn) + 1 + strlen(fc) + 1 + strlen(joined) + 1;
    char* out = malloc(outLen);
    if (out) snprintf(out, outLen, "%s$%s$%s", ldLn, fc, joined);

    free(joined);
    free(copy);
    return out;
}
