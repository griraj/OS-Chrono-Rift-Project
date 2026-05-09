
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>
#include <cmath>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <semaphore.h>
#include <time.h>
#include <errno.h>

#include <SFML/Graphics.hpp>
#include <SFML/System.hpp>
#include <SFML/Window.hpp>

#include "shared_state.h"
#include "inventory.h"

static SharedState *gs = nullptr;
static int g_num_players = 0;
static volatile sig_atomic_t g_quit = 0;
static volatile sig_atomic_t g_stunned = 0;
static sem_t player_sem[MAX_PLAYERS];

static volatile int g_ui_pidx = -1;
static Action g_ui_result = {};
static sem_t g_ui_req_sem;
static sem_t g_ui_done_sem;
static pthread_mutex_t g_ui_mtx = PTHREAD_MUTEX_INITIALIZER;

static const char *FONT_PATHS[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
    nullptr};

static void sig_term(int) { g_quit = 1; }

static void sig_stun(int)
{
    g_stunned = 1;
    int stunned_idx = gs->current_turn;
    if (stunned_idx >= 0 && stunned_idx < gs->num_players)
        gs->entities[stunned_idx].stunned = true;
    sleep(STUN_DURATION);
    if (stunned_idx >= 0 && stunned_idx < gs->num_players)
        gs->entities[stunned_idx].stunned = false;
    g_stunned = 0;
}

static SharedState *shm_open_existing()
{
    int fd = -1;
    for (int i = 0; i < 50; ++i)
    {
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (fd >= 0) break;
        usleep(100000);
    }
    if (fd < 0) { perror("hip: shm_open"); exit(1); }
    void *p = mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("hip: mmap"); exit(1); }
    close(fd);
    return static_cast<SharedState *>(p);
}

static const unsigned WW = 900, WH = 700;

static const sf::Color BG{8, 10, 20};
static const sf::Color PANEL{16, 18, 38};
static const sf::Color BDR{40, 80, 160};
static const sf::Color BDR_HI{80, 160, 255};
static const sf::Color GOLD{220, 185, 80};
static const sf::Color WHITE{230, 235, 245};
static const sf::Color DIM{100, 110, 130};
static const sf::Color ACCENT{60, 200, 255};
static const sf::Color GREEN{60, 220, 120};
static const sf::Color RED{220, 70, 70};
static const sf::Color ORANGE{255, 140, 0};
static const sf::Color PURPLE{180, 100, 255};
static const sf::Color HP_P{60, 200, 80};
static const sf::Color HP_E{220, 60, 60};
static const sf::Color STAM{60, 180, 255};
static const sf::Color SCAN{0, 20, 60, 15};
static const sf::Color SEL{20, 50, 100};
static const sf::Color ULT{255, 100, 200};

static sf::Text T(const sf::Font &f, const std::string &s,
                  unsigned sz, sf::Color c, bool b = false)
{
    sf::Text t; t.setFont(f); t.setString(s);
    t.setCharacterSize(sz); t.setFillColor(c);
    if (b) t.setStyle(sf::Text::Bold);
    return t;
}

static void Tcx(sf::RenderTarget &r, sf::Text t, float cx, float y)
{
    sf::FloatRect b = t.getLocalBounds();
    t.setOrigin(b.left + b.width / 2.f, b.top);
    t.setPosition(cx, y); r.draw(t);
}

static void Box(sf::RenderTarget &r, float x, float y, float w, float h,
                sf::Color fill, sf::Color bdr, float th = 1.5f)
{
    sf::RectangleShape s({w, h}); s.setPosition(x, y);
    s.setFillColor(fill); s.setOutlineColor(bdr);
    s.setOutlineThickness(th); r.draw(s);
}

static void Bar(sf::RenderTarget &r, float x, float y, float w, float h,
                float v, float mx, sf::Color c)
{
    Box(r, x, y, w, h, {30, 30, 50}, BDR, 1.f);
    if (mx > 0)
    {
        float rt = v / mx;
        if (rt < 0) rt = 0;
        if (rt > 1) rt = 1;
        Box(r, x, y, w * rt, h, c, c, 0.f);
    }
}

static void Corners(sf::RenderTarget &r, float x, float y, float w, float h, float sz = 10.f)
{
    sf::RectangleShape s; s.setFillColor(GOLD);
    auto seg = [&](float px, float py, float pw, float ph)
    { s.setSize({pw,ph}); s.setPosition(px,py); r.draw(s); };
    seg(x,y,sz,2); seg(x,y,2,sz);
    seg(x+w-sz,y,sz,2); seg(x+w-2,y,2,sz);
    seg(x,y+h-2,sz,2); seg(x,y+h-sz,2,sz);
    seg(x+w-sz,y+h-2,sz,2); seg(x+w-2,y+h-sz,2,sz);
}

static void Scanlines(sf::RenderTarget &r)
{
    sf::RectangleShape l({(float)WW, 1.f}); l.setFillColor(SCAN);
    for (unsigned y = 0; y < WH; y += 4) { l.setPosition(0, (float)y); r.draw(l); }
}

static void Grid(sf::RenderTarget &r)
{
    sf::RectangleShape l({(float)WW, 1.f}); l.setFillColor({20, 30, 60, 30});
    for (unsigned y = 0; y < WH; y += 32) { l.setPosition(0, (float)y); r.draw(l); }
    l.setSize({1.f, (float)WH});
    for (unsigned x = 0; x < WW; x += 32) { l.setPosition((float)x, 0); r.draw(l); }
}

static bool draw_pause_menu(sf::RenderWindow &win, const sf::Font &font)
{
    sf::FloatRect resume_b = {WW / 2.f - 130.f, WH / 2.f - 10.f, 260.f, 54.f};
    sf::FloatRect quit_b = {WW / 2.f - 130.f, WH / 2.f + 72.f, 260.f, 54.f};

    while (win.isOpen() && !g_quit)
    {
        sf::Vector2f ms = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        sf::Event ev{};
        while (win.pollEvent(ev))
        {
            if (ev.type == sf::Event::Closed) { g_quit = 1; return true; }
            if (ev.type == sf::Event::KeyPressed)
            {
                if (ev.key.code == sf::Keyboard::P || ev.key.code == sf::Keyboard::Escape)
                    return false;
            }
            if (ev.type == sf::Event::MouseButtonPressed && ev.mouseButton.button == sf::Mouse::Left)
            {
                sf::Vector2f msc = win.mapPixelToCoords({ev.mouseButton.x, ev.mouseButton.y});
                if (resume_b.contains(msc)) return false;
                if (quit_b.contains(msc)) { kill(gs->arbiter_pid, SIGTERM); g_quit = 1; return true; }
            }
        }

        sf::RectangleShape dim({(float)WW, (float)WH}); dim.setFillColor({0, 0, 0, 180}); win.draw(dim);
        Box(win, WW/2.f-180.f, WH/2.f-80.f, 360.f, 240.f, PANEL, GOLD, 2.f);
        Corners(win, WW/2.f-180.f, WH/2.f-80.f, 360.f, 240.f, 14.f);
        Tcx(win, T(font, "PAUSED", 28, GOLD, true), WW/2.f, WH/2.f-72.f);
        sf::RectangleShape rl({300.f, 1.f}); rl.setPosition(WW/2.f-150.f, WH/2.f-32.f); rl.setFillColor(BDR); win.draw(rl);

        bool rh = resume_b.contains(ms);
        Box(win, resume_b.left, resume_b.top, resume_b.width, resume_b.height,
            rh?sf::Color{10,40,90}:PANEL, rh?ACCENT:BDR, rh?2.f:1.5f);
        Tcx(win, T(font, "RESUME  (P / Esc)", 16, rh?WHITE:DIM, rh), WW/2.f, resume_b.top+16.f);

        bool qh = quit_b.contains(ms);
        Box(win, quit_b.left, quit_b.top, quit_b.width, quit_b.height,
            qh?sf::Color{80,10,10}:PANEL, qh?RED:BDR, qh?2.f:1.5f);
        Tcx(win, T(font, "QUIT GAME", 16, qh?WHITE:DIM, qh), WW/2.f, quit_b.top+16.f);

        Scanlines(win); win.display();
    }
    return false;
}

enum class UIMode { WAIT, PICKING };

struct HipUI
{
    sf::Font font;
    UIMode mode = UIMode::WAIT;
    int pidx = -1;
    float clock_t = 0.f;

    enum class Step { ACTION, WEAPON, ENEMY, LTS };
    Step step = Step::ACTION;
    int sel_act = -1;
    int sel_wpn_slot = -1;
    std::string status;

    Entity me{};
    Entity ents[MAX_ENTITIES]{};
    int np = 0, nte = 0;
    bool can_ult = false;

    struct Btn { sf::FloatRect b; std::string lbl, sub; sf::Color acc; bool enabled = true; };
    std::vector<Btn> action_btns;
    struct EBtn { sf::FloatRect b; int idx; };
    std::vector<EBtn> enemy_btns;
    struct WBtn { sf::FloatRect b; int slot; WeaponID wid; };
    std::vector<WBtn> wpn_btns;
    struct LBtn { sf::FloatRect b; int li; WeaponID wid; };
    std::vector<LBtn> lts_btns;

    void load_font()
    {
        for (int i = 0; FONT_PATHS[i]; ++i)
            if (font.loadFromFile(FONT_PATHS[i])) return;
    }

    void begin_turn(int p)
    {
        pidx = p; mode = UIMode::PICKING; step = Step::ACTION;
        sel_act = -1; sel_wpn_slot = -1; status = "";

        sem_wait(&gs->state_mutex);
        me = gs->entities[p];
        memcpy(ents, gs->entities, sizeof(ents));
        np = gs->num_players; nte = gs->total_entities;
        sem_post(&gs->state_mutex);

        can_ult = inv_has_weapon(me, WPN_SOLAR_CORE) && inv_has_weapon(me, WPN_LUNAR_BLADE);

        action_btns.clear();
        float by = 220.f;
        const float BW = 340.f, BH = 52.f, BG = 8.f, BX = 30.f;
        auto add = [&](const std::string &l, const std::string &s, sf::Color a, bool en = true)
        { action_btns.push_back({{BX,by,BW,BH},l,s,a,en}); by+=BH+BG; };

        add("1  Strike", "Direct damage to enemy", RED);
        add("2  Exhaust", "Drain enemy stamina", ORANGE);
        bool hi = false;
        for (int s = 0; s < INVENTORY_SLOTS; s++) if (me.inventory.slots[s] != WPN_NONE) { hi = true; break; }
        add("3  Use Weapon", "Attack with inventory weapon", ACCENT, hi);
        add("4  Swap In", "Retrieve from Long-Term Storage", PURPLE, me.lts.count > 0);
        add("5  Heal", "Restore 10% of max HP", GREEN);
        add("6  Skip", "50% stamina refund", DIM);
        if (can_ult) add("7  ULTIMATE", "Solar Core + Lunar Blade", ULT);
        add("0  Quit Game", "Send SIGTERM to arbiter", RED);

        enemy_btns.clear();
        float ey = 238.f;
        for (int i = np; i < nte; i++)
            if (ents[i].alive) { enemy_btns.push_back({{400.f, ey, WW - 420.f, 38.f}, i}); ey += 44.f; }

        wpn_btns.clear();
        float wy = 238.f;
        bool vis[INVENTORY_SLOTS] = {};
        for (int s = 0; s < INVENTORY_SLOTS; s++)
        {
            if (vis[s]) continue;
            WeaponID w = me.inventory.slots[s];
            if (w == WPN_NONE) continue;
            if (inv_first_slot_of(me.inventory, s) != s) continue;
            wpn_btns.push_back({{30.f, wy, 340.f, 42.f}, s, w}); wy += 48.f;
            for (int k = s; k < INVENTORY_SLOTS && me.inventory.slots[k] == w; k++) vis[k] = true;
        }

        lts_btns.clear();
        float ly = 238.f;
        for (int i = 0; i < me.lts.count; i++)
        { lts_btns.push_back({{30.f, ly, 340.f, 42.f}, i, me.lts.weapons[i]}); ly += 48.f; }
    }

    bool handle_click(sf::Vector2f ms, Action &out)
    {
        if (step == Step::ACTION)
        {
            for (int i = 0; i < (int)action_btns.size(); i++)
            {
                auto &b = action_btns[i];
                if (!b.b.contains(ms) || !b.enabled) continue;
                sel_act = i;
                const std::string &l = b.lbl;
                if (l[0] == '0') { kill(gs->arbiter_pid, SIGTERM); g_quit = 1; out = {ActionType::SKIP,-1,-1,WPN_NONE}; return true; }
                if (l.find("Strike") != std::string::npos) { step = Step::ENEMY; status = "Click an enemy to strike"; }
                else if (l.find("Exhaust") != std::string::npos) { step = Step::ENEMY; status = "Click an enemy to exhaust"; }
                else if (l.find("Weapon") != std::string::npos) { step = Step::WEAPON; status = "Click a weapon to use"; }
                else if (l.find("Swap") != std::string::npos) { step = Step::LTS; status = "Click weapon from storage"; }
                else if (l.find("Heal") != std::string::npos) { out = {ActionType::HEAL,-1,-1,WPN_NONE}; return true; }
                else if (l.find("Skip") != std::string::npos) { out = {ActionType::SKIP,-1,-1,WPN_NONE}; return true; }
                else if (l.find("ULTIMATE") != std::string::npos) { out = {ActionType::ULTIMATE,-1,-1,WPN_NONE}; return true; }
                return false;
            }
        }
        if (step == Step::ENEMY)
        {
            for (auto &eb : enemy_btns)
            {
                if (!eb.b.contains(ms)) continue;
                ActionType at = ActionType::STRIKE;
                if (sel_act >= 0)
                {
                    const std::string &l = action_btns[sel_act].lbl;
                    if (l.find("Exhaust") != std::string::npos) at = ActionType::EXHAUST;
                    else if (l.find("Weapon") != std::string::npos) at = ActionType::USE_WEAPON;
                }
                out = {at, eb.idx, sel_wpn_slot, WPN_NONE}; return true;
            }
        }
        if (step == Step::WEAPON)
        {
            for (auto &wb : wpn_btns)
            {
                if (!wb.b.contains(ms)) continue;
                sel_wpn_slot = wb.slot; step = Step::ENEMY; status = "Now click a target enemy"; return false;
            }
        }
        if (step == Step::LTS)
        {
            for (auto &lb : lts_btns)
            {
                if (!lb.b.contains(ms)) continue;
                out = {ActionType::SWAP_IN,-1,-1,lb.wid}; return true;
            }
        }
        return false;
    }

    void handle_esc()
    {
        if (step != Step::ACTION) { step = Step::ACTION; sel_act = -1; status = ""; }
    }

    void draw_wait(sf::RenderWindow &win)
    {
        win.clear(BG); Grid(win);
        Tcx(win, T(font, "CHRONO RIFT", 28, GOLD, true), WW/2.f, 14.f);

        sem_wait(&gs->state_mutex);
        int np2 = gs->num_players, ne = gs->num_enemies, ct = gs->current_turn;
        int kills = gs->enemies_killed;
        Entity ents2[MAX_ENTITIES];
        memcpy(ents2, gs->entities, sizeof(ents2));
        sem_post(&gs->state_mutex);

        char kb[32]; snprintf(kb, sizeof(kb), "Kills: %d / %d", kills, MAX_ENEMIES_KILL);
        Tcx(win, T(font, kb, 13, sf::Color{80,200,80}), WW/2.f, 48.f);

        float col_w = (WW - 40.f) / 2.f;
        auto ph = T(font, "PLAYERS", 12, sf::Color{100,120,180}); ph.setPosition(24.f, 74.f); win.draw(ph);
        float py = 96.f;
        for (int i = 0; i < np2; i++)
        {
            const Entity &e = ents2[i];
            bool active = (ct == i);
            sf::Color nc = !e.alive ? sf::Color{80,40,40} : active ? GOLD : WHITE;
            char ln[80]; snprintf(ln, sizeof(ln), "%s%s  HP:%d/%d  ST:%.0f", e.name, active?" <":"", e.hp, e.max_hp, e.stamina);
            auto t = T(font, ln, 13, nc); t.setPosition(24.f, py); win.draw(t);
            float bw = col_w-20.f, fill = e.max_hp>0?(float)e.hp/e.max_hp*bw:0.f;
            sf::RectangleShape bg({bw,5.f}); bg.setPosition(24.f,py+17.f); bg.setFillColor({30,40,30}); win.draw(bg);
            sf::RectangleShape bar({fill,5.f}); bar.setPosition(24.f,py+17.f); bar.setFillColor({60,200,80}); win.draw(bar);
            float sf2 = e.max_stamina>0?e.stamina/e.max_stamina*bw:0.f;
            sf::RectangleShape sbg({bw,3.f}); sbg.setPosition(24.f,py+24.f); sbg.setFillColor({20,20,50}); win.draw(sbg);
            sf::RectangleShape sbar({sf2,3.f}); sbar.setPosition(24.f,py+24.f); sbar.setFillColor({60,140,255}); win.draw(sbar);
            py += 36.f;
        }

        auto eh = T(font, "ENEMIES", 12, sf::Color{180,80,80}); eh.setPosition(WW/2.f+4.f, 74.f); win.draw(eh);
        float ey = 96.f;
        for (int i = 0; i < ne; i++)
        {
            const Entity &e = ents2[np2+i];
            bool active = (ct == np2+i);
            sf::Color nc = !e.alive?sf::Color{60,30,30}:active?sf::Color{255,160,60}:sf::Color{200,160,160};
            char ln[80]; snprintf(ln, sizeof(ln), "%s%s  HP:%d  ST:%.0f", e.name, active?" <":"", e.hp, e.stamina);
            auto t = T(font, ln, 13, nc); t.setPosition(WW/2.f+4.f, ey); win.draw(t);
            float bw = col_w-20.f, fill = e.max_hp>0?(float)e.hp/e.max_hp*bw:0.f;
            sf::RectangleShape bg({bw,5.f}); bg.setPosition(WW/2.f+4.f,ey+17.f); bg.setFillColor({40,20,20}); win.draw(bg);
            sf::RectangleShape bar({fill,5.f}); bar.setPosition(WW/2.f+4.f,ey+17.f); bar.setFillColor({220,60,60}); win.draw(bar);
            ey += 36.f;
        }

        Tcx(win, T(font, "P = menu / quit", 11, sf::Color{50,60,80}), WW/2.f, WH-22.f);
        Scanlines(win); win.display();
    }

    void draw_picking(sf::RenderWindow &win)
    {
        win.clear(BG); Grid(win);
        sem_wait(&gs->state_mutex); memcpy(ents, gs->entities, sizeof(ents)); sem_post(&gs->state_mutex);
        sf::Vector2f ms = win.mapPixelToCoords(sf::Mouse::getPosition(win));

        {
            float p = 0.5f + 0.5f * sinf(clock_t * 2.f);
            sf::RectangleShape glow({(float)WW, 52.f}); glow.setFillColor({10,20,60,(sf::Uint8)(20+int(p*15))}); win.draw(glow);
            auto ti = T(font, std::string(me.name) + "'s Turn", 22, GOLD, true); ti.setPosition(20.f, 10.f); win.draw(ti);
            const char *ph2 = step==Step::ACTION?"Choose Action":step==Step::ENEMY?"Choose Target":step==Step::WEAPON?"Choose Weapon":"Choose from Storage";
            auto pt = T(font, ph2, 14, ACCENT);
            sf::FloatRect pb = pt.getLocalBounds(); pt.setPosition(WW-pb.width-20.f, 16.f); win.draw(pt);
            auto ph3 = T(font, "P = menu", 11, sf::Color{60,70,90});
            sf::FloatRect pb2 = ph3.getLocalBounds(); ph3.setPosition(WW-pb2.width-20.f, 36.f); win.draw(ph3);
            sf::RectangleShape rule({(float)WW-40.f, 1.f}); rule.setPosition(20.f, 50.f); rule.setFillColor(BDR); win.draw(rule);
        }

        Box(win, 20.f, 58.f, WW-40.f, 148.f, PANEL, BDR);
        Corners(win, 20.f, 58.f, WW-40.f, 148.f);
        {
            auto nm = T(font, me.name, 16, WHITE, true); nm.setPosition(34.f, 66.f); win.draw(nm);
            std::ostringstream hs, ss;
            hs << me.hp << "/" << me.max_hp; ss << (int)me.stamina << "/" << me.max_stamina;
            auto hl = T(font, "HP", 11, DIM); hl.setPosition(34.f, 90.f); win.draw(hl);
            Bar(win, 60.f, 92.f, 280.f, 14.f, (float)me.hp, (float)me.max_hp, HP_P);
            auto hv = T(font, hs.str(), 11, WHITE); hv.setPosition(348.f, 90.f); win.draw(hv);
            auto sl = T(font, "ST", 11, DIM); sl.setPosition(34.f, 114.f); win.draw(sl);
            Bar(win, 60.f, 116.f, 280.f, 14.f, me.stamina, (float)me.max_stamina, STAM);
            auto sv = T(font, ss.str(), 11, WHITE); sv.setPosition(348.f, 114.f); win.draw(sv);
            std::ostringstream ds; ds << "DMG: " << me.damage << "   SPD: " << me.speed;
            auto dv = T(font, ds.str(), 13, DIM); dv.setPosition(34.f, 138.f); win.draw(dv);

            float ix = 450.f, iy = 68.f;
            auto il = T(font, "INVENTORY", 11, DIM); il.setPosition(ix, iy); win.draw(il); iy += 16.f;
            bool vis[INVENTORY_SLOTS] = {}, any = false;
            for (int s = 0; s < INVENTORY_SLOTS; s++)
            {
                if (vis[s]) continue;
                WeaponID w = me.inventory.slots[s]; if (w == WPN_NONE) continue;
                if (inv_first_slot_of(me.inventory, s) != s) continue;
                std::ostringstream ws; ws << "[" << s << "] " << weapon_def(w).name;
                auto wt = T(font, ws.str(), 12, ACCENT); wt.setPosition(ix, iy); win.draw(wt); iy += 15.f; any = true;
                for (int k = s; k < INVENTORY_SLOTS && me.inventory.slots[k] == w; k++) vis[k] = true;
            }
            if (!any) { auto et = T(font, "(empty)", 12, DIM); et.setPosition(ix, iy); win.draw(et); }
            if (can_ult) { auto ut = T(font, "*** ULTIMATE AVAILABLE ***", 13, ULT, true); ut.setPosition(34.f, 158.f); win.draw(ut); }
        }

        Box(win, 20.f, 214.f, 375.f, WH-224.f, PANEL, BDR);
        Corners(win, 20.f, 214.f, 375.f, WH-224.f);

        if (step == Step::ACTION)
        {
            auto lhdr = T(font, "ACTIONS", 11, DIM); lhdr.setPosition(34.f, 220.f); win.draw(lhdr);
            for (auto &b : action_btns)
            {
                bool hov = b.b.contains(ms) && b.enabled;
                Box(win, b.b.left, b.b.top, b.b.width, b.b.height,
                    hov?SEL:PANEL, hov?BDR_HI:b.enabled?BDR:sf::Color{25,28,50}, hov?2.f:1.5f);
                if (b.enabled)
                {
                    sf::RectangleShape bar({3.f, b.b.height-8.f});
                    bar.setPosition(b.b.left+4.f, b.b.top+4.f);
                    bar.setFillColor(hov?b.acc:sf::Color{b.acc.r,b.acc.g,b.acc.b,80}); win.draw(bar);
                }
                sf::Color tc = !b.enabled?DIM:hov?WHITE:sf::Color{160,170,190};
                auto lt = T(font, b.lbl, 15, tc, hov); lt.setPosition(b.b.left+14.f, b.b.top+7.f); win.draw(lt);
                if (!b.sub.empty()) { auto st = T(font, b.sub, 11, hov?ACCENT:DIM); st.setPosition(b.b.left+14.f, b.b.top+b.b.height-18.f); win.draw(st); }
            }
        }
        else if (step == Step::WEAPON)
        {
            auto lhdr = T(font, "SELECT WEAPON  (ESC=back)", 11, ACCENT); lhdr.setPosition(34.f, 220.f); win.draw(lhdr);
            for (auto &wb : wpn_btns)
            {
                bool hov = wb.b.contains(ms);
                Box(win, wb.b.left, wb.b.top, wb.b.width, wb.b.height, hov?SEL:PANEL, hov?BDR_HI:BDR, hov?2.f:1.5f);
                std::ostringstream ws; ws << "[" << wb.slot << "] " << weapon_def(wb.wid).name << "  dmg " << weapon_def(wb.wid).damage;
                auto wt = T(font, ws.str(), 14, hov?WHITE:DIM); wt.setPosition(wb.b.left+8.f, wb.b.top+12.f); win.draw(wt);
            }
        }
        else if (step == Step::LTS)
        {
            auto lhdr = T(font, "LONG-TERM STORAGE  (ESC=back)", 11, PURPLE); lhdr.setPosition(34.f, 220.f); win.draw(lhdr);
            for (auto &lb : lts_btns)
            {
                bool hov = lb.b.contains(ms);
                Box(win, lb.b.left, lb.b.top, lb.b.width, lb.b.height, hov?SEL:PANEL, hov?BDR_HI:BDR, hov?2.f:1.5f);
                auto lt = T(font, weapon_def(lb.wid).name, 14, hov?WHITE:DIM); lt.setPosition(lb.b.left+8.f, lb.b.top+12.f); win.draw(lt);
            }
        }
        else
        {
            auto lhdr = T(font, "CLICK ENEMY ON RIGHT  (ESC=back)", 11, RED); lhdr.setPosition(34.f, 220.f); win.draw(lhdr);
            auto arr = T(font, ">>>", 22, RED, true); arr.setPosition(34.f, 260.f); win.draw(arr);
            if (sel_act >= 0) { auto ab = T(font, "Action: "+action_btns[sel_act].lbl, 13, ACCENT); ab.setPosition(34.f, 300.f); win.draw(ab); }
        }

        Box(win, 400.f, 214.f, WW-420.f, WH-224.f, PANEL, BDR);
        Corners(win, 400.f, 214.f, WW-420.f, WH-224.f);
        {
            sf::Color ec = step==Step::ENEMY?RED:DIM;
            auto eh = T(font, step==Step::ENEMY?"CLICK TO SELECT TARGET":"ENEMIES", 11, ec, step==Step::ENEMY);
            eh.setPosition(414.f, 220.f); win.draw(eh);
        }
        float ey = 238.f;
        for (int i = np; i < nte; i++)
        {
            const Entity &en = ents[i];
            bool clickable = step==Step::ENEMY && en.alive;
            bool hov = false;
            for (auto &eb : enemy_btns) if (eb.idx==i) { eb.b={400.f,ey,WW-420.f,38.f}; hov=clickable&&eb.b.contains(ms); break; }
            Box(win, 400.f, ey, WW-420.f, 38.f, hov?SEL:PANEL, hov?RED:BDR, hov?2.f:1.5f);
            std::string nm = en.name;
            if (!en.alive) nm += " [DEAD]"; else if (en.stunned) nm += " [STUN]";
            auto nt = T(font, nm, 13, en.alive?WHITE:sf::Color{80,50,50}); nt.setPosition(408.f, ey+4.f); win.draw(nt);
            if (en.alive)
            {
                Bar(win, 408.f, ey+22.f, 110.f, 7.f, (float)en.hp, (float)en.max_hp, HP_E);
                std::ostringstream hs; hs << en.hp << "/" << en.max_hp;
                auto hv = T(font, hs.str(), 10, DIM); hv.setPosition(524.f, ey+20.f); win.draw(hv);
                Bar(win, 570.f, ey+22.f, 80.f, 7.f, en.stamina, (float)en.max_stamina, STAM);
                std::ostringstream ss; ss << "ST " << (int)en.stamina;
                auto sv = T(font, ss.str(), 10, DIM); sv.setPosition(656.f, ey+20.f); win.draw(sv);
            }
            ey += 44.f;
        }

        if (!status.empty())
        {
            float p = 0.5f + 0.5f*sinf(clock_t*4.f);
            auto sm = T(font, status, 14, {60,200,255,(sf::Uint8)(180+int(p*75))}, true);
            sf::FloatRect sb = sm.getLocalBounds(); sm.setOrigin(sb.left+sb.width/2.f, sb.top);
            sm.setPosition(WW/2.f, WH-22.f); win.draw(sm);
        }
        Scanlines(win); win.display();
    }
};

struct PlayerArg { int pidx; };

static void *player_thread(void *arg_)
{
    PlayerArg *a = static_cast<PlayerArg *>(arg_);
    int pidx = a->pidx; delete a;
    gs->entities[pidx].pid = getpid();

    while (!g_quit)
    {
        sem_wait(&player_sem[pidx]);
        if (g_quit) break;
        if (gs->current_turn != pidx || !gs->turn_ready || !gs->entities[pidx].alive) continue;

        if (gs->entities[pidx].stunned || g_stunned)
        {
            Action act{ActionType::SKIP,-1,-1,WPN_NONE};
            sem_wait(&gs->state_mutex); gs->entities[pidx].pending_action = act; gs->entities[pidx].action_ready = true; sem_post(&gs->state_mutex);
            sem_post(&gs->action_sem); continue;
        }

        pthread_mutex_lock(&g_ui_mtx); g_ui_pidx = pidx; pthread_mutex_unlock(&g_ui_mtx);
        sem_post(&g_ui_req_sem); sem_wait(&g_ui_done_sem);

        Action act = g_ui_result;
        sem_wait(&gs->state_mutex); gs->entities[pidx].pending_action = act; gs->entities[pidx].action_ready = true; sem_post(&gs->state_mutex);
        sem_post(&gs->action_sem);
    }
    return nullptr;
}

static void *dispatcher(void *)
{
    while (!g_quit)
    {
        sem_wait(&gs->hip_turn_sem);
        if (g_quit) break;
        int ct = gs->current_turn;
        if (ct >= 0 && ct < gs->num_players && gs->entities[ct].alive)
            sem_post(&player_sem[ct]);
    }
    return nullptr;
}

int main(int argc, char *argv[])
{
    if (argc < 3) { fprintf(stderr, "Usage: hip <roll_no> <num_players>\n"); return 1; }
    g_num_players = atoi(argv[2]);
    gs = shm_open_existing();

    struct sigaction sa{};
    sa.sa_handler = sig_term; sigaction(SIGTERM, &sa, nullptr);
    sa.sa_handler = sig_stun; sigaction(SIGUSR1, &sa, nullptr);

    sem_init(&g_ui_req_sem, 0, 0); sem_init(&g_ui_done_sem, 0, 0);
    for (int i = 0; i < g_num_players; i++) sem_init(&player_sem[i], 0, 0);

    sf::RenderWindow win(sf::VideoMode(WW, WH), "Chrono Rift - Player Actions",
                         sf::Style::Titlebar | sf::Style::Close);
    win.setFramerateLimit(60); win.setVerticalSyncEnabled(false);

    HipUI ui; ui.load_font();

    pthread_t disp_tid; pthread_create(&disp_tid, nullptr, dispatcher, nullptr);
    pthread_t ptids[MAX_PLAYERS];
    for (int i = 0; i < g_num_players; i++)
    { auto *a = new PlayerArg{i}; pthread_create(&ptids[i], nullptr, player_thread, a); }

    sf::Clock clk;

    while (!g_quit && gs->phase != GamePhase::GAME_OVER && win.isOpen())
    {
        ui.clock_t = clk.getElapsedTime().asSeconds();

        if (gs->pending_drop_ready && !gs->pending_drop_done)
        {
            WeaponID dwpn = gs->pending_drop_wpn;
            int dfor = gs->pending_drop_for;
            bool taken = false;

            if (dfor >= 0 && dfor < g_num_players)
            {
                sf::RenderWindow dwin(sf::VideoMode(480, 240), "Weapon Drop!", sf::Style::Titlebar|sf::Style::Close);
                dwin.setFramerateLimit(60);
                sf::FloatRect yes_b = {60.f,162.f,150.f,44.f}, no_b = {270.f,162.f,150.f,44.f};
                while (dwin.isOpen())
                {
                    sf::Vector2f ms = dwin.mapPixelToCoords(sf::Mouse::getPosition(dwin));
                    sf::Event dev{};
                    while (dwin.pollEvent(dev))
                    {
                        if (dev.type==sf::Event::Closed) { dwin.close(); break; }
                        if (dev.type==sf::Event::KeyPressed)
                        {
                            if (dev.key.code==sf::Keyboard::Y||dev.key.code==sf::Keyboard::Return) { taken=true; dwin.close(); }
                            if (dev.key.code==sf::Keyboard::N||dev.key.code==sf::Keyboard::Escape) { taken=false; dwin.close(); }
                        }
                        if (dev.type==sf::Event::MouseButtonPressed&&dev.mouseButton.button==sf::Mouse::Left)
                        {
                            if (yes_b.contains(ms)) { taken=true; dwin.close(); }
                            if (no_b.contains(ms)) { taken=false; dwin.close(); }
                        }
                    }
                    if (!dwin.isOpen()) break;
                    dwin.clear(sf::Color{8,10,20});
                    auto ttl = T(ui.font, "WEAPON DROP!", 22, sf::Color{220,185,80}, true);
                    sf::FloatRect tb = ttl.getLocalBounds(); ttl.setOrigin(tb.left+tb.width/2.f,tb.top); ttl.setPosition(240.f,14.f); dwin.draw(ttl);
                    char wbuf[64]; snprintf(wbuf,sizeof(wbuf),"%s  dmg=%d  slots=%d",weapon_def(dwpn).name,weapon_def(dwpn).damage,weapon_def(dwpn).slots);
                    auto wt = T(ui.font, wbuf, 16, sf::Color{60,200,255});
                    sf::FloatRect wb2=wt.getLocalBounds(); wt.setOrigin(wb2.left+wb2.width/2.f,wb2.top); wt.setPosition(240.f,56.f); dwin.draw(wt);
                    char qbuf[64]; snprintf(qbuf,sizeof(qbuf),"%s: pick it up?",gs->entities[dfor].name);
                    auto qt=T(ui.font,qbuf,14,sf::Color{200,210,220});
                    sf::FloatRect qb=qt.getLocalBounds(); qt.setOrigin(qb.left+qb.width/2.f,qb.top); qt.setPosition(240.f,86.f); dwin.draw(qt);
                    auto hint=T(ui.font,"Y / Enter = Yes     N / Esc = No",11,sf::Color{80,90,110});
                    sf::FloatRect hb=hint.getLocalBounds(); hint.setOrigin(hb.left+hb.width/2.f,hb.top); hint.setPosition(240.f,112.f); dwin.draw(hint);

                    bool yh=yes_b.contains(ms);
                    sf::RectangleShape ybox({yes_b.width,yes_b.height}); ybox.setPosition(yes_b.left,yes_b.top);
                    ybox.setFillColor(yh?sf::Color{10,50,20}:sf::Color{16,18,35}); ybox.setOutlineColor(yh?sf::Color{60,220,120}:sf::Color{40,80,60}); ybox.setOutlineThickness(1.5f); dwin.draw(ybox);
                    auto yt=T(ui.font,"YES (Y)",16,yh?sf::Color{60,220,120}:sf::Color{80,100,80},yh);
                    sf::FloatRect ytb=yt.getLocalBounds(); yt.setOrigin(ytb.left+ytb.width/2.f,ytb.top); yt.setPosition(yes_b.left+yes_b.width/2.f,yes_b.top+12.f); dwin.draw(yt);

                    bool nh=no_b.contains(ms);
                    sf::RectangleShape nbox({no_b.width,no_b.height}); nbox.setPosition(no_b.left,no_b.top);
                    nbox.setFillColor(nh?sf::Color{50,10,10}:sf::Color{16,18,35}); nbox.setOutlineColor(nh?sf::Color{220,60,60}:sf::Color{80,40,40}); nbox.setOutlineThickness(1.5f); dwin.draw(nbox);
                    auto nt2=T(ui.font,"NO  (N)",16,nh?sf::Color{220,60,60}:sf::Color{100,60,60},nh);
                    sf::FloatRect ntb=nt2.getLocalBounds(); nt2.setOrigin(ntb.left+ntb.width/2.f,ntb.top); nt2.setPosition(no_b.left+no_b.width/2.f,no_b.top+12.f); dwin.draw(nt2);
                    dwin.display();
                }
            }

            sem_wait(&gs->state_mutex); gs->pending_drop_taken=taken; gs->pending_drop_done=true; sem_post(&gs->state_mutex);
        }

        struct timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 16000000LL;
        if (ts.tv_nsec >= 1000000000LL) { ts.tv_sec++; ts.tv_nsec -= 1000000000LL; }

        bool got_req = (sem_timedwait(&g_ui_req_sem, &ts) == 0);

        if (got_req)
        {
            pthread_mutex_lock(&g_ui_mtx); int pidx = g_ui_pidx; g_ui_pidx = -1; pthread_mutex_unlock(&g_ui_mtx);

            if (pidx >= 0 && pidx < g_num_players)
            {
                ui.begin_turn(pidx);
                Action result{}; bool done = false;
                while (!done && !g_quit && win.isOpen())
                {
                    ui.clock_t = clk.getElapsedTime().asSeconds();
                    sf::Event ev{};
                    while (win.pollEvent(ev))
                    {
                        if (ev.type==sf::Event::Closed) { g_quit=1; break; }
                        if (ev.type==sf::Event::KeyPressed)
                        {
                            if (ev.key.code==sf::Keyboard::Escape) ui.handle_esc();
                            if (ev.key.code==sf::Keyboard::P)
                            { bool q=draw_pause_menu(win,ui.font); if(q){done=true;result={ActionType::SKIP,-1,-1,WPN_NONE};} }
                        }
                        if (ev.type==sf::Event::MouseButtonPressed&&ev.mouseButton.button==sf::Mouse::Left)
                        { sf::Vector2f ms=win.mapPixelToCoords({ev.mouseButton.x,ev.mouseButton.y}); if(ui.handle_click(ms,result)) done=true; }
                    }
                    if (!done) ui.draw_picking(win);
                }
                if (g_quit) result={ActionType::SKIP,-1,-1,WPN_NONE};
                g_ui_result=result; ui.mode=UIMode::WAIT;
            }
            else { g_ui_result={ActionType::SKIP,-1,-1,WPN_NONE}; ui.mode=UIMode::WAIT; }
            sem_post(&g_ui_done_sem);
        }

        sf::Event ev{};
        while (win.pollEvent(ev))
        {
            if (ev.type==sf::Event::Closed) { g_quit=1; kill(gs->arbiter_pid,SIGTERM); }
            if (ev.type==sf::Event::KeyPressed&&ev.key.code==sf::Keyboard::P) draw_pause_menu(win,ui.font);
        }
        if (ui.mode==UIMode::WAIT) ui.draw_wait(win);
    }

    if (win.isOpen()) win.close();
    g_quit=1;
    for (int i=0;i<g_num_players;i++) sem_post(&player_sem[i]);
    sem_post(&gs->hip_turn_sem); sem_post(&g_ui_req_sem); sem_post(&g_ui_done_sem);

    pthread_join(disp_tid, nullptr);
    for (int i=0;i<g_num_players;i++) pthread_join(ptids[i],nullptr);
    for (int i=0;i<g_num_players;i++) sem_destroy(&player_sem[i]);
    sem_destroy(&g_ui_req_sem); sem_destroy(&g_ui_done_sem);
    munmap(gs, sizeof(SharedState));
    return 0;
}
