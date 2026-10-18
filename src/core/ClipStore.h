#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Clip.h"

namespace clipper {

struct NewClip {
    ClipKind kind = ClipKind::Text;
    std::string text;  // text, or file:// uris one per line
    std::string png;   // image bytes
    bool cut = false;
    std::string source;
};

// the history, in memory, backed by an append-only journal in <dir>/history.bin.
// every change appends one small entry, startup replays them and compacts when most of the
// file is dead. images live next to it as <dir>/images/<hash>.png so the journal stays small.
// ui thread only
class ClipStore {
public:
    explicit ClipStore(std::string dir);
    ~ClipStore();
    ClipStore(const ClipStore&) = delete;
    ClipStore& operator=(const ClipStore&) = delete;

    // false + error if the journal can't be opened for writing. a damaged tail gets dropped, not reported
    bool load(std::string* error = nullptr);

    const std::vector<Clip>& clips() const { return clips_; }  // insertion order, use filterClips() to show them
    const Clip* find(uint64_t id) const;
    const std::vector<Folder>& folders() const { return folders_; }
    const Folder* findFolder(uint32_t id) const;
    uint64_t revision() const { return revision_; }  // changes whenever anything does
    size_t keptCount() const;

    // new clip on top. the same content again moves the existing clip to the top instead.
    // returns its id, 0 if empty or the image couldn't be saved
    uint64_t add(NewClip clip, int64_t nowMs);
    void touch(uint64_t id, int64_t nowMs);  // pasted / copied from clipper: to the top, useCount + 1
    void setPinned(uint64_t id, bool pinned);
    void setFolder(uint64_t id, uint32_t folder);
    void setTitle(uint64_t id, std::string title);
    bool setText(uint64_t id, std::string text);  // edit a text clip. false if that text is already another clip
    void remove(uint64_t id);
    size_t clearHistory();  // everything that isn't kept(), returns how many

    uint32_t addFolder(std::string name);
    void renameFolder(uint32_t id, std::string name);
    void removeFolder(uint32_t id);  // its clips stay, back in the plain history

    // drop the oldest clips that aren't kept() until at most maxClips of them are left
    size_t prune(size_t maxClips);

    std::string imagePath(const Clip& clip) const;  // "" for non-images
    bool readImage(const Clip& clip, std::string* png) const;

    // rewrite the journal with just what's alive. load() does it on its own when it's worth it
    bool compact();
    uint64_t fileBytes() const { return fileBytes_; }  // the journal
    uint64_t diskBytes() const;                         // journal + images

private:
    enum class Op : uint8_t { PutClip = 1, Meta = 2, Remove = 3, PutFolder = 4, RemoveFolder = 5 };

    Clip* findMut(uint64_t id);
    void apply(const Clip& clip);
    bool replay(const std::string& data, size_t* goodBytes);
    void append(const std::string& body);
    void writeClip(const Clip& clip);
    void writeMeta(const Clip& clip);
    void eraseAt(size_t index);
    void reindex();
    uint64_t liveBytes() const;

    std::string dir_;
    std::string journalPath_;
    std::FILE* journal_ = nullptr;
    uint64_t fileBytes_ = 0;

    std::vector<Clip> clips_;
    std::unordered_map<uint64_t, size_t> byId_;    // id -> index in clips_
    std::unordered_map<uint64_t, uint64_t> byHash_;  // content hash -> id
    std::vector<Folder> folders_;
    uint64_t nextId_ = 1;
    uint64_t nextSeq_ = 1;
    uint32_t nextFolder_ = 1;
    uint64_t revision_ = 1;
};

uint64_t contentHash(ClipKind kind, const std::string& payload);

}  // namespace clipper
