#include "mapping.h"

#include "dbf.h"
#include "util.h"

static const size_t kMaxSkuLength = 30;   /* INE.StockItems.Sku nvarchar(30) */
static const size_t kMaxNameLength = 150; /* INE.StockItems.Name nvarchar(150) */
static const size_t kMaxCategoryLength = 50;

bool NormalizeDecimal(const std::string& in, int maxScale, std::string& out)
{
    std::string s = Trim(in);
    if (s.empty()) return false;
    bool neg = false;
    size_t p = 0;
    if (s[p] == '-' || s[p] == '+') {
        neg = s[p] == '-';
        ++p;
    }
    std::string intPart, fracPart;
    bool seenDot = false;
    for (; p < s.size(); ++p) {
        char c = s[p];
        if (c == '.' && !seenDot) seenDot = true;
        else if (c >= '0' && c <= '9') (seenDot ? fracPart : intPart) += c;
        else return false; /* '*' overflow markers, stray text, exponents */
    }
    if (intPart.empty() && fracPart.empty()) return false;
    if (intPart.empty()) intPart = "0";

    bool roundUp = false;
    if ((int)fracPart.size() > maxScale) {
        roundUp = fracPart[maxScale] >= '5'; /* half away from zero: work on the magnitude */
        fracPart.resize(maxScale);
    }
    std::string digits = intPart + fracPart;
    if (roundUp) {
        int i = (int)digits.size() - 1;
        while (i >= 0) {
            if (digits[i] == '9') {
                digits[i] = '0';
                --i;
            } else {
                ++digits[i];
                break;
            }
        }
        if (i < 0) digits.insert(digits.begin(), '1');
    }
    size_t scale = fracPart.size();
    intPart = digits.substr(0, digits.size() - scale);
    fracPart = digits.substr(digits.size() - scale);

    size_t nz = intPart.find_first_not_of('0');
    intPart = nz == std::string::npos ? "0" : intPart.substr(nz);
    size_t lastNz = fracPart.find_last_not_of('0');
    fracPart = lastNz == std::string::npos ? std::string() : fracPart.substr(0, lastNz + 1);

    bool zero = intPart == "0" && fracPart.empty();
    out = (neg && !zero ? "-" : "") + intPart + (fracPart.empty() ? "" : "." + fracPart);
    return true;
}

bool DecimalIsNegative(const std::string& normalized)
{
    return !normalized.empty() && normalized[0] == '-';
}

bool DecimalIsPositive(const std::string& normalized)
{
    return !normalized.empty() && normalized != "0" && !DecimalIsNegative(normalized);
}

static std::string JsonStringOrNull(const std::string& s)
{
    return s.empty() ? std::string("null") : JsonQuote(s);
}

static std::string JsonNumberOrNull(const std::string& s)
{
    return s.empty() ? std::string("null") : s;
}

std::string Item::ToJson() const
{
    return "{\"externalId\":" + JsonQuote(externalId) + ",\"sku\":" + JsonQuote(sku) + ",\"name\":" + JsonQuote(name) +
           ",\"category\":" + JsonStringOrNull(category) + ",\"unitOfMeasure\":" + JsonQuote(unitOfMeasure) +
           ",\"standardCost\":" + JsonNumberOrNull(standardCost) + ",\"listPrice\":" + JsonNumberOrNull(listPrice) + "}";
}

std::string BuildBatchJson(const Config& cfg, const std::vector<Item>& items, size_t begin, size_t end, bool pretty)
{
    std::string nl = pretty ? "\n" : "";
    std::string ind = pretty ? "  " : "";
    std::string sp = pretty ? " " : "";
    std::string body = "{" + nl + ind + "\"source\":" + sp + JsonQuote(cfg.source) + "," + nl + ind + "\"company\":" + sp +
                       JsonQuote(cfg.company) + "," + nl + ind + "\"items\":" + sp + "[" + nl;
    for (size_t i = begin; i < end && i < items.size(); ++i) {
        body += ind + ind + items[i].ToJson();
        if (i + 1 < end && i + 1 < items.size()) body += ",";
        body += nl;
    }
    body += ind + "]" + nl + "}" + nl;
    return body;
}

static bool MapUnit(const std::string& vistaUnit, std::string& out)
{
    std::string u = ToUpperAscii(Trim(vistaUnit));
    if (u == "EA") out = "each";
    else if (u == "FT") out = "ft";
    else return false;
    return true;
}

bool MapParts(const DbfTable& table, const Config& cfg, MapResult& result, std::string& err)
{
    result = MapResult();
    result.liveRecords = 0;
    result.deletedRecords = 0;

    const char* required[] = { "IPPART", "IPDESC", "IPCLAS", "IPIUM", "IPSMUC", "IPAMUC", "IPUPRC" };
    int idx[7];
    for (int i = 0; i < 7; ++i) {
        idx[i] = table.FieldIndex(required[i]);
        if (idx[i] < 0) {
            err = std::string("item table has no ") + required[i] + " column";
            return false;
        }
    }
    const int fPart = idx[0], fDesc = idx[1], fClass = idx[2], fUnit = idx[3], fStdCost = idx[4], fAvgCost = idx[5],
              fPrice = idx[6];

    std::vector<std::string> seenSkus;
    for (unsigned r = 0; r < table.RecordCount(); ++r) {
        if (table.IsDeleted(r)) {
            ++result.deletedRecords;
            continue;
        }
        ++result.liveRecords;

        std::string part = table.Value(r, fPart);
        std::string cls = ToUpperAscii(table.Value(r, fClass));
        SkippedPart skip;
        skip.part = part.empty() ? Format("(record %u)", r + 1) : part;

        if (part.empty()) {
            skip.reason = "blank part number";
            result.skipped.push_back(skip);
            continue;
        }
        std::string sku = ToUpperAscii(part);

        bool skipped = false;
        for (size_t i = 0; i < cfg.skipClasses.size() && !skipped; ++i)
            if (cls == cfg.skipClasses[i]) {
                skip.reason = "class " + cls;
                skipped = true;
            }
        for (size_t i = 0; i < cfg.skipSkuPrefixes.size() && !skipped; ++i)
            if (StartsWith(sku, cfg.skipSkuPrefixes[i])) {
                skip.reason = "SKU prefix " + cfg.skipSkuPrefixes[i];
                skipped = true;
            }
        if (!skipped && sku.size() > kMaxSkuLength) {
            skip.reason = Format("SKU longer than %u characters", (unsigned)kMaxSkuLength);
            skipped = true;
        }
        Item item;
        if (!skipped && !MapUnit(table.Value(r, fUnit), item.unitOfMeasure)) {
            skip.reason = "unit '" + table.Value(r, fUnit) + "' has no Solasta equivalent (only EA and FT)";
            skipped = true;
        }
        if (!skipped) {
            for (size_t i = 0; i < seenSkus.size(); ++i)
                if (seenSkus[i] == sku) {
                    skip.reason = "duplicate SKU";
                    skipped = true;
                    break;
                }
        }
        if (skipped) {
            result.skipped.push_back(skip);
            continue;
        }
        seenSkus.push_back(sku);

        item.externalId = part;
        item.sku = sku;
        item.name = table.Value(r, fDesc);
        if (item.name.empty()) {
            item.name = sku;
            result.warnings.push_back(sku + ": blank description, using the SKU as the name");
        }
        if (item.name.size() > kMaxNameLength) item.name.resize(kMaxNameLength);
        item.category = cls;
        if (item.category.size() > kMaxCategoryLength) item.category.resize(kMaxCategoryLength);

        std::string stdCost, avgCost;
        bool hasStd = NormalizeDecimal(table.Value(r, fStdCost), 4, stdCost);
        bool hasAvg = NormalizeDecimal(table.Value(r, fAvgCost), 4, avgCost);
        if (hasStd && DecimalIsPositive(stdCost)) item.standardCost = stdCost;
        else if (hasAvg) item.standardCost = avgCost;
        if (DecimalIsNegative(item.standardCost)) {
            result.warnings.push_back(sku + ": negative cost " + item.standardCost + " sent as null");
            item.standardCost.clear();
        }

        if (NormalizeDecimal(table.Value(r, fPrice), 4, item.listPrice) && DecimalIsNegative(item.listPrice)) {
            result.warnings.push_back(sku + ": negative price " + item.listPrice + " sent as null");
            item.listPrice.clear();
        }
        result.items.push_back(item);
    }
    return true;
}
