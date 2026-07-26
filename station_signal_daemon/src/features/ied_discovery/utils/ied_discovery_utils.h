#ifndef IED_DISCOVERY_UTILS_H_
#define IED_DISCOVERY_UTILS_H_

/* Small reusable string helper shared by the data/service layers. */

/* NULL-safe strdup - returns NULL if s is NULL. Caller owns the result (free). */
char*
IedDiscoveryUtils_safeStringDup(const char* s);

#endif /* IED_DISCOVERY_UTILS_H_ */
