#include "core/ClipStore.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "core/Png.h"

namespace clipper {

namespace fs = std::filesystem;

// journal layout, little endian (we only run on x86 / arm64 linux):
//   header  "CLIPHIST" u32 version u32 0
//   entry   u32 bodySize, u32 crc32(body), body
//   body    u8 op, then per op:
//     PutClip       id u64, kind u8, flags u8, created i64, lastUsed i64, seq u64, useCount u32, folder u32,
//                   hash u64, width u32, height u32, bytes u64, title str, source str, text str
//     Meta          id u64, flags u8, lastUsed i64, seq u64, useCount u32, folder u32, title str
//     Remove        id u64
//     PutFolder     id u32, name str
//     RemoveFolder  id u32
//   str = u32 length + bytes. flags: 1 pinned, 2 cut
// a crash mid-append leaves a short or bad-crc entry at the end, load() drops it.

namespace {

constexpr char kMagic[8] = {'C', 'L', 'I', 'P', 'H', 'I', 'S', 'T'};
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderSize = 16;
constexpr uint8_t kFlagPinned = 1, kFlagCut = 2;

uint32_t crc32(const char* data, size_t size) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) c = table[(c ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

class Writer {
public:
    template <typename T>
    void put(T v) {
        char b[sizeof(T)];
        std::memcpy(b, &v, sizeof(T));
        out.append(b, sizeof(T));
    }
    void str(const std::string& s) {
        put<uint32_t>(static_cast<uint32_t>(s.size()));
        out += s;
    }
    std::string out;
};

class Reader {
public:
    Reader(const char* p, size_t n) : p_(p), end_(p + n) {}
    template <typename T>
    T get() {
        T v{};
        if (static_cast<size_t>(end_ - p_) < sizeof(T)) {
            ok_ = false;
            return v;
        }
        std::memcpy(&v, p_, sizeof(T));
        p_ += sizeof(T);
        return v;
    }
    std::string str() {
        const uint32_t n = get<uint32_t>();
        if (!ok_ || static_cast<size_t>(end_ - p_) < n) {
            ok_ = false;
            return {};
        }
        std::string s(p_, n);
        p_ += n;
        return s;
    }
    bool ok() const { return ok_; }

private:
    const char* p_;
    const char* end_;
    bool ok_ = true;
};

uint8_t flagsOf(const Clip& c) { return (c.pinned ? kFlagPinned : 0) | (c.cut ? kFlagCut : 0); }

std::string header() {
    Writer w;
    w.out.append(kMagic, sizeof kMagic);
    w.put<uint32_t>(kVersion);
    w.put<uint32_t>(0);
    return w.out;
}

std::string frame(const std::string& body) {
    Writer w;
    w.put<uint32_t>(static_cast<uint32_t>(body.size()));
    w.put<uint32_t>(crc32(body.data(), body.size()));
    return w.out + body;
}

std::string encodeClip(const Clip& c) {
    Writer w;
    w.put<uint8_t>(1);  // Op::PutClip
    w.put<uint64_t>(c.id);
    w.put<uint8_t>(static_cast<uint8_t>(c.kind));
    w.put<uint8_t>(flagsOf(c));
    w.put<int64_t>(c.created);
    w.put<int64_t>(c.lastUsed);
    w.put<uint64_t>(c.seq);
    w.put<uint32_t>(c.useCount);
    w.put<uint32_t>(c.folder);
    w.put<uint64_t>(c.hash);
    w.put<uint32_t>(c.width);
    w.put<uint32_t>(c.height);
    w.put<uint64_t>(c.bytes);
    w.str(c.title);
    w.str(c.source);
    w.str(c.text);
    return w.out;
}

std::string encodeFolder(const Folder& f) {
    Writer w;
    w.put<uint8_t>(4);  // Op::PutFolder
    w.put<uint32_t>(f.id);
    w.str(f.name);
    return w.out;
}

bool writeFileAtomic(const fs::path& path, const std::string& data) {
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out.flush()) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

}  // namespace

const char* kindName(ClipKind kind) {
    switch (kind) {
        case ClipKind::Text:
            return "Text";
        case ClipKind::Image:
            return "Image";
        case ClipKind::Files:
            return "Files";
    }
    return "?";
}

uint64_t contentHash(ClipKind kind, const std::string& payload) {
    uint64_t h = 0xcbf29ce484222325ull ^ static_cast<uint8_t>(kind);  // fnv-1a
    for (unsigned char c : payload) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h ? h : 1;
}

ClipStore::ClipStore(std::string dir) : dir_(std::move(dir)) {
    if (!dir_.empty()) journalPath_ = (fs::path(dir_) / "history.bin").string();
}

ClipStore::~ClipStore() {
    if (journal_) std::fclose(journal_);
}

const Clip* ClipStore::find(uint64_t id) const {
    auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : &clips_[it->second];
}

Clip* ClipStore::findMut(uint64_t id) {
    auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : &clips_[it->second];
}

const Folder* ClipStore::findFolder(uint32_t id) const {
    for (const Folder& f : folders_) {
        if (f.id == id) return &f;
    }
    return nullptr;
}

size_t ClipStore::keptCount() const {
    return static_cast<size_t>(std::count_if(clips_.begin(), clips_.end(), [](const Clip& c) { return c.kept(); }));
}

void ClipStore::reindex() {
    byId_.clear();
    byHash_.clear();
    for (size_t i = 0; i < clips_.size(); ++i) {
        byId_[clips_[i].id] = i;
        byHash_[clips_[i].hash] = clips_[i].id;
    }
}

void ClipStore::apply(const Clip& clip) {
    if (Clip* existing = findMut(clip.id)) {
        if (existing->hash != clip.hash) byHash_.erase(existing->hash);
        *existing = clip;
    } else {
        byId_[clip.id] = clips_.size();
        clips_.push_back(clip);
    }
    byHash_[clip.hash] = clip.id;
    nextId_ = std::max(nextId_, clip.id + 1);
    nextSeq_ = std::max(nextSeq_, clip.seq + 1);
}

void ClipStore::eraseAt(size_t index) {
    clips_.erase(clips_.begin() + static_cast<std::ptrdiff_t>(index));
    reindex();
}

bool ClipStore::replay(const std::string& data, size_t* goodBytes) {
    size_t pos = kHeaderSize;
    *goodBytes = pos;
    while (data.size() - pos >= 8) {
        uint32_t size, crc;
        std::memcpy(&size, data.data() + pos, 4);
        std::memcpy(&crc, data.data() + pos + 4, 4);
        if (data.size() - pos - 8 < size) break;  // cut off mid-write
        const char* body = data.data() + pos + 8;
        if (crc32(body, size) != crc) break;
        Reader r(body, size);
        const auto op = static_cast<Op>(r.get<uint8_t>());
        switch (op) {
            case Op::PutClip: {
                Clip c;
                c.id = r.get<uint64_t>();
                c.kind = static_cast<ClipKind>(r.get<uint8_t>());
                const uint8_t flags = r.get<uint8_t>();
                c.pinned = flags & kFlagPinned;
                c.cut = flags & kFlagCut;
                c.created = r.get<int64_t>();
                c.lastUsed = r.get<int64_t>();
                c.seq = r.get<uint64_t>();
                c.useCount = r.get<uint32_t>();
                c.folder = r.get<uint32_t>();
                c.hash = r.get<uint64_t>();
                c.width = r.get<uint32_t>();
                c.height = r.get<uint32_t>();
                c.bytes = r.get<uint64_t>();
                c.title = r.str();
                c.source = r.str();
                c.text = r.str();
                if (r.ok() && c.kind <= ClipKind::Files) apply(c);
                break;
            }
            case Op::Meta: {
                const uint64_t id = r.get<uint64_t>();
                const uint8_t flags = r.get<uint8_t>();
                const int64_t lastUsed = r.get<int64_t>();
                const uint64_t seq = r.get<uint64_t>();
                const uint32_t useCount = r.get<uint32_t>();
                const uint32_t folder = r.get<uint32_t>();
                std::string title = r.str();
                Clip* c = findMut(id);
                if (!r.ok() || !c) break;
                c->pinned = flags & kFlagPinned;
                c->cut = flags & kFlagCut;
                c->lastUsed = lastUsed;
                c->seq = seq;
                c->useCount = useCount;
                c->folder = folder;
                c->title = std::move(title);
                nextSeq_ = std::max(nextSeq_, seq + 1);
                break;
            }
            case Op::Remove: {
                const uint64_t id = r.get<uint64_t>();
                if (auto it = byId_.find(id); r.ok() && it != byId_.end()) eraseAt(it->second);
                break;
            }
            case Op::PutFolder: {
                Folder f;
                f.id = r.get<uint32_t>();
                f.name = r.str();
                if (!r.ok()) break;
                auto it = std::find_if(folders_.begin(), folders_.end(), [&](const Folder& x) { return x.id == f.id; });
                if (it != folders_.end())
                    it->name = f.name;
                else
                    folders_.push_back(f);
                nextFolder_ = std::max(nextFolder_, f.id + 1);
                break;
            }
            case Op::RemoveFolder: {
                const uint32_t id = r.get<uint32_t>();
                if (!r.ok()) break;
                std::erase_if(folders_, [&](const Folder& f) { return f.id == id; });
                for (Clip& c : clips_) {
                    if (c.folder == id) c.folder = 0;
                }
                break;
            }
        }
        pos += 8 + size;
        *goodBytes = pos;
    }
    return *goodBytes == data.size();
}

bool ClipStore::load(std::string* error) {
    auto fail = [&](std::string msg) {
        if (error) *error = std::move(msg);
        return false;
    };
    if (dir_.empty()) return fail("no data directory");
    std::error_code ec;
    fs::create_directories(fs::path(dir_) / "images", ec);
    if (ec) return fail("can't create " + dir_ + ": " + ec.message());

    clips_.clear();
    folders_.clear();
    byId_.clear();
    byHash_.clear();

    std::string data;
    {
        std::ifstream in(journalPath_, std::ios::binary);
        if (in) data.assign(std::istreambuf_iterator<char>(in), {});
    }
    bool rewrite = false;
    if (!data.empty()) {
        if (data.size() < kHeaderSize || std::memcmp(data.data(), kMagic, sizeof kMagic) != 0) {
            // not ours or from the future: keep it around instead of overwriting someone's history
            fs::rename(journalPath_, journalPath_ + ".unreadable", ec);
            data.clear();
        } else {
            size_t good = 0;
            rewrite = !replay(data, &good);
        }
    }

    // images that went missing (cleaned up by hand?) take their clip with them
    for (size_t i = clips_.size(); i-- > 0;) {
        if (clips_[i].kind == ClipKind::Image && !fs::exists(imagePath(clips_[i]), ec)) {
            clips_.erase(clips_.begin() + static_cast<std::ptrdiff_t>(i));
            rewrite = true;
        }
    }
    reindex();

    fileBytes_ = data.size();
    if (rewrite || (fileBytes_ > 256 * 1024 && fileBytes_ > 2 * liveBytes())) {
        if (!compact()) return fail("can't write " + journalPath_);
    } else {
        journal_ = std::fopen(journalPath_.c_str(), "ab");
        if (!journal_) return fail("can't open " + journalPath_ + " for writing");
        if (data.empty()) {
            const std::string h = header();
            std::fwrite(h.data(), 1, h.size(), journal_);
            std::fflush(journal_);
            fileBytes_ = h.size();
        }
    }
    ++revision_;
    return true;
}

uint64_t ClipStore::diskBytes() const {
    uint64_t n = fileBytes_;
    for (const Clip& c : clips_) {
        if (c.kind == ClipKind::Image) n += c.bytes;
    }
    return n;
}

uint64_t ClipStore::liveBytes() const {
    uint64_t n = kHeaderSize;
    for (const Clip& c : clips_) n += 8 + 80 + c.title.size() + c.source.size() + c.text.size();
    for (const Folder& f : folders_) n += 8 + 9 + f.name.size();
    return n;
}

bool ClipStore::compact() {
    if (journal_) {
        std::fclose(journal_);
        journal_ = nullptr;
    }
    std::string out = header();
    for (const Folder& f : folders_) out += frame(encodeFolder(f));
    std::vector<const Clip*> order;
    for (const Clip& c : clips_) order.push_back(&c);
    std::sort(order.begin(), order.end(), [](const Clip* a, const Clip* b) { return a->seq < b->seq; });
    for (const Clip* c : order) out += frame(encodeClip(*c));

    const bool ok = writeFileAtomic(journalPath_, out);
    if (ok) fileBytes_ = out.size();
    journal_ = std::fopen(journalPath_.c_str(), "ab");
    return ok && journal_;
}

void ClipStore::append(const std::string& body) {
    ++revision_;
    if (!journal_) return;
    const std::string entry = frame(body);
    std::fwrite(entry.data(), 1, entry.size(), journal_);
    std::fflush(journal_);  // the kernel has it, survives us crashing (not a power cut, that's fine)
    fileBytes_ += entry.size();
}

void ClipStore::writeClip(const Clip& clip) { append(encodeClip(clip)); }

void ClipStore::writeMeta(const Clip& c) {
    Writer w;
    w.put<uint8_t>(static_cast<uint8_t>(Op::Meta));
    w.put<uint64_t>(c.id);
    w.put<uint8_t>(flagsOf(c));
    w.put<int64_t>(c.lastUsed);
    w.put<uint64_t>(c.seq);
    w.put<uint32_t>(c.useCount);
    w.put<uint32_t>(c.folder);
    w.str(c.title);
    append(w.out);
}

std::string ClipStore::imagePath(const Clip& clip) const {
    if (clip.kind != ClipKind::Image || dir_.empty()) return {};
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.png", static_cast<unsigned long long>(clip.hash));
    return (fs::path(dir_) / "images" / name).string();
}

bool ClipStore::readImage(const Clip& clip, std::string* png) const {
    std::ifstream in(imagePath(clip), std::ios::binary);
    if (!in) return false;
    png->assign(std::istreambuf_iterator<char>(in), {});
    return !png->empty();
}

uint64_t ClipStore::add(NewClip in, int64_t nowMs) {
    const std::string& payload = in.kind == ClipKind::Image ? in.png : in.text;
    if (payload.empty()) return 0;
    const uint64_t hash = contentHash(in.kind, payload);

    if (auto it = byHash_.find(hash); it != byHash_.end()) {
        Clip* c = findMut(it->second);
        c->cut = in.cut;
        touch(c->id, nowMs);
        return c->id;
    }

    Clip c;
    c.id = nextId_++;
    c.kind = in.kind;
    c.created = c.lastUsed = nowMs;
    c.seq = nextSeq_++;
    c.cut = in.cut;
    c.hash = hash;
    c.bytes = payload.size();
    c.source = std::move(in.source);
    if (in.kind == ClipKind::Image) {
        pngSize(in.png, &c.width, &c.height);
        if (!dir_.empty() && !writeFileAtomic(imagePath(c), in.png)) return 0;
    } else {
        c.text = std::move(in.text);
    }
    writeClip(c);
    apply(c);
    return c.id;
}

void ClipStore::touch(uint64_t id, int64_t nowMs) {
    Clip* c = findMut(id);
    if (!c) return;
    c->lastUsed = nowMs;
    c->seq = nextSeq_++;
    ++c->useCount;
    writeMeta(*c);
}

void ClipStore::setPinned(uint64_t id, bool pinned) {
    Clip* c = findMut(id);
    if (!c || c->pinned == pinned) return;
    c->pinned = pinned;
    writeMeta(*c);
}

void ClipStore::setFolder(uint64_t id, uint32_t folder) {
    Clip* c = findMut(id);
    if (!c || c->folder == folder || (folder && !findFolder(folder))) return;
    c->folder = folder;
    writeMeta(*c);
}

void ClipStore::setTitle(uint64_t id, std::string title) {
    Clip* c = findMut(id);
    if (!c || c->title == title) return;
    c->title = std::move(title);
    writeMeta(*c);
}

bool ClipStore::setText(uint64_t id, std::string text) {
    Clip* c = findMut(id);
    if (!c || c->kind != ClipKind::Text || text.empty()) return false;
    if (text == c->text) return true;
    const uint64_t hash = contentHash(ClipKind::Text, text);
    if (byHash_.count(hash)) return false;
    Clip edited = *c;
    edited.text = std::move(text);
    edited.hash = hash;
    edited.bytes = edited.text.size();
    writeClip(edited);
    apply(edited);
    return true;
}

void ClipStore::remove(uint64_t id) {
    auto it = byId_.find(id);
    if (it == byId_.end()) return;
    if (clips_[it->second].kind == ClipKind::Image) {
        std::error_code ec;
        fs::remove(imagePath(clips_[it->second]), ec);
    }
    Writer w;
    w.put<uint8_t>(static_cast<uint8_t>(Op::Remove));
    w.put<uint64_t>(id);
    append(w.out);
    eraseAt(it->second);
}

size_t ClipStore::clearHistory() {
    std::vector<uint64_t> ids;
    for (const Clip& c : clips_) {
        if (!c.kept()) ids.push_back(c.id);
    }
    for (uint64_t id : ids) remove(id);
    if (!ids.empty() && fileBytes_ > 2 * liveBytes()) compact();
    return ids.size();
}

uint32_t ClipStore::addFolder(std::string name) {
    Folder f{nextFolder_++, std::move(name)};
    append(encodeFolder(f));
    folders_.push_back(f);
    return f.id;
}

void ClipStore::renameFolder(uint32_t id, std::string name) {
    auto it = std::find_if(folders_.begin(), folders_.end(), [&](const Folder& f) { return f.id == id; });
    if (it == folders_.end() || it->name == name) return;
    it->name = std::move(name);
    append(encodeFolder(*it));
}

void ClipStore::removeFolder(uint32_t id) {
    if (!findFolder(id)) return;
    Writer w;
    w.put<uint8_t>(static_cast<uint8_t>(Op::RemoveFolder));
    w.put<uint32_t>(id);
    append(w.out);
    std::erase_if(folders_, [&](const Folder& f) { return f.id == id; });
    for (Clip& c : clips_) {
        if (c.folder == id) c.folder = 0;
    }
}

size_t ClipStore::prune(size_t maxClips) {
    std::vector<const Clip*> loose;
    for (const Clip& c : clips_) {
        if (!c.kept()) loose.push_back(&c);
    }
    if (loose.size() <= maxClips) return 0;
    std::sort(loose.begin(), loose.end(), [](const Clip* a, const Clip* b) { return a->seq < b->seq; });
    std::vector<uint64_t> ids;
    for (size_t i = 0; i < loose.size() - maxClips; ++i) ids.push_back(loose[i]->id);
    for (uint64_t id : ids) remove(id);
    return ids.size();
}

}  // namespace clipper
