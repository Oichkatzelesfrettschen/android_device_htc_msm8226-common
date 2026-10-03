#ifndef XTRA_VALIDITY_QUERY_H
#define XTRA_VALIDITY_QUERY_H

#include <stdint.h>

struct XtraValidity {
    bool answered;
    bool known;
    int clientStatus;
    int modemStatus;
    uint64_t startUtc;
    uint16_t durationHours;
};

XtraValidity queryXtraValidity();

// True when the window the modem holds covers nowUtc; with
// XTRA_VALIDITY_ACCEPT_WEEK_ERA_ALIAS, also when it covers nowUtc a whole
// number of 1024-week GPS eras later.
bool xtraValidityCurrent(const XtraValidity& validity, uint64_t nowUtc);

#endif
