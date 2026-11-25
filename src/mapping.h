#ifndef VISTA_SYNC_MAPPING_H
#define VISTA_SYNC_MAPPING_H

#include <string>
#include <vector>

#include "config.h"

class DbfTable;

/* One Vista part as sent to the Solasta import endpoint. Optional values are
 * empty strings and are sent as JSON null. Decimal values are kept as text so
 * they are never put through binary floating point. */
struct Item {
    std::string externalId;    /* IPPART as stored in Vista (trimmed) */
    std::string sku;           /* IPPART, upper-cased */
    std::string name;          /* IPDESC */
    std::string category;      /* IPCLAS */
    std::string unitOfMeasure; /* each | ft */
    std::string standardCost;  /* IPSMUC when > 0, otherwise IPAMUC */
    std::string listPrice;     /* IPUPRC */

    std::string ToJson() const;
};

struct SkippedPart {
    std::string part;
    std::string reason;
};

struct MapResult {
    unsigned liveRecords;
    unsigned deletedRecords;
    std::vector<Item> items;
    std::vector<SkippedPart> skipped;
    std::vector<std::string> warnings;
};

/* Maps every live IPART record. Returns false (with err) only when the table
 * lacks a required column. */
bool MapParts(const DbfTable& table, const Config& cfg, MapResult& result, std::string& err);

/* Request body: {"source":..,"company":..,"items":[...]}. pretty puts one item per line. */
std::string BuildBatchJson(const Config& cfg, const std::vector<Item>& items, size_t begin, size_t end, bool pretty);

/* Decimal text helpers (exposed for tests).
 * NormalizeDecimal rounds half away from zero to maxScale places and strips
 * trailing zeros: " 12.50000" -> "12.5", "-0.00004" (scale 4) -> "0". */
bool NormalizeDecimal(const std::string& in, int maxScale, std::string& out);
bool DecimalIsPositive(const std::string& normalized);
bool DecimalIsNegative(const std::string& normalized);

#endif
