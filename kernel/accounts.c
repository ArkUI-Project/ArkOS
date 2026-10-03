/* ArkOS native accounts. Original code, MIT license.
 * SHA-256 / HMAC / PBKDF2 implement the published algorithms without a runtime
 * crypto library. This is authentication, not encryption or secure boot. */
#include "ark.h"
#include "storage.h"
#include "extfs.h"
#include "accounts.h"

typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    unsigned used;
    uint8_t block[64];
} Sha256;
typedef struct {
    Sha256 inner, outer;
} Hmac256;

static void erase(void *p, size_t n) {
    volatile uint8_t *b = p;
    while (n--)
        *b++ = 0;
}
static uint32_t rotr(uint32_t v, unsigned n) {
    return (v >> n) | (v << (32 - n));
}
static uint32_t be32(const uint8_t *b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
static void sha_block(Sha256 *s, const uint8_t *block) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i)
        w[i] = be32(block + 4 * i);
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t x = w[i - 15], y = w[i - 2];
        w[i] = w[i - 16] + (rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3)) + w[i - 7] +
               (rotr(y, 17) ^ rotr(y, 19) ^ (y >> 10));
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5],
             g = s->h[6], h = s->h[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t t1 =
            h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
    erase(w, sizeof(w));
}
static void sha_init(Sha256 *s) {
    static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memset(s, 0, sizeof(*s));
    memcpy(s->h, initial, sizeof(initial));
}
static void sha_update(Sha256 *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->bytes += n;
    while (n) {
        unsigned take = 64 - s->used;
        if (take > n)
            take = (unsigned)n;
        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        n -= take;
        if (s->used == 64) {
            sha_block(s, s->block);
            s->used = 0;
        }
    }
}
static void sha_final(Sha256 *s, uint8_t digest[32]) {
    uint64_t bits = s->bytes * 8;
    s->block[s->used++] = 0x80;
    if (s->used > 56) {
        memset(s->block + s->used, 0, 64 - s->used);
        sha_block(s, s->block);
        s->used = 0;
    }
    memset(s->block + s->used, 0, 56 - s->used);
    for (unsigned i = 0; i < 8; ++i)
        s->block[63 - i] = (uint8_t)(bits >> (8 * i));
    sha_block(s, s->block);
    for (unsigned i = 0; i < 32; ++i)
        digest[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
    erase(s, sizeof(*s));
}
static void hmac_init(Hmac256 *h, const uint8_t *key, size_t length) {
    uint8_t pad[64], digest[32];
    memset(pad, 0, sizeof(pad));
    if (length > 64) {
        Sha256 s;
        sha_init(&s);
        sha_update(&s, key, length);
        sha_final(&s, digest);
        memcpy(pad, digest, 32);
    } else
        memcpy(pad, key, length);
    for (unsigned i = 0; i < 64; ++i)
        pad[i] ^= 0x36;
    sha_init(&h->inner);
    sha_update(&h->inner, pad, 64);
    for (unsigned i = 0; i < 64; ++i)
        pad[i] ^= 0x36 ^ 0x5c;
    sha_init(&h->outer);
    sha_update(&h->outer, pad, 64);
    erase(pad, sizeof(pad));
    erase(digest, sizeof(digest));
}
static void hmac_digest(const Hmac256 *h, const void *data, size_t length, uint8_t out[32]) {
    Sha256 s = h->inner;
    uint8_t inner[32];
    sha_update(&s, data, length);
    sha_final(&s, inner);
    s = h->outer;
    sha_update(&s, inner, 32);
    sha_final(&s, out);
    erase(inner, sizeof(inner));
}
/* One 32-byte output block. Account salts are always 16 bytes. */
static void pbkdf2(const char *password, size_t length, const uint8_t *salt, size_t salt_length,
                   uint32_t rounds, uint8_t out[32]) {
    Hmac256 h;
    uint8_t first[68], u[32];
    hmac_init(&h, (const uint8_t *)password, length);
    memcpy(first, salt, salt_length);
    memset(first + salt_length, 0, 4);
    first[salt_length + 3] = 1;
    hmac_digest(&h, first, salt_length + 4, u);
    memcpy(out, u, 32);
    for (uint32_t i = 1; i < rounds; ++i) {
        hmac_digest(&h, u, 32, u);
        for (unsigned j = 0; j < 32; ++j)
            out[j] ^= u[j];
    }
    erase(&h, sizeof(h));
    erase(first, sizeof(first));
    erase(u, sizeof(u));
}
static bool equal_verifier(const uint8_t a[32], const uint8_t b[32]) {
    /* Volatile accumulator prevents a compiler from introducing an early exit. */
    volatile uint32_t diff = 0;
    for (unsigned i = 0; i < 32; ++i)
        diff |= (uint32_t)(a[i] ^ b[i]);
    return diff == 0;
}
static bool same_bytes(const void *left, const void *right, size_t n) {
    const uint8_t *a = left, *b = right;
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

typedef struct {
    AccountProfile public;
    uint8_t salt[16], verifier[32];
} Record;
static Record records[ACCOUNTS_MAX];
static unsigned count;
static int selected = -1;
static AccountState state = ACCOUNT_ERROR;
static uint64_t nonce, generation, blocked_since;
static unsigned failures;
static bool blocked;
static const char *last_error = "Account service not initialized";
#define DB_CAP 4096u
static char serialized[DB_CAP], previous[DB_CAP];

static bool fail(const char *message) {
    last_error = message;
    return false;
}
static bool trusted(void) {
    return accounts_caller_is_system() || fail("Permission denied: SYSTEM service required");
}
static size_t bounded_length(const char *s, size_t cap) {
    if (!s)
        return cap;
    size_t n = 0;
    while (n < cap && s[n])
        ++n;
    return n;
}
static bool utf8_text(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint32_t c = (uint8_t)s[i++], min;
        unsigned more;
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f)
                return false;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            more = 1;
            min = 0x80;
            c &= 31;
        } else if (c >= 0xe0 && c <= 0xef) {
            more = 2;
            min = 0x800;
            c &= 15;
        } else if (c >= 0xf0 && c <= 0xf4) {
            more = 3;
            min = 0x10000;
            c &= 7;
        } else
            return false;
        if (more > n - i)
            return false;
        while (more--) {
            uint8_t b = (uint8_t)s[i++];
            if ((b & 0xc0) != 0x80)
                return false;
            c = (c << 6) | (b & 63);
        }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) || (c >= 0x80 && c <= 0x9f))
            return false;
    }
    return true;
}
static bool slug_valid(const char *s) {
    size_t n = bounded_length(s, 24);
    if (!n || n >= 24 || s[0] < 'a' || s[0] > 'z')
        return false;
    for (size_t i = 1; i < n; ++i)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' ||
              s[i] == '-'))
            return false;
    return strcmp(s, "root") && strcmp(s, "system") && strcmp(s, "admin");
}
static bool display_valid(const char *s) {
    size_t n = bounded_length(s, 64);
    return n && n < 64 && s[0] != ' ' && s[n - 1] != ' ' && utf8_text(s, n);
}
static bool password_valid(const char *s) {
    size_t n = bounded_length(s, ACCOUNTS_PASSWORD_MAX + 1);
    if (n < ACCOUNTS_PASSWORD_MIN || n > ACCOUNTS_PASSWORD_MAX || !utf8_text(s, n))
        return fail("Password must contain 8-64 UTF-8 bytes without control characters");
    unsigned distinct = 0;
    bool seen[256] = {false};
    for (size_t i = 0; i < n; ++i)
        if (!seen[(uint8_t)s[i]]) {
            seen[(uint8_t)s[i]] = true;
            ++distinct;
        }
    if (distinct < 3)
        return fail("Password is too repetitive");
    static const char *common[] = {"password",   "password1",  "password123", "12345678",
                                   "123456789",  "1234567890", "qwerty123",   "qwerty12345",
                                   "letmein123", "abcdefgh",   "11111111"};
    char lower[ACCOUNTS_PASSWORD_MAX + 1];
    for (size_t i = 0; i <= n; ++i)
        lower[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
    bool weak = false;
    for (unsigned i = 0; i < sizeof(common) / sizeof(common[0]); ++i)
        if (!strcmp(lower, common[i]))
            weak = true;
    erase(lower, sizeof(lower));
    erase(seen, sizeof(seen));
    return !weak || fail("Choose a less common password");
}
static void make_home(AccountProfile *p) {
    strcopy(p->home, "/home/", sizeof(p->home));
    strcopy(p->home + 6, p->slug, sizeof(p->home) - 6);
}
static int index_for_uid(uint32_t uid) {
    for (unsigned i = 0; i < count; ++i)
        if (records[i].public.uid == uid)
            return (int)i;
    return -1;
}
static int index_for_slug(const char *slug) {
    if (!slug_valid(slug))
        return -1;
    for (unsigned i = 0; i < count; ++i)
        if (!strcmp(records[i].public.slug, slug))
            return (int)i;
    return -1;
}
static int unhex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}
static bool decode_hex(const char *s, size_t n, uint8_t *out, size_t bytes) {
    if (n != bytes * 2)
        return false;
    for (size_t i = 0; i < bytes; ++i) {
        int a = unhex(s[i * 2]), b = unhex(s[i * 2 + 1]);
        if (a < 0 || b < 0)
            return false;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return true;
}
static char *append_hex(char *p, const uint8_t *bytes, size_t n) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        *p++ = hex[bytes[i] >> 4];
        *p++ = hex[bytes[i] & 15];
    }
    return p;
}
static char *append_text(char *p, const char *s) {
    while (*s)
        *p++ = *s++;
    return p;
}
static char *append_number(char *p, uint32_t number) {
    char text[16];
    uint_to_str(number, text);
    return append_text(p, text);
}
static void serialize(void) {
    char *p = append_text(serialized, "ARKACCT1|");
    uint8_t bytes[8];
    for (unsigned i = 0; i < 8; ++i)
        bytes[i] = (uint8_t)(nonce >> (56 - i * 8));
    p = append_hex(p, bytes, 8);
    *p++ = '\n';
    for (unsigned i = 0; i < count; ++i) {
        Record *r = &records[i];
        p = append_number(p, r->public.uid);
        *p++ = '|';
        p = append_number(p, r->public.flags);
        *p++ = '|';
        p = append_text(p, r->public.slug);
        *p++ = '|';
        p = append_hex(p, (const uint8_t *)r->public.display_name, strlen(r->public.display_name));
        *p++ = '|';
        p = append_hex(p, r->salt, 16);
        *p++ = '|';
        p = append_hex(p, r->verifier, 32);
        *p++ = '|';
        p = append_number(p, ACCOUNTS_KDF_ITERATIONS);
        *p++ = '\n';
    }
    *p = 0; /* Maximum: 8 * 270 + 26 < DB_CAP, fields validated before insertion. */
}
static bool number(const char *s, size_t n, uint32_t *out) {
    if (!n || n > 10 || (n > 1 && s[0] == '0'))
        return false;
    uint32_t value = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        unsigned digit = (unsigned)(s[i] - '0');
        if (value > (UINT32_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}
static bool parse(const char *data, size_t size) {
    if (size < 26 || size >= DB_CAP || !same_bytes(data, "ARKACCT1|", 9))
        return false;
    uint8_t counter[8];
    if (!decode_hex(data + 9, 16, counter, 8) || data[25] != '\n')
        return false;
    nonce = 0;
    for (unsigned i = 0; i < 8; ++i)
        nonce = (nonce << 8) | counter[i];
    size_t at = 26;
    unsigned enabled_admins = 0;
    while (at < size) {
        if (count >= ACCOUNTS_MAX)
            return false;
        const char *field[7];
        size_t length[7];
        for (unsigned f = 0; f < 7; ++f) {
            size_t begin = at;
            char delimiter = f == 6 ? '\n' : '|';
            while (at < size && data[at] != delimiter) {
                if (!data[at] || data[at] == '\n' || data[at] == '|')
                    return false;
                ++at;
            }
            if (at >= size)
                return false;
            field[f] = data + begin;
            length[f] = at - begin;
            ++at;
        }
        Record *r = &records[count];
        uint32_t rounds;
        if (!number(field[0], length[0], &r->public.uid) ||
            r->public.uid != ACCOUNTS_FIRST_UID + count ||
            !number(field[1], length[1], &r->public.flags) || r->public.flags > 3 || !length[2] ||
            length[2] >= sizeof(r->public.slug))
            return false;
        memcpy(r->public.slug, field[2], length[2]);
        r->public.slug[length[2]] = 0;
        if (!slug_valid(r->public.slug) || (!count && strcmp(r->public.slug, "ark")))
            return false;
        if (!length[3] || (length[3] & 1) || length[3] >= sizeof(r->public.display_name) * 2 ||
            !decode_hex(field[3], length[3], (uint8_t *)r->public.display_name, length[3] / 2))
            return false;
        r->public.display_name[length[3] / 2] = 0;
        if (bounded_length(r->public.display_name, 64) != length[3] / 2 ||
            !display_valid(r->public.display_name) ||
            !decode_hex(field[4], length[4], r->salt, 16) ||
            !decode_hex(field[5], length[5], r->verifier, 32) ||
            !number(field[6], length[6], &rounds) || rounds != ACCOUNTS_KDF_ITERATIONS)
            return false;
        for (unsigned j = 0; j < count; ++j) {
            if (!strcmp(records[j].public.slug, r->public.slug) ||
                same_bytes(records[j].salt, r->salt, 16))
                return false;
        }
        uint64_t salt_nonce = 0;
        for (unsigned j = 0; j < 8; ++j)
            salt_nonce = (salt_nonce << 8) | r->salt[j];
        if (!salt_nonce || salt_nonce > nonce)
            return false;
        make_home(&r->public);
        if ((r->public.flags & (ACCOUNT_ADMIN | ACCOUNT_DISABLED)) == ACCOUNT_ADMIN)
            ++enabled_admins;
        ++count;
    }
    return count && enabled_admins && nonce >= count;
}

/* A CPUID-guarded RDRAND contribution, never an unconditional instruction.
 * The persistent counter supplies uniqueness in an installation even when the
 * CPU has no hardware random generator. Clock/counter fallback is NOT a CSPRNG. */
static bool random32(uint32_t *out) {
#if defined(__x86_64__) && !defined(ARK_ACCOUNTS_HOST_TEST)
    uint32_t a, b, c, d;
    a = 1;
    c = 0;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if (!(c & (1u << 30)))
        return false;
    for (unsigned i = 0; i < 10; ++i) {
        unsigned char ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok));
        if (ok)
            return true;
    }
#else
    (void)out;
#endif
    return false;
}
static bool new_salt(uint8_t salt[16], uint32_t uid) {
    if (nonce == UINT64_MAX)
        return fail("Account salt counter exhausted");
    ++nonce;
    uint64_t ticks = platform_ticks(), tsc = 0;
    uint32_t random[4] = {0};
#if defined(__x86_64__) && !defined(ARK_ACCOUNTS_HOST_TEST)
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    tsc = ((uint64_t)high << 32) | low;
#endif
    for (unsigned i = 0; i < 4; ++i)
        (void)random32(&random[i]);
    Sha256 s;
    uint8_t digest[32];
    sha_init(&s);
    sha_update(&s, "ArkOS account salt v1", 21);
    sha_update(&s, &nonce, sizeof(nonce));
    sha_update(&s, &uid, sizeof(uid));
    sha_update(&s, &ticks, sizeof(ticks));
    sha_update(&s, &tsc, sizeof(tsc));
    sha_update(&s, random, sizeof(random));
    sha_final(&s, digest);
    /* Counter is explicit in the first half, guaranteeing no same-installation
     * reuse even if the entropy contribution repeats. Counter is not secret. */
    for (unsigned i = 0; i < 8; ++i)
        salt[i] = (uint8_t)(nonce >> (56 - i * 8));
    memcpy(salt + 8, digest, 8);
    erase(digest, sizeof(digest));
    erase(random, sizeof(random));
    return true;
}
static bool save_database(void) {
    int old = vfs_find(ACCOUNTS_DB_PATH);
    bool exists = old >= 0;
    if (exists) {
        VFile *f = vfs_entry(old);
        if (!f || f->is_dir || f->size >= sizeof(previous))
            return fail("Account database file is invalid");
        memcpy(previous, f->data, f->size + 1);
    } else
        previous[0] = 0;
    serialize();
    if (!vfs_mkdir("/.system"))
        return fail("Cannot create account storage directory");
    int file = vfs_create(ACCOUNTS_DB_PATH);
    if (file < 0 || !vfs_write(file, serialized))
        return fail("Cannot write account database");
    if (storage_mounted() && !storage_sync()) {
        /* A failed flush can have an uncertain on-disk outcome. Close the
         * session and require reboot/recovery instead of claiming success. */
        if (exists)
            (void)vfs_write(file, previous);
        else
            (void)vfs_remove(ACCOUNTS_DB_PATH);
        state = ACCOUNT_ERROR;
        selected = -1;
        ++generation;
        erase(previous, sizeof(previous));
        erase(serialized, sizeof(serialized));
        return fail("Account disk commit failed; session closed, restart required");
    }
    erase(previous, sizeof(previous));
    erase(serialized, sizeof(serialized));
    return true;
}
static bool make_user_home(const AccountProfile *p) {
    if (!vfs_mkdir("/home") || !vfs_mkdir(p->home))
        return fail("Cannot create user home: filesystem full or path occupied");
    /* One home directory per user: app subdirectories are created on demand. */
    return true;
}

bool accounts_init(void) {
    erase(records, sizeof(records));
    count = 0;
    selected = -1;
    nonce = 0;
    failures = 0;
    blocked = false;
    ++generation;
    last_error = "";
    state = ACCOUNT_ERROR;
    int index = vfs_find(ACCOUNTS_DB_PATH);
    if (index < 0) {
        state = ACCOUNT_NEEDS_SETUP;
        return true;
    }
    VFile *f = vfs_entry(index);
    if (!f || f->is_dir || f->size >= DB_CAP || !parse(f->data, f->size)) {
        erase(records, sizeof(records));
        count = 0;
        nonce = 0;
        return fail("Account database malformed; automatic reset refused");
    }
    for (unsigned i = 0; i < count; ++i) {
        int home = vfs_find(records[i].public.home);
        VFile *entry = vfs_entry(home);
        if (home < 0 || !entry || !entry->is_dir) {
            erase(records, sizeof(records));
            count = 0;
            return fail("Account home missing or invalid");
        }
    }
    state = ACCOUNT_LOGGED_OUT;
    return true;
}
AccountState accounts_state(void) {
    return state;
}
const char *accounts_error(void) {
    return last_error;
}
bool accounts_persistent(void) {
    return storage_mounted();
}
unsigned accounts_count(void) {
    return count;
}
bool accounts_list(unsigned index, AccountProfile *out) {
    if (!out || index >= count)
        return false;
    *out = records[index].public;
    return true;
}
bool accounts_profile(uint32_t uid, AccountProfile *out) {
    int i = index_for_uid(uid);
    return i >= 0 && accounts_list((unsigned)i, out);
}
bool accounts_session_profile(AccountProfile *out) {
    return (state == ACCOUNT_ACTIVE || state == ACCOUNT_LOCKED) && selected >= 0 &&
           accounts_list((unsigned)selected, out);
}
uint32_t accounts_current_uid(void) {
    return state == ACCOUNT_ACTIVE && selected >= 0 ? records[selected].public.uid
                                                    : ACCOUNTS_UID_NONE;
}
const char *accounts_current_home(void) {
    return state == ACCOUNT_ACTIVE && selected >= 0 ? records[selected].public.home : "";
}
bool accounts_current_admin(void) {
    return state == ACCOUNT_ACTIVE && selected >= 0 &&
           (records[selected].public.flags & ACCOUNT_ADMIN);
}
uint64_t accounts_session_generation(void) {
    return generation;
}
uint32_t accounts_retry_after_ticks(void) {
    if (!blocked)
        return 0;
    uint64_t now = platform_ticks();
    if (now >= blocked_since && now - blocked_since >= 3000) {
        blocked = false;
        failures = 0;
        return 0;
    }
    return now < blocked_since ? 3000 : (uint32_t)(3000 - (now - blocked_since));
}
static bool authenticate(int index, const char *password) {
    if (accounts_retry_after_ticks())
        return fail("Too many failed attempts; wait 30 seconds");
    size_t n = bounded_length(password, ACCOUNTS_PASSWORD_MAX + 1);
    static const uint8_t dummy_salt[16] = {0x41, 0x72, 0x6b, 0x4f, 0x53, 0x20, 0x64, 0x75,
                                           0x6d, 0x6d, 0x79, 0,    0,    0,    0,    1};
    static const uint8_t dummy_verifier[32] = {0};
    uint8_t result[32];
    bool valid = password && n <= ACCOUNTS_PASSWORD_MAX;
    pbkdf2(valid ? password : "", valid ? n : 0, index >= 0 ? records[index].salt : dummy_salt, 16,
           ACCOUNTS_KDF_ITERATIONS, result);
    bool same = equal_verifier(result, index >= 0 ? records[index].verifier : dummy_verifier);
    erase(result, sizeof(result));
    if (!valid || index < 0 || !same || (records[index].public.flags & ACCOUNT_DISABLED)) {
        if (++failures >= 5) {
            blocked = true;
            blocked_since = platform_ticks();
        }
        return fail("Incorrect account or password");
    }
    failures = 0;
    blocked = false;
    last_error = "";
    return true;
}
bool accounts_enroll(const char *password) {
    if (!trusted())
        return false;
    if (state != ACCOUNT_NEEDS_SETUP || count)
        return fail("First account already configured or database unavailable");
    if (!password_valid(password))
        return false;
    Record *r = &records[0];
    memset(r, 0, sizeof(*r));
    r->public.uid = ACCOUNTS_FIRST_UID;
    r->public.flags = ACCOUNT_ADMIN;
    strcopy(r->public.slug, "ark", sizeof(r->public.slug));
    strcopy(r->public.display_name, "Ark", sizeof(r->public.display_name));
    make_home(&r->public);
    if (!make_user_home(&r->public) || !new_salt(r->salt, r->public.uid)) {
        erase(r, sizeof(*r));
        return false;
    }
    pbkdf2(password, strlen(password), r->salt, 16, ACCOUNTS_KDF_ITERATIONS, r->verifier);
    count = 1;
    if (!save_database()) {
        count = 0;
        erase(r, sizeof(*r));
        return false;
    }
    selected = 0;
    state = ACCOUNT_ACTIVE;
    ++generation;
    last_error = "";
    return true;
}
bool accounts_login(const char *slug, const char *password) {
    if (!trusted())
        return false;
    if (state != ACCOUNT_LOGGED_OUT)
        return fail("Log out before starting another session");
    int index = index_for_slug(slug);
    if (!authenticate(index, password))
        return false;
    selected = index;
    state = ACCOUNT_ACTIVE;
    ++generation;
    return true;
}
bool accounts_lock(void) {
    if (!trusted())
        return false;
    if (state == ACCOUNT_LOCKED)
        return true;
    if (state != ACCOUNT_ACTIVE)
        return fail("No active session to lock");
    state = ACCOUNT_LOCKED;
    ++generation;
    last_error = "";
    return true;
}
bool accounts_unlock(const char *password) {
    if (!trusted())
        return false;
    if (state != ACCOUNT_LOCKED || selected < 0)
        return fail("No locked session to unlock");
    if (!authenticate(selected, password))
        return false;
    state = ACCOUNT_ACTIVE;
    ++generation;
    return true;
}
bool accounts_logout(void) {
    if (!trusted())
        return false;
    if (state != ACCOUNT_ACTIVE && state != ACCOUNT_LOCKED && state != ACCOUNT_LOGGED_OUT)
        return fail("No account session configured");
    state = ACCOUNT_LOGGED_OUT;
    selected = -1;
    ++generation;
    last_error = "";
    return true;
}
bool accounts_change_password(const char *old_password, const char *new_password) {
    if (!trusted())
        return false;
    if (state != ACCOUNT_ACTIVE || selected < 0)
        return fail("Sign in before changing password");
    if (!password_valid(new_password) || !authenticate(selected, old_password))
        return false;
    int index = selected;
    Record before = records[index];
    if (!new_salt(records[index].salt, records[index].public.uid)) {
        erase(&before, sizeof(before));
        return false;
    }
    pbkdf2(new_password, strlen(new_password), records[index].salt, 16, ACCOUNTS_KDF_ITERATIONS,
           records[index].verifier);
    bool saved = save_database();
    if (!saved)
        records[index] = before;
    erase(&before, sizeof(before));
    if (saved)
        last_error = "";
    return saved;
}
bool accounts_create_user(const char *slug, const char *display_name, const char *password,
                          bool administrator) {
    if (!trusted())
        return false;
    if (!accounts_current_admin())
        return fail("Administrator session required");
    if (count >= ACCOUNTS_MAX)
        return fail("Eight-account limit reached");
    if (!slug_valid(slug))
        return fail("User ID must be 1-23 lowercase ASCII letters, digits, underscore or hyphen; "
                    "begin with a letter");
    if (index_for_slug(slug) >= 0)
        return fail("User ID already exists");
    if (!display_valid(display_name))
        return fail("Display name must be 1-63 valid UTF-8 bytes");
    if (!password_valid(password))
        return false;
    Record *r = &records[count];
    memset(r, 0, sizeof(*r));
    r->public.uid = ACCOUNTS_FIRST_UID + count;
    r->public.flags = administrator ? ACCOUNT_ADMIN : 0;
    strcopy(r->public.slug, slug, sizeof(r->public.slug));
    strcopy(r->public.display_name, display_name, sizeof(r->public.display_name));
    make_home(&r->public);
    /* Reject an unrelated pre-existing home, preventing adoption of another
     * directory's contents by choosing its basename as a new account name. */
    if (vfs_find(r->public.home) >= 0) {
        erase(r, sizeof(*r));
        return fail("User home already exists");
    }
    if (!make_user_home(&r->public)) {
        erase(r, sizeof(*r));
        return false;
    }
    if (!new_salt(r->salt, r->public.uid)) {
        (void)vfs_remove(r->public.home);
        erase(r, sizeof(*r));
        return false;
    }
    pbkdf2(password, strlen(password), r->salt, 16, ACCOUNTS_KDF_ITERATIONS, r->verifier);
    ++count;
    if (!save_database()) {
        --count;
        (void)vfs_remove(r->public.home);
        erase(r, sizeof(*r));
        return false;
    }
    last_error = "";
    return true;
}
bool accounts_set_disabled(uint32_t uid, bool disabled) {
    if (!trusted())
        return false;
    if (!accounts_current_admin())
        return fail("Administrator session required");
    int index = index_for_uid(uid);
    if (index < 0)
        return fail("Unknown account");
    if (disabled && index == selected)
        return fail("Cannot disable the current account");
    if (disabled && (records[index].public.flags & ACCOUNT_ADMIN)) {
        unsigned admins = 0;
        for (unsigned i = 0; i < count; ++i)
            if ((records[i].public.flags & 3) == ACCOUNT_ADMIN)
                ++admins;
        if (admins <= 1)
            return fail("Cannot disable the last enabled administrator");
    }
    uint32_t before = records[index].public.flags;
    if (disabled)
        records[index].public.flags |= ACCOUNT_DISABLED;
    else
        records[index].public.flags &= ~ACCOUNT_DISABLED;
    if (before == records[index].public.flags) {
        last_error = "";
        return true;
    }
    if (!save_database()) {
        records[index].public.flags = before;
        return false;
    }
    last_error = "";
    return true;
}

extern bool vfs_path_canonical(char out[128], const char *path);
static bool within(const char *path, const char *parent) {
    size_t n = strlen(parent);
    return !strncmp(path, parent, n) && (path[n] == 0 || path[n] == '/');
}
bool accounts_path_allowed(uint32_t uid, const char *absolute_path, bool write) {
    if (uid == ACCOUNTS_UID_NONE || uid != accounts_current_uid() || !absolute_path ||
        absolute_path[0] != '/')
        return false;
    if (bounded_length(absolute_path, 128) >= 128)
        return false;
    char path[128];
    if (!vfs_path_canonical(path, absolute_path))
        return false;
    if (within(path, "/.system"))
        return false;
    if (within(path, accounts_current_home()))
        return !write || strcmp(path, accounts_current_home()) != 0;
    if (within(path, "/mnt"))
        return true;
    if (!write && (within(path, "/etc") || !strcmp(path, "/") || !strcmp(path, "/home")))
        return true;
    return false;
}
