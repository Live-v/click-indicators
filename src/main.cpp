#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#if __has_include(<Geode/ui/GeodeUI.hpp>)
    #include <Geode/ui/GeodeUI.hpp>
    #define HAS_GEODE_UI 1
#endif
// cheat api ships its header under two different paths depending on version,
// so try both. if neither is there this compiles to nothing and the indicator
// silently never lights up, which is exactly what was happening.
#if __has_include(<legowiifun.cheat_api/include/cheatAPI.hpp>)
    #include <legowiifun.cheat_api/include/cheatAPI.hpp>
    #define HAS_CHEAT_API 1
#elif __has_include(<legowiifun.cheatAPI/include/cheatAPI.hpp>)
    #include <legowiifun.cheatAPI/include/cheatAPI.hpp>
    #define HAS_CHEAT_API 1
#endif

#include "gdr2.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

using namespace geode::prelude;

constexpr int kWorldZ = 500;
constexpr int kLaneZ  = 1000;
// a tap is short in TIME not frames, 1200tps macros exist and they broke this
constexpr double kDefaultTapMs = 45.0;

constexpr float kMaxStep = 0.1f;

// speed portals or sum
constexpr float kSpeeds[5] = { 251.16f, 311.58f, 387.42f, 468.00f, 576.00f };
constexpr int   kSpeedIDs[5] = { 200, 201, 202, 203, 1334 };

// how long stuff is
constexpr double kFlashTime = 0.28;

// between 0.36 and 0.44 i guess, what the fuck dude
constexpr float kBodyWidth = 0.44f;

// how many frames counts as a tap on THIS macro
static uint64_t tapFrames(double fps) {
    const double ms = double(Mod::get()->getSettingValue<int64_t>("tap-ms"));
    return uint64_t(std::max(1.0, fps * ms / 1000.0));
}

// press state.
enum : uint8_t { kPending = 0, kActive = 1, kDone = 2 };

// loadin up macros

static std::filesystem::path macroDir() {
    auto p = Mod::get()->getConfigDir() / "macros";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
}

static std::vector<uint8_t> readFile(std::filesystem::path const& p) {
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

// which file the user explicitly picked for this level, if any
static std::string levelKey(GJGameLevel* level) {
    if (!level) return "macro-none";
    const int id = level->m_levelID.value();
    if (id > 0) return "macro-" + std::to_string(id);
    std::string s = "macro-local-" + std::string(level->m_levelName);
    for (auto& c : s) if (!isalnum((unsigned char)c) && c != '-') c = '_';
    return s;
}

static std::string pickedMacro(GJGameLevel* level) {
    return Mod::get()->getSavedValue<std::string>(levelKey(level), "");
}

static void setPickedMacro(GJGameLevel* level, std::string const& file) {
    Mod::get()->setSavedValue<std::string>(levelKey(level), file);
}

// every parseable .gdr2 in the folder, for the ui list
struct MacroEntry {
    std::string file;
    size_t presses = 0;
    double fps = 240.0;
    double seconds = 0.0;
    uint32_t levelID = 0;
    std::string levelName;
    size_t inputs = 0;
};

static std::vector<MacroEntry> listMacros() {
    std::vector<MacroEntry> out;
    std::error_code ec;
    for (auto const& e : std::filesystem::directory_iterator(macroDir(), ec)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension().string() != ".gdr2") continue;
        auto rep = gdr2::parse(readFile(e.path()));
        if (!rep) continue;
        MacroEntry m;
        m.file = e.path().filename().string();
        m.presses = rep->holds().size();
        m.inputs = rep->inputs.size();
        m.fps = rep->framerate;
        m.seconds = rep->seconds();
        m.levelID = rep->levelID;
        m.levelName = rep->levelName;
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(),
              [](MacroEntry const& a, MacroEntry const& b) { return a.file < b.file; });
    return out;
}

static std::optional<gdr2::Replay> findMacro(GJGameLevel* level) {
    // an explicit pick from the pause menu beats any automatic match
    const auto forced = pickedMacro(level);
    if (!forced.empty()) {
        const auto p = macroDir() / forced;
        std::error_code fec;
        if (std::filesystem::exists(p, fec)) {
            auto rep = gdr2::parse(readFile(p));
            if (rep) {
                log::info("macro (picked): {} | {} inputs | {} tps | {:.2f}s",
                          forced, rep->inputs.size(), rep->framerate, rep->seconds());
                return rep;
            }
            log::warn("picked macro {} will not parse, falling back", forced);
        } else {
            log::warn("picked macro {} is gone, falling back", forced);
        }
    }

    const int wantID = level->m_levelID.value();
    const std::string wantName = level->m_levelName;
    log::info("level {} \"{}\" | scanning {}", wantID, wantName, macroDir().string());

    std::error_code ec;
    for (auto const& e : std::filesystem::directory_iterator(macroDir(), ec)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension().string() != ".gdr2") continue;
        const auto fname = e.path().filename().string();

        auto rep = gdr2::parse(readFile(e.path()));
        if (!rep) { log::warn("  {} failed to parse", fname); continue; }

        const bool byID   = wantID > 0 && int(rep->levelID) == wantID;
        const bool byStem = e.path().stem().string() == std::to_string(wantID);
        const bool byName = wantID <= 0 && !rep->levelName.empty() && rep->levelName == wantName;
        if (!(byID || byStem || byName)) continue;

        log::info("macro: {} | {} inputs | {} tps | {:.2f}s | bot {}",
                  fname, rep->inputs.size(), rep->framerate, rep->seconds(), rep->botName);
        return rep;
    }
    log::info("no macro for this level");
    return std::nullopt;
}

// settings

static ccColor4F tint(char const* colourKey, char const* transKey) {
    const auto c = Mod::get()->getSettingValue<ccColor3B>(colourKey);
    const auto t = Mod::get()->getSettingValue<int64_t>(transKey);   // 0 solid, 100 gone
    const float a = 1.f - float(std::clamp<int64_t>(t, 0, 100)) / 100.f;
    return { c.r / 255.f, c.g / 255.f, c.b / 255.f, a };
}

struct Look {
    ccColor4F indicator, p2, player, lane, laneP2;
    float width, ahead;
    double offset;
    bool  showPlayer, showLane, laneLeft, showAccuracy, splitLane, edgeMarker;
    bool  holdTracks, pressBadge, playerSquare, circleNotes;
    float badgeSize, laneWindow, laneScale, laneDepth;
    int   perfectFrames, okFrames, maxNotes;
};

static Look readSettings() {
    // reads every setting fresh. call readSettingsCached() in the hot path.
    Look l;
    l.indicator     = tint("color", "transparency");
    l.p2            = tint("p2-color", "p2-transparency");
    l.player        = tint("player-color", "player-transparency");
    l.lane          = tint("color", "lane-transparency");
    l.laneP2        = tint("p2-color", "lane-transparency");
    l.width         = float(Mod::get()->getSettingValue<double>("line-width"));
    l.ahead         = float(Mod::get()->getSettingValue<double>("look-ahead"));
    l.offset        = double(Mod::get()->getSettingValue<int64_t>("timing-offset")) / 1000.0;
    l.showPlayer    = Mod::get()->getSettingValue<bool>("player-line");
    l.showLane      = Mod::get()->getSettingValue<bool>("lane");
    l.laneLeft      = Mod::get()->getSettingValue<std::string>("lane-side") != "Right";
    l.laneWindow    = float(Mod::get()->getSettingValue<double>("lane-window"));
    l.laneScale     = float(Mod::get()->getSettingValue<double>("lane-scale"));
    l.laneDepth     = float(Mod::get()->getSettingValue<double>("lane-depth"));
    l.circleNotes   = Mod::get()->getSettingValue<std::string>("note-shape") == "Circle";
    l.splitLane     = Mod::get()->getSettingValue<bool>("p2-lane");
    l.edgeMarker    = Mod::get()->getSettingValue<bool>("edge-marker");
    l.holdTracks    = Mod::get()->getSettingValue<bool>("hold-tracks");
    l.pressBadge    = Mod::get()->getSettingValue<bool>("press-badge");
    l.playerSquare  = Mod::get()->getSettingValue<bool>("player-square");
    l.badgeSize     = float(Mod::get()->getSettingValue<double>("badge-size"));
    l.showAccuracy  = Mod::get()->getSettingValue<bool>("accuracy");
    l.perfectFrames = int(Mod::get()->getSettingValue<int64_t>("perfect-window"));
    l.okFrames      = int(Mod::get()->getSettingValue<int64_t>("ok-window"));
    l.maxNotes      = int(Mod::get()->getSettingValue<int64_t>("max-notes"));
    return l;
}

// bits

// GD's CCDrawNode blends with GL_ONE as the source factor, so alpha only eats
// i did this with Claude Code lord forgive me
static CCDrawNode* makeNode() {
    auto n = CCDrawNode::create();
    n->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
    return n;
}

static void poly(CCDrawNode* n, CCPoint a, CCPoint b, CCPoint c, CCPoint d, ccColor4F col) {
    if (col.a <= 0.002f) return;
    CCPoint v[4] = { a, b, c, d };
    n->drawPolygon(v, 4, col, 0.f, { 0.f, 0.f, 0.f, 0.f });
}

static void quad(CCDrawNode* n, float x0, float x1, float y0, float y1, ccColor4F c) {
    if (x1 <= x0 || y1 <= y0) return;
    poly(n, { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 }, c);
}

// Thick line between two points, for the chevrons.
static void stroke(CCDrawNode* n, CCPoint a, CCPoint b, float t, ccColor4F c) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.001f) return;
    const float nx = -dy / len * t, ny = dx / len * t;
    poly(n, { a.x + nx, a.y + ny }, { a.x - nx, a.y - ny },
            { b.x - nx, b.y - ny }, { b.x + nx, b.y + ny }, c);
}

// one convex polygon shaped like a pill. has to be a single shape or the
// caps end up on a different layer than the body and you get a white seam
// straight through the middle of every hold.
static void stadium(CCDrawNode* n, float cx, float yBot, float yTop, float w, ccColor4F c) {
    if (c.a <= 0.002f || w <= 0.f) return;
    const int SEG = 14;
    const float r = w * 0.5f;
    const float PI = 3.14159265f;
    if (yTop < yBot) std::swap(yTop, yBot);

    CCPoint v[2 * (SEG + 1)];
    int k = 0;
    for (int i = 0; i <= SEG; ++i) {           // bottom cap, left round to right
        const float a = PI + PI * float(i) / float(SEG);
        v[k++] = CCPoint(cx + r * std::cos(a), yBot + r * std::sin(a));
    }
    for (int i = 0; i <= SEG; ++i) {           // top cap, right round to left
        const float a = PI * float(i) / float(SEG);
        v[k++] = CCPoint(cx + r * std::cos(a), yTop + r * std::sin(a));
    }
    n->drawPolygon(v, unsigned(k), c, 0.f, { 0.f, 0.f, 0.f, 0.f });
}

// Hollow square, so the level still reads through the middle.
static void square(CCDrawNode* n, float cx, float cy, float half, float t, ccColor4F c) {
    quad(n, cx - half, cx + half, cy + half - t, cy + half, c);
    quad(n, cx - half, cx + half, cy - half, cy - half + t, c);
    quad(n, cx - half, cx - half + t, cy - half, cy + half, c);
    quad(n, cx + half - t, cx + half, cy - half, cy + half, c);
}

// green when perfect, then yellow sliding to orange across the ok window,
// red once you are outside it. how far off you were, as a colour.
static ccColor4F verdictColour(int off, int perfect, int ok) {
    if (off <= perfect) return { 0.28f, 1.00f, 0.42f, 1.f };
    if (off <= ok) {
        const float t = (ok > perfect)
                      ? float(off - perfect) / float(ok - perfect) : 0.f;
        return { 1.00f, 0.90f - 0.42f * t, 0.18f - 0.12f * t, 1.f };
    }
    return { 1.00f, 0.26f, 0.30f, 1.f };
}

static ccColor3B toC3B(ccColor4F c) {
    return { GLubyte(std::clamp(c.r, 0.f, 1.f) * 255),
             GLubyte(std::clamp(c.g, 0.f, 1.f) * 255),
             GLubyte(std::clamp(c.b, 0.f, 1.f) * 255) };
}

static ccColor4F lighten(ccColor4F c, float k) {
    return { std::clamp(c.r * k, 0.f, 1.f), std::clamp(c.g * k, 0.f, 1.f),
             std::clamp(c.b * k, 0.f, 1.f), c.a };
}

static float snapSpeed(float v) {
    float best = kSpeeds[1], bestErr = 1e9f;
    for (float s : kSpeeds) {
        const float e = std::abs(s - v);
        if (e < bestErr) { bestErr = e; best = s; }
    }
    return best;
}

// this one too because im a bum

// android runs this every frame on a phone cpu, so only actually hit the
// settings store a few times a second. 0.2s is way below noticing it.
//
// BUGFIX: this used std::clock(), which on POSIX measures CPU time consumed
// by the process, not wall-clock time. It drifts from real time under load
// and barely advances while the app is backgrounded, so the "refresh every
// 0.2s" interval was unreliable (and the "t < nextAt - 5.0" branch was a
// workaround for clock_t wraparound that a steady clock doesn't even need).
// std::chrono::steady_clock is monotonic wall time and can't wrap here.
static Look readSettingsCached() {
    static Look cached = readSettings();
    static std::chrono::steady_clock::time_point nextAt{};
    const auto now = std::chrono::steady_clock::now();
    if (now >= nextAt) {
        nextAt = now + std::chrono::milliseconds(200);
        cached = readSettings();
    }
    return cached;
}


// mod resources come out as "<modid>/<file>"
static const char* kDisc   = "bogdoner.click-indicators/taikohitcircle.png";
static const char* kRing   = "bogdoner.click-indicators/taikohitcircleoverlay.png";

class $modify(IndicatorLayer, PlayLayer) {
    struct Portal { float x; float v; };
    struct Seg    { double t; float x; float v; };

    struct Fields {
        std::vector<gdr2::Hold> holds;
        std::vector<uint8_t>    state;    // pending / active / done
        std::vector<uint8_t>    judged;   // has the scorer looked at it
        std::vector<Portal>     portals;
        std::vector<Seg>        segs;
        float levelEndX = 0.f;   // furthest object, used to sanity check the walk
        bool  portalsScanned = false;
        int   scanTries = 0;
        double macroSeconds = 0.0;

        CCDrawNode*     world = nullptr;
        CCDrawNode*     lane  = nullptr;
        CCLabelBMFont*  verdict = nullptr;
        CCLabelBMFont*  tally = nullptr;

        double fps = 240.0;
        bool   active = false;
        bool   hasP2 = false;

        double levelTime = 0.0;    // wall clock for fades, not for macro timing
        float  startX = 0.f;
        float  speed = kSpeeds[1];
        float  lastX = 0.f;
        double lastSpeedTime = 0.0;

        // Position fixes where the attempt begins; elapsed time carries it from
        // there. Model error then applies once instead of compounding.
        double clockBase = 0.0;
        double clockAccum = 0.0;
        bool   clockBased = false;
        double lastDriftLog = 0.0;
        float  startSpeed = kSpeeds[1];
        bool   startSpeedKnown = false;
        std::string speedKey;
        double nextCheckAt = 0.0;
        bool   posClock = false;   // false until the level scan succeeds
        double fallbackNow = 0.0;  // accumulated clock, used only if it does not

        size_t cursor = 0;
        size_t missCursor = 0;

        int nPerfect = 0, nOk = 0, nMiss = 0;
        double verdictAt = -10.0;
        double flashAt = -10.0;
        ccColor4F flashCol = { 1.f, 1.f, 1.f, 1.f };

        static constexpr int kQueue = 32;
        double  pressTimes[kQueue] = {};
        uint8_t pressIsP2[kQueue] = {};
        std::atomic<int> pressWrite{ 0 };
        int    pressRead = 0;

        std::atomic<int> holdMask{ 0 };
        bool cheatFlagged = false;
        bool noteAtLine = false;

        // real sprites for the osu circles. drawDot gives you a blob, these
        // give you an actual circle because they ARE one.
        CCNode* spriteHost = nullptr;
        std::vector<CCSprite*> discs, rings;
        size_t discUsed = 0, ringUsed = 0;   // bit 0 p1 down, bit 1 p2 down
    };

    // ------------------------------------------------------------ setup

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        // BUGFIX: this used to bail out here if "enabled" was off, which meant
        // the draw nodes and macro were never built for this level session.
        // Flipping "Master enable" back on mid-level then did nothing until
        // you left and re-entered, contradicting the setting's own description
        // ("Applies immediately, no level reload."). Node creation and macro
        // loading now always happen; postUpdate()'s live "enabled" check is
        // what actually gates drawing/scoring, so toggling works instantly.
        auto f = m_fields.self();

        // name for local levels, id for levels on servers, fuck startpos copies they ruin everything angry emoji
        f->speedKey = "startspeed-" + (level->m_levelID.value() > 0
                        ? std::to_string(level->m_levelID.value())
                        : ("local-" + std::string(level->m_levelName)));
        const double cached = Mod::get()->getSavedValue<double>(f->speedKey, 0.0);
        if (cached > 100.0) {
            f->startSpeed = float(cached);
            f->startSpeedKnown = true;
            log::info("start speed {:.0f} u/s from cache", f->startSpeed);
        }

        // built even when no macro matched. picking one from the pause menu
        // later flips active on, and it would explode on null nodes otherwise.
        f->world = makeNode();
        m_objectLayer->addChild(f->world, kWorldZ);

        f->lane = makeNode();
        this->addChild(f->lane, kLaneZ);

        f->spriteHost = CCNode::create();
        this->addChild(f->spriteHost, kLaneZ + 1);

        f->verdict = CCLabelBMFont::create("", "chatFont.fnt");
        f->verdict->setScale(0.42f);
        f->verdict->setAnchorPoint({ 0.5f, 0.5f });
        this->addChild(f->verdict, kLaneZ + 1);

        f->tally = CCLabelBMFont::create("", "chatFont.fnt");
        f->tally->setScale(0.34f);
        f->tally->setAnchorPoint({ 0.5f, 0.5f });
        f->tally->setOpacity(150);
        this->addChild(f->tally, kLaneZ + 1);

        auto rep = findMacro(level);
        if (!rep) {
            log::info("no macro for this level, nodes ready for a manual pick");
            return true;
        }
        if (rep->platformer) { log::info("platformer macro, skipped"); return true; }

        f->fps   = rep->framerate > 0 ? rep->framerate : 240.0;
        f->holds = rep->holds();
        f->state.assign(f->holds.size(), kPending);
        f->judged.assign(f->holds.size(), 0);
        for (auto const& h : f->holds)
            if (h.player2) { f->hasP2 = true; break; }
        f->macroSeconds = double(rep->lastFrame()) / f->fps;

        f->active = true;
        log::info("{} presses ready ({}), macro runs {:.2f}s",
                  f->holds.size(), f->hasP2 ? "dual" : "single", f->macroSeconds);
        return true;
    }

    // Objects do not exist during PlayLayer::init, GD builds them afterwards,
    // so this runs from the update loop and retries until the level is there.
    void tryScanPortals() {
        auto f = m_fields.self();
        if (f->portalsScanned) return;
        if (++f->scanTries > 300) {
            f->portalsScanned = true;
            log::warn("gave up looking for level objects, assuming a flat 1x level");
            return;
        }
        if (!m_objects || m_objects->count() == 0) return;

        scanPortals();
        f->portalsScanned = true;

        const double walk = timeToReach(f->levelEndX);
        if (walk > 1.0) f->posClock = true;
        else log::warn("walk came out at {:.2f}s, keeping the accumulated clock", walk);
        // if macro shorter its fine because end screens, if longer "fuck" i guesss
        const bool bad = walk > 1.0 && f->macroSeconds > walk * 1.02;
        log::info("scanned {} objects on try {}: {} speed portals, level ends at x {:.0f}",
                  m_objects->count(), f->scanTries, f->portals.size(), f->levelEndX);
        log::info("portal walk check: whole level {:.2f}s vs macro {:.2f}s  ratio {:.3f}  "
                  "start speed {:.0f}{}",
                  walk, f->macroSeconds,
                  f->macroSeconds > 0.01 ? walk / f->macroSeconds : 0.0,
                  levelStartSpeed(),
                  bad ? "  <-- macro longer than the level, wrong macro?" : "  ok");
        log::info("clock source: {}, start speed {:.0f} u/s ({})",
                  f->posClock ? "player position" : "accumulated time",
                  levelStartSpeed(),
                  f->startSpeedKnown ? "known" : "ASSUMED, play once from the start");
        if (!f->portals.empty())
            log::info("first portals: {:.0f}@{:.0f}  {:.0f}@{:.0f}",
                      f->portals[0].v, f->portals[0].x,
                      f->portals.size() > 1 ? f->portals[1].v : 0.f,
                      f->portals.size() > 1 ? f->portals[1].x : 0.f);

        // if statement after if statement this does something important
    }

    void scanPortals() {
        auto f = m_fields.self();
        if (!m_objects) return;
        for (unsigned i = 0; i < m_objects->count(); ++i) {
            auto obj = static_cast<GameObject*>(m_objects->objectAtIndex(i));
            if (!obj) continue;
            for (int k = 0; k < 5; ++k)
                if (obj->m_objectID == kSpeedIDs[k]) {
                    f->portals.push_back({ obj->getPositionX(), kSpeeds[k] });
                    break;
                }
            if (obj->getPositionX() > f->levelEndX) f->levelEndX = obj->getPositionX();
        }
        std::sort(f->portals.begin(), f->portals.end(),
                  [](Portal const& a, Portal const& b) { return a.x < b.x; });

        // checks speed and x = 0 and y = 0 on start otherwise gay
        f->segs.clear();
        float x = 0.f, v = levelStartSpeed();
        double t = 0.0;
        f->segs.push_back({ 0.0, 0.f, v });
        for (auto const& p : f->portals) {
            if (p.x <= x) { f->segs.back().v = p.v; v = p.v; continue; }
            t += double(p.x - x) / double(v);
            x = p.x; v = p.v;
            f->segs.push_back({ t, x, v });
        }
    }

    // macro position time + position time checks macro which is a bi-curious relationship!
    double tAtX(float px) {
        auto const& S = m_fields->segs;
        if (S.empty()) return 0.0;
        size_t lo = 0, hi = S.size() - 1;
        while (lo < hi) {
            const size_t mid = (lo + hi + 1) / 2;
            if (S[mid].x <= px) lo = mid; else hi = mid - 1;
        }
        return S[lo].t + double(px - S[lo].x) / double(S[lo].v);
    }

    // tells megahack / eclipse / whatever that this is on, via cheat api.
    // silently does nothing if the api mod isnt installed
    void setCheating(bool on) {
        auto f = m_fields.self();
        if (on == f->cheatFlagged) return;
        if (!Mod::get()->getSettingValue<bool>("cheat-indicator")) return;
        f->cheatFlagged = on;
#ifdef HAS_CHEAT_API
        // these return a Result, nothing useful to do with it here
        if (on) (void)cheatAPIEvents::setCheatingAll();
        else    (void)cheatAPIEvents::endCheatingAll();
        log::debug("cheat api told: {}", on);
#endif
    }

    // drop the cheat flag on the way out so it does not stay lit in the menus
    void onQuit() {
        setCheating(false);
        PlayLayer::onQuit();
    }

    // called by the pause menu after LOAD, so a pick takes effect right away
    void reloadMacro() {
        auto f = m_fields.self();
        auto rep = findMacro(m_level);
        f->holds.clear();
        f->hasP2 = false;
        f->active = false;
        if (rep && !rep->platformer) {
            f->fps = rep->framerate > 0 ? rep->framerate : 240.0;
            f->holds = rep->holds();
            f->macroSeconds = double(rep->lastFrame()) / f->fps;
            for (auto const& h : f->holds)
                if (h.player2) { f->hasP2 = true; break; }
            f->active = true;
        }
        f->state.assign(f->holds.size(), kPending);
        f->judged.assign(f->holds.size(), 0);
        f->cursor = 0;
        f->missCursor = 0;
        f->nPerfect = f->nOk = f->nMiss = 0;
        if (f->world) f->world->clear();
        if (f->lane)  f->lane->clear();
        if (f->spriteHost) hideSprites();
        log::info("macro reloaded: {} presses", f->holds.size());
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto f = m_fields.self();
        if (!f->active) return;

        f->levelTime = 0.0;
        f->startX = m_player1 ? m_player1->getPositionX() : 0.f;
        f->lastX = f->startX;
        f->speed = kSpeeds[1];
        f->lastSpeedTime = 0.0;
        f->cursor = 0;
        f->missCursor = 0;
        f->fallbackNow = 0.0;
        f->clockBase = 0.0;
        f->clockAccum = 0.0;
        f->clockBased = false;
        f->lastDriftLog = 0.0;
        f->nPerfect = f->nOk = f->nMiss = 0;
        f->verdictAt = -10.0;
        f->flashAt = -10.0;
        f->state.assign(f->holds.size(), kPending);
        f->judged.assign(f->holds.size(), 0);
        f->pressRead = 0;
        f->pressWrite.store(0, std::memory_order_release);
        f->holdMask.store(0, std::memory_order_release);
        if (f->verdict) f->verdict->setString("");
        if (f->tally) f->tally->setString("");
    }

    // and this code has even gotten me back to drinking
    double absTime() {
        auto f = m_fields.self();
        if (!f->posClock || !f->clockBased) return f->fallbackNow;
        return f->clockBase + f->clockAccum;
    }

    // check if macro is off by a bijilion units
    double posTime() {
        return tAtX(m_player1 ? m_player1->getPositionX() : 0.f);
    }

    // The level's opening speed lives in the level header, not in any object, (CLAUDE CODE FABLE 5 SORRY KINGS)
    // so a level that starts at 4x with its first portal thousands of units in
    // looks like a 1x level to an object scan. Measure it instead: run from the
    // start once and the real value is observable, then cache it forever.
    float levelStartSpeed() {
        auto f = m_fields.self();
        // only the very first portal (closest to x=0) can count as "at the
        // start"; portals are sorted by x, so we only ever need to look at
        // index 0. (previously a for-loop with an unconditional break after
        // one iteration -- same result, just confusing to read.)
        if (!f->portals.empty() && f->portals[0].x <= 20.f)
            return f->portals[0].v;
        return f->startSpeed;
    }

    void learnStartSpeed(float px) {
        auto f = m_fields.self();
        if (f->startSpeedKnown || !f->portalsScanned) return;
        if (f->levelTime < 0.20) return;
        const float firstPortalX = f->portals.empty() ? 1e9f : f->portals[0].x;
        if (px >= firstPortalX) return;    // past it, no longer observable

        const float s = snapSpeed(f->speed);
        f->startSpeed = s;
        f->startSpeedKnown = true;
        Mod::get()->setSavedValue<double>(f->speedKey, double(s));
        scanPortals();                     // rebuild the timeline on the new opening
        log::info("learned start speed {:.0f} u/s, cached as \"{}\"", s, f->speedKey);
    }

    double timeToReach(float targetX) { return tAtX(targetX); }

    float speedAt(float px) {
        auto const& S = m_fields->segs;
        if (S.empty()) return m_fields->startSpeed;
        float v = S[0].v;
        for (auto const& s : S) { if (s.x <= px) v = s.v; else break; }
        return v;
    }

    // Position at a macro time, the exact inverse of tAtX.
    // (previously divided t by a "scale" field that was declared, documented,
    // and read here -- but never assigned anywhere else, so it was always its
    // default 1.0. Dead code that did nothing; removed instead of leaving a
    // trap for the next person who "fixes" it by wiring it up incorrectly.)
    float xAt(double t) {
        auto f = m_fields.self();
        auto const& S = f->segs;
        if (S.empty()) return f->startX + float(t * f->speed);
        size_t lo = 0, hi = S.size() - 1;
        while (lo < hi) {
            const size_t mid = (lo + hi + 1) / 2;
            if (S[mid].t <= t) lo = mid; else hi = mid - 1;
        }
        return S[lo].x + float((t - S[lo].t) * S[lo].v);
    }

    // FRAMES

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto f = m_fields.self();
        if (!f->active || !m_player1) return;
        if (!f->world || !f->lane || !f->spriteHost) return;   // nodes gone, nothing to draw

        // master switch, live. off means nothing draws and nothing scores.
        if (!Mod::get()->getSettingValue<bool>("enabled")) {
            if (f->world) f->world->clear();
            if (f->lane)  f->lane->clear();
            if (f->spriteHost) hideSprites();
            if (f->verdict) f->verdict->setVisible(false);
            if (f->tally)   f->tally->setVisible(false);
            setCheating(false);
            return;
        }
        setCheating(true);

        const bool alive = !m_player1->m_isDead;
        if (alive) {
            f->levelTime += std::min(dt, kMaxStep);
            if (!f->posClock) f->fallbackNow += std::min(dt, kMaxStep);
        }

        const float px = m_player1->getPositionX();

        if (alive) {
            const double span = f->levelTime - f->lastSpeedTime;
            if (span >= 0.05) {
                const float v = float((px - f->lastX) / span);
                if (v > 1.f && v < 2000.f) f->speed = f->speed * 0.5f + v * 0.5f;
                f->lastX = px;
                f->lastSpeedTime = f->levelTime;
            }
        }

        tryScanPortals();
        learnStartSpeed(px);

        // test if works w lag love
        if (f->posClock && alive) {
            if (!f->clockBased) {
                f->clockBase = posTime();
                f->clockAccum = 0.0;
                f->clockBased = true;
                if (f->clockBase > 0.05)
                    log::info("attempt starts at x {:.0f} = {:.2f}s into the macro",
                              px, f->clockBase);
            } else {
                f->clockAccum += std::min(dt, kMaxStep);
                const double drift = posTime() - (f->clockBase + f->clockAccum);
                if (std::abs(drift) > 1.0) {
                    log::info("re-anchoring, position is {:.2f}s from the clock", drift);
                    f->clockBase = posTime();
                    f->clockAccum = 0.0;
                } else if (f->levelTime > f->lastDriftLog + 5.0) {
                    f->lastDriftLog = f->levelTime;
                    log::debug("drift at x {:.0f}: {:+.3f}s ({:+.0f} frames)",
                               px, drift, drift * f->fps);
                }
            }
        }

        const Look L = readSettingsCached();
        const double macroNow = absTime();
        const double now = macroNow - L.offset;

        // no if statements aura this checksoff Bravo 3 checkmark which kinda works?
        const int w = f->pressWrite.load(std::memory_order_acquire);
        while (f->pressRead < w) {
            const int slot = f->pressRead % Fields::kQueue;
            const double back = std::max(0.0, f->levelTime - f->pressTimes[slot]);
            judgePress(macroNow - back, f->pressIsP2[slot] != 0, L);
            ++f->pressRead;
        }

        // timeline speed must be equal speed user is at
        if (f->posClock && alive && f->levelTime > f->nextCheckAt) {
            f->nextCheckAt = f->levelTime + 2.0;
            const float want = speedAt(px), got = f->speed;
            if (std::abs(want - got) > 25.f)
                log::warn("speed mismatch at x {:.0f}: timeline says {:.0f}, player is doing {:.0f}",
                          px, want, got);
        }

        retireActive(now);
        scoreMissed(now, L);
        drawWorld(px, now, L);
        drawLane(now, L);
        updateLabels(L);
    }

    // hold and leave
    void retireActive(double now) {
        auto f = m_fields.self();
        const int held = f->holdMask.load(std::memory_order_acquire);
        for (size_t i = f->cursor; i < f->holds.size(); ++i) {
            if (f->state[i] != kActive) continue;
            auto const& h = f->holds[i];
            const bool stillDown = (held & (h.player2 ? 2 : 1)) != 0;
            if (!stillDown || now > double(h.end) / f->fps)
                f->state[i] = kDone;
        }
    }

    // grabs the next free sprite from a pool, making one if we ran out
    CCSprite* take(std::vector<CCSprite*>& pool, size_t& used, const char* file) {
        auto f = m_fields.self();
        if (!f->spriteHost) return nullptr;
        if (used >= pool.size()) {
            auto s = CCSprite::create(file);
            if (!s) return nullptr;
            s->setAnchorPoint({ 0.5f, 0.5f });
            f->spriteHost->addChild(s);
            pool.push_back(s);
        }
        auto s = pool[used++];
        s->setVisible(true);
        return s;
    }

    void placeCircle(CCSprite* s, float x, float y, float diameter, ccColor4F c) {
        if (!s) return;
        const float w = s->getContentSize().width;
        s->setScale(w > 0.1f ? diameter / w : 1.f);
        s->setPosition({ x, y });
        s->setColor({ GLubyte(c.r * 255), GLubyte(c.g * 255), GLubyte(c.b * 255) });
        s->setOpacity(GLubyte(std::clamp(c.a, 0.f, 1.f) * 255));
    }

    void hideSprites() {
        auto f = m_fields.self();
        for (auto s : f->discs) s->setVisible(false);
        for (auto s : f->rings) s->setVisible(false);
        f->discUsed = f->ringUsed = 0;
    }

    // click accuracy

    void onPlayerInput(bool down, bool isP2) {
        auto f = m_fields.self();
        if (!f->active) return;

        const int bit = isP2 ? 2 : 1;
        int cur = f->holdMask.load(std::memory_order_relaxed);
        f->holdMask.store(down ? (cur | bit) : (cur & ~bit), std::memory_order_release);
        if (!down) return;

        const int w = f->pressWrite.load(std::memory_order_relaxed);
        const int slot = w % Fields::kQueue;
        f->pressTimes[slot] = f->levelTime;
        f->pressIsP2[slot]  = isP2 ? 1 : 0;
        f->pressWrite.store(w + 1, std::memory_order_release);
    }

    void judgePress(double pressTime, bool isP2, Look const& L) {
        auto f = m_fields.self();
        if (f->holds.empty()) return;
        if (f->state.size() != f->holds.size()) return;
        if (f->judged.size() != f->holds.size()) return;

        const double now = pressTime - L.offset;
        const double missWindow = 0.30;
        const char* who = isP2 ? "P2 " : "";

        size_t best = SIZE_MAX;
        double bestDelta = 1e9;
        for (size_t i = f->cursor; i < f->holds.size(); ++i) {
            const double t0 = double(f->holds[i].start) / f->fps;
            if (t0 - now > missWindow) break;
            if (f->state[i] != kPending || f->holds[i].player2 != isP2) continue;
            const double d = now - t0;
            if (std::abs(d) < std::abs(bestDelta)) { bestDelta = d; best = i; }
        }

        if (best == SIZE_MAX || std::abs(bestDelta) > missWindow) {
            ++f->nMiss;
            const ccColor4F miss = verdictColour(9999, L.perfectFrames, L.okFrames);
            flash(miss);
            if (L.showAccuracy) setVerdict(fmt::format("{}MISS", who), toC3B(miss));
            return;
        }

        // click click clique
        const bool tap = f->holds[best].length() <= tapFrames(f->fps);
        f->state[best]  = tap ? kDone : kActive;
        f->judged[best] = 1;

        const int frames = int(std::lround(bestDelta * f->fps));
        const int off = std::abs(frames);
        const char* sign = frames > 0 ? "+" : "-";

        const ccColor4F col = verdictColour(off, L.perfectFrames, L.okFrames);
        flash(col);
        if (off <= L.perfectFrames) {
            ++f->nPerfect;
            if (L.showAccuracy) setVerdict(fmt::format("{}PERFECT", who), toC3B(col));
        } else if (off <= L.okFrames) {
            ++f->nOk;
            if (L.showAccuracy) setVerdict(fmt::format("{}OK {}{}f", who, sign, off), toC3B(col));
        } else {
            ++f->nMiss;
            if (L.showAccuracy) setVerdict(fmt::format("{}MISS {}{}f", who, sign, off), toC3B(col));
        }
    }

    // beat up children part 0 (im a child)
    void scoreMissed(double now, Look const& L) {
        auto f = m_fields.self();
        if (f->judged.size() != f->holds.size()) return;
        const double late = 0.30;
        while (f->missCursor < f->holds.size()) {
            const double t0 = double(f->holds[f->missCursor].start) / f->fps;
            if (t0 > now - late) break;
            if (!f->judged[f->missCursor]) {
                f->judged[f->missCursor] = 1;
                ++f->nMiss;
                flash(verdictColour(9999, L.perfectFrames, L.okFrames));
                if (L.showAccuracy)
                    setVerdict(f->holds[f->missCursor].player2 ? "P2 MISS" : "MISS",
                               { 255, 80, 90 });
            }
            ++f->missCursor;
        }
    }

    void flash(ccColor4F c) {
        auto f = m_fields.self();
        f->flashAt = f->levelTime;
        f->flashCol = c;
    }

    void setVerdict(std::string const& text, ccColor3B col) {
        auto f = m_fields.self();
        if (!f->verdict) return;
        f->verdict->setString(text.c_str());
        f->verdict->setColor(col);
        f->verdictAt = f->levelTime;
    }

    void updateLabels(Look const& L) {
        auto f = m_fields.self();
        if (!f->verdict || !f->tally) return;
        if (!L.showAccuracy) {
            f->verdict->setVisible(false);
            f->tally->setVisible(false);
            return;
        }
        f->verdict->setVisible(true);
        f->tally->setVisible(true);
        const double age = f->levelTime - f->verdictAt;
        const float a = age < 0.5 ? 1.f : std::max(0.f, 1.f - float((age - 0.5) / 0.6));
        f->verdict->setOpacity(GLubyte(a * 255.f));
        f->tally->setString(fmt::format("{} / {} / {}", f->nPerfect, f->nOk, f->nMiss).c_str());
    }

    // indicator which the BMWs don't use :laugh please:

    void drawWorld(float px, double now, Look const& L) {
        auto f = m_fields.self();
        auto n = f->world;
        n->clear();

        const auto win = CCDirector::get()->getWinSize();
        const CCPoint bl = m_objectLayer->convertToNodeSpace({ 0.f, 0.f });
        const CCPoint tr = m_objectLayer->convertToNodeSpace({ win.width, win.height });
        const float yLo = std::min(bl.y, tr.y) - 60.f;
        const float yHi = std::max(bl.y, tr.y) + 60.f;
        const float viewL = std::min(bl.x, tr.x);
        const float viewR = std::max(bl.x, tr.x);

        const float zoom  = m_objectLayer->getScale();
        const float half  = (L.badgeSize * 0.5f) / (zoom > 0.01f ? zoom : 1.f);
        const float thick = std::max(half * 0.34f, 0.6f);
        const float p1Y = m_player1 ? m_player1->getPositionY() : (bl.y + tr.y) * .5f;
        const float p2Y = m_player2 ? m_player2->getPositionY() : p1Y;

        const int held = f->holdMask.load(std::memory_order_acquire);

        // beat up children by draining the indicator
        while (f->cursor < f->holds.size()
               && f->state[f->cursor] == kDone
               && xAt(double(f->holds[f->cursor].end) / f->fps) < viewL)
            ++f->cursor;

        int drawnCount = 0;
        for (size_t i = f->cursor; i < f->holds.size(); ++i) {
            auto const& h = f->holds[i];
            if (f->state[i] == kDone) continue;
            if (drawnCount >= L.maxNotes) break;   // dense macros drown the screen

            const double t0 = double(h.start) / f->fps;
            const double t1 = double(h.end) / f->fps;
            if (t0 - now > L.ahead) break;

            const float x0 = xAt(t0);
            const float x1 = xAt(t1);
            if (std::max(x1, x0) < viewL) continue;

            const ccColor4F c = h.player2 ? L.p2 : L.indicator;
            const bool tap = h.length() <= tapFrames(f->fps);

            // drain bar
            const bool riding = L.holdTracks && !tap
                             && f->state[i] == kActive
                             && (held & (h.player2 ? 2 : 1)) != 0;

            float left, right;
            if (tap) {
                left  = x0 - L.width * .5f;
                right = x0 + L.width * .5f;
            } else {
                left  = riding ? std::max(px, x0) : x0;
                right = std::max(x1, x0 + L.width);
            }
            if (right <= left || left > viewR) continue;

            quad(n, left, right, yLo, yHi, c);
            ++drawnCount;

            if (!riding) {
                ccColor4F m = c;
                if (L.edgeMarker) {
                    m.a = std::clamp(c.a * 2.4f + 0.12f, 0.f, 1.f);
                    quad(n, left, left + std::max(1.5f, L.width * 0.4f), yLo, yHi, m);
                }
                if (L.pressBadge) {
                    m.a = std::clamp(c.a * 3.2f + 0.4f, 0.f, 1.f);
                    square(n, left, h.player2 ? p2Y : p1Y, half, thick, m);
                }
            }
        }

        if (L.showPlayer)
            quad(n, px - L.width * .5f, px + L.width * .5f, yLo, yHi, L.player);

        // if then if if if if if if i code ts like toby fox lmaoooooooooooooooooo
        if (L.playerSquare) {
            ccColor4F pc = L.player;
            pc.a = std::clamp(pc.a * 1.6f + 0.2f, 0.f, 1.f);
            square(n, px, p1Y, half * 1.35f, thick, pc);
            if (f->hasP2 && m_player2)
                square(n, m_player2->getPositionX(), p2Y, half * 1.35f, thick, pc);
        }
    }

    // ------------------------------------------------------------ ladder lane (I used AI here)


    // flat osu mania lane. no perspective, no rails, just columns with dots
    // and grey capsules for holds. totally separate from the ladder path.
    // straight osu mania. flat lane, solid discs, grey capsules, black
    // receptors at the bottom. no rims, no perspective, no hit bar.
    // osu mania, circle skin. black lane, big solid discs at full column
    // width, white ring receptors at the base. no rims on the notes.
    // osu mania. white outlined discs, coloured capsules for holds with a
    // brighter head, dashed empty rings for receptors.
    void drawLaneCircles(double now, Look const& L) {
        auto f = m_fields.self();
        auto n = f->lane;
        n->clear();
        hideSprites();

        const auto win = CCDirector::get()->getWinSize();
        const float k     = std::clamp(L.laneScale, 0.4f, 2.5f);
        const bool  split = f->hasP2 && L.splitLane;
        const int   cols  = split ? 2 : 1;
        const float colW  = win.width * 0.082f * k;
        const float total = cols * colW;
        const float left  = L.laneLeft ? 20.f : win.width - 20.f - total;
        const float dia   = colW * 0.86f;          // outer, including the white rim
        const float inner = dia * 0.84f;           // the coloured part
        const float hitY  = win.height * 0.10f + dia * 0.5f;
        const float topY  = hitY + win.height * 0.80f * k;
        const float span  = topY - hitY;
        const ccColor4F WHITE = { 1.f, 1.f, 1.f, 0.95f };

        auto colX = [&](bool p2) {
            return left + colW * 0.5f + ((split && p2) ? colW : 0.f);
        };

        if (f->verdict) f->verdict->setPosition({ left + total * .5f, hitY - dia * 0.9f });
        if (f->tally)   f->tally->setPosition({ left + total * .5f, hitY - dia * 1.25f });
        if (!L.showLane) return;

        // dark shaft with a bright rule down each outer edge
        quad(n, left, left + total, hitY - dia * 0.7f, topY, { 0.f, 0.f, 0.f, 0.45f });
        const ccColor4F rule = { 0.85f, 0.95f, 1.f, 0.85f };
        quad(n, left - 2.f, left, hitY - dia * 0.7f, topY, rule);
        quad(n, left + total, left + total + 2.f, hitY - dia * 0.7f, topY, rule);

        f->noteAtLine = false;

        const double perfSec = double(L.perfectFrames) / f->fps;
        auto yOfT = [&](double t) { return hitY + float((t - now) / L.laneWindow) * span; };

        // one unbroken ring, chords stepped all the way round with no gaps
        auto solidRing = [&](float cx, float cy, float r, float thick, ccColor4F c) {
            const int SEG = 40;
            const float TAU = 6.2831853f;
            for (int i = 0; i < SEG; ++i) {
                const float b0 = float(i)     * TAU / SEG;
                const float b1 = float(i + 1) * TAU / SEG;
                stroke(n, { cx + r * std::cos(b0), cy + r * std::sin(b0) },
                          { cx + r * std::cos(b1), cy + r * std::sin(b1) }, thick, c);
            }
        };

        int laneCount = 0;
        for (int pass = 0; pass < 2; ++pass) {
            int seen = 0;
            for (size_t i = f->cursor; i < f->holds.size(); ++i) {
                auto const& h = f->holds[i];
                if (f->state[i] == kDone) continue;
                if (seen >= L.maxNotes) break;

                const double t0 = double(h.start) / f->fps;
                const double t1 = double(h.end) / f->fps;
                if ((t0 - now) / L.laneWindow > 1.0) break;

                const bool tap    = h.length() <= tapFrames(f->fps);
                const bool active = f->state[i] == kActive;
                const double uEnd = (t1 - now) / L.laneWindow;
                if ((tap ? (t0 - now) / L.laneWindow : uEnd) < -0.25) continue;

                const float cx = colX(h.player2);
                const float y0 = std::max(yOfT(t0), hitY);
                if (y0 > topY + dia) continue;
                ++seen;

                const bool onTime = f->state[i] == kPending && std::abs(t0 - now) <= perfSec;

                ccColor4F body = h.player2 ? L.laneP2 : L.lane;
                body.a = std::clamp(body.a * 2.4f + 0.25f, 0.f, 1.f);
                if (active) body = lighten(body, 1.25f);

                if (pass == 0) {
                    // a hold is ONE pill, length = how long you hold it. no
                    // separate head disc, that is what made it look like two
                    // circles welded together.
                    if (tap) continue;
                    if (onTime || active) f->noteAtLine = true;
                    ccColor4F fill = onTime ? ccColor4F{ 1.f, 1.f, 1.f, 1.f } : body;
                    const float y1 = std::max(std::min(yOfT(t1), topY), y0);
                    stadium(n, cx, y0, y1, dia, WHITE);     // outline
                    stadium(n, cx, y0, y1, inner, fill);    // fill
                    ++laneCount;
                } else {
                    if (!tap) continue;
                    if (onTime) f->noteAtLine = true;
                    ccColor4F fill = onTime ? ccColor4F{ 1.f, 1.f, 1.f, 1.f } : body;
                    // BUGFIX/POLISH: this drew kDisc (a filled circle) twice,
                    // once oversized in white as a fake "outline". kRing --
                    // the actual ring-shaped overlay texture bundled with the
                    // mod -- was declared but never used anywhere, so its
                    // sprite pool (f->rings) sat empty the whole time. Using
                    // the real ring texture gives a proper thin outline
                    // instead of a solid white disc peeking out from behind.
                    placeCircle(take(f->rings, f->ringUsed, kRing), cx, y0, dia, WHITE);
                    placeCircle(take(f->discs, f->discUsed, kDisc), cx, y0, inner, fill);
                    ++laneCount;
                }
            }
        }

        // solid outlined receptors, empty inside
        const double age = f->levelTime - f->flashAt;
        ccColor4F ring = { 0.88f, 0.90f, 0.94f, 0.85f };
        if (age >= 0 && age < kFlashTime)
            ring = f->flashCol;
        else if (f->noteAtLine) ring = { 1.f, 1.f, 1.f, 1.f };

        for (int i = 0; i < cols; ++i)
            solidRing(left + colW * 0.5f + i * colW, hitY, dia * 0.5f, dia * 0.032f, ring);
    }

    void drawLane(double now, Look const& L) {
        if (L.circleNotes) { drawLaneCircles(now, L); return; }
        hideSprites();
        auto f = m_fields.self();
        auto n = f->lane;
        n->clear();

        const auto win = CCDirector::get()->getWinSize();
        const float k    = std::clamp(L.laneScale, 0.4f, 2.5f);
        const float cx   = L.laneLeft ? win.width * 0.115f : win.width * 0.885f;
        const float hitY = win.height * 0.215f;
        const float topY = hitY + win.height * 0.45f * k;
        const float span = topY - hitY;
        const float halfBot = win.width * 0.041f * k;
        const float railBot = win.width * 0.0055f * k;
        const float rungBot = span * 0.062f;

        // Perspective. Depth z grows with time to the press, width goes as 1/z,
        // and screen height follows so that width is linear in height, which is
        // what a one point projection gives you.
        const float depth = std::clamp(L.laneDepth, 0.2f, 4.f);
        const float sTop  = 1.f / (1.f + depth);
        auto scaleAt = [&](double u) { return 1.f / (1.f + depth * float(u)); };
        auto yOf     = [&](float s) { return hitY + span * (1.f - s) / (1.f - sTop); };

        if (f->verdict) f->verdict->setPosition({ cx, hitY - win.height * 0.055f });
        if (f->tally)   f->tally->setPosition({ cx, hitY - win.height * 0.095f });

        if (!L.showLane) return;

        const int held = f->holdMask.load(std::memory_order_acquire);
        const bool anyHeld = held != 0;

        ccColor4F rail = lighten(L.lane, 1.0f);
        rail.a = std::clamp(rail.a * 1.9f + 0.10f, 0.f, 1.f);
        ccColor4F face   = rail;
        ccColor4F faceP2 = L.laneP2;
        faceP2.a = rail.a;
        ccColor4F top    = lighten(rail, 1.45f);
        ccColor4F topP2  = lighten(faceP2, 1.45f);
        ccColor4F shade  = lighten(rail, 0.42f);
        ccColor4F shadeP2 = lighten(faceP2, 0.42f);
        // Hold bodies sit behind the slab in a darkened shade of the same colour.
        ccColor4F bodyC   = lighten(rail, 0.55f);
        ccColor4F bodyP2  = lighten(faceP2, 0.55f);

        // ---- rails, tapering with the perspective
        {
            const float ht = halfBot * sTop, rt = railBot * sTop;
            poly(n, { cx - halfBot, hitY }, { cx - halfBot + railBot, hitY },
                    { cx - ht + rt, topY }, { cx - ht, topY }, rail);
            poly(n, { cx + halfBot - railBot, hitY }, { cx + halfBot, hitY },
                    { cx + ht, topY }, { cx + ht - rt, topY }, rail);
        }

        // green zone bands under the notes, real windows not vibes
        // sized off the actual perfect/ok frame settings
        const double perfSec = double(L.perfectFrames) / f->fps;
        const double okSec   = double(L.okFrames) / f->fps;
        auto yForSec = [&](double sec) { return yOf(scaleAt(sec / L.laneWindow)); };
        {
            ccColor4F okBand = L.lane;
            okBand.a = std::clamp(okBand.a * 0.55f, 0.f, 1.f);
            quad(n, cx - halfBot * 0.94f, cx + halfBot * 0.94f,
                    hitY, yForSec(okSec), okBand);

            // target slot is exactly one slab tall so a note fills it dead on
            ccColor4F perfBand = L.player;
            perfBand.a = std::clamp(std::max(L.player.a, 0.5f) * 0.75f, 0.f, 1.f);
            quad(n, cx - halfBot * 0.98f, cx + halfBot * 0.98f,
                    hitY, hitY + rungBot, perfBand);
        }

        f->noteAtLine = false;

        // ---- rungs and holds
        for (size_t i = f->cursor; i < f->holds.size(); ++i) {
            auto const& h = f->holds[i];
            if (f->state[i] == kDone) continue;

            const double t0 = double(h.start) / f->fps;
            const double t1 = double(h.end) / f->fps;
            double u0 = (t0 - now) / L.laneWindow;
            if (u0 > 1.0) break;

            const bool tap = h.length() <= tapFrames(f->fps);
            const bool active = f->state[i] == kActive;
            const double uEnd = (t1 - now) / L.laneWindow;

            // more osu mania stuff
            if ((tap ? u0 : uEnd) < -0.25) continue;
            u0 = std::max(u0, 0.0);

            const float s0 = scaleAt(u0);
            const float y0 = yOf(s0);
            const float hw0 = halfBot * s0;

            ccColor4F fc = h.player2 ? faceP2 : face;
            ccColor4F tc = h.player2 ? topP2 : top;
            ccColor4F sc = h.player2 ? shadeP2 : shade;
            ccColor4F bc = h.player2 ? bodyP2 : bodyC;
            if (active) { fc = lighten(fc, 1.5f); tc = lighten(tc, 1.5f); bc = lighten(bc, 1.5f); f->noteAtLine = true; }

            // perfect score like my ass (perfect)
            const bool onTime = f->state[i] == kPending && std::abs(t0 - now) <= perfSec;
            if (onTime) {
                fc = { 1.f, 1.f, 1.f, std::clamp(fc.a * 1.8f + 0.25f, 0.f, 1.f) };
                tc = fc;
            }

            // kay?
            float colC = cx, colH = hw0;
            if (f->hasP2 && L.splitLane) {
                colH = hw0 * 0.47f;
                colC = cx + (h.player2 ? hw0 * 0.53f : -hw0 * 0.53f);
            }

            // Amen
            const float th = rungBot * s0;
            const float capTop = y0 + th * 1.34f;
            if (onTime) f->noteAtLine = true;

            if (onTime) {
                ccColor4F halo = fc;
                halo.a *= 0.45f;
                quad(n, colC - colH * 1.22f, colC + colH * 1.22f,
                        y0 - th * 0.55f, capTop + th * 0.4f, halo);
            }
            quad(n, colC - colH, colC + colH, y0 - th * 0.16f, y0, sc);
            quad(n, colC - colH, colC + colH, y0, y0 + th, fc);
            quad(n, colC - colH, colC + colH, y0 + th, capTop, tc);

            // OSU Mania bullshit
            if (!tap) {
                const double u1 = std::clamp(uEnd, u0, 1.0);
                const float s1 = scaleAt(u1);
                const float y1 = std::max(yOf(s1), capTop + th * 0.4f);

                float colH1 = halfBot * s1;
                float colC1 = cx;
                if (f->hasP2 && L.splitLane) {
                    colC1 = cx + (h.player2 ? colH1 * 0.53f : -colH1 * 0.53f);
                    colH1 *= 0.47f;
                }
                const float bw0 = colH * kBodyWidth;
                const float bw1 = colH1 * kBodyWidth;

                poly(n, { colC - bw0, capTop }, { colC + bw0, capTop },
                        { colC1 + bw1, y1 }, { colC1 - bw1, y1 }, bc);
                quad(n, colC1 - bw1, colC1 + bw1, y1, y1 + rungBot * s1 * 0.30f,
                     lighten(bc, 1.45f));
            }
        }

        // hit bar maybe good maybe not
        const double age = f->levelTime - f->flashAt;
        ccColor4F bar = L.player;
        bar.a = std::max(bar.a, 0.75f);
        // lights up the moment a bar reaches all the way down
        if (f->noteAtLine) bar = { 1.f, 1.f, 1.f, 1.f };
        if (age >= 0 && age < kFlashTime) {
            const float m = 1.f - float(age / kFlashTime) * 0.35f;
            bar = { f->flashCol.r * m, f->flashCol.g * m, f->flashCol.b * m, 1.f };
        }
        const float pad = halfBot * 0.10f;
        const float barH = span * 0.022f;
        quad(n, cx - halfBot - pad, cx + halfBot + pad, hitY - barH, hitY + barH, bar);

        // how tf did this work lmao
        ccColor4F chev = anyHeld || (age >= 0 && age < kFlashTime)
                       ? bar : ccColor4F{ 0.78f, 0.80f, 0.82f, std::max(L.player.a, 0.6f) };
        const float cs = halfBot * 0.42f;
        const float ct = halfBot * 0.10f;
        const float gap = halfBot * 0.18f;
        const float lx = cx - halfBot - pad - gap;
        const float rx = cx + halfBot + pad + gap;
        stroke(n, { lx - cs, hitY + cs }, { lx, hitY }, ct, chev);
        stroke(n, { lx - cs, hitY - cs }, { lx, hitY }, ct, chev);
        stroke(n, { rx + cs, hitY + cs }, { rx, hitY }, ct, chev);
        stroke(n, { rx + cs, hitY - cs }, { rx, hitY }, ct, chev);
    }
};

// input player client side only but what if the client is fat if (player = fat) sudo apt install opsec delete sys32 (this is a joke ignore)
class $modify(IndicatorInput, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (button != 1) return;
        auto pl = PlayLayer::get();
        if (!pl || pl != static_cast<GJBaseGameLayer*>(this)) return;
        if (!pl->m_player1) return;
        static_cast<IndicatorLayer*>(pl)->onPlayerInput(down, !isPlayer1);
    }
};

// ---------------------------------------------------------------- pause ui

// built on plain cocos and gd bindings only. geode's Popup and ScrollLayer
// templates move between sdk versions and kept failing to resolve, so this
// uses nothing that can wander off: a colour layer, a scale9 panel, menus.
class MacroPopup : public CCLayerColor {
    CCNode*        m_panel = nullptr;
    CCMenu*        m_rows  = nullptr;
    CCLabelBMFont* m_info  = nullptr;
    int            m_page  = 0;
    std::vector<CCLabelBMFont*> m_labels;

    static constexpr int   kPerPage = 5;      // rows per page
    static constexpr float kW = 360.f;
    static constexpr float kH = 250.f;

    static CCMenuItemSpriteExtra* mkBtn(char const* text, float w,
                                        CCObject* t, SEL_MenuHandler sel) {
        auto spr = ButtonSprite::create(text);
        if (spr && spr->getContentSize().width > w)
            spr->setScale(w / spr->getContentSize().width);
        return CCMenuItemSpriteExtra::create(spr, t, sel);
    }

public:
    static MacroPopup* create() {
        auto ret = new MacroPopup();
        if (ret && ret->build()) { ret->autorelease(); return ret; }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    bool build() {
        if (!this->initWithColor({ 0, 0, 0, 150 })) return false;
        const auto win = CCDirector::get()->getWinSize();

        auto panel = cocos2d::extension::CCScale9Sprite::create("GJ_square01.png");
        if (!panel) return false;
        panel->setContentSize({ kW, kH });
        panel->setPosition({ win.width * .5f, win.height * .5f });
        this->addChild(panel);
        m_panel = panel;

        const float bx = win.width * .5f - kW * .5f;
        const float by = win.height * .5f - kH * .5f;

        auto title = CCLabelBMFont::create("Click Guide", "goldFont.fnt");
        title->setPosition({ win.width * .5f, by + kH - 22.f });
        title->setScale(0.8f);
        this->addChild(title);

        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        menu->setTouchPriority(-600);
        this->addChild(menu);

        if (auto x = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png")) {
            x->setScale(0.7f);
            auto b = CCMenuItemSpriteExtra::create(x, this, menu_selector(MacroPopup::onClose));
            b->setPosition({ bx + 16.f, by + kH - 16.f });
            menu->addChild(b);
        }

        auto bImport  = mkBtn("Import Macro", 96.f, this, menu_selector(MacroPopup::onImport));
        auto bRefresh = mkBtn("Refresh", 62.f, this, menu_selector(MacroPopup::onRefresh));
        auto bSet     = mkBtn("Settings", 70.f, this, menu_selector(MacroPopup::onSettings));
        bImport->setPosition({ bx + kW * .24f, by + kH - 52.f });
        bRefresh->setPosition({ bx + kW * .54f, by + kH - 52.f });
        bSet->setPosition({ bx + kW * .82f, by + kH - 52.f });
        menu->addChild(bImport);
        menu->addChild(bRefresh);
        menu->addChild(bSet);

        auto prev = mkBtn("<", 26.f, this, menu_selector(MacroPopup::onPrev));
        auto next = mkBtn(">", 26.f, this, menu_selector(MacroPopup::onNext));
        prev->setPosition({ bx + 24.f, by + 22.f });
        next->setPosition({ bx + kW - 24.f, by + 22.f });
        menu->addChild(prev);
        menu->addChild(next);

        m_rows = CCMenu::create();
        m_rows->setPosition({ 0.f, 0.f });
        m_rows->setTouchPriority(-610);
        this->addChild(m_rows);

        m_info = CCLabelBMFont::create("", "chatFont.fnt");
        m_info->setScale(0.5f);
        m_info->setPosition({ win.width * .5f, by + 22.f });
        this->addChild(m_info);

        this->setTouchEnabled(true);
        this->setKeypadEnabled(true);
        refresh();
        return true;
    }

    void keyBackClicked() { onClose(nullptr); }

    GJGameLevel* level() {
        auto pl = PlayLayer::get();
        return pl ? pl->m_level : nullptr;
    }

    void refresh() {
        m_rows->removeAllChildren();
        const auto entries = listMacros();
        const auto picked = pickedMacro(level());

        const int pages = std::max(1, int((entries.size() + kPerPage - 1) / kPerPage));
        m_page = std::clamp(m_page, 0, pages - 1);

        const auto win = CCDirector::get()->getWinSize();
        const float bx = win.width * .5f - kW * .5f;
        const float by = win.height * .5f - kH * .5f;
        const float top = by + kH - 78.f;

        for (int r = 0; r < kPerPage; ++r) {
            const size_t idx = size_t(m_page * kPerPage + r);
            if (idx >= entries.size()) break;
            auto const& e = entries[idx];
            const float y = top - 26.f * float(r);
            const bool on = (e.file == picked);

            auto name = CCLabelBMFont::create(e.file.c_str(), "chatFont.fnt");
            name->setAnchorPoint({ 0.f, 0.5f });
            name->setScale(0.5f);
            if (name->getContentSize().width * 0.5f > kW - 130.f)
                name->setScale((kW - 130.f) / name->getContentSize().width);
            name->setPosition({ bx + 16.f, y });
            name->setColor(on ? ccColor3B{ 110, 255, 150 } : ccColor3B{ 235, 235, 235 });
            this->addChild(name);
            m_labels.push_back(name);

            auto btn = mkBtn(on ? "Loaded" : "Load", 54.f, this,
                             menu_selector(MacroPopup::onLoad));
            btn->setPosition({ bx + kW - 44.f, y });
            btn->setTag(int(idx));
            m_rows->addChild(btn);
        }

        if (entries.empty()) {
            m_info->setString("No .gdr2 files in the macros folder");
        } else {
            auto it = std::find_if(entries.begin(), entries.end(),
                                   [&](MacroEntry const& e) { return e.file == picked; });
            if (it == entries.end())
                m_info->setString(fmt::format("{} macros, page {}/{}",
                                              entries.size(), m_page + 1, pages).c_str());
            else
                m_info->setString(fmt::format("{} - {} inputs @ {:.0f} FPS",
                                              it->file, it->inputs, it->fps).c_str());
        }
    }

    void clearLabels() {
        for (auto l : m_labels) if (l) l->removeFromParent();
        m_labels.clear();
    }

    void onPrev(CCObject*) { clearLabels(); --m_page; refresh(); }
    void onNext(CCObject*) { clearLabels(); ++m_page; refresh(); }
    void onRefresh(CCObject*) { clearLabels(); refresh(); }

    void onLoad(CCObject* sender) {
        const auto entries = listMacros();
        const int idx = sender->getTag();
        if (idx < 0 || idx >= int(entries.size())) return;

        auto lvl = level();
        const bool already = pickedMacro(lvl) == entries[idx].file;
        setPickedMacro(lvl, already ? "" : entries[idx].file);

        if (auto pl = PlayLayer::get())
            static_cast<IndicatorLayer*>(pl)->reloadMacro();
        clearLabels();
        refresh();
    }

    void onImport(CCObject*) {
        // openFolder does nothing on a phone, so always show the path too
    #ifdef GEODE_IS_DESKTOP
        geode::utils::file::openFolder(macroDir());
    #endif
        FLAlertLayer::create(
            "Import Macro",
            ("Put your <cg>.gdr2</c> files here, then press <cy>Refresh</c>:\n\n<cl>"
             + macroDir().string() + "</c>").c_str(),
            "OK")->show();
    }

    void onSettings(CCObject*) {
#ifdef HAS_GEODE_UI
        geode::openSettingsPopup(Mod::get());
#else
        FLAlertLayer::create("Settings",
            "Open the Geode menu and find <cy>Click Indicators</c>.", "OK")->show();
#endif
    }

    void onClose(CCObject*) {
        clearLabels();
        this->removeFromParentAndCleanup(true);
    }
};

class $modify(IndicatorPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        CCNode* face = nullptr;
        for (auto nm : { "GJ_optionsBtn_001.png", "GJ_optionsBtn02_001.png",
                         "GJ_hammerBtn_001.png" }) {
            if (auto s = CCSprite::createWithSpriteFrameName(nm)) { face = s; break; }
        }
        if (face) face->setScale(0.75f);
        else      face = ButtonSprite::create("Guide");

        auto btn = CCMenuItemSpriteExtra::create(
            face, this, menu_selector(IndicatorPause::onGuide));

        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        menu->addChild(btn);
        btn->setPosition({ 34.f, 34.f });
        this->addChild(menu, 100);
    }

    void onGuide(CCObject*) {
        // BUGFIX: a fast double-tap on the guide button could fire this twice
        // before the first popup finished showing, stacking two MacroPopups
        // on top of each other. Bail if one is already open.
        auto scene = CCDirector::get()->getRunningScene();
        if (scene && scene->getChildByType<MacroPopup>(0)) return;
        if (auto p = MacroPopup::create())
            scene->addChild(p, 9999);
    }
};

$on_mod(Loaded) {
    log::info("macros go in {}", macroDir().string());
#ifdef HAS_CHEAT_API
    log::info("cheat api linked, the indicator will work");
#else
    log::warn("cheat api header missing at build time, the cheat indicator "
              "will do nothing");
#endif
}
