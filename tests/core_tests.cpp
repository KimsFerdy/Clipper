// no framework yet, CHECK + exit code is enough for now

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/ClipStore.h"
#include "core/FileList.h"
#include "core/Filter.h"
#include "core/Png.h"

namespace fs = std::filesystem;

static int g_failures = 0;

#define CHECK(expr)                                                                       \
    do {                                                                                  \
        if (!(expr)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

namespace {

// fresh dir per test, gone afterwards
struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("clipper_test_" + std::to_string(getpid()) + "_" + std::to_string(counter++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
    std::string str() const { return path.string(); }
    static inline int counter = 0;
};

clipper::NewClip text(const std::string& t, const std::string& source = "Gedit") {
    clipper::NewClip c;
    c.kind = clipper::ClipKind::Text;
    c.text = t;
    c.source = source;
    return c;
}

// smallest valid-looking png header: signature + IHDR with the size, enough for pngSize()
std::string fakePng(uint32_t w, uint32_t h) {
    std::string p = std::string("\x89PNG\r\n\x1a\n", 8) + std::string("\0\0\0\x0dIHDR", 8);
    for (uint32_t v : {w, h}) {
        p += static_cast<char>(v >> 24);
        p += static_cast<char>(v >> 16);
        p += static_cast<char>(v >> 8);
        p += static_cast<char>(v);
    }
    return p + std::string(13, '\0');
}

}  // namespace

static void testAddDedupAndOrder() {
    TempDir dir;
    clipper::ClipStore s(dir.str());
    CHECK(s.load());
    const uint64_t a = s.add(text("alpha"), 1000);
    const uint64_t b = s.add(text("beta"), 2000);
    CHECK(a && b && a != b);
    CHECK(s.clips().size() == 2);
    CHECK(s.add(text(""), 2500) == 0);  // nothing to save

    // same text again: no new clip, it moves to the top
    CHECK(s.add(text("alpha"), 3000) == a);
    CHECK(s.clips().size() == 2);
    CHECK(s.find(a)->useCount == 2);
    std::vector<uint64_t> ids = clipper::filterClips(s, {});
    CHECK(ids.size() == 2 && ids[0] == a && ids[1] == b);

    s.touch(b, 4000);
    ids = clipper::filterClips(s, {});
    CHECK(ids[0] == b);
}

static void testPersistence() {
    TempDir dir;
    uint64_t a, b, c;
    uint32_t folder;
    {
        clipper::ClipStore s(dir.str());
        CHECK(s.load());
        a = s.add(text("one"), 1);
        b = s.add(text("two"), 2);
        c = s.add(text("three"), 3);
        folder = s.addFolder("Snippets");
        s.setPinned(a, true);
        s.setFolder(b, folder);
        s.setTitle(b, "the second");
        s.remove(c);
        CHECK(s.setText(a, "one, edited"));
        CHECK(!s.setText(a, "two"));  // that's clip b already
        s.renameFolder(folder, "Code");
    }
    clipper::ClipStore s(dir.str());
    CHECK(s.load());
    CHECK(s.clips().size() == 2);
    CHECK(s.find(a) && s.find(a)->pinned && s.find(a)->text == "one, edited");
    CHECK(s.find(b) && s.find(b)->folder == folder && s.find(b)->title == "the second");
    CHECK(!s.find(c));
    CHECK(s.folders().size() == 1 && s.folders()[0].name == "Code");
    // ids keep counting after a reload, never reused
    const uint64_t d = s.add(text("four"), 4);
    CHECK(d > c);
    // dedup survives a reload too
    CHECK(s.add(text("two"), 5) == b);

    s.removeFolder(folder);
    CHECK(s.find(b)->folder == 0);
}

static void testTornTail() {
    TempDir dir;
    {
        clipper::ClipStore s(dir.str());
        CHECK(s.load());
        s.add(text("kept"), 1);
        s.add(text("also kept"), 2);
    }
    // crash mid-write: half an entry at the end
    {
        std::ofstream out(dir.path / "history.bin", std::ios::binary | std::ios::app);
        out.write("\x40\x00\x00\x00\x12\x34", 6);
    }
    clipper::ClipStore s(dir.str());
    CHECK(s.load());
    CHECK(s.clips().size() == 2);
    s.add(text("after the crash"), 3);  // the journal got rewritten, appending works again
    clipper::ClipStore again(dir.str());
    CHECK(again.load());
    CHECK(again.clips().size() == 3);
}

static void testNotOurFile() {
    TempDir dir;
    {
        std::ofstream out(dir.path / "history.bin", std::ios::binary);
        out << "this is not a clipper history file";
    }
    clipper::ClipStore s(dir.str());
    CHECK(s.load());
    CHECK(s.clips().empty());
    CHECK(fs::exists(dir.path / "history.bin.unreadable"));  // moved aside, not overwritten
}

static void testPruneAndClear() {
    TempDir dir;
    clipper::ClipStore s(dir.str());
    CHECK(s.load());
    uint64_t first = 0, pinned = 0;
    for (int i = 0; i < 10; ++i) {
        const uint64_t id = s.add(text("clip " + std::to_string(i)), i);
        if (i == 0) first = id;
        if (i == 1) pinned = id;
    }
    s.setPinned(pinned, true);
    CHECK(s.prune(5) == 4);  // 9 loose ones, keep 5
    CHECK(s.clips().size() == 6);
    CHECK(!s.find(first));
    CHECK(s.find(pinned));  // old but pinned
    CHECK(s.clearHistory() == 5);
    CHECK(s.clips().size() == 1 && s.keptCount() == 1);

    clipper::ClipStore again(dir.str());
    CHECK(again.load());
    CHECK(again.clips().size() == 1);
}

static void testCompaction() {
    TempDir dir;
    {
        clipper::ClipStore s(dir.str());
        CHECK(s.load());
        const std::string big(4096, 'x');
        for (int i = 0; i < 200; ++i) s.add(text(big + std::to_string(i)), i);
        s.clearHistory();  // compacts on the way out
        s.add(text("survivor"), 1000);
    }
    const auto size = fs::file_size(dir.path / "history.bin");
    CHECK(size < 4096);
    clipper::ClipStore s(dir.str());
    CHECK(s.load());
    CHECK(s.clips().size() == 1 && s.clips()[0].text == "survivor");
}

static void testImages() {
    TempDir dir;
    uint64_t id;
    {
        clipper::ClipStore s(dir.str());
        CHECK(s.load());
        clipper::NewClip c;
        c.kind = clipper::ClipKind::Image;
        c.png = fakePng(1920, 1080);
        id = s.add(c, 1);
        CHECK(id);
        const clipper::Clip* clip = s.find(id);
        CHECK(clip->width == 1920 && clip->height == 1080);
        CHECK(fs::exists(s.imagePath(*clip)));
        std::string back;
        CHECK(s.readImage(*clip, &back) && back == c.png);
        CHECK(s.add(c, 2) == id);  // same picture again
    }
    {
        clipper::ClipStore s(dir.str());
        CHECK(s.load());
        CHECK(s.find(id));
        const std::string path = s.imagePath(*s.find(id));
        s.remove(id);
        CHECK(!fs::exists(path));  // the png goes with it
    }
    // a png deleted by hand drops its clip on the next load
    {
        clipper::ClipStore s(dir.str());
        CHECK(s.load());
        clipper::NewClip c;
        c.kind = clipper::ClipKind::Image;
        c.png = fakePng(10, 10);
        id = s.add(c, 3);
        fs::remove(s.imagePath(*s.find(id)));
    }
    clipper::ClipStore s(dir.str());
    CHECK(s.load());
    CHECK(!s.find(id));
}

static void testFilter() {
    TempDir dir;
    clipper::ClipStore s(dir.str());
    CHECK(s.load());
    const uint64_t a = s.add(text("git commit -m \"Fix the build\"", "Gnome-terminal"), 1);
    const uint64_t b = s.add(text("https://example.com/build/42", "firefox"), 2);
    clipper::NewClip files;
    files.kind = clipper::ClipKind::Files;
    files.text = "file:///home/me/Report%202026.pdf\nfile:///home/me/notes.txt";
    const uint64_t f = s.add(files, 3);
    s.setPinned(b, true);

    clipper::ClipFilter q;
    q.query = "BUILD";
    std::vector<uint64_t> ids = clipper::filterClips(s, q);
    CHECK(ids.size() == 2 && ids[0] == b && ids[1] == a);
    q.query = "build fix";
    CHECK(clipper::filterClips(s, q) == std::vector<uint64_t>{a});
    q.query = "terminal";  // source app counts
    CHECK(clipper::filterClips(s, q) == std::vector<uint64_t>{a});
    q.query = "report 2026";  // decoded file names
    CHECK(clipper::filterClips(s, q) == std::vector<uint64_t>{f});

    q = {};
    q.view = clipper::ClipFilter::View::Pinned;
    CHECK(clipper::filterClips(s, q) == std::vector<uint64_t>{b});
    q = {};
    q.type = clipper::ClipFilter::Type::Files;
    CHECK(clipper::filterClips(s, q) == std::vector<uint64_t>{f});
}

static void testPreview() {
    clipper::Clip c;
    c.text = "  first line\n\n\tsecond   line  ";
    CHECK(clipper::previewLine(c, 100) == "first line second line");
    CHECK(clipper::previewLine(c, 5) == "first…");
    c.text = "ééééé";  // 2 bytes each, never cut in half
    CHECK(clipper::previewLine(c, 3) == "ééé…");

    c.kind = clipper::ClipKind::Files;
    c.text = "file:///home/me/a.txt\nfile:///home/me/My%20Folder";
    CHECK(clipper::previewLine(c, 100) == "2 items: a.txt, My Folder");
    c.kind = clipper::ClipKind::Image;
    c.width = 800;
    c.height = 600;
    CHECK(clipper::previewLine(c, 100) == "Image 800 × 600");
}

static void testFileList() {
    CHECK(clipper::pathToUri("/home/me/My File#1.txt") == "file:///home/me/My%20File%231.txt");
    CHECK(clipper::uriToPath("file:///home/me/My%20File%231.txt") == "/home/me/My File#1.txt");
    CHECK(clipper::uriToPath("file://localhost/tmp/x") == "/tmp/x");
    CHECK(clipper::uriToPath("file://otherhost/tmp/x").empty());
    CHECK(clipper::uriToPath("https://example.com/").empty());
    CHECK(clipper::uriToPath("file:///100%") == "/100%");  // broken escape stays as is

    bool cut = false;
    std::string uris;
    CHECK(clipper::parseCopiedFiles("cut\nfile:///a\nfile:///b\n", &cut, &uris));
    CHECK(cut && uris == "file:///a\nfile:///b");
    CHECK(clipper::parseCopiedFiles("copy\r\nfile:///a", &cut, &uris) && !cut && uris == "file:///a");
    CHECK(!clipper::parseCopiedFiles("hello\nworld", &cut, &uris));
    CHECK(clipper::formatCopiedFiles(false, "file:///a\nfile:///b") == "copy\nfile:///a\nfile:///b");
    CHECK(clipper::formatUriList("file:///a\nfile:///b") == "file:///a\r\nfile:///b\r\n");
    CHECK(clipper::allFileUris("# comment\r\nfile:///a\r\n"));
    CHECK(!clipper::allFileUris("file:///a\nhttps://b"));

    clipper::Clip c;
    c.kind = clipper::ClipKind::Files;
    c.text = "file:///tmp/a%20b";
    CHECK(clipper::clipPaths(c) == std::vector<std::string>{"/tmp/a b"});
    CHECK(clipper::baseName("/home/me/dir/") == "dir");
    CHECK(clipper::baseName("/") == "/");
}

static void testPngSize() {
    uint32_t w = 0, h = 0;
    CHECK(clipper::pngSize(fakePng(3840, 2160), &w, &h) && w == 3840 && h == 2160);
    CHECK(!clipper::pngSize("GIF89a not a png at all......", &w, &h));
}

int main() {
    testAddDedupAndOrder();
    testPersistence();
    testTornTail();
    testNotOurFile();
    testPruneAndClear();
    testCompaction();
    testImages();
    testFilter();
    testPreview();
    testFileList();
    testPngSize();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all core tests passed");
    return 0;
}
