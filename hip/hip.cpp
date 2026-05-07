/*
 * hip.cpp  –  Human Interfacing Process
 *
 * Threading architecture (OS requirement preserved):
 *  - One pthread per player  (player_thread[0..N-1])
 *  - One dispatcher pthread  (watches gs->hip_turn_sem, wakes active player)
 *  - Main thread owns ALL SFML windows (X11/Mesa requires this on WSL2)
 *
 * Flow per turn:
 *  1. Arbiter posts hip_turn_sem  →  dispatcher wakes player_sem[i]
 *  2. player_thread[i] wakes, sets g_ui_request = {pidx, ...}
 *  3. player_thread[i] waits on g_ui_done semaphore
 *  4. Main thread sees g_ui_request, opens SFML window, collects action
 *  5. Main thread stores result in g_ui_result, posts g_ui_done
 *  6. player_thread[i] reads g_ui_result, posts action to shared memory
 *
 * This is the ONLY legal way to use SFML on WSL2/X11 with pthreads:
 * windows must be created and driven from the process's main thread.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>
#include <cmath>
#include <atomic>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>
#include <time.h>
#include <errno.h>

#include <SFML/Graphics.hpp>
#include <SFML/System.hpp>
#include <SFML/Window.hpp>

#include "shared_state.h"
#include "inventory.h"

/* ════════════════════════════════════════════════════════════════════
 * Globals
 * ════════════════════════════════════════════════════════════════════ */
static SharedState*          gs            = nullptr;
static int                   g_num_players = 0;
static volatile sig_atomic_t g_quit        = 0;
static volatile sig_atomic_t g_stunned     = 0;
static sem_t                 player_sem[MAX_PLAYERS];

/* ── Main-thread UI request/response channel ── */
struct UIRequest {
    int  pidx     = -1;   /* which player needs input; -1 = none pending */
    bool pending  = false;
};
static UIRequest             g_ui_req{};
static Action                g_ui_result{};
static sem_t                 g_ui_request_sem;   /* player posts → main wakes */
static sem_t                 g_ui_done_sem;      /* main posts → player wakes */
static pthread_mutex_t       g_ui_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Font paths ── */
static const char* FONT_PATHS[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
    nullptr
};

/* ════════════════════════════════════════════════════════════════════
 * Signal handlers
 * ════════════════════════════════════════════════════════════════════ */
static void sig_term(int) { g_quit = 1; }

static void sig_stun(int)
{
    g_stunned = 1;
    int ct = gs->current_turn;
    if (ct >= 0 && ct < gs->num_players)
        gs->entities[ct].stunned = true;
    sleep(STUN_DURATION);
    if (ct >= 0 && ct < gs->num_players) {
        gs->entities[ct].stunned = false;
        gs->entities[ct].stamina = 0;
    }
    g_stunned = 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Shared memory
 * ════════════════════════════════════════════════════════════════════ */
static SharedState* shm_open_existing()
{
    int fd = -1;
    for (int i = 0; i < 30; ++i) {
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (fd >= 0) break;
        usleep(100000);
    }
    if (fd < 0) { perror("hip: shm_open"); exit(1); }
    void* p = mmap(nullptr, sizeof(SharedState),
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("hip: mmap"); exit(1); }
    close(fd);
    return static_cast<SharedState*>(p);
}

/* ════════════════════════════════════════════════════════════════════
 * UI drawing helpers  (used by main thread only)
 * ════════════════════════════════════════════════════════════════════ */
static const unsigned UI_W = 820;
static const unsigned UI_H = 680;

static const sf::Color C_BG       {  8,  10,  20 };
static const sf::Color C_PANEL    { 16,  18,  38 };
static const sf::Color C_BORDER   { 40,  80, 160 };
static const sf::Color C_BORDER_HI{ 80, 160, 255 };
static const sf::Color C_GOLD     {220, 185,  80 };
static const sf::Color C_WHITE    {230, 235, 245 };
static const sf::Color C_DIM      {100, 110, 130 };
static const sf::Color C_ACCENT   { 60, 200, 255 };
static const sf::Color C_GREEN    { 60, 220, 120 };
static const sf::Color C_RED      {220,  70,  70 };
static const sf::Color C_HP_P     { 60, 200,  80 };
static const sf::Color C_HP_E     {220,  60,  60 };
static const sf::Color C_STAMINA  { 60, 180, 255 };
static const sf::Color C_SCAN     {  0,  20,  60,  15 };
static const sf::Color C_SELECT   { 20,  50, 100 };
static const sf::Color C_ULTIMATE {255, 100, 200 };

static sf::Text mkt(const sf::Font& f, const std::string& s,
                    unsigned sz, sf::Color c, bool bold=false)
{
    sf::Text t; t.setFont(f); t.setString(s);
    t.setCharacterSize(sz); t.setFillColor(c);
    if (bold) t.setStyle(sf::Text::Bold);
    return t;
}

static void draw_centered(sf::RenderTarget& rt, sf::Text t, float cx, float y)
{
    sf::FloatRect b = t.getLocalBounds();
    t.setOrigin(b.left+b.width/2.f, b.top);
    t.setPosition(cx, y); rt.draw(t);
}

static void draw_box(sf::RenderTarget& rt,
                     float x, float y, float w, float h,
                     sf::Color fill, sf::Color border, float thick=1.5f)
{
    sf::RectangleShape r({w,h}); r.setPosition(x,y);
    r.setFillColor(fill); r.setOutlineColor(border);
    r.setOutlineThickness(thick); rt.draw(r);
}

static void draw_bar(sf::RenderTarget& rt,
                     float x, float y, float w, float h,
                     float val, float maxv, sf::Color col)
{
    draw_box(rt,x,y,w,h,{30,30,50},C_BORDER,1.f);
    if (maxv>0.f) {
        float r=val/maxv; if(r<0)r=0; if(r>1)r=1;
        draw_box(rt,x,y,w*r,h,col,col,0.f);
    }
}

static void draw_corners(sf::RenderTarget& rt,
                          float x, float y, float w, float h, float sz=10.f)
{
    sf::RectangleShape r; r.setFillColor(C_GOLD);
    auto seg=[&](float px,float py,float pw,float ph){
        r.setSize({pw,ph}); r.setPosition(px,py); rt.draw(r);};
    seg(x,y,sz,2);    seg(x,y,2,sz);
    seg(x+w-sz,y,sz,2); seg(x+w-2,y,2,sz);
    seg(x,y+h-2,sz,2);  seg(x,y+h-sz,2,sz);
    seg(x+w-sz,y+h-2,sz,2); seg(x+w-2,y+h-sz,2,sz);
}

static void draw_scanlines(sf::RenderTarget& rt)
{
    sf::RectangleShape l({static_cast<float>(UI_W),1.f}); l.setFillColor(C_SCAN);
    for(unsigned y=0;y<UI_H;y+=4){l.setPosition(0,static_cast<float>(y));rt.draw(l);}
}


/* ════════════════════════════════════════════════════════════════════
 * show_drop_window()
 * Called from main thread when arbiter signals a weapon drop.
 * Shows a YES / NO prompt. Returns true if player picks up the weapon.
 * ════════════════════════════════════════════════════════════════════ */
static bool show_drop_window(WeaponID drop_wpn, int player_idx, const sf::Font& font)
{
    const Entity& me = gs->entities[player_idx];
    std::string wname = weapon_def(drop_wpn).name;

    sf::RenderWindow win(sf::VideoMode(500, 260),
                         "Weapon Drop!",
                         sf::Style::Titlebar | sf::Style::Close);
    win.setFramerateLimit(60);
    sf::Clock clk;

    sf::FloatRect yes_b = {60.f,  180.f, 160.f, 48.f};
    sf::FloatRect no_b  = {280.f, 180.f, 160.f, 48.f};

    while (win.isOpen()) {
        sf::Vector2f ms = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        float t = clk.getElapsedTime().asSeconds();
        (void)t;

        sf::Event ev{};
        while (win.pollEvent(ev)) {
            if (ev.type == sf::Event::Closed)        { win.close(); return false; }
            if (ev.type == sf::Event::KeyPressed) {
                if (ev.key.code == sf::Keyboard::Y)  { win.close(); return true;  }
                if (ev.key.code == sf::Keyboard::N)  { win.close(); return false; }
                if (ev.key.code == sf::Keyboard::Return){ win.close(); return true; }
                if (ev.key.code == sf::Keyboard::Escape){ win.close(); return false;}
            }
            if (ev.type == sf::Event::MouseButtonPressed &&
                ev.mouseButton.button == sf::Mouse::Left) {
                if (yes_b.contains(ms)) { win.close(); return true;  }
                if (no_b.contains(ms))  { win.close(); return false; }
            }
        }

        win.clear(C_BG);

        // Title
        auto title = mkt(font, "WEAPON DROP!", 24, C_GOLD, true);
        draw_centered(win, title, 250.f, 18.f);

        sf::RectangleShape rule({460.f, 1.f});
        rule.setPosition(20.f, 52.f); rule.setFillColor(C_BORDER); win.draw(rule);

        // Weapon info
        std::string line1 = wname + "  (dmg " +
                            std::to_string(weapon_def(drop_wpn).damage) + ", " +
                            std::to_string(weapon_def(drop_wpn).slots) + " slots)";
        auto wt = mkt(font, line1, 17, C_ACCENT);
        draw_centered(win, wt, 250.f, 68.f);

        std::string line2 = std::string(me.name) + ", do you want to pick it up?";
        auto qt = mkt(font, line2, 14, C_WHITE);
        draw_centered(win, qt, 250.f, 100.f);

        auto hint = mkt(font, "Y / Enter = Yes     N / Esc = No", 12, C_DIM);
        draw_centered(win, hint, 250.f, 124.f);

        // YES button
        bool yes_hov = yes_b.contains(ms);
        draw_box(win, yes_b.left, yes_b.top, yes_b.width, yes_b.height,
                 yes_hov ? sf::Color{10,60,20} : C_PANEL,
                 yes_hov ? C_GREEN : C_BORDER, yes_hov ? 2.f : 1.5f);
        auto yt = mkt(font, "YES  (Y)", 18, yes_hov ? C_GREEN : C_DIM, yes_hov);
        draw_centered(win, yt, yes_b.left + yes_b.width/2.f, yes_b.top + 12.f);

        // NO button
        bool no_hov = no_b.contains(ms);
        draw_box(win, no_b.left, no_b.top, no_b.width, no_b.height,
                 no_hov ? sf::Color{60,10,10} : C_PANEL,
                 no_hov ? C_RED : C_BORDER, no_hov ? 2.f : 1.5f);
        auto nt2 = mkt(font, "NO   (N)", 18, no_hov ? C_RED : C_DIM, no_hov);
        draw_centered(win, nt2, no_b.left + no_b.width/2.f, no_b.top + 12.f);

        win.display();
    }
    return false;
}

/* ════════════════════════════════════════════════════════════════════
 * show_action_window()
 * Called ONLY from the main thread.
 * Opens window, runs event loop, returns chosen Action.
 * ════════════════════════════════════════════════════════════════════ */
static Action show_action_window(int pidx, const sf::Font& font)
{
    /* snapshot under lock */
    sem_wait(&gs->state_mutex);
    Entity me = gs->entities[pidx];
    Entity ents[MAX_ENTITIES];
    memcpy(ents, gs->entities, sizeof(ents));
    int np = gs->num_players, nte = gs->total_entities;
    sem_post(&gs->state_mutex);

    bool can_ultimate = inv_has_weapon(me, WPN_SOLAR_CORE) &&
                        inv_has_weapon(me, WPN_LUNAR_BLADE);

    /* ── Window (main thread — always works on WSL2) ── */
    sf::RenderWindow win(sf::VideoMode(UI_W,UI_H),
                         std::string(me.name)+" - Choose Action",
                         sf::Style::Titlebar|sf::Style::Close);
    win.setFramerateLimit(60);

    sf::Clock clk;

    /* ── Build action button list ── */
    const float BTN_X=30.f, BTN_W=340.f, BTN_H=52.f, BTN_GAP=8.f;
    struct Btn {
        sf::FloatRect b; std::string lbl,sub; sf::Color acc;
        bool enabled=true, hov=false;
    };
    std::vector<Btn> btns;
    float by=220.f;
    auto add=[&](const std::string& l,const std::string& s,sf::Color a,bool en=true){
        btns.push_back({{BTN_X,by,BTN_W,BTN_H},l,s,a,en});
        by+=BTN_H+BTN_GAP;
    };
    add("1  Strike",      "Direct damage to enemy",          C_RED);
    add("2  Exhaust",     "Drain enemy stamina",             {255,140,0});
    bool has_inv=false;
    for(int s=0;s<INVENTORY_SLOTS;s++) if(me.inventory.slots[s]!=WPN_NONE){has_inv=true;break;}
    add("3  Use Weapon",  "Attack with inventory weapon",    C_ACCENT, has_inv);
    add("4  Swap In",     "Retrieve from Long-Term Storage", {180,100,255}, me.lts.count>0);
    add("5  Heal",        "Restore 10% of max HP",           C_GREEN);
    add("6  Skip",        "50% stamina refund",              C_DIM);
    if(can_ultimate)
        add("7  ULTIMATE","Solar Core + Lunar Blade attack", C_ULTIMATE);
    add("0  Quit Game",   "Send SIGTERM to arbiter",         C_RED);

    /* ── Enemy buttons ── */
    struct EBtn { sf::FloatRect b; int idx; bool hov=false; };
    std::vector<EBtn> ebtns;
    const float EB_X=390.f,EB_W=400.f,EB_H=38.f,EB_G=6.f;
    { float ey=220.f;
      for(int i=np;i<nte;i++) if(ents[i].alive){
          ebtns.push_back({{EB_X,ey,EB_W,EB_H},i}); ey+=EB_H+EB_G; }}

    /* ── Weapon buttons ── */
    struct WBtn { sf::FloatRect b; int slot; WeaponID wid; bool hov=false; };
    std::vector<WBtn> wbtns;
    { float wy=220.f; bool vis[INVENTORY_SLOTS]={};
      for(int s=0;s<INVENTORY_SLOTS;s++){
          if(vis[s]) continue;
          WeaponID w=me.inventory.slots[s]; if(w==WPN_NONE) continue;
          if(inv_first_slot_of(me.inventory,s)!=s) continue;
          wbtns.push_back({{BTN_X,wy,BTN_W,BTN_H},s,w}); wy+=BTN_H+BTN_GAP;
          for(int k=s;k<INVENTORY_SLOTS&&me.inventory.slots[k]==w;k++) vis[k]=true; }}

    /* ── LTS buttons ── */
    struct LBtn { sf::FloatRect b; int li; WeaponID wid; bool hov=false; };
    std::vector<LBtn> lbtns;
    { float ly=220.f;
      for(int i=0;i<me.lts.count;i++){
          lbtns.push_back({{BTN_X,ly,BTN_W,BTN_H},i,me.lts.weapons[i]}); ly+=BTN_H+BTN_GAP; }}

    enum class St { ACTION, ENEMY, WEAPON, LTS };
    St state=St::ACTION;
    int sel_act=-1, sel_wpn_slot=-1;
    std::string status;
    Action result{};


    while(win.isOpen()&&!g_quit){
        sf::Vector2f ms=win.mapPixelToCoords(sf::Mouse::getPosition(win));
        float t=clk.getElapsedTime().asSeconds();

        /* refresh snapshot */
        sem_wait(&gs->state_mutex);
        memcpy(ents,gs->entities,sizeof(ents));
        sem_post(&gs->state_mutex);

        for(auto& b:btns)  b.hov=b.b.contains(ms);
        for(auto& b:ebtns) b.hov=b.b.contains(ms);
        for(auto& b:wbtns) b.hov=b.b.contains(ms);
        for(auto& b:lbtns) b.hov=b.b.contains(ms);

        sf::Event ev{};
        while(win.pollEvent(ev)){
            if(ev.type==sf::Event::Closed){
                result={ActionType::SKIP,-1,-1,WPN_NONE};
                win.close(); return result;
            }
            if(ev.type==sf::Event::KeyPressed&&ev.key.code==sf::Keyboard::Escape){
                if(state!=St::ACTION){ state=St::ACTION; sel_act=-1; status=""; }
            }
            if(ev.type==sf::Event::MouseButtonPressed&&
               ev.mouseButton.button==sf::Mouse::Left){

                if(state==St::ACTION){
                    for(int i=0;i<(int)btns.size();i++){
                        if(!btns[i].b.contains(ms)||!btns[i].enabled) continue;
                        sel_act=i;
                        const std::string& lbl=btns[i].lbl;
                        if(lbl[0]=='0'){ kill(gs->arbiter_pid,SIGTERM); g_quit=1;
                            result={ActionType::SKIP,-1,-1,WPN_NONE}; win.close(); return result; }
                        if(lbl.find("Strike") !=std::string::npos){state=St::ENEMY;  status="Select a target enemy";}
                        else if(lbl.find("Exhaust")!=std::string::npos){state=St::ENEMY; status="Select enemy to exhaust";}
                        else if(lbl.find("Weapon") !=std::string::npos){state=St::WEAPON;status="Select a weapon";}
                        else if(lbl.find("Swap")   !=std::string::npos){state=St::LTS;   status="Select from Long-Term Storage";}
                        else if(lbl.find("Heal")   !=std::string::npos){result={ActionType::HEAL,-1,-1,WPN_NONE};win.close();return result;}
                        else if(lbl.find("Skip")   !=std::string::npos){result={ActionType::SKIP,-1,-1,WPN_NONE};win.close();return result;}
                        else if(lbl.find("ULTIMATE")!=std::string::npos){result={ActionType::ULTIMATE,-1,-1,WPN_NONE};win.close();return result;}
                        break;
                    }
                }
                else if(state==St::ENEMY){
                    for(auto& eb:ebtns){
                        if(!eb.b.contains(ms)) continue;
                        ActionType at=ActionType::STRIKE;
                        if(sel_act>=0){
                            const std::string& l=btns[sel_act].lbl;
                            if(l.find("Exhaust")!=std::string::npos) at=ActionType::EXHAUST;
                            else if(l.find("Weapon")!=std::string::npos) at=ActionType::USE_WEAPON;
                        }
                        result={at,eb.idx,sel_wpn_slot,WPN_NONE};
                        win.close(); return result;
                    }
                }
                else if(state==St::WEAPON){
                    for(auto& wb:wbtns){
                        if(!wb.b.contains(ms)) continue;
                        sel_wpn_slot=wb.slot; state=St::ENEMY; status="Now select a target enemy"; break;
                    }
                }
                else if(state==St::LTS){
                    for(auto& lb:lbtns){
                        if(!lb.b.contains(ms)) continue;
                        result={ActionType::SWAP_IN,-1,-1,lb.wid}; win.close(); return result;
                    }
                }
            }
        }

        /* ══ DRAW ══ */
        win.clear(C_BG);

        /* grid */
        {sf::RectangleShape gl({static_cast<float>(UI_W),1.f}); gl.setFillColor({20,30,60,30});
         for(unsigned gy=0;gy<UI_H;gy+=32){gl.setPosition(0,static_cast<float>(gy));win.draw(gl);}
         gl.setSize({1.f,static_cast<float>(UI_H)});
         for(unsigned gx=0;gx<UI_W;gx+=32){gl.setPosition(static_cast<float>(gx),0);win.draw(gl);}
        }

        /* title */
        {float pulse=0.5f+0.5f*std::sin(t*2.f);
         sf::RectangleShape glow({static_cast<float>(UI_W),52.f});
         glow.setFillColor({10,20,60,static_cast<sf::Uint8>(20+int(pulse*15))}); win.draw(glow);
         auto ti=mkt(font,std::string(me.name)+"'s Turn",22,C_GOLD,true); ti.setPosition(20.f,10.f); win.draw(ti);
         const char* ph= state==St::ACTION?"Choose Action":state==St::ENEMY?"Choose Target":
                         state==St::WEAPON?"Choose Weapon":"Choose from Storage";
         auto pt=mkt(font,ph,14,C_ACCENT); sf::FloatRect pb=pt.getLocalBounds();
         pt.setPosition(UI_W-pb.width-20.f,16.f); win.draw(pt);
         sf::RectangleShape rule({static_cast<float>(UI_W)-40.f,1.f});
         rule.setPosition(20.f,50.f); rule.setFillColor(C_BORDER); win.draw(rule);}

        /* stats panel */
        {draw_box(win,20.f,58.f,UI_W-40.f,148.f,C_PANEL,C_BORDER);
         draw_corners(win,20.f,58.f,UI_W-40.f,148.f);
         auto nm=mkt(font,me.name,16,C_WHITE,true); nm.setPosition(34.f,66.f); win.draw(nm);
         /* HP */
         std::ostringstream hps; hps<<me.hp<<"/"<<me.max_hp;
         auto hl=mkt(font,"HP",11,C_DIM); hl.setPosition(34.f,90.f); win.draw(hl);
         draw_bar(win,60.f,92.f,300.f,14.f,(float)me.hp,(float)me.max_hp,C_HP_P);
         auto hv=mkt(font,hps.str(),11,C_WHITE); hv.setPosition(368.f,90.f); win.draw(hv);
         /* Stamina */
         std::ostringstream sts; sts<<(int)me.stamina<<"/"<<me.max_stamina;
         auto sl=mkt(font,"ST",11,C_DIM); sl.setPosition(34.f,114.f); win.draw(sl);
         draw_bar(win,60.f,116.f,300.f,14.f,me.stamina,(float)me.max_stamina,C_STAMINA);
         auto sv=mkt(font,sts.str(),11,C_WHITE); sv.setPosition(368.f,114.f); win.draw(sv);
         /* stats */
         std::ostringstream ss2; ss2<<"DMG: "<<me.damage<<"   SPD: "<<me.speed;
         auto sv2=mkt(font,ss2.str(),13,C_DIM); sv2.setPosition(34.f,138.f); win.draw(sv2);
         /* inventory quick view */
         float ix=450.f,iy=68.f;
         auto il=mkt(font,"INVENTORY",11,C_DIM); il.setPosition(ix,iy); win.draw(il); iy+=16.f;
         bool vis[INVENTORY_SLOTS]={},any=false;
         for(int s=0;s<INVENTORY_SLOTS;s++){
             if(vis[s]) continue;
             WeaponID w=me.inventory.slots[s];
             if(w==WPN_NONE) continue;
             if(inv_first_slot_of(me.inventory,s)!=s) continue;
             std::ostringstream ws; ws<<"["<<s<<"] "<<weapon_def(w).name<<" (dmg "<<weapon_def(w).damage<<")";
             auto wt=mkt(font,ws.str(),12,C_ACCENT); wt.setPosition(ix,iy); win.draw(wt); iy+=16.f; any=true;
             for(int k=s;k<INVENTORY_SLOTS&&me.inventory.slots[k]==w;k++) { vis[k]=true; }}
         if(!any){auto et=mkt(font,"(empty)",12,C_DIM); et.setPosition(ix,iy); win.draw(et); iy+=16.f;}
         if(me.lts.count>0){
             auto ll=mkt(font,"LTS:",11,C_DIM); ll.setPosition(ix,iy); win.draw(ll); iy+=14.f;
             for(int i=0;i<me.lts.count;i++){
                 auto lt=mkt(font,weapon_def(me.lts.weapons[i]).name,11,{180,100,255});
                 lt.setPosition(ix+8.f,iy); win.draw(lt); iy+=14.f;}}
         if(can_ultimate){auto ut=mkt(font,"*** ULTIMATE AVAILABLE ***",13,C_ULTIMATE,true); ut.setPosition(34.f,158.f); win.draw(ut);}
        }

        /* left panel header */
        draw_box(win,20.f,214.f,360.f,UI_H-224.f,C_PANEL,C_BORDER);
        draw_corners(win,20.f,214.f,360.f,UI_H-224.f);
        {const char* sec=state==St::WEAPON?"SELECT WEAPON":state==St::LTS?"LONG-TERM STORAGE":"ACTIONS";
         auto sl2=mkt(font,sec,11,C_DIM); sl2.setPosition(34.f,220.f); win.draw(sl2);}

        if(state==St::ACTION){
            for(auto& b:btns){
                sf::Color fill =b.hov&&b.enabled?sf::Color{15,25,55}:C_PANEL;
                sf::Color bord =b.hov&&b.enabled?C_BORDER_HI:b.enabled?C_BORDER:sf::Color{25,28,50};
                sf::Color tcol =!b.enabled?C_DIM:b.hov?C_WHITE:sf::Color{160,170,190};
                draw_box(win,b.b.left,b.b.top,b.b.width,b.b.height,fill,bord,b.hov&&b.enabled?2.f:1.5f);
                if(b.enabled){sf::RectangleShape bar({3.f,b.b.height-8.f});
                    bar.setPosition(b.b.left+4.f,b.b.top+4.f);
                    bar.setFillColor(b.hov?b.acc:sf::Color{b.acc.r,b.acc.g,b.acc.b,80}); win.draw(bar);}
                auto lt=mkt(font,b.lbl,15,tcol,b.hov&&b.enabled); lt.setPosition(b.b.left+14.f,b.b.top+7.f); win.draw(lt);
                if(!b.sub.empty()){auto st=mkt(font,b.sub,11,b.hov?C_ACCENT:C_DIM); st.setPosition(b.b.left+14.f,b.b.top+b.b.height-18.f); win.draw(st);}
            }
        } else if(state==St::WEAPON){
            for(auto& wb:wbtns){
                // Grey out weapon that was just swapped in (unusable this turn)
                bool locked = (me.swap_in_slot >= 0 && me.swap_in_slot == wb.slot);
                sf::Color fill = locked ? sf::Color{20,15,15}
                                : wb.hov ? sf::Color{15,25,55} : C_PANEL;
                sf::Color bord = locked ? sf::Color{80,40,40}
                                : wb.hov ? C_BORDER_HI : C_BORDER;
                draw_box(win,wb.b.left,wb.b.top,wb.b.width,wb.b.height,fill,bord,wb.hov?2.f:1.5f);
                if(!locked && wb.hov){sf::RectangleShape bar({3.f,wb.b.height-8.f});
                    bar.setPosition(wb.b.left+4.f,wb.b.top+4.f);
                    bar.setFillColor(C_ACCENT); win.draw(bar);}
                std::ostringstream ws;
                ws<<weapon_def(wb.wid).name<<"  dmg "<<weapon_def(wb.wid).damage
                  <<"  ["<<weapon_def(wb.wid).slots<<" slots]";
                if(locked) ws<<" (next turn only)";
                sf::Color txtCol = locked ? sf::Color{100,70,70}
                                 : wb.hov ? C_WHITE : sf::Color{160,170,190};
                auto lt=mkt(font,ws.str(),15,txtCol,wb.hov&&!locked);
                lt.setPosition(wb.b.left+14.f,wb.b.top+7.f); win.draw(lt);
                std::ostringstream ss2; ss2<<"slot "<<wb.slot;
                auto st=mkt(font,ss2.str(),11,locked?sf::Color{80,50,50}:wb.hov?C_ACCENT:C_DIM);
                st.setPosition(wb.b.left+14.f,wb.b.top+wb.b.height-18.f); win.draw(st);
            }
            auto bk=mkt(font,"ESC to go back",11,C_DIM); bk.setPosition(34.f,by+10.f); win.draw(bk);
        } else if(state==St::LTS){
            for(auto& lb:lbtns){
                sf::Color fill=lb.hov?sf::Color{15,25,55}:C_PANEL;
                sf::Color bord=lb.hov?C_BORDER_HI:C_BORDER;
                draw_box(win,lb.b.left,lb.b.top,lb.b.width,lb.b.height,fill,bord,lb.hov?2.f:1.5f);
                std::ostringstream ls;
                ls<<weapon_def(lb.wid).name<<"  dmg "<<weapon_def(lb.wid).damage;
                auto lt=mkt(font,ls.str(),15,lb.hov?C_WHITE:sf::Color{160,170,190},lb.hov);
                lt.setPosition(lb.b.left+14.f,lb.b.top+7.f); win.draw(lt);
            }
            auto bk=mkt(font,"ESC to go back",11,C_DIM); bk.setPosition(34.f,by+10.f); win.draw(bk);
        } else {
            auto wt=mkt(font,"Click an enemy on the right",13,C_DIM); wt.setPosition(34.f,240.f); win.draw(wt);
            auto bk=mkt(font,"ESC to go back",11,C_DIM); bk.setPosition(34.f,264.f); win.draw(bk);
        }

        /* right panel: enemies */
        draw_box(win,390.f,214.f,UI_W-410.f,UI_H-224.f,C_PANEL,C_BORDER);
        draw_corners(win,390.f,214.f,UI_W-410.f,UI_H-224.f);
        {const char* esec=state==St::ENEMY?"CLICK TO SELECT TARGET":"ENEMIES";
         sf::Color ec=state==St::ENEMY?C_RED:C_DIM;
         auto el=mkt(font,esec,11,ec,state==St::ENEMY); el.setPosition(404.f,220.f); win.draw(el);}
        {float ey2=238.f; int ebi=0;
         for(int i=np;i<nte;i++){
             const Entity& en=ents[i]; float eh=EB_H;
             bool hov=state==St::ENEMY&&ebi<(int)ebtns.size()&&ebtns[ebi].b.contains(ms);
             if(state==St::ENEMY&&ebi<(int)ebtns.size()) ebtns[ebi].b={EB_X,ey2,EB_W,eh};
             sf::Color fill=hov?C_SELECT:C_PANEL, bord=hov?C_RED:C_BORDER;
             draw_box(win,EB_X,ey2,EB_W,eh,fill,bord,hov?2.f:1.5f);
             std::string ename=en.name; if(!en.alive)ename+=" [DEAD]"; else if(en.stunned)ename+=" [STUN]";
             auto nt=mkt(font,ename,13,en.alive?C_WHITE:sf::Color{80,50,50}); nt.setPosition(EB_X+8.f,ey2+4.f); win.draw(nt);
             if(en.alive){
                 draw_bar(win,EB_X+8.f,ey2+22.f,110.f,6.f,(float)en.hp,(float)en.max_hp,C_HP_E);
                 std::ostringstream hs; hs<<en.hp<<"/"<<en.max_hp;
                 auto hv2=mkt(font,hs.str(),10,C_DIM); hv2.setPosition(EB_X+124.f,ey2+18.f); win.draw(hv2);
                 draw_bar(win,EB_X+200.f,ey2+22.f,80.f,6.f,en.stamina,(float)en.max_stamina,C_STAMINA);
                 std::ostringstream ss3; ss3<<"ST "<<(int)en.stamina;
                 auto sv3=mkt(font,ss3.str(),10,C_DIM); sv3.setPosition(EB_X+286.f,ey2+18.f); win.draw(sv3);}
             ey2+=eh+EB_G; if(en.alive) ebi++;
         }}

        if(!status.empty()){
            float pulse=0.5f+0.5f*std::sin(t*4.f);
            sf::Uint8 al=static_cast<sf::Uint8>(180+int(pulse*75));
            auto sm=mkt(font,status,14,{60,200,255,al},true);
            draw_centered(win,sm,UI_W/2.f,UI_H-20.f);}

        draw_scanlines(win);
        win.display();
    }

    result={ActionType::SKIP,-1,-1,WPN_NONE};
    return result;
}

/* ════════════════════════════════════════════════════════════════════
 * Player thread
 * Posts a UI request to main thread, waits for result.
 * ════════════════════════════════════════════════════════════════════ */
struct PlayerArg { int pidx; };

static void* player_thread(void* arg_)
{
    PlayerArg* arg=static_cast<PlayerArg*>(arg_);
    int pidx=arg->pidx; delete arg;

    gs->entities[pidx].pid=getpid();

    while(!g_quit){
        sem_wait(&player_sem[pidx]);
        if(g_quit) break;

        if(gs->current_turn!=pidx||!gs->turn_ready||!gs->entities[pidx].alive) continue;

        /* Stunned: auto-skip */
        if(gs->entities[pidx].stunned||g_stunned){
            Action act{ActionType::SKIP,-1,-1,WPN_NONE};
            sem_wait(&gs->state_mutex);
            gs->entities[pidx].pending_action=act;
            gs->entities[pidx].action_ready=true;
            sem_post(&gs->state_mutex);
            sem_post(&gs->action_sem);
            continue;
        }

        /* Request UI from main thread */
        pthread_mutex_lock(&g_ui_mutex);
        g_ui_req={pidx,true};
        pthread_mutex_unlock(&g_ui_mutex);
        sem_post(&g_ui_request_sem);     /* wake main thread */
        sem_wait(&g_ui_done_sem);        /* wait for result  */

        Action act=g_ui_result;

        sem_wait(&gs->state_mutex);
        gs->entities[pidx].pending_action=act;
        gs->entities[pidx].action_ready=true;
        sem_post(&gs->state_mutex);
        sem_post(&gs->action_sem);

        while(!g_quit&&!gs->entities[pidx].action_done) usleep(10000);
    }
    return nullptr;
}

/* ════════════════════════════════════════════════════════════════════
 * Dispatcher thread
 * ════════════════════════════════════════════════════════════════════ */
static void* dispatcher(void*)
{
    while(!g_quit){
        sem_wait(&gs->hip_turn_sem);
        if(g_quit) break;
        int ct=gs->current_turn;
        if(ct>=0&&ct<gs->num_players&&gs->entities[ct].alive)
            sem_post(&player_sem[ct]);
    }
    return nullptr;
}

/* ════════════════════════════════════════════════════════════════════
 * main  —  owns the SFML event loop
 * ════════════════════════════════════════════════════════════════════ */
int main(int argc, char* argv[])
{
    if(argc<3){fprintf(stderr,"Usage: hip <roll_no> <num_players>\n");return 1;}
    g_num_players=atoi(argv[2]);

    gs=shm_open_existing();

    struct sigaction sa{};
    sa.sa_handler=sig_term; sigaction(SIGTERM,&sa,nullptr);
    sa.sa_handler=sig_stun; sigaction(SIGUSR1,&sa,nullptr);

    /* Load font on main thread */
    sf::Font font;
    for(int i=0;FONT_PATHS[i];++i) if(font.loadFromFile(FONT_PATHS[i])) break;

    /* Init semaphores */
    sem_init(&g_ui_request_sem,0,0);
    sem_init(&g_ui_done_sem,   0,0);
    for(int i=0;i<g_num_players;i++) sem_init(&player_sem[i],0,0);

    /* Spawn dispatcher + player threads */
    pthread_t disp_tid;
    pthread_create(&disp_tid,nullptr,dispatcher,nullptr);

    pthread_t ptids[MAX_PLAYERS];
    for(int i=0;i<g_num_players;i++){
        auto* a=new PlayerArg{i};
        pthread_create(&ptids[i],nullptr,player_thread,a);
    }

    /* ── Main thread: serve UI requests until game over ── */
    while(!g_quit&&gs->phase!=GamePhase::GAME_OVER){

        /* Check for pending weapon drop (arbiter waiting for player response) */
        if(gs->pending_drop_ready && !gs->pending_drop_done) {
            int pidx = gs->pending_drop_for;
            WeaponID wpn = gs->pending_drop_wpn;
            bool taken = false;
            if(pidx >= 0 && pidx < g_num_players)
                taken = show_drop_window(wpn, pidx, font);
            sem_wait(&gs->state_mutex);
            gs->pending_drop_taken = taken;
            gs->pending_drop_done  = true;
            sem_post(&gs->state_mutex);
        }

        /* Non-blocking check for UI request (timeout 100ms) */
        struct timespec ts{}; clock_gettime(CLOCK_REALTIME,&ts);
        ts.tv_nsec+=100000000LL; if(ts.tv_nsec>=1000000000LL){ts.tv_sec++;ts.tv_nsec-=1000000000LL;}

        if(sem_timedwait(&g_ui_request_sem,&ts)==0){
            /* Got a request — read which player */
            pthread_mutex_lock(&g_ui_mutex);
            int pidx=g_ui_req.pidx;
            g_ui_req.pending=false;
            pthread_mutex_unlock(&g_ui_mutex);

            if(pidx>=0&&pidx<g_num_players&&!g_quit){
                /* Open window on main thread, collect action */
                g_ui_result=show_action_window(pidx,font);
            } else {
                g_ui_result={ActionType::SKIP,-1,-1,WPN_NONE};
            }
            sem_post(&g_ui_done_sem);   /* wake player thread with result */
        }
    }

    /* Shutdown */
    g_quit=1;
    /* Unblock anything waiting */
    for(int i=0;i<g_num_players;i++) sem_post(&player_sem[i]);
    sem_post(&gs->hip_turn_sem);
    sem_post(&g_ui_request_sem);
    sem_post(&g_ui_done_sem);

    pthread_join(disp_tid,nullptr);
    for(int i=0;i<g_num_players;i++) pthread_join(ptids[i],nullptr);

    for(int i=0;i<g_num_players;i++) sem_destroy(&player_sem[i]);
    sem_destroy(&g_ui_request_sem);
    sem_destroy(&g_ui_done_sem);

    munmap(gs,sizeof(SharedState));
    return 0;
}
