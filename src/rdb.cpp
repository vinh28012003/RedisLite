#include "rdb.hpp"
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace {

// ─── CRC64 (Jones polynomial, same as Redis) ───────────────────────
// Polynomial: 0xad93d23594c935a9 (reflected)
// Produces 8-byte checksum appended to every RDB file.

static const uint64_t crc64_table[256] = {
    0x0000000000000000ULL, 0x7ad870c830358979ULL, 0xf5b0e190606b12f2ULL, 0x8f689158505e9b8bULL,
    0xc038e5739841b68fULL, 0xbae095bba8743ff6ULL, 0x358804e3f82aa47dULL, 0x4f50742bc5870b04ULL,
    0xab28ecb46814fe75ULL, 0xd1f09c7c5821770eULL, 0x5e980d24087fec85ULL, 0x24407decb04cfdfcULL,
    0x6b1009c7f05548f8ULL, 0x11c8790fc060c183ULL, 0x9ea0e857903e5a08ULL, 0xe4789c8d00d62171ULL,
    0x7d08ff3b88be6f81ULL, 0x07d08ff3b88be6f8ULL, 0x88b81eabef8c9a73ULL, 0xf2606e63d8e0f40aULL,
    0xbd301a4810ffd90eULL, 0xc7e86a8020ca5077ULL, 0x4880fbd87094cbfcULL, 0x32588b1040a14285ULL,
    0xd620138fe0aa91f4ULL, 0xacf86347d09f188dULL, 0x2390f21f80c18306ULL, 0x594882d7b0f40a7fULL,
    0x1618f6fc78eb277bULL, 0x6cc0863448deae02ULL, 0xe3a8176c18803589ULL, 0x997067a428b5bcf0ULL,
    0xfa11fe77117cdf02ULL, 0x80c98ebf2149567bULL, 0x0fa11fe77117cdf0ULL, 0x75796f2f41224489ULL,
    0x3a291b04893d698dULL, 0x40f16bccb908e0f4ULL, 0xcf99fa94e9567b7fULL, 0xb5418a5cd963f206ULL,
    0x513912c379682177ULL, 0x2be1620b495da80eULL, 0xa489f35319033385ULL, 0xde51839b2936bafcULL,
    0x9101f7b0e12997f8ULL, 0xebd98778d11c1e81ULL, 0x64b116208142850aULL, 0x1e6966e8955e4873ULL,
    0x8719014c99c2b083ULL, 0xfdc17184a9f739faULL, 0x72a9e0dcf9a9a271ULL, 0x08719014c99c2b08ULL,
    0x4721e43f0183060cULL, 0x3df994f731b68f75ULL, 0xb29105af61e814feULL, 0xc849756751dd9d87ULL,
    0x2c31edf8f1d64ef6ULL, 0x56e99d30c1e3c78fULL, 0xd9810c6891bd5c04ULL, 0xa3597ca0a188d57dULL,
    0xec09088b6997f879ULL, 0x96d1784359a27100ULL, 0x19b9e91b09fcea8bULL, 0x636199d339c963f2ULL,
    0xdf7adabd7a6e2d6fULL, 0xa5a2aa754a5ba416ULL, 0x2aca3b2d1a053f9dULL, 0x50124be52a30b6e4ULL,
    0x1f423fcee22f9be0ULL, 0x65ba4f4eda9a1e99ULL, 0xeaD2de5da9bf8612ULL, 0x900aae6dc5bef06bULL,
    0x74523609127ad31aULL, 0x0e8a46c1224f5a63ULL, 0x81e2d7997211c1e8ULL, 0xfb3aa75142244891ULL,
    0xb46ad37a8a3b6595ULL, 0xceb2a3b2ba0eececULL, 0x41da32eaea507767ULL, 0x3b024222da65fe1eULL,
    0xa2722586f2d042eeULL, 0xd8aa554ec2e5cb97ULL, 0x57c2c41692bb501cULL, 0x2d1ab4dea28ed965ULL,
    0x624ac0f56a91f461ULL, 0x1892b03d5aa47d18ULL, 0x97fa21650afae693ULL, 0xed2251ad3acf6feaULL,
    0x095ac9329ac4bc9bULL, 0x7382b9faaaf135e2ULL, 0xfcea28a2faafae69ULL, 0x8632586aca9a2710ULL,
    0xc9622c4102850a14ULL, 0xb3ba5c8932b0836dULL, 0x3cd2cdd162ee18e6ULL, 0x4660bd3473dc479fULL,
    0x7930d90f22b64e36ULL, 0x03e8a94d52b0094fULL, 0x8c80381590b15cc4ULL, 0xf65848d8a0b727bdULL,
    0xb9083cf027e22db9ULL, 0xc3d04c3017d6a4c0ULL, 0x4cb85cd47bff2c4bULL, 0x36602c3c4b8fc532ULL,
    0xd218b4e474ba1643ULL, 0xa8c0c48265252e3aULL, 0x27a855db152b85b1ULL, 0x5d7025a1e4b896c8ULL,
    0x122051b14a0a4cccULL, 0x68f821737a174eb5ULL, 0xe790b07c53e3583eULL, 0x9d48c01048d66547ULL,
    0x042990b37a56e6b7ULL, 0x7ef1e09482b8d0ceULL, 0xf19971ccd2569545ULL, 0x8b41014cf18e1c3cULL,
    0xc4115530b78fc138ULL, 0xbeC92571c72e4841ULL, 0x31a1b4ccab16d3caULL, 0x4b79c4049e72dab3ULL,
    0xaf015c8b7b9d47c2ULL, 0xd5d92c438ed466bbULL, 0x5ab1bd1b9ff4dd30ULL, 0x2069cd0d5ee58549ULL,
    0x6f39b9d4c406a84dULL, 0x15e1c94c1236f134ULL, 0x9a89581442966abfULL, 0xe05128267293e3c6ULL,
    0xbfc70d848af58397ULL, 0xc51f7d4cbf010aeeULL, 0x4a77ec105e87f165ULL, 0x30af9c30deb2781cULL,
    0x7fffc821f2a05718ULL, 0x0527b8193e23de61ULL, 0x8a4f296b6e58a7eaULL, 0xf09759b3c8760693ULL,
    0x14efc18abcf0d5e2ULL, 0x6e37b1af56ea569bULL, 0xe15f20f706a67d10ULL, 0x9b87508c36ba7469ULL,
    0xd4d7249d4a88846dULL, 0xae0f54eb78ade914ULL, 0x2167c5b3286e729fULL, 0x5bbfb5a92c9e7ae6ULL,
    0xe2989b10c0e4bc16ULL, 0x9840eb28d0d1b56fULL, 0x17287a8034db8ee4ULL, 0x6df00af804c6e79dULL,
    0x22a07ec3c9f8ca99ULL, 0x58780ee3c9cd43e0ULL, 0xd7109f9b7fce0b6bULL, 0xade2efb334efc212ULL,
    0x49ba77c9c8dc3963ULL, 0x336207f8e8e3b01aULL, 0xbc0a96a09bb87b91ULL, 0xc6d2e6803b99e2e8ULL,
    0x89829291cba2cfecULL, 0xf35ae259fb81a695ULL, 0x7c32733ba31d3d1eULL, 0x06ea030b93326d67ULL,
    0x9f9a1643008bec97ULL, 0xe542667a60bbe5eeULL, 0x6a2af73a30f05265ULL, 0x10f287020a04db1cULL,
    0x5fa2f37e07b6db18ULL, 0x2570839b50a95261ULL, 0xaa1812e328e4c9eaULL, 0xd0c062d318d14093ULL,
    0x34b8fa169c7b93e2ULL, 0x4e608a019e4ee49bULL, 0xc1081bfc911e0710ULL, 0xbbD06b379ce67e69ULL,
    0xf4801f4cd84a476dULL, 0x8e586f24e858ce14ULL, 0x0130fe7c7c698d9fULL, 0x7be88eb43c1a78e6ULL,
    0x1cb9285deea7c957ULL, 0x6661584c1ec2d02eULL, 0xe909c914a49b19a5ULL, 0x93d1b93c25c8e4dcULL,
    0xdc81cd151a047fd8ULL, 0xa659bd735b5e36a1ULL, 0x29312c8b1bce832aULL, 0x53e95c3b2bf26c53ULL,
    0xb791c4bae391e522ULL, 0xcd49b4b4d39f6c5bULL, 0x422125ac8364a7d0ULL, 0x38f955f36e1c49a9ULL,
    0x77a921164736ceadULL, 0x0D71513c274b45d4ULL, 0x8219c064a38dc65fULL, 0xf8c1b0ecb5a1cf26ULL,
    0x6151b1746f93cfd6ULL, 0x1b89c18e5f91c6afULL, 0x94e150d613c97d24ULL, 0xee3920ae83856e5dULL,
    0xa16954c7ac310459ULL, 0xdbb124bfa38a8d20ULL, 0x54d9b5b3ea2f56abULL, 0x2e01c5b2c29b8dd2ULL,
    0xca795d6d68fdf0a3ULL, 0xb0a12d5bd8deb9daULL, 0x3fc9bc5448a19c51ULL, 0x4511cc8c780a6228ULL,
    0x0a41b80f6e77e22cULL, 0x7099c8c7e648cb55ULL, 0xffF159bfb6211ddeULL, 0x852929d732ae10a7ULL,
    0x9321cfc7b24e8b98ULL, 0xe9f9bf2ca25584e1ULL, 0x66912e7a3af91f6aULL, 0x1c495e1a5a8f5613ULL,
    0x53192a11a1507b17ULL, 0x29c15acbb1a71c6eULL, 0xa6a9cb93f5b6d5e5ULL, 0xdc71bb3bc588a49cULL,
    0x380923064a09a5edULL, 0x42d153387a067694ULL, 0xCDB9C2D82A78ED1FULL, 0xB761B2F03A4D9466ULL,
    0xf831C60FC38B5762ULL, 0x82e9b647d3ca341bULL, 0x0d812731271e1f90ULL, 0x775957D97B6512E9ULL,
    0xee29d4d93eba4819ULL, 0x94F1A4B12E0D4160ULL, 0x1B99358666B2FAEBULL, 0x614165A856B83E92ULL,
    0x2e111193e78d1196ULL, 0x54C9610B4779AAEFULL, 0xdBA1F0531765A164ULL, 0xA179803B276B281DULL,
    0x450118DCA345CF6CULL, 0x3FD968E4937E0615ULL, 0xB0B1F9BCF37C4D9EULL, 0xCA698999C345D1E7ULL,
    0x8539FDB2B2A1E3E3ULL, 0xFFE18D7AE28B2A9AULL, 0x70891C22D26FD111ULL, 0x0A516C0242424968ULL,
    0x7CE8F478E27EA919ULL, 0x063084B0D250D060ULL, 0x895815889D1C4BEBULL, 0xF380650AAD0C9292ULL,
    0xBCD011C90CB0BF96ULL, 0xC608618E0C80E6EFULL, 0x4960F0D05C1B4D64ULL, 0x33B880F82C2F641DULL,
    0xD7C0186C5E3E9B6CULL, 0xAD1868845C22E315ULL, 0x2270F9DC44DBAA9EULL, 0x58A889149415B3E7ULL,
    0x17F8FD3F7A45AEE3ULL, 0x6D208DF74A7ECF9AULL, 0xE2481CAF1AE06D11ULL, 0x98906C972AF13E68ULL,
};

uint64_t crc64(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc = crc64_table[static_cast<uint8_t>(crc ^ p[i])] ^ (crc >> 8);
    }
    return crc;
}

// ─── RDB length encoding ────────────────────────────────────────────
// 0-63:      1 byte   00xxxxxx
// 64-16383:  2 bytes  01xxxxxx yyyyyyyy
// >=16384:   5 bytes  10000000 + 4-byte LE

void write_length(std::string& out, uint64_t len) {
    if (len < 64) {
        out += static_cast<char>(len);                          // 00xxxxxx
    } else if (len < 16384) {
        out += static_cast<char>(0x40 | (len >> 8));            // 01xxxxxx (high 6 bits)
        out += static_cast<char>(len & 0xFF);                   // yyyyyyyy (low 8 bits)
    } else {
        out += static_cast<char>(0x80);                         // 10000000 marker
        uint32_t len32 = static_cast<uint32_t>(len);
        out.append(reinterpret_cast<const char*>(&len32), 4);   // 4-byte LE
    }
}

uint64_t read_length(const char*& p, const char* end) {
    if (p >= end) throw std::runtime_error("RDB: unexpected end in length");
    uint8_t first = static_cast<uint8_t>(*p++);
    uint8_t type = (first >> 6) & 0x03;     // top 2 bits select encoding

    if (type == 0) {                         // 00xxxxxx → 0..63
        return first & 0x3F;
    } else if (type == 1) {                  // 01xxxxxx yyyyyyyy → 64..16383
        if (p >= end) throw std::runtime_error("RDB: unexpected end in length");
        uint8_t second = static_cast<uint8_t>(*p++);
        return ((first & 0x3F) << 8) | second;
    } else if (type == 2) {                  // 10000000 + 4-byte LE → big lengths
        if (p + 4 > end) throw std::runtime_error("RDB: unexpected end in length");
        uint32_t len;
        std::memcpy(&len, p, 4);
        p += 4;
        return len;
    } else {                                 // 11xxxxxx → special encoding (integers)
        // Used for integer-encoded strings in RDB (e.g., 0xC0 = 8-bit int)
        // We need this for aux field parsing — return raw byte for caller to handle
        return first;
    }
}

// ─── RDB string encoding ────────────────────────────────────────────
// Length-prefixed: <length><raw bytes>
// Special: 0xC0=8-bit int, 0xC1=16-bit int, 0xC2=32-bit int

void write_string(std::string& out, const std::string& s) {
    write_length(out, s.size());
    out += s;
}

std::string read_string(const char*& p, const char* end) {
    if (p >= end) throw std::runtime_error("RDB: unexpected end in string");

    uint8_t first = static_cast<uint8_t>(*p);
    uint8_t type = (first >> 6) & 0x03;

    // Special encoding: integer-as-string (0xC0, 0xC1, 0xC2)
    if (type == 3) {
        uint8_t enc = first & 0x3F;
        p++;   // consume the type byte
        if (enc == 0) {           // 0xC0: 8-bit signed int
            if (p >= end) throw std::runtime_error("RDB: unexpected end in int8 string");
            int8_t val;
            std::memcpy(&val, p, 1);
            p += 1;
            return std::to_string(val);
        } else if (enc == 1) {    // 0xC1: 16-bit signed int LE
            if (p + 2 > end) throw std::runtime_error("RDB: unexpected end in int16 string");
            int16_t val;
            std::memcpy(&val, p, 2);
            p += 2;
            return std::to_string(val);
        } else if (enc == 2) {    // 0xC2: 32-bit signed int LE
            if (p + 4 > end) throw std::runtime_error("RDB: unexpected end in int32 string");
            int32_t val;
            std::memcpy(&val, p, 4);
            p += 4;
            return std::to_string(val);
        }
        throw std::runtime_error("RDB: unsupported special encoding " + std::to_string(enc));
    }

    // Normal: length-prefixed raw bytes
    uint64_t len = read_length(p, end);
    if (p + len > end) throw std::runtime_error("RDB: unexpected end in string data");
    std::string result(p, len);
    p += len;
    return result;
}

// ─── Little-endian helpers ──────────────────────────────────────────

void write_le64(std::string& out, uint64_t val) {
    char buf[8];
    std::memcpy(buf, &val, 8);   // x86/ARM are LE natively
    out.append(buf, 8);
}

uint64_t read_le64(const char*& p, const char* end) {
    if (p + 8 > end) throw std::runtime_error("RDB: unexpected end in le64");
    uint64_t val;
    std::memcpy(&val, p, 8);
    p += 8;
    return val;
}

void write_le32(std::string& out, uint32_t val) {
    char buf[4];
    std::memcpy(buf, &val, 4);
    out.append(buf, 4);
}

uint32_t read_le32(const char*& p, const char* end) {
    if (p + 4 > end) throw std::runtime_error("RDB: unexpected end in le32");
    uint32_t val;
    std::memcpy(&val, p, 4);
    p += 4;
    return val;
}

// RDB opcodes
constexpr uint8_t RDB_OPCODE_AUX       = 0xFA;
constexpr uint8_t RDB_OPCODE_SELECTDB   = 0xFE;
constexpr uint8_t RDB_OPCODE_RESIZEDB   = 0xFB;
constexpr uint8_t RDB_OPCODE_EXPIRETIME_MS = 0xFC;
constexpr uint8_t RDB_OPCODE_EXPIRETIME    = 0xFD;
constexpr uint8_t RDB_OPCODE_EOF        = 0xFF;
constexpr uint8_t RDB_TYPE_STRING       = 0x00;

}  // anonymous namespace

// ─── Public API ─────────────────────────────────────────────────────

std::string rdb::serialize(const Store& store) {
    std::string out;
    out.reserve(1024);  // avoid frequent reallocs for small stores

    // 1. Magic: "REDIS0011" (RDB version 11)
    out += "REDIS0011";

    // 2. Aux fields — metadata (matches our existing EMPTY_RDB format)
    //    FA <key> <value> pairs for redis-ver, redis-bits, ctime, used-mem, aof-base
    out += static_cast<char>(RDB_OPCODE_AUX);
    write_string(out, "redis-ver");
    write_string(out, "7.2.0");

    out += static_cast<char>(RDB_OPCODE_AUX);
    write_string(out, "redis-bits");
    // 0xC0 0x40 = integer-encoded 64
    out += static_cast<char>(0xC0);
    out += static_cast<char>(0x40);

    out += static_cast<char>(RDB_OPCODE_AUX);
    write_string(out, "ctime");
    auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // 0xC2 = 32-bit integer encoding
    out += static_cast<char>(0xC2);
    write_le32(out, static_cast<uint32_t>(now_sec));

    out += static_cast<char>(RDB_OPCODE_AUX);
    write_string(out, "used-mem");
    out += static_cast<char>(0xC2);
    write_le32(out, 0);  // approximate — not critical

    out += static_cast<char>(RDB_OPCODE_AUX);
    write_string(out, "aof-base");
    out += static_cast<char>(0xC0);
    out += static_cast<char>(0x00);

    // 3. DB selector — always database 0
    out += static_cast<char>(RDB_OPCODE_SELECTDB);
    write_length(out, 0);

    // 4. Resize DB hint — count non-expired entries
    auto steady_now = std::chrono::steady_clock::now();
    auto sys_now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    size_t db_size = 0;
    size_t expires_size = 0;
    for (const auto& [key, entry] : store.data()) {
        if (entry.expiry && steady_now >= *entry.expiry) continue;  // skip expired
        db_size++;
        if (entry.expiry) expires_size++;
    }

    out += static_cast<char>(RDB_OPCODE_RESIZEDB);
    write_length(out, db_size);
    write_length(out, expires_size);

    // 5. Key-value entries
    for (const auto& [key, entry] : store.data()) {
        // Skip expired entries — don't serialize stale data
        if (entry.expiry && steady_now >= *entry.expiry) continue;

        // Expiry: FC <8-byte absolute ms LE> before the key-value pair
        if (entry.expiry) {
            auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                *entry.expiry - steady_now).count();
            int64_t abs_ms = sys_now_ms + remaining_ms;

            out += static_cast<char>(RDB_OPCODE_EXPIRETIME_MS);
            write_le64(out, static_cast<uint64_t>(abs_ms));
        }

        // Type byte: 0x00 = string
        out += static_cast<char>(RDB_TYPE_STRING);
        write_string(out, key);
        write_string(out, entry.value);
    }

    // 6. EOF marker
    out += static_cast<char>(RDB_OPCODE_EOF);

    // 7. CRC64 checksum over everything
    uint64_t checksum = crc64(out.data(), out.size());
    write_le64(out, checksum);

    return out;
}

void rdb::load(const std::string& data, Store& store) {
    store.clear();  // clean slate — full resync replaces everything

    if (data.size() < 9) throw std::runtime_error("RDB: too short");

    const char* p = data.data();
    const char* end = data.data() + data.size();

    // 1. Verify magic — must start with "REDIS" + 4-digit version
    if (std::string(p, 9).substr(0, 5) != "REDIS") {
        throw std::runtime_error("RDB: invalid magic");
    }
    p += 9;  // skip "REDIS0011"

    // 2. Walk through opcodes
    auto sys_now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::optional<int64_t> pending_expiry;  // set by FC/FD, consumed by next key

    while (p < end) {
        uint8_t opcode = static_cast<uint8_t>(*p++);

        switch (opcode) {
            case RDB_OPCODE_AUX: {
                // FA <key-string> <value-string> — metadata, skip
                read_string(p, end);
                read_string(p, end);
                break;
            }
            case RDB_OPCODE_SELECTDB: {
                // FE <db-number> — we only support db 0, just skip
                read_length(p, end);
                break;
            }
            case RDB_OPCODE_RESIZEDB: {
                // FB <db-size> <expires-size> — hash table hints, skip
                read_length(p, end);
                read_length(p, end);
                break;
            }
            case RDB_OPCODE_EXPIRETIME_MS: {
                // FC <8-byte absolute ms LE> — applies to next key
                pending_expiry = static_cast<int64_t>(read_le64(p, end));
                break;
            }
            case RDB_OPCODE_EXPIRETIME: {
                // FD <4-byte absolute sec LE> — convert to ms
                pending_expiry = static_cast<int64_t>(read_le32(p, end)) * 1000;
                break;
            }
            case RDB_OPCODE_EOF: {
                // FF — end of data. CRC64 follows but we don't verify it on load
                return;
            }
            case RDB_TYPE_STRING: {
                // 00 <key> <value> — string type entry
                std::string key = read_string(p, end);
                std::string val = read_string(p, end);

                if (pending_expiry) {
                    int64_t remaining = *pending_expiry - sys_now_ms;
                    if (remaining > 0) {
                        store.set(key, val, remaining);  // px_millis
                    }
                    // else: already expired, don't insert
                    pending_expiry.reset();
                } else {
                    store.set(key, val);  // no expiry
                }
                break;
            }
            default:
                throw std::runtime_error("RDB: unsupported type " + std::to_string(opcode));
        }
    }
}
