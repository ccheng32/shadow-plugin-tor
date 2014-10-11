/*ni
 * See LICENSE for licensing information
 */

#include "torflow.h"

struct _TorFlowAggregator {
	ShadowLogFunc slogf;
	gint numWorkers;
	gboolean gotInitial;
	GString* filename;
	GHashTable* relayStats;
	gdouble nodeCap;
	gint version;
};

typedef struct _TorFlowRelayStats {
	GString * nickname;
	GString * identity;
	gint descriptorBandwidth;
	gint advertisedBandwidth;
	gint newBandwidth;
	gint meanBandwidth;
	gint filteredBandwidth;
} TorFlowRelayStats;

static void _torflowaggregator_torFlowRelayStatsFree(gpointer toFree) {
	TorFlowRelayStats* tfrs = (TorFlowRelayStats*)toFree;
	g_assert(tfrs);
	
	g_string_free(tfrs->nickname, TRUE);
	g_string_free(tfrs->identity, TRUE);
	g_free(tfrs);
}

static void _torflowaggregator_printToFile(TorFlowAggregator* tfa) {

	// loop through measured nodes and aggregate stats
	gint totalMeanBW = 0;
	gint totalFiltBW = 0;
	gint measuredNodes = 0;

	GHashTableIter iter;
	gpointer key;
	gpointer value;
	g_hash_table_iter_init(&iter, tfa->relayStats);
	while(g_hash_table_iter_next(&iter, &key, &value)) {
		TorFlowRelayStats* current = (TorFlowRelayStats*)value;
		totalMeanBW += current->meanBandwidth;
		totalFiltBW += current->filteredBandwidth;
	}

	// calculate averages
	gdouble avgMeanBW = (gdouble)totalMeanBW/g_hash_table_size(tfa->relayStats);
	gdouble avgFiltBW = (gdouble)totalFiltBW/g_hash_table_size(tfa->relayStats);
	gint totalBW = 0;

	//loop through nodes and calculate new bandwidths
	g_hash_table_iter_init(&iter, tfa->relayStats);
	while(g_hash_table_iter_next(&iter, &key, &value)) {
		TorFlowRelayStats* current = (TorFlowRelayStats*)value;
		//use the better of the mean and filtered ratios, because that's what torflow does
		current->newBandwidth = (gint)(current->advertisedBandwidth * fmax(current->meanBandwidth/avgMeanBW, current->filteredBandwidth/avgFiltBW));
		totalBW += current->newBandwidth;
	}

	//create new file to print to, and increment version
	GString* newFilename = g_string_new(tfa->filename->str);
	g_string_append_printf(newFilename, ".%d", tfa->version++);
	struct timespec now_ts;
	clock_gettime(CLOCK_REALTIME, &now_ts);
	FILE * fp;
	fp = fopen(newFilename->str, "w");
	fprintf(fp, "%li\n", now_ts.tv_sec);



	//loop through nodes and cap bandwidths that are too large, then print to file
	g_hash_table_iter_init(&iter, tfa->relayStats);
	while(g_hash_table_iter_next(&iter, &key, &value)) {
		TorFlowRelayStats* current = (TorFlowRelayStats*)value;
		if (current->newBandwidth > (gint)(totalBW * tfa->nodeCap)){
			tfa->slogf(SHADOW_LOG_LEVEL_MESSAGE, __FUNCTION__,
					"Capping bandwidth for extremely fast relay %s\n",
					current->nickname->str);
			current->newBandwidth = (gint)(totalBW * tfa->nodeCap);
		}

		fprintf(fp, "node_id=$%s bw=%i nick=%s\n",
				current->identity->str,
				current->newBandwidth,
				current->nickname->str);
	}
	fclose(fp);
	if(!unlink(tfa->filename->str)) {
		tfa->slogf(SHADOW_LOG_LEVEL_WARNING, __FUNCTION__,
				"Unable to remove symlink to %s!\n",
				tfa->filename->str);
	}
	if(!symlink(newFilename->str, tfa->filename->str)) {
		tfa->slogf(SHADOW_LOG_LEVEL_WARNING, __FUNCTION__,
				"Unable to create symlink from %s to %s!\n",
				newFilename->str, tfa->filename->str);
	}
	g_string_free(newFilename, TRUE);
}

void torflowaggregator_reportInitial(TorFlowAggregator* tfa, GSList* relays) {
	g_assert(tfa);

	//We only need to populate with initial data once.
	if(tfa->gotInitial) {
		return;
	} else {
		tfa->gotInitial = TRUE;
	}
	
	//add all relays that the worker measured to our stats list
	for(GSList* currentNode = relays; currentNode; currentNode = g_slist_next(currentNode)) {
		TorFlowRelay* current = currentNode->data;
		TorFlowRelayStats* tfrs = g_new0(TorFlowRelayStats, 1);
		tfrs->nickname = g_string_new(current->nickname->str);
		tfrs->identity = g_string_new(current->identity->str);
		tfrs->descriptorBandwidth = current->descriptorBandwidth;
		tfrs->advertisedBandwidth = current->advertisedBandwidth;
		tfrs->meanBandwidth = current->descriptorBandwidth;
		tfrs->filteredBandwidth = current->descriptorBandwidth;
		g_hash_table_insert(tfa->relayStats, tfrs->identity->str, tfrs);
	}
}

void torflowaggregator_reportMeasurements(TorFlowAggregator* tfa, GSList* measuredRelays, gint sliceSize, gint currSlice) {
	g_assert(tfa);
	
	//add all relays that the worker measured to our stats list
	GSList* currentNode = g_slist_nth(measuredRelays, sliceSize * currSlice);
	gint i = 0;
	while (currentNode && i < sliceSize) {
		TorFlowRelay* current = currentNode->data;
		if (current->measureCount >= MEASUREMENTS_PER_SLICE) {
			TorFlowRelayStats* tfrs = g_new0(TorFlowRelayStats, 1);
			tfrs->nickname = g_string_new(current->nickname->str);
			tfrs->identity = g_string_new(current->identity->str);
			tfrs->descriptorBandwidth = current->descriptorBandwidth;
			tfrs->advertisedBandwidth = current->advertisedBandwidth;
			tfrs->meanBandwidth = torflowutil_meanBandwidth(current);
			tfrs->filteredBandwidth = torflowutil_filteredBandwidth(current, tfrs->meanBandwidth);
			g_hash_table_insert(tfa->relayStats, tfrs->identity->str, tfrs);
		}
		currentNode = g_slist_next(currentNode);
		i++;
	}

	//print results to file	
	_torflowaggregator_printToFile(tfa);
}

void torflowaggregator_free(TorFlowAggregator* tfa) {
	g_assert(tfa);

	g_hash_table_destroy(tfa->relayStats);

	g_string_free(tfa->filename, TRUE);
	g_free(tfa);
}

TorFlowAggregator* torflowaggregator_new(ShadowLogFunc slogf,
		gchar* filename, gint numWorkers, gdouble nodeCap) {

	TorFlowAggregator* tfa = g_new0(TorFlowAggregator, 1);
	tfa = g_new0(TorFlowAggregator, 1);
	tfa->slogf = slogf;
	tfa->numWorkers = numWorkers;
	tfa->filename = g_string_new(filename);
	tfa->nodeCap = nodeCap;
	tfa->version = 0;
	tfa->relayStats = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, _torflowaggregator_torFlowRelayStatsFree);
	tfa->gotInitial = FALSE;

	return tfa;
}