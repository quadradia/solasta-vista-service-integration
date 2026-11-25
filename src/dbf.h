#ifndef VISTA_SYNC_DBF_H
#define VISTA_SYNC_DBF_H

#include <string>
#include <vector>

struct DbfField {
    std::string name;  /* upper case, e.g. IPPART */
    char type;         /* C, N, F, I, Y, B, D, T, L, M, ... */
    int length;
    int decimals;
    int offset;        /* byte offset inside a record (after the deletion flag) */
};

/* Read-only reader for dBASE / Visual FoxPro tables (.dbf). The whole file is
 * loaded into memory; Vista tables are small (IPART is ~17 KB for 48 rows). */
class DbfTable {
public:
    DbfTable();

    bool Open(const std::wstring& path, std::string& err);

    const std::vector<DbfField>& Fields() const { return fields_; }
    unsigned RecordCount() const { return recordCount_; }
    int FieldIndex(const std::string& name) const; /* -1 when missing */
    unsigned char CodePageMark() const { return codePage_; }

    /* True when record i is marked deleted ('*'). */
    bool IsDeleted(unsigned i) const;
    /* Field value as UTF-8 text with trailing/leading blanks removed. Character
     * fields are decoded from Windows-1252; numeric fields come back as their
     * decimal text; I/Y/B fields are converted to decimal text; memo fields
     * (M/G/W) return an empty string (the .fpt file is not read). */
    std::string Value(unsigned i, int field) const;

private:
    const unsigned char* Record(unsigned i) const;

    std::string data_;
    std::vector<DbfField> fields_;
    unsigned recordCount_;
    unsigned headerLength_;
    unsigned recordLength_;
    unsigned char codePage_;
};

#endif
