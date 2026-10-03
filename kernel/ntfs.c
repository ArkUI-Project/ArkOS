/* Original ArkOS read-only NTFS driver, MIT. No Linux or ntfs-3g source used.
 * Format references: Microsoft NTFS Developer Notes (FILE_RECORD_SEGMENT_HEADER,
 * ATTRIBUTE_RECORD_HEADER); public NTFS on-disk format documentation.
 * Conservative limits deliberately reject unsupported/ambiguous metadata. */
#include "ntfs.h"
#define RECORD_MAX 4096u
#define INDEX_MAX 8192u
#define RUN_MAX 128u
#define TREE_DEPTH 16u
#define TREE_NODES 4096u
#define REF_MASK UINT64_C(0x0000ffffffffffff)
typedef struct {
    uint64_t vcn, lcn, clusters;
} Run;
typedef struct {
    bool resident;
    uint64_t size, initialized;
    unsigned count;
    Run runs[RUN_MAX];
    unsigned char data[RECORD_MAX];
} Stream;
typedef struct {
    unsigned char data[INDEX_MAX];
    uint32_t pos, end;
    uint64_t vcn;
    bool child_done;
} Frame;
static struct {
    FsReadBlocks read;
    void *ctx;
    uint64_t start, sectors, clusters;
    uint32_t cluster_bytes, record_bytes, index_bytes;
    bool mounted;
    Stream mft;
} volume;
static const char *error_text = "NTFS not mounted";
static unsigned char sector[512], record[RECORD_MAX];
static Frame frames[TREE_DEPTH];
static Stream file_stream, index_stream;
static uint16_t parent_sequence;
static bool error(const char *text) {
    error_text = text;
    return false;
}
const char *ntfs_error(void) {
    return error_text;
}
static uint16_t u16(const unsigned char *p) {
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}
static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t u64(const unsigned char *p) {
    return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32;
}
static int bytes_compare(const void *aa, const void *bb, size_t n) {
    const unsigned char *a = aa, *b = bb;
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}
static bool power2(uint32_t n) {
    return n && !(n & (n - 1));
}
static bool range(uint64_t offset, uint64_t bytes, uint64_t limit) {
    return offset <= limit && bytes <= limit - offset;
}
static bool disk_read(uint64_t offset, void *target, size_t bytes) {
    if (!range(offset, bytes, volume.sectors * 512))
        return error("NTFS disk read outside partition");
    unsigned char *out = target;
    while (bytes) {
        unsigned within = (unsigned)(offset % 512), take = 512 - within;
        if (take > bytes)
            take = (unsigned)bytes;
        if (!volume.read(volume.ctx, volume.start + offset / 512, 1, sector))
            return error("NTFS block read failed");
        memcpy(out, sector + within, take);
        out += take;
        bytes -= take;
        offset += take;
    }
    return true;
}
static bool fixup(unsigned char *data, uint32_t bytes, const char *magic) {
    if (bytes_compare(data, magic, 4))
        return error("NTFS invalid record signature");
    unsigned offset = u16(data + 4), count = u16(data + 6);
    if (bytes < 512 || bytes % 512 || offset < 8 || count != bytes / 512 + 1 ||
        !range(offset, (uint64_t)count * 2, bytes) || offset + count * 2 > 510)
        return error("NTFS invalid update-sequence array bounds");
    uint16_t sequence = u16(data + offset);
    for (unsigned i = 1; i < count; ++i) {
        unsigned tail = i * 512 - 2;
        if (u16(data + tail) != sequence)
            return error("NTFS torn record: update-sequence mismatch");
        data[tail] = data[offset + i * 2];
        data[tail + 1] = data[offset + i * 2 + 1];
    }
    return true;
}
static bool record_header(void) {
    uint32_t used = u32(record + 24), allocated = u32(record + 28);
    unsigned first = u16(record + 20), usa = u16(record + 4) + u16(record + 6) * 2;
    if (!(u16(record + 22) & 1))
        return error("NTFS record is not in use");
    if (u64(record + 32))
        return error("NTFS extension records require unsupported ATTRIBUTE_LIST");
    if (allocated != volume.record_bytes || used > allocated || first < 42 || first < usa ||
        first % 8 || first + 4 > used)
        return error("NTFS invalid file-record bounds");
    return true;
}
static bool stream_read(const Stream *stream, uint64_t offset, void *buffer, size_t bytes) {
    if (!range(offset, bytes, stream->size))
        return error("NTFS stream read outside logical size");
    unsigned char *out = buffer;
    if (stream->resident) {
        memcpy(out, stream->data + (size_t)offset, bytes);
        return true;
    }
    while (bytes) {
        if (offset >= stream->initialized) {
            memset(out, 0, bytes);
            return true;
        }
        uint64_t vcn = offset / volume.cluster_bytes, within = offset % volume.cluster_bytes;
        const Run *run = 0;
        for (unsigned i = 0; i < stream->count; ++i)
            if (vcn >= stream->runs[i].vcn &&
                vcn - stream->runs[i].vcn < stream->runs[i].clusters) {
                run = &stream->runs[i];
                break;
            }
        if (!run)
            return error("NTFS stream has unmapped clusters");
        uint64_t left = (run->clusters - (vcn - run->vcn)) * volume.cluster_bytes - within;
        if (left > stream->initialized - offset)
            left = stream->initialized - offset;
        size_t take = left > bytes ? bytes : (size_t)left;
        uint64_t physical = (run->lcn + vcn - run->vcn) * volume.cluster_bytes + within;
        if (!disk_read(physical, out, take))
            return false;
        out += take;
        bytes -= take;
        offset += take;
    }
    return true;
}
/* Verify every attribute, even those that the caller does not use. */
static const unsigned char *attribute(uint32_t type, const char *name) {
    uint32_t used = u32(record + 24), pos = u16(record + 20);
    const unsigned char *found = 0;
    while (range(pos, 4, used)) {
        const unsigned char *a = record + pos;
        uint32_t code = u32(a);
        if (code == UINT32_MAX)
            return found;
        if (!range(pos, 16, used)) {
            error("NTFS truncated attribute header");
            return 0;
        }
        uint32_t length = u32(a + 4);
        unsigned form = a[8], names = a[9], nameoff = u16(a + 10);
        unsigned header = form ? 64 : 24;
        if (form > 1 || length < header || length % 8 || !range(pos, length, used) ||
            (names && (nameoff < header || !range(nameoff, names * 2u, length)))) {
            error("NTFS malformed attribute bounds");
            return 0;
        }
        if (!form && (u16(a + 20) < header || !range(u16(a + 20), u32(a + 16), length))) {
            error("NTFS malformed resident value");
            return 0;
        }
        if (code == 0x20) {
            error("NTFS ATTRIBUTE_LIST continuation unsupported");
            return 0;
        }
        bool match = code == type;
        if (!name)
            match = match && !names;
        else {
            size_t n = strlen(name);
            match = match && names == n;
            for (unsigned i = 0; match && i < names; ++i)
                if (u16(a + nameoff + i * 2) != (unsigned char)name[i])
                    match = false;
        }
        if (match) {
            if (found) {
                error("NTFS duplicate or segmented attribute unsupported");
                return 0;
            }
            found = a;
        }
        pos += length;
    }
    error("NTFS missing attribute terminator");
    return 0;
}
static bool load_stream(const unsigned char *a, Stream *stream) {
    if (!a)
        return error("NTFS required stream missing");
    memset(stream, 0, sizeof(*stream));
    if (u16(a + 12) & 0xc0ff)
        return error("NTFS compressed, encrypted or sparse stream unsupported");
    uint32_t length = u32(a + 4);
    if (!a[8]) {
        uint32_t size = u32(a + 16);
        unsigned offset = u16(a + 20);
        if (size > RECORD_MAX || !range(offset, size, length))
            return error("NTFS resident stream too large");
        stream->resident = true;
        stream->size = stream->initialized = size;
        memcpy(stream->data, a + offset, size);
        return true;
    }
    if (u64(a + 16) || u16(a + 34))
        return error("NTFS segmented or compressed stream unsupported");
    unsigned pos = u16(a + 32);
    if (pos < 64 || pos >= length)
        return error("NTFS invalid runlist offset");
    uint64_t highest = u64(a + 24), allocated = u64(a + 40);
    stream->size = u64(a + 48);
    stream->initialized = u64(a + 56);
    if (stream->initialized > stream->size || stream->size > allocated ||
        allocated > volume.clusters * volume.cluster_bytes || allocated % volume.cluster_bytes)
        return error("NTFS stream sizes inconsistent");
    uint64_t vcn = 0, lcn = 0;
    bool ended = false;
    while (pos < length) {
        unsigned header = a[pos++];
        if (!header) {
            ended = true;
            break;
        }
        unsigned lengthbytes = header & 15, offsetbytes = header >> 4;
        if (!lengthbytes || lengthbytes > 8 || !offsetbytes || offsetbytes > 8 ||
            !range(pos, lengthbytes + offsetbytes, length))
            return error("NTFS sparse or malformed runlist unsupported");
        if (stream->count == RUN_MAX)
            return error("NTFS runlist exceeds 128 extents");
        uint64_t clusters = 0, delta = 0;
        for (unsigned j = 0; j < lengthbytes; ++j)
            clusters |= (uint64_t)a[pos++] << (j * 8);
        for (unsigned j = 0; j < offsetbytes; ++j)
            delta |= (uint64_t)a[pos++] << (j * 8);
        bool negative = (a[pos - 1] & 128) != 0;
        if (negative) {
            if (offsetbytes < 8)
                delta |= UINT64_MAX << (offsetbytes * 8);
            uint64_t magnitude = (~delta) + 1;
            if (magnitude > lcn)
                return error("NTFS negative LCN outside volume");
            lcn -= magnitude;
        } else {
            if (delta > volume.clusters || lcn > volume.clusters - delta)
                return error("NTFS run LCN overflow");
            lcn += delta;
        }
        if (!clusters || clusters > volume.clusters || lcn > volume.clusters - clusters ||
            vcn > volume.clusters - clusters)
            return error("NTFS run extent outside volume");
        stream->runs[stream->count++] = (Run){vcn, lcn, clusters};
        vcn += clusters;
    }
    if (!ended || (vcn && highest != vcn - 1) || (!vcn && highest != UINT64_MAX && highest != 0) ||
        allocated != vcn * volume.cluster_bytes)
        return error("NTFS runlist coverage mismatch");
    return true;
}
static bool mft_record(uint64_t reference, bool check_sequence) {
    uint64_t id = reference & REF_MASK;
    if (id > UINT64_MAX / volume.record_bytes ||
        !range(id * volume.record_bytes, volume.record_bytes, volume.mft.size))
        return error("NTFS file reference outside MFT");
    if (!stream_read(&volume.mft, id * volume.record_bytes, record, volume.record_bytes) ||
        !fixup(record, volume.record_bytes, "FILE") || !record_header())
        return false;
    if (check_sequence && u16(record + 16) != (uint16_t)(reference >> 48))
        return error("NTFS stale file-reference sequence");
    return true;
}
static bool utf16_name(const unsigned char *name, unsigned units, char out[1024]) {
    unsigned n = 0;
    for (unsigned i = 0; i < units; ++i) {
        uint32_t c = u16(name + i * 2);
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i >= units)
                return error("NTFS malformed UTF-16 filename");
            uint32_t low = u16(name + i * 2);
            if (low < 0xdc00 || low > 0xdfff)
                return error("NTFS malformed UTF-16 surrogate");
            c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
        } else if (c >= 0xdc00 && c <= 0xdfff)
            return error("NTFS isolated UTF-16 surrogate");
        if (!c || c == '/' || c < 32)
            return error("NTFS unsafe filename character");
        if (c < 0x80)
            out[n++] = (char)c;
        else if (c < 0x800) {
            out[n++] = (char)(0xc0 | (c >> 6));
            out[n++] = (char)(0x80 | (c & 63));
        } else if (c < 0x10000) {
            out[n++] = (char)(0xe0 | (c >> 12));
            out[n++] = (char)(0x80 | ((c >> 6) & 63));
            out[n++] = (char)(0x80 | (c & 63));
        } else {
            out[n++] = (char)(0xf0 | (c >> 18));
            out[n++] = (char)(0x80 | ((c >> 12) & 63));
            out[n++] = (char)(0x80 | ((c >> 6) & 63));
            out[n++] = (char)(0x80 | (c & 63));
        }
    }
    out[n] = 0;
    return true;
}
static bool frame_header(Frame *frame, unsigned header, unsigned bytes) {
    if (!range(header, 16, bytes))
        return error("NTFS short index header");
    uint32_t first = u32(frame->data + header), used = u32(frame->data + header + 4),
             allocated = u32(frame->data + header + 8);
    if (first < 16 || first % 8 || used < first || allocated < used ||
        !range(header, allocated, bytes))
        return error("NTFS invalid directory index bounds");
    frame->pos = header + first;
    frame->end = header + used;
    frame->child_done = false;
    return true;
}
typedef bool (*Visit)(void *, const char *, uint64_t, bool, uint64_t);
/* In-order traversal uses bounded static frames: malformed cycles cannot recurse. */
static bool walk_directory(uint64_t reference, Visit visit, void *ctx) {
    if (!mft_record(reference, true))
        return false;
    if (!(u16(record + 22) & 2))
        return error("NTFS path component is not a directory");
    parent_sequence = u16(record + 16);
    error_text = "";
    const unsigned char *reparse = attribute(0xc0, 0);
    if (*error_text)
        return false;
    if (reparse)
        return error("NTFS reparse directories are unsupported");
    const unsigned char *root = attribute(0x90, "$I30");
    if (*error_text)
        return false;
    if (!root || root[8])
        return error("NTFS resident $I30 index root missing");
    unsigned size = u32(root + 16), offset = u16(root + 20);
    if (size < 32 || size > INDEX_MAX)
        return error("NTFS index root size unsupported");
    memcpy(frames[0].data, root + offset, size);
    frames[0].vcn = UINT64_MAX;
    if (u32(frames[0].data) != 0x30 || u32(frames[0].data + 4) != 1 ||
        u32(frames[0].data + 8) != volume.index_bytes)
        return error("NTFS filename index format unsupported");
    if (!frame_header(&frames[0], 16, size))
        return false;
    const unsigned char *allocation = attribute(0xa0, "$I30");
    if (*error_text)
        return false;
    bool has_allocation = allocation != 0;
    if (allocation && allocation[8] != 1)
        return error("NTFS index allocation must be nonresident");
    if (allocation && !load_stream(allocation, &index_stream))
        return false;
    unsigned depth = 1, nodes = 0;
    while (depth) {
        Frame *f = &frames[depth - 1];
        if (!range(f->pos, 16, f->end))
            return error("NTFS missing index end entry");
        const unsigned char *e = f->data + f->pos;
        unsigned length = u16(e + 8), key = u16(e + 10), flags = u16(e + 12);
        if (length < 16 || length % 8 || !range(f->pos, length, f->end) || flags & ~3u ||
            key > length - 16 || ((flags & 1) && (length < 24 || key > length - 24)))
            return error("NTFS malformed directory entry");
        if ((flags & 1) && !f->child_done) {
            if (!has_allocation || depth == TREE_DEPTH || ++nodes > TREE_NODES)
                return error("NTFS directory tree exceeds bounds");
            uint64_t vcn = u64(e + length - 8);
            for (unsigned i = 1; i < depth; ++i)
                if (frames[i].vcn == vcn)
                    return error("NTFS directory index cycle");
            uint64_t unit = volume.cluster_bytes <= volume.index_bytes ? volume.cluster_bytes : 512;
            if (vcn > UINT64_MAX / unit ||
                !range(vcn * unit, volume.index_bytes, index_stream.size))
                return error("NTFS index child outside allocation");
            f->child_done = true;
            Frame *child = &frames[depth];
            child->vcn = vcn;
            if (!stream_read(&index_stream, vcn * unit, child->data, volume.index_bytes) ||
                !fixup(child->data, volume.index_bytes, "INDX") || u64(child->data + 16) != vcn ||
                !frame_header(child, 24, volume.index_bytes))
                return error("NTFS invalid index child record");
            ++depth;
            continue;
        }
        if (flags & 2) {
            --depth;
            continue;
        }
        if (key < 66 || e[16 + 64] == 0 || 66u + e[16 + 64] * 2u > key || e[16 + 65] > 3)
            return error("NTFS invalid FILE_NAME index key");
        uint64_t parent = u64(e + 16);
        if ((parent & REF_MASK) != (reference & REF_MASK) ||
            (uint16_t)(parent >> 48) != parent_sequence)
            return error("NTFS index entry has wrong parent reference");
        /* Skip DOS-only aliases, retain POSIX/Win32/combined long names. */
        if (e[16 + 65] != 2) {
            char name[1024];
            if (!utf16_name(e + 16 + 66, e[16 + 64], name))
                return false;
            if (strcmp(name, ".") &&
                !visit(ctx, name, u64(e), (u32(e + 16 + 56) & 0x10000000u) != 0, u64(e + 16 + 48)))
                return true;
        }
        f->pos += length;
        f->child_done = false;
    }
    return true;
}
typedef struct {
    const char *name;
    uint64_t reference;
    bool found;
} Lookup;
static bool match_name(void *opaque, const char *name, uint64_t reference, bool dir,
                       uint64_t size) {
    (void)dir;
    (void)size;
    Lookup *lookup = opaque;
    if (!strcmp(lookup->name, name)) {
        lookup->reference = reference;
        lookup->found = true;
        return false;
    }
    return true;
}
static bool resolve(const char *path, uint64_t *result) {
    if (!volume.mounted)
        return error("NTFS not mounted");
    if (!path || *path != '/')
        return error("NTFS path must be volume-absolute");
    if (strlen(path) > 1023)
        return error("NTFS path exceeds 1023 bytes");
    if (!mft_record(5, false))
        return false;
    uint64_t reference = 5 | (uint64_t)u16(record + 16) << 48;
    const char *p = path;
    unsigned components = 0;
    while (*p) {
        while (*p == '/')
            ++p;
        if (!*p)
            break;
        char name[1024];
        unsigned n = 0;
        while (*p && *p != '/')
            name[n++] = *p++;
        name[n] = 0;
        if (!strcmp(name, "."))
            continue;
        if (!strcmp(name, ".."))
            return error("NTFS caller must normalize parent path segments");
        if (++components > 32)
            return error("NTFS path nesting exceeds 32 components");
        Lookup lookup = {name, 0, false};
        if (!walk_directory(reference, match_name, &lookup))
            return false;
        if (!lookup.found)
            return error("NTFS path not found (names are case-sensitive)");
        reference = lookup.reference;
    }
    *result = reference;
    return true;
}
static bool data_stream(void) {
    error_text = "";
    const unsigned char *reparse = attribute(0xc0, 0);
    if (*error_text)
        return false;
    if (reparse)
        return error("NTFS reparse points are unsupported");
    const unsigned char *data = attribute(0x80, 0);
    if (*error_text)
        return false;
    return load_stream(data, &file_stream);
}
bool ntfs_mount(FsReadBlocks read, void *ctx, uint64_t start, uint64_t sectors) {
    memset(&volume, 0, sizeof(volume));
    error_text = "";
    if (!read || sectors < 16 || sectors > UINT64_MAX / 512 || start > UINT64_MAX - sectors)
        return error("NTFS invalid partition bounds");
    volume.read = read;
    volume.ctx = ctx;
    volume.start = start;
    volume.sectors = sectors;
    unsigned char boot[512];
    if (!disk_read(0, boot, sizeof(boot)))
        return false;
    if (bytes_compare(boot + 3, "NTFS    ", 8) || u16(boot + 510) != 0xaa55 ||
        u16(boot + 11) != 512)
        return error("NTFS requires NTFS signature and 512-byte sectors");
    if (!power2(boot[13]) || boot[13] > 128)
        return error("NTFS invalid cluster size");
    uint64_t total = u64(boot + 40);
    if (total < 16 || total > sectors)
        return error("NTFS declared volume exceeds partition");
    volume.sectors = total;
    volume.cluster_bytes = (uint32_t)boot[13] * 512;
    volume.clusters = total / boot[13];
    int recordcode = (int8_t)boot[64], indexcode = (int8_t)boot[68];
    if (recordcode < 0) {
        if (recordcode < -12 || recordcode > -9)
            return error("NTFS MFT record size unsupported");
        volume.record_bytes = 1u << -recordcode;
    } else
        volume.record_bytes = (uint32_t)recordcode * volume.cluster_bytes;
    if (indexcode < 0) {
        if (indexcode < -13 || indexcode > -9)
            return error("NTFS index record size unsupported");
        volume.index_bytes = 1u << -indexcode;
    } else
        volume.index_bytes = (uint32_t)indexcode * volume.cluster_bytes;
    if (!power2(volume.record_bytes) || volume.record_bytes < 512 ||
        volume.record_bytes > RECORD_MAX || !power2(volume.index_bytes) ||
        volume.index_bytes < 512 || volume.index_bytes > INDEX_MAX)
        return error("NTFS record/index geometry unsupported");
    uint64_t mft_lcn = u64(boot + 48);
    if (mft_lcn >= volume.clusters ||
        !disk_read(mft_lcn * volume.cluster_bytes, record, volume.record_bytes) ||
        !fixup(record, volume.record_bytes, "FILE") || !record_header())
        return error("NTFS MFT bootstrap invalid");
    const unsigned char *data = attribute(0x80, 0);
    if (*error_text || !load_stream(data, &volume.mft))
        return false;
    if (volume.mft.resident || volume.mft.size < 6u * volume.record_bytes ||
        volume.mft.initialized < volume.mft.size)
        return error("NTFS MFT stream unsupported");
    if (!mft_record(3, false))
        return false;
    const unsigned char *info = attribute(0x70, 0);
    if (*error_text || !info || info[8] || u32(info + 16) < 12)
        return error("NTFS volume information missing");
    info += u16(info + 20);
    if (info[8] != 3 || info[9] > 1)
        return error("Only NTFS 3.0/3.1 supported");
    if (u16(info + 10) & 1)
        return error("Dirty NTFS volume rejected; journal replay is not implemented");
    if (!mft_record(5, false) || !(u16(record + 22) & 2))
        return error("NTFS root directory invalid");
    volume.mounted = true;
    error_text = "";
    return true;
}
bool ntfs_stat(const char *path, bool *is_dir, uint64_t *size) {
    error_text = "";
    uint64_t reference;
    if (!is_dir || !size)
        return error("NTFS stat output arguments missing");
    if (!resolve(path, &reference) || !mft_record(reference, true))
        return false;
    *is_dir = (u16(record + 22) & 2) != 0;
    *size = 0;
    if (*is_dir) {
        const unsigned char *reparse = attribute(0xc0, 0);
        if (*error_text)
            return false;
        return reparse ? error("NTFS reparse directories are unsupported") : true;
    }
    if (!data_stream())
        return false;
    *size = file_stream.size;
    return true;
}
typedef struct {
    FsEmit emit;
    void *ctx;
} EmitContext;
static bool emit_entry(void *opaque, const char *name, uint64_t reference, bool dir,
                       uint64_t size) {
    (void)reference;
    EmitContext *ctx = opaque;
    return ctx->emit(ctx->ctx, name, dir, size);
}
bool ntfs_list(const char *path, FsEmit emit, void *ctx) {
    error_text = "";
    uint64_t reference;
    if (!emit)
        return error("NTFS list callback missing");
    if (!resolve(path, &reference))
        return false;
    EmitContext emitctx = {emit, ctx};
    return walk_directory(reference, emit_entry, &emitctx);
}
bool ntfs_read(const char *path, uint64_t offset, void *buffer, size_t bytes, size_t *read) {
    error_text = "";
    uint64_t reference;
    if (!read || (!buffer && bytes))
        return error("NTFS invalid read arguments");
    *read = 0;
    if (!resolve(path, &reference) || !mft_record(reference, true))
        return false;
    if (u16(record + 22) & 2)
        return error("Cannot read NTFS directory as file");
    if (!data_stream())
        return false;
    if (offset >= file_stream.size)
        return true;
    if ((uint64_t)bytes > file_stream.size - offset)
        bytes = (size_t)(file_stream.size - offset);
    if (!bytes)
        return true;
    if (!stream_read(&file_stream, offset, buffer, bytes))
        return false;
    *read = bytes;
    return true;
}
