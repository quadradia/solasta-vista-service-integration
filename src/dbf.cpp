#include "dbf.h"

#include <cstring>

#include "util.h"

static unsigned ReadU16(const unsigned char* p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned ReadU32(const unsigned char* p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

DbfTable::DbfTable() : recordCount_(0), headerLength_(0), recordLength_(0), codePage_(0) {}

bool DbfTable::Open(const std::wstring& path, std::string& err)
{
    fields_.clear();
    if (!ReadWholeFile(path, data_)) {
        err = "cannot read " + WideToUtf8(path);
        return false;
    }
    if (data_.size() < 33) {
        err = "file too small to be a DBF table";
        return false;
    }
    const unsigned char* h = (const unsigned char*)data_.data();
    unsigned char version = h[0];
    /* 0x03 dBASE III, 0x83 dBASE III+memo, 0x30/0x31/0x32 Visual FoxPro, 0xF5 FoxPro 2 memo */
    if (version != 0x03 && version != 0x83 && version != 0x30 && version != 0x31 && version != 0x32 &&
        version != 0xF5 && version != 0x8B) {
        err = Format("unsupported DBF version byte 0x%02X", version);
        return false;
    }
    recordCount_ = ReadU32(h + 4);
    headerLength_ = ReadU16(h + 8);
    recordLength_ = ReadU16(h + 10);
    codePage_ = h[29];
    if (headerLength_ < 33 || headerLength_ > data_.size() || recordLength_ < 1) {
        err = "corrupt DBF header";
        return false;
    }

    unsigned offset = 1; /* byte 0 of each record is the deletion flag */
    for (unsigned p = 32; p + 32 <= headerLength_; p += 32) {
        if (h[p] == 0x0D) break;
        DbfField f;
        char name[12];
        memcpy(name, h + p, 11);
        name[11] = '\0';
        f.name = ToUpperAscii(Trim(name));
        f.type = (char)h[p + 11];
        f.length = h[p + 16];
        f.decimals = h[p + 17];
        f.offset = (int)offset;
        offset += (unsigned)f.length;
        fields_.push_back(f);
    }
    if (fields_.empty()) {
        err = "DBF has no fields";
        return false;
    }
    if (offset > recordLength_) {
        err = Format("field lengths (%u) exceed record length (%u)", offset, recordLength_);
        return false;
    }
    /* Trust the data that is actually present if the header count is stale. */
    unsigned available = (unsigned)((data_.size() - headerLength_) / recordLength_);
    if (recordCount_ > available) recordCount_ = available;
    return true;
}

int DbfTable::FieldIndex(const std::string& name) const
{
    std::string up = ToUpperAscii(name);
    for (size_t i = 0; i < fields_.size(); ++i)
        if (fields_[i].name == up) return (int)i;
    return -1;
}

const unsigned char* DbfTable::Record(unsigned i) const
{
    return (const unsigned char*)data_.data() + headerLength_ + (size_t)i * recordLength_;
}

bool DbfTable::IsDeleted(unsigned i) const
{
    return Record(i)[0] == '*';
}

std::string DbfTable::Value(unsigned i, int field) const
{
    if (field < 0 || field >= (int)fields_.size() || i >= recordCount_) return std::string();
    const DbfField& f = fields_[field];
    const unsigned char* p = Record(i) + f.offset;
    switch (f.type) {
    case 'C':
        return Trim(Cp1252ToUtf8((const char*)p, (size_t)f.length));
    case 'N':
    case 'F':
    case 'D': /* YYYYMMDD */
    case 'L': /* T/F/Y/N/? */
        return Trim(std::string((const char*)p, (size_t)f.length));
    case 'I': {
        if (f.length < 4) return std::string();
        int v = (int)ReadU32(p);
        return Format("%d", v);
    }
    case 'Y': { /* currency: int64, 4 implied decimals */
        if (f.length < 8) return std::string();
        long long v = (long long)ReadU32(p) | ((long long)ReadU32(p + 4) << 32);
        bool neg = v < 0;
        unsigned long long a = neg ? (unsigned long long)(-v) : (unsigned long long)v;
        return Format("%s%llu.%04llu", neg ? "-" : "", a / 10000ULL, a % 10000ULL);
    }
    case 'B': {
        if (f.length < 8) return std::string();
        double d;
        memcpy(&d, p, 8);
        return Format("%.*f", f.decimals, d);
    }
    default: /* M, G, W (memo blocks in the .fpt), T (datetime), 0 (_NullFlags) */
        return std::string();
    }
}
