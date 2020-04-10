/*
 * The Shadow Simulator
 * See LICENSE for licensing information
 */

#include "torflow.h"

struct _TorFlowSlice {
    guint sliceID;
    gdouble percentile;
    guint numProbesPerRelay;
    guint totalProbesRemaining;

    gchar* relayIDSearch;
    gboolean relayIDFound;

    GHashTable* entries;
    GHashTable* exits;
    GHashTable* auths;
};

static void _torflowslice_computeMinProbes(gchar* key, gpointer value, guint* minProbes) {
    guint probes = GPOINTER_TO_UINT(value);
    *minProbes = MIN(*minProbes, probes);
}

static GQueue* _torflowslice_getCandidates(TorFlowSlice* slice, GHashTable* table) {
    g_assert(slice);
    g_assert(table);

    /* the strategy here is to choose among the relay that have been measured the least
       number of times when selecting for the next measurement. */

    /* first get the minimum number of probes that we have for any relay */
    guint minProbes = G_MAXUINT;
    g_hash_table_foreach(table, (GHFunc)_torflowslice_computeMinProbes, &minProbes);

    /* now collect all relays that have the same minimum value */
    GQueue* candidates = g_queue_new();

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, table);
    while(g_hash_table_iter_next(&iter, &key, &value)) {
        /* the val is the number of probes, the key is the gchar* relay identity */
        guint numProbes = GPOINTER_TO_UINT(value);
        if(numProbes == minProbes) {
            g_queue_push_tail(candidates, key);
        }
    }

    return candidates;
}

static guint _torflowslice_randomIndex(guint numElements) {
    if(numElements <= 0) {
        return 0;
    }

    /* get the random values */
    gint rInt = rand();
    gdouble rDouble = (gdouble)(((gdouble)rInt) / ((gdouble)RAND_MAX));

    /* the range 0...1 counts for index 0, 1...2 counts for index 1, n...n+1 counts for n
     * this leaves n=numElements as an outlier, which we need to adjust down. */
    gdouble rIndex = rDouble * ((gdouble)(numElements));
    guint randomIndex = (guint)(floor(rIndex));
    return randomIndex >= numElements ? (numElements-1) : randomIndex;
}

static void _torflowslice_countProbesRemaining(gchar* key, gpointer value, TorFlowSlice* slice) {
    g_assert(slice);

    guint numProbes = GPOINTER_TO_UINT(value);
    guint remaining = numProbes >= slice->numProbesPerRelay ? 0 : slice->numProbesPerRelay - numProbes;

    slice->totalProbesRemaining += remaining;
}

TorFlowSlice* torflowslice_new(guint sliceID, gdouble percentile, guint numProbesPerRelay) {
    TorFlowSlice* slice = g_new0(TorFlowSlice, 1);

    slice->sliceID = sliceID;
    slice->percentile = percentile;
    slice->numProbesPerRelay = numProbesPerRelay;

    slice->entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    slice->auths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    slice->exits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    return slice;
}

void torflowslice_free(TorFlowSlice* slice) {
    g_assert(slice);

    if(slice->entries) {
        g_hash_table_destroy(slice->entries);
    }

    if(slice->exits) {
        g_hash_table_destroy(slice->exits);
    }

    if(slice->auths) {
        g_hash_table_destroy(slice->auths);
    }

    if(slice->relayIDSearch) {
        g_free(slice->relayIDSearch);
        slice->relayIDSearch = NULL;
    }

    g_free(slice);
}

void torflowslice_addRelay(TorFlowSlice* slice, TorFlowRelay* relay, gboolean onlyMeasureExits) {
    g_assert(slice);
    g_assert(relay);

    gchar* relayID = g_strdup(torflowrelay_getIdentity(relay));
    guint numProbes = 0;

    if (torflowrelay_getIsAuth(relay)){
        g_hash_table_replace(slice->auths, relayID, GUINT_TO_POINTER(numProbes));
    } else if (torflowrelay_getIsExit(relay)) {
        g_hash_table_replace(slice->exits, relayID, GUINT_TO_POINTER(numProbes));
    } else if (!onlyMeasureExits) {
        g_hash_table_replace(slice->entries, relayID, GUINT_TO_POINTER(numProbes));
    }
}

guint torflowslice_getLength(TorFlowSlice* slice) {
    g_assert(slice);
    return g_hash_table_size(slice->exits) + g_hash_table_size(slice->entries);
}

guint torflowslice_getNumProbesRemaining(TorFlowSlice* slice) {
    g_assert(slice);

    slice->totalProbesRemaining = 0;
    g_hash_table_foreach(slice->entries, (GHFunc)_torflowslice_countProbesRemaining, slice);
    g_hash_table_foreach(slice->exits, (GHFunc)_torflowslice_countProbesRemaining, slice);

    return slice->totalProbesRemaining;
}

gsize torflowslice_getTransferSize(TorFlowSlice* slice) {
    g_assert(slice);

    gsize transferSize = 0;

#ifdef SMALLFILES
    /* Have torflow download smaller files than the real Torflow does.
     * This improves actual running time but should have little effect on
     * simulated timings. */
    if (percentile < 0.25) {
        transferSize = 256*1024; /* 256 KiB */
    } else if (percentile < 0.5) {
        transferSize = 128*1024; /* 128 KiB */
    } else if (percentile < 0.75) {
        transferSize = 64*1024; /* 64 KiB */
    } else {
        transferSize = 32*1024; /* 32 KiB */
    }
#else
    /* This is based not on the spec, but on the file read by TorFlow,
     * NetworkScanners/BwAuthority/data/bwfiles. */
    if (slice->percentile < 0.01) {
        transferSize = 1024*1024*1024; /* 1 GiB*/
    } else if (slice->percentile < 0.07) {
        transferSize = 2*1024*1024; /* 2 MiB */
    } else if (slice->percentile < 0.23) {
        transferSize = 1024*1024; /* 1 MiB */
    } else if (slice->percentile < 0.53) {
        transferSize = 512*1024; /* 512 KiB */
    } else if (slice->percentile < 0.82) {
        transferSize = 256*1024; /* 256 KiB */
    } else if (slice->percentile < 0.95) {
        transferSize = 128*1024; /* 128 KiB */
    } else if (slice->percentile < 0.99) {
        transferSize = 64*1024; /* 64 KiB */
    } else {
        transferSize = 32*1024; /* 32 KiB */
    }
#endif

    return transferSize;
}

static void _torflowslice_mergeTables(GHashTable* dest, GHashTable* one, GHashTable* two) {
    g_assert(dest);
    g_assert(one);
    g_assert(two);

    GHashTable* tables[2] = {one, two};
    GHashTableIter iter;
    gpointer key, value;
    for (int i = 0; i < 2; i++) {
        g_hash_table_iter_init(&iter, tables[i]);
        while(g_hash_table_iter_next(&iter, &key, &value)) {
            g_hash_table_replace(dest, key, value);
        }
    }
}

gboolean torflowslice_chooseRelayPair(TorFlowSlice* slice, gchar** entryRelayIdentity, gchar** exitRelayIdentity) {
    g_assert(slice);

    /* return false if we have already measured all relays */
    guint remaining = torflowslice_getNumProbesRemaining(slice);
    if(remaining <= 0) {
        return FALSE;
    }

    GHashTable* targets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    _torflowslice_mergeTables(targets, slice->entries, slice->exits);

    /* choose an entry and exit among entries and exits with the lowest measurement counts */
    GQueue* candidateTargets = _torflowslice_getCandidates(slice, targets);
    GQueue* candidateAuths = _torflowslice_getCandidates(slice, slice->auths);

    /* choose uniformly from all candidates */
    guint targetPosition = _torflowslice_randomIndex(g_queue_get_length(candidateTargets));
    guint authPosition = _torflowslice_randomIndex(g_queue_get_length(candidateAuths));

    gchar* targetID = g_queue_peek_nth(candidateTargets, targetPosition);
    gchar* authID = g_queue_peek_nth(candidateAuths, authPosition);

    if(targetID == NULL || authID == NULL) {
        error("slice %u: we had candidate exits and entries, but found NULL ids: target=%s auth=%s", slice->sliceID, targetID, authID);

        g_queue_free(candidateTargets);
        g_queue_free(candidateAuths);

        return FALSE;
    }

    /* update the measurement count for the chosen relays */
    gpointer entryProbeCount = g_hash_table_lookup(slice->entries, targetID);
    gpointer exitProbeCount = g_hash_table_lookup(slice->exits, targetID);

    if(entryProbeCount == NULL && authID == NULL) {
        error("slice %u: we had candidate exits and entries, but found NULL ids: target=%s auth=%s", slice->sliceID, targetID, authID);

        g_queue_free(candidateTargets);
        g_queue_free(candidateAuths);

        return FALSE;
    }
    
    gboolean measureEntry = entryProbeCount != NULL ? TRUE : FALSE;

    /* steal the keys so we don't free them */
    if (measureEntry) g_hash_table_steal(slice->entries, targetID);
    else g_hash_table_steal(slice->exits, targetID);

    /* increment the measurement counts */
    guint newTargetCount = measureEntry ? GPOINTER_TO_UINT(entryProbeCount) :
                                          GPOINTER_TO_UINT(exitProbeCount);

    newTargetCount++;

    /* store them in the table again */
    if (measureEntry) g_hash_table_replace(slice->entries, targetID, GUINT_TO_POINTER(newTargetCount));
    else g_hash_table_replace(slice->exits, targetID, GUINT_TO_POINTER(newTargetCount));

    if (measureEntry) {
        info("slice %u: choosing relay pair: found %u candidates of %u targets and %u candidates of %u auths, "
                "choosing entry %s at position %u and auth %s at position %u, "
                "new target probe count is %u",
                slice->sliceID,
                g_queue_get_length(candidateTargets), g_hash_table_size(targets),
                g_queue_get_length(candidateAuths), g_hash_table_size(slice->auths),
                targetID, targetPosition, authID, authPosition, newTargetCount); 
    } else {
        info("slice %u: choosing relay pair: found %u candidates of %u targets and %u candidates of %u auths, "
                "choosing auth %s at position %u and exit %s at position %u, "
                "new target probe count is %u",
                slice->sliceID,
                g_queue_get_length(candidateAuths), g_hash_table_size(slice->auths),
                g_queue_get_length(candidateTargets), g_hash_table_size(targets),
                authID, targetPosition, targetID, authPosition, newTargetCount); 
    }

    /* cleanup the queues */
    g_queue_free(candidateTargets);
    g_queue_free(candidateAuths);
    g_hash_table_destroy(targets);

    /* return values */
    if (measureEntry) {
        if(entryRelayIdentity) {
            *entryRelayIdentity = targetID;
        }
        if(exitRelayIdentity) {
            *exitRelayIdentity = authID;
        }
    } else {
        if(entryRelayIdentity) {
            *entryRelayIdentity = authID;
        }
        if(exitRelayIdentity) {
            *exitRelayIdentity = targetID;
        }
    }
    return TRUE;
}

void torflowslice_logStatus(TorFlowSlice* slice) {
    g_assert(slice);

    guint numEntries = g_hash_table_size(slice->entries);
    guint numExits = g_hash_table_size(slice->exits);
    guint remaining = torflowslice_getNumProbesRemaining(slice);

    info("slice %u: we have %u entries and %u exits, and %u probes remaining",
            slice->sliceID, numEntries, numExits, remaining);
}

static void _torflowslice_FindRelayID(gchar* key, gpointer value, TorFlowSlice* slice) {
    if(key) {
        if(!g_ascii_strcasecmp(key, slice->relayIDSearch)) {
            slice->relayIDFound = TRUE;
        }
    }
}

gboolean torflowslice_contains(TorFlowSlice* slice, const gchar* relayID) {
    g_assert(slice);

    if(!relayID) {
        return FALSE;
    }

    /* check if cache exists */
    if(slice->relayIDSearch) {
        /* cache exists, do we have a cache hit? */
        if(!g_ascii_strcasecmp(relayID, slice->relayIDSearch)) {
            /* cache hit, return cached result */
            return slice->relayIDFound;
        } else {
            /* cache miss, we need to refresh cache */
            g_free(slice->relayIDSearch);
            slice->relayIDSearch = NULL;
            slice->relayIDFound = FALSE;
        }
    }

    g_assert(!slice->relayIDSearch);

    /* do search and cache result */
    slice->relayIDSearch = g_strdup(relayID);
    slice->relayIDFound = FALSE;

    g_hash_table_foreach(slice->entries, (GHFunc)_torflowslice_FindRelayID, slice);

    if(!slice->relayIDFound) {
        g_hash_table_foreach(slice->exits, (GHFunc)_torflowslice_FindRelayID, slice);
    }

    return slice->relayIDFound;
}
