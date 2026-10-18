#include "platform/X11Clipboard.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include "core/ClipStore.h"  // contentHash
#include "core/FileList.h"

namespace clipper {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxText = 16u << 20;    // a 16 MB log is a lot of clipboard already
constexpr size_t kMaxImage = 64u << 20;   // 8k screenshot as png is ~30 MB
constexpr size_t kChunk = 256u << 10;     // INCR chunk + threshold, well under any max request size
constexpr auto kReadTimeout = std::chrono::seconds(2);
constexpr auto kSendTimeout = std::chrono::seconds(10);
// when an app quits, muffin / mutter (and csd-clipboard) take its clipboard over and serve a copy, minus
// markers like the password hint. we give them this long before we put the last copy back ourselves
constexpr auto kRestoreDelay = std::chrono::milliseconds(300);

// requestor windows go away mid-transfer all the time (BadWindow). the default handler exits
// the process, so: ignore. errors on glfw's connection land here too, same deal
int ignoreXError(Display*, XErrorEvent*) { return 0; }

enum AtomId {
    CLIPBOARD,
    TARGETS,
    TIMESTAMP,
    MULTIPLE,
    INCR,
    UTF8_STRING,
    TEXT,
    TEXT_PLAIN,
    TEXT_PLAIN_UTF8,
    IMAGE_PNG,
    URI_LIST,
    GNOME_COPIED_FILES,
    PASSWORD_HINT,
    NET_ACTIVE_WINDOW,
    CLIPPER_DATA,
    CLIPPER_TIME,
    kAtomCount
};

const char* kAtomNames[kAtomCount] = {
    "CLIPBOARD",
    "TARGETS",
    "TIMESTAMP",
    "MULTIPLE",
    "INCR",
    "UTF8_STRING",
    "TEXT",
    "text/plain",
    "text/plain;charset=utf-8",
    "image/png",
    "text/uri-list",
    "x-special/gnome-copied-files",
    "x-kde-passwordManagerHint",  // keepassxc and friends set it on secrets
    "_NET_ACTIVE_WINDOW",
    "CLIPPER_DATA",
    "CLIPPER_TIME",
};

std::string latin1ToUtf8(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

bool isBlank(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; });
}

}  // namespace

struct X11Clipboard::Impl {
    X11Clipboard& self;
    Display* dpy = nullptr;
    Window root = 0, win = 0;
    Atom atoms[kAtomCount] = {};
    int xfixesEvent = 0;

    // reading someone else's clipboard, one transfer at a time
    enum class ReadState { Idle, Targets, Data, Incr };
    struct Read {
        ReadState state = ReadState::Idle;
        Atom target = 0;
        Time time = CurrentTime;
        std::string data;
        std::string source;
        bool secret = false;  // marked by a password manager: only its hash is kept, never the text
        Clock::time_point deadline;
    } read;
    bool readAgain = false;  // the owner changed again mid-read
    Time againTime = CurrentTime;

    // what we serve while we own CLIPBOARD / PRIMARY
    struct Format {
        Atom target;
        std::shared_ptr<const std::string> bytes;
    };
    std::vector<Format> formats;
    std::shared_ptr<const ClipboardData> offered;
    bool ownClipboard = false, ownPrimary = false;
    Time ownedSince = CurrentTime;

    // INCR sends in progress
    struct Transfer {
        Window requestor;
        Atom property, target;
        std::shared_ptr<const std::string> bytes;
        size_t offset = 0;
        Clock::time_point deadline;
    };
    std::vector<Transfer> transfers;

    // last thing copied, put back when its app quits and takes the clipboard with it.
    // lastIsCurrent: it's what's on the clipboard (false after a skipped secret / unknown format)
    std::shared_ptr<const ClipboardData> last;
    bool lastIsCurrent = false;
    bool restorePending = false;
    Clock::time_point restoreAt{};

    explicit Impl(X11Clipboard& s) : self(s) {}
    ~Impl() {
        if (dpy) XCloseDisplay(dpy);  // drops our selections with it
    }

    Atom A(AtomId id) const { return atoms[id]; }

    bool init() {
        dpy = XOpenDisplay(nullptr);
        if (!dpy) return false;
        XSetErrorHandler(ignoreXError);
        int errorBase = 0;
        if (!XFixesQueryExtension(dpy, &xfixesEvent, &errorBase)) return false;
        root = DefaultRootWindow(dpy);
        win = XCreateSimpleWindow(dpy, root, -10, -10, 1, 1, 0, 0, 0);
        XSelectInput(dpy, win, PropertyChangeMask);
        XInternAtoms(dpy, const_cast<char**>(kAtomNames), kAtomCount, False, atoms);
        XFixesSelectSelectionInput(dpy, root, A(CLIPBOARD),
                                   XFixesSetSelectionOwnerNotifyMask | XFixesSelectionWindowDestroyNotifyMask |
                                       XFixesSelectionClientCloseNotifyMask);
        XFlush(dpy);
        return true;
    }

    // ---- reading ----

    std::string activeWindowClass() {
        Atom type;
        int format;
        unsigned long count, after;
        unsigned char* prop = nullptr;
        Window active = 0;
        if (XGetWindowProperty(dpy, root, A(NET_ACTIVE_WINDOW), 0, 1, False, XA_WINDOW, &type, &format, &count, &after,
                               &prop) == Success &&
            prop) {
            if (count == 1 && format == 32) active = static_cast<Window>(*reinterpret_cast<unsigned long*>(prop));
            XFree(prop);
        }
        if (!active) return {};
        XClassHint hint{};
        std::string out;
        if (XGetClassHint(dpy, active, &hint)) {
            if (hint.res_class) out = hint.res_class;
            if (hint.res_name) XFree(hint.res_name);
            if (hint.res_class) XFree(hint.res_class);
        }
        return out;
    }

    void requestRead(Time time) {
        if (!self.capturing_) {
            lastIsCurrent = false;
            return;
        }
        if (read.state != ReadState::Idle) {
            readAgain = true;
            againTime = time;
            return;
        }
        read = Read{};
        read.state = ReadState::Targets;
        read.time = time;
        read.source = activeWindowClass();
        read.deadline = Clock::now() + kReadTimeout;
        XDeleteProperty(dpy, win, A(CLIPPER_DATA));
        XConvertSelection(dpy, A(CLIPBOARD), A(TARGETS), A(CLIPPER_DATA), win, time);
        XFlush(dpy);
    }

    // the whole property in one go (8 bit data), deleting it tells an INCR sender to go on
    bool takeProperty(Atom* type, std::string* out) {
        out->clear();
        long offset = 0;
        for (;;) {
            Atom t;
            int format;
            unsigned long count, after;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(dpy, win, A(CLIPPER_DATA), offset, 1 << 20, False, AnyPropertyType, &t, &format,
                                   &count, &after, &data) != Success)
                return false;
            *type = t;
            if (data) {
                const size_t unit = format == 32 ? sizeof(long) : static_cast<size_t>(format / 8);
                out->append(reinterpret_cast<char*>(data), count * unit);
                XFree(data);
            }
            if (after == 0) break;
            offset += static_cast<long>(count * static_cast<unsigned long>(format) / 32);  // offsets count 32 bit units
        }
        XDeleteProperty(dpy, win, A(CLIPPER_DATA));
        return true;
    }

    std::vector<Atom> takeAtoms() {
        std::vector<Atom> out;
        Atom type;
        int format;
        unsigned long count, after;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(dpy, win, A(CLIPPER_DATA), 0, 4096, True, AnyPropertyType, &type, &format, &count,
                               &after, &data) == Success &&
            data) {
            if (format == 32) {
                const auto* list = reinterpret_cast<const unsigned long*>(data);  // format 32 = longs in memory
                out.assign(list, list + count);
            }
            XFree(data);
        }
        return out;
    }

    // hashes of recent secrets. when the app quits, the wm serves the same text again without the
    // password hint, this is how we recognise it
    std::vector<uint64_t> secretHashes;

    // text first: apps that offer text *and* an image (office suites) mean the text.
    // files beat text, nemo also offers the paths as text
    Atom chooseTarget(const std::vector<Atom>& targets) const {
        auto has = [&](AtomId id) { return std::find(targets.begin(), targets.end(), A(id)) != targets.end(); };
        if (has(GNOME_COPIED_FILES)) return A(GNOME_COPIED_FILES);
        for (AtomId t : {UTF8_STRING, TEXT_PLAIN_UTF8, TEXT_PLAIN}) {
            if (has(t)) return A(t);
        }
        if (std::find(targets.begin(), targets.end(), XA_STRING) != targets.end()) return XA_STRING;
        if (self.captureImages_ && has(IMAGE_PNG)) return A(IMAGE_PNG);
        if (has(URI_LIST)) return A(URI_LIST);
        return None;
    }

    size_t limitFor(Atom target) const { return target == A(IMAGE_PNG) ? kMaxImage : kMaxText; }

    void onSelectionNotify(const XSelectionEvent& e) {
        if (e.requestor != win || e.selection != A(CLIPBOARD) || read.state == ReadState::Idle) return;
        if (e.property == None) return finishRead(false);

        if (read.state == ReadState::Targets) {
            const std::vector<Atom> targets = takeAtoms();
            read.secret = std::find(targets.begin(), targets.end(), A(PASSWORD_HINT)) != targets.end();
            const Atom target = chooseTarget(targets);
            if (target == None) return finishRead(false);
            read.target = target;
            read.state = ReadState::Data;
            read.deadline = Clock::now() + kReadTimeout;
            XConvertSelection(dpy, A(CLIPBOARD), target, A(CLIPPER_DATA), win, read.time);
            XFlush(dpy);
            return;
        }
        if (read.state == ReadState::Data) {
            Atom type = None;
            if (!takeProperty(&type, &read.data)) return finishRead(false);
            if (type == A(INCR)) {  // big one, comes in chunks. deleting the property (done) starts it
                read.data.clear();
                read.state = ReadState::Incr;
                read.deadline = Clock::now() + kReadTimeout;
                XFlush(dpy);
                return;
            }
            finishRead(read.data.size() <= limitFor(read.target));
        }
    }

    void onIncrChunk() {
        std::string chunk;
        Atom type = None;
        if (!takeProperty(&type, &chunk)) return finishRead(false);
        XFlush(dpy);
        if (chunk.empty()) return finishRead(true);
        read.data += chunk;
        read.deadline = Clock::now() + kReadTimeout;
        if (read.data.size() > limitFor(read.target)) finishRead(false);  // too big, the rest just gets ignored
    }

    void finishRead(bool ok) {
        if (ok) {
            ClipboardData d;
            d.source = std::move(read.source);
            const Atom t = read.target;
            if (t == A(GNOME_COPIED_FILES)) {
                d.kind = ClipKind::Files;
                ok = parseCopiedFiles(read.data, &d.cut, &d.text);
            } else if (t == A(URI_LIST)) {
                ok = allFileUris(read.data);
                d.kind = ClipKind::Files;
                for (const std::string& u : uriLines(read.data)) d.text += (d.text.empty() ? "" : "\n") + u;
            } else if (t == A(IMAGE_PNG)) {
                d.kind = ClipKind::Image;
                d.png = std::move(read.data);
                ok = !d.png.empty();
            } else {
                d.kind = ClipKind::Text;
                d.text = t == XA_STRING ? latin1ToUtf8(read.data) : std::move(read.data);
                while (!d.text.empty() && d.text.back() == '\0') d.text.pop_back();  // some apps send the nul
                ok = !d.text.empty() && !isBlank(d.text);
            }
            const std::string& payload = d.kind == ClipKind::Image ? d.png : d.text;
            const uint64_t hash = contentHash(d.kind, payload);
            if (read.secret) {
                secretHashes.push_back(hash);
                if (secretHashes.size() > 32) secretHashes.erase(secretHashes.begin());
                ok = false;
            } else if (std::find(secretHashes.begin(), secretHashes.end(), hash) != secretHashes.end()) {
                ok = false;  // a secret again, served by someone else
            } else if (ok && sameAsLast(d)) {
                // same thing again: the wm taking over from a closed app, or copying what we just pasted
                read = Read{};
                lastIsCurrent = true;
                return finishReadAgain();
            }
            if (ok) {
                last = std::make_shared<const ClipboardData>(d);
                {
                    std::lock_guard lock(self.mutex_);
                    self.captured_.push_back(std::move(d));
                }
                if (self.wake_) self.wake_();
            }
        }
        lastIsCurrent = ok;
        read = Read{};
        finishReadAgain();
    }

    // the wm takes over a closed app's clipboard with only the text: copied files come back as their
    // uris or paths. that's still the same copy
    bool sameAsLast(const ClipboardData& d) const {
        if (!last) return false;
        if (last->kind == d.kind) return d.kind == ClipKind::Image ? last->png == d.png : last->text == d.text;
        if (last->kind != ClipKind::Files || d.kind != ClipKind::Text) return false;
        std::string text = d.text;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        std::string paths;
        for (const std::string& u : uriLines(last->text)) {
            const std::string p = uriToPath(u);
            paths += (paths.empty() ? "" : "\n") + (p.empty() ? u : p);
        }
        return text == last->text || text == paths;
    }

    void finishReadAgain() {
        if (readAgain) {
            readAgain = false;
            requestRead(againTime);
        }
    }

    // ---- owning ----

    Time serverTime() {
        // the only way to get a timestamp out of the server: touch a property and look at the event
        XChangeProperty(dpy, win, A(CLIPPER_TIME), XA_STRING, 8, PropModeAppend, nullptr, 0);
        XEvent ev;
        XIfEvent(
            dpy, &ev,
            [](Display*, XEvent* e, XPointer arg) -> Bool {
                const auto* impl = reinterpret_cast<const Impl*>(arg);
                return e->type == PropertyNotify && e->xproperty.window == impl->win &&
                       e->xproperty.atom == impl->A(CLIPPER_TIME);
            },
            reinterpret_cast<XPointer>(this));
        return ev.xproperty.time;
    }

    void buildFormats(const ClipboardData& d) {
        formats.clear();
        auto add = [&](Atom target, std::shared_ptr<const std::string> bytes) { formats.push_back({target, bytes}); };
        switch (d.kind) {
            case ClipKind::Text: {
                auto text = std::make_shared<const std::string>(d.text);
                for (AtomId t : {UTF8_STRING, TEXT_PLAIN_UTF8, TEXT_PLAIN, TEXT}) add(A(t), text);
                add(XA_STRING, text);  // should be latin-1, nobody asks for it anymore
                break;
            }
            case ClipKind::Image:
                add(A(IMAGE_PNG), std::make_shared<const std::string>(d.png));
                break;
            case ClipKind::Files: {
                // a "copy" pasted into a file manager copies the files there. cut + paste moves them
                add(A(GNOME_COPIED_FILES), std::make_shared<const std::string>(formatCopiedFiles(d.cut, d.text)));
                add(A(URI_LIST), std::make_shared<const std::string>(formatUriList(d.text)));
                std::string paths;
                for (const std::string& u : uriLines(d.text)) {
                    const std::string p = uriToPath(u);
                    paths += (paths.empty() ? "" : "\n") + (p.empty() ? u : p);
                }
                auto text = std::make_shared<const std::string>(paths);  // pasting into an editor gives the paths
                for (AtomId t : {UTF8_STRING, TEXT_PLAIN_UTF8, TEXT_PLAIN}) add(A(t), text);
                break;
            }
        }
    }

    bool own(std::shared_ptr<const ClipboardData> data, bool primaryToo) {
        buildFormats(*data);
        offered = data;
        last = data;
        lastIsCurrent = true;
        ownedSince = serverTime();
        XSetSelectionOwner(dpy, A(CLIPBOARD), win, ownedSince);
        ownClipboard = XGetSelectionOwner(dpy, A(CLIPBOARD)) == win;
        if (primaryToo) {
            XSetSelectionOwner(dpy, XA_PRIMARY, win, ownedSince);
            ownPrimary = XGetSelectionOwner(dpy, XA_PRIMARY) == win;
        }
        XFlush(dpy);
        return ownClipboard;
    }

    void onSelectionRequest(const XSelectionRequestEvent& r) {
        XSelectionEvent reply{};
        reply.type = SelectionNotify;
        reply.display = dpy;
        reply.requestor = r.requestor;
        reply.selection = r.selection;
        reply.target = r.target;
        reply.property = None;  // = refused
        reply.time = r.time;

        const bool owned = (r.selection == A(CLIPBOARD) && ownClipboard) || (r.selection == XA_PRIMARY && ownPrimary);
        const Atom prop = r.property != None ? r.property : r.target;  // ancient clients send None
        if (owned) {
            if (r.target == A(TARGETS)) {
                std::vector<unsigned long> list = {A(TARGETS), A(TIMESTAMP)};
                for (const Format& f : formats) list.push_back(f.target);
                XChangeProperty(dpy, r.requestor, prop, XA_ATOM, 32, PropModeReplace,
                                reinterpret_cast<unsigned char*>(list.data()), static_cast<int>(list.size()));
                reply.property = prop;
            } else if (r.target == A(TIMESTAMP)) {
                unsigned long t = ownedSince;
                XChangeProperty(dpy, r.requestor, prop, XA_INTEGER, 32, PropModeReplace,
                                reinterpret_cast<unsigned char*>(&t), 1);
                reply.property = prop;
            } else {
                auto it = std::find_if(formats.begin(), formats.end(), [&](const Format& f) { return f.target == r.target; });
                if (it != formats.end()) {
                    const std::string& bytes = *it->bytes;
                    if (bytes.size() > kChunk) {
                        // announce the size, then a chunk every time they delete the property
                        XSelectInput(dpy, r.requestor, PropertyChangeMask);
                        unsigned long size = bytes.size();
                        XChangeProperty(dpy, r.requestor, prop, A(INCR), 32, PropModeReplace,
                                        reinterpret_cast<unsigned char*>(&size), 1);
                        transfers.push_back({r.requestor, prop, r.target, it->bytes, 0, Clock::now() + kSendTimeout});
                    } else {
                        XChangeProperty(dpy, r.requestor, prop, r.target, 8, PropModeReplace,
                                        reinterpret_cast<const unsigned char*>(bytes.data()),
                                        static_cast<int>(bytes.size()));
                    }
                    reply.property = prop;
                }
            }
        }
        XSendEvent(dpy, r.requestor, False, NoEventMask, reinterpret_cast<XEvent*>(&reply));
        XFlush(dpy);
    }

    void onRequestorPropertyDeleted(Window w, Atom property) {
        auto it = std::find_if(transfers.begin(), transfers.end(),
                               [&](const Transfer& t) { return t.requestor == w && t.property == property; });
        if (it == transfers.end()) return;
        const std::string& bytes = *it->bytes;
        const size_t n = std::min(kChunk, bytes.size() - it->offset);
        // n == 0: the empty property that says "that was all"
        XChangeProperty(dpy, w, property, it->target, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(bytes.data() + it->offset), static_cast<int>(n));
        it->offset += n;
        it->deadline = Clock::now() + kSendTimeout;
        if (n == 0) endTransfer(it);
        XFlush(dpy);
    }

    void endTransfer(std::vector<Transfer>::iterator it) {
        const Window w = it->requestor;
        transfers.erase(it);
        const bool more = std::any_of(transfers.begin(), transfers.end(), [&](const Transfer& t) { return t.requestor == w; });
        if (!more) XSelectInput(dpy, w, NoEventMask);
    }

    // ---- loop ----

    void handle(XEvent& ev) {
        if (ev.type == xfixesEvent + XFixesSelectionNotify) {
            const auto& e = reinterpret_cast<XFixesSelectionNotifyEvent&>(ev);
            if (e.selection != A(CLIPBOARD)) return;
            if (e.subtype != XFixesSetSelectionOwnerNotify) {
                // the owner quit. the wm usually takes over in a moment, if not we put ours back
                restorePending = last && lastIsCurrent;
                restoreAt = Clock::now() + kRestoreDelay;
                return;
            }
            if (e.owner == win) return;
            restorePending = false;  // somebody has it
            if (e.owner != None) requestRead(e.selection_timestamp);
            return;
        }
        switch (ev.type) {
            case SelectionNotify:
                onSelectionNotify(ev.xselection);
                break;
            case SelectionRequest:
                onSelectionRequest(ev.xselectionrequest);
                break;
            case SelectionClear:
                if (ev.xselectionclear.selection == A(CLIPBOARD)) ownClipboard = false;
                if (ev.xselectionclear.selection == XA_PRIMARY) ownPrimary = false;
                break;
            case PropertyNotify:
                if (ev.xproperty.window == win) {
                    if (ev.xproperty.atom == A(CLIPPER_DATA) && ev.xproperty.state == PropertyNewValue &&
                        read.state == ReadState::Incr)
                        onIncrChunk();
                } else if (ev.xproperty.state == PropertyDelete) {
                    onRequestorPropertyDeleted(ev.xproperty.window, ev.xproperty.atom);
                }
                break;
            default:
                break;
        }
    }

    void checkTimeouts() {
        const auto now = Clock::now();
        if (read.state != ReadState::Idle && now > read.deadline) finishRead(false);  // owner never answered
        if (restorePending && now > restoreAt) {
            restorePending = false;
            if (XGetSelectionOwner(dpy, A(CLIPBOARD)) == None) own(last, false);
        }
        for (auto it = transfers.begin(); it != transfers.end();) {
            if (now > it->deadline) {
                endTransfer(it);
                it = transfers.begin();
            } else {
                ++it;
            }
        }
    }
};

X11Clipboard::X11Clipboard(std::function<void()> wake) : wake_(std::move(wake)) {
    wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    thread_ = std::thread([this] { run(); });
}

X11Clipboard::~X11Clipboard() {
    stop_ = true;
    wakeThread();
    if (thread_.joinable()) thread_.join();
    if (wakeFd_ >= 0) close(wakeFd_);
}

void X11Clipboard::wakeThread() {
    if (wakeFd_ < 0) return;
    const uint64_t one = 1;
    [[maybe_unused]] ssize_t n = write(wakeFd_, &one, sizeof one);
}

std::vector<ClipboardData> X11Clipboard::takeCaptured() {
    std::lock_guard lock(mutex_);
    return std::exchange(captured_, {});
}

bool X11Clipboard::offer(ClipboardData data, bool primaryToo) {
    if (!available_) return false;
    auto cmd = std::make_unique<Command>();
    cmd->data = std::make_shared<const ClipboardData>(std::move(data));
    cmd->primaryToo = primaryToo;
    std::future<bool> done = cmd->done.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.push_back(std::move(cmd));
    }
    wakeThread();
    return done.wait_for(std::chrono::seconds(1)) == std::future_status::ready && done.get();
}

void X11Clipboard::run() {
    Impl x(*this);
    if (!x.init()) {
        // still answer offer() calls with "no"
        while (!stop_) {
            std::vector<std::unique_ptr<Command>> cmds;
            {
                std::lock_guard lock(mutex_);
                cmds = std::exchange(commands_, {});
            }
            for (auto& c : cmds) c->done.set_value(false);
            pollfd p{wakeFd_, POLLIN, 0};
            poll(&p, 1, 250);
            uint64_t n;
            [[maybe_unused]] ssize_t r = read(wakeFd_, &n, sizeof n);
        }
        return;
    }
    available_ = true;

    // whatever is on the clipboard right now counts as the first copy
    if (Window owner = XGetSelectionOwner(x.dpy, x.A(CLIPBOARD)); owner != None) x.requestRead(CurrentTime);

    const int xfd = ConnectionNumber(x.dpy);
    while (!stop_) {
        std::vector<std::unique_ptr<Command>> cmds;
        {
            std::lock_guard lock(mutex_);
            cmds = std::exchange(commands_, {});
        }
        for (auto& c : cmds) c->done.set_value(x.own(c->data, c->primaryToo));

        while (XPending(x.dpy)) {
            XEvent ev;
            XNextEvent(x.dpy, &ev);
            x.handle(ev);
        }
        x.checkTimeouts();

        pollfd fds[2] = {{xfd, POLLIN, 0}, {wakeFd_, POLLIN, 0}};
        poll(fds, 2, 200);
        if (fds[1].revents & POLLIN) {
            uint64_t n;
            [[maybe_unused]] ssize_t r = read(wakeFd_, &n, sizeof n);
        }
    }
    available_ = false;
}

}  // namespace clipper
