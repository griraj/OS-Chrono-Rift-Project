/*
 * main_launcher.cpp — SFML graphical launcher (SFML 2.x compatible)
 * Cyberpunk / tactical-RPG aesthetic.
 * Replaces the broken scanf loop with a full GUI.
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sstream>
#include <cmath>
#include <SFML/Graphics.hpp>
#include <SFML/System.hpp>
#include <SFML/Window.hpp>

extern int arbiter_main(int, char*[]);

/* ── Palette ── */
static const sf::Color C_BG       {  8,  10,  20 };
static const sf::Color C_PANEL    { 16,  18,  35 };
static const sf::Color C_BORDER   { 40,  80, 160 };
static const sf::Color C_BORDER_HI{ 80, 160, 255 };
static const sf::Color C_GOLD     {220, 185,  80 };
static const sf::Color C_GOLD_DIM {120, 100,  40 };
static const sf::Color C_WHITE    {230, 235, 245 };
static const sf::Color C_DIM      {100, 110, 130 };
static const sf::Color C_ACCENT   { 60, 200, 255 };
static const sf::Color C_GREEN    { 60, 220, 120 };
static const sf::Color C_RED      {220,  70,  70 };
static const sf::Color C_SCAN     {  0,  20,  60,  18 };

static const unsigned WIN_W = 860;
static const unsigned WIN_H = 620;

/* ── Helper: make a configured sf::Text ── */
static sf::Text mkt(const sf::Font& f, const std::string& s,
                    unsigned sz, sf::Color col, bool bold = false)
{
    sf::Text t;
    t.setFont(f);
    t.setString(s);
    t.setCharacterSize(sz);
    t.setFillColor(col);
    if (bold) t.setStyle(sf::Text::Bold);
    return t;
}

/* ── Helper: draw centered text at (cx, y) ── */
static void draw_centered(sf::RenderTarget& rt, sf::Text t, float cx, float y)
{
    sf::FloatRect b = t.getLocalBounds();
    t.setOrigin(b.left + b.width / 2.f, b.top);
    t.setPosition(cx, y);
    rt.draw(t);
}

/* ── Helper: filled+outlined rectangle ── */
static void draw_box(sf::RenderTarget& rt,
                     float x, float y, float w, float h,
                     sf::Color fill, sf::Color border, float thick = 1.5f)
{
    sf::RectangleShape r({w, h});
    r.setPosition(x, y);
    r.setFillColor(fill);
    r.setOutlineColor(border);
    r.setOutlineThickness(thick);
    rt.draw(r);
}

/* ── Helper: gold corner brackets ── */
static void draw_corners(sf::RenderTarget& rt,
                          float x, float y, float w, float h, float sz = 12.f)
{
    sf::RectangleShape r;
    r.setFillColor(C_GOLD);
    auto seg = [&](float px, float py, float pw, float ph){
        r.setSize({pw, ph}); r.setPosition(px, py); rt.draw(r);
    };
    seg(x,       y,       sz,   2.f);  seg(x,       y,       2.f,  sz);
    seg(x+w-sz,  y,       sz,   2.f);  seg(x+w-2.f, y,       2.f,  sz);
    seg(x,       y+h-2.f, sz,   2.f);  seg(x,       y+h-sz,  2.f,  sz);
    seg(x+w-sz,  y+h-2.f, sz,   2.f);  seg(x+w-2.f, y+h-sz,  2.f,  sz);
}

/* ── Helper: scanlines ── */
static void draw_scanlines(sf::RenderTarget& rt)
{
    sf::RectangleShape line({static_cast<float>(WIN_W), 1.f});
    line.setFillColor(C_SCAN);
    for (unsigned y = 0; y < WIN_H; y += 4) {
        line.setPosition(0.f, static_cast<float>(y));
        rt.draw(line);
    }
}

/* ══════════════════════════════════════════════════════════════════
 * run_launcher
 * ══════════════════════════════════════════════════════════════════ */
static std::pair<unsigned,int> run_launcher()
{
    sf::RenderWindow window(sf::VideoMode(WIN_W, WIN_H),
                            "Chrono Rift - Launch",
                            sf::Style::Titlebar | sf::Style::Close);
    window.setFramerateLimit(60);

    sf::Font font;
    const char* fpaths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf", nullptr
    };
    for (int i = 0; fpaths[i]; ++i)
        if (font.loadFromFile(fpaths[i])) break;

    std::string roll_str;
    int         party = 0;
    std::string error_msg;
    sf::Clock   clk;

    /* ── Numpad: digit layout [7,8,9],[4,5,6],[1,2,3],[back,0,clr] ── */
    const float NP_X=54.f, NP_Y=298.f, NP_W=62.f, NP_H=46.f, NP_G=8.f;
    struct NPBtn { sf::FloatRect b; std::string lbl; int digit; };
    NPBtn np[12];
    const int layout[4][3] = {{7,8,9},{4,5,6},{1,2,3},{10,0,11}};
    for (int r=0; r<4; r++) for (int c=0; c<3; c++) {
        int d = layout[r][c];
        int idx = (d>=0&&d<=9) ? d : (d==10) ? 10 : 11;
        np[idx].b = { NP_X+c*(NP_W+NP_G), NP_Y+r*(NP_H+NP_G), NP_W, NP_H };
        np[idx].digit = d;
        np[idx].lbl = (d>=0&&d<=9) ? std::to_string(d) : (d==10) ? "<--" : "CLR";
    }

    /* ── Party cards ── */
    const float PC_Y=148.f, PC_H=56.f, PC_W=70.f, PC_SX=474.f, PC_G=10.f;
    sf::FloatRect pc_b[4];
    for (int i=0; i<4; i++) pc_b[i] = {PC_SX+i*(PC_W+PC_G), PC_Y, PC_W, PC_H};
    const char* pc_lbl[4] = {"1","2","3","4"};
    const char* pc_sub[4] = {"SOLO","DUO","TRIO","SQUAD"};

    /* ── Launch button ── */
    sf::FloatRect launch_b = {WIN_W/2.f-120.f, WIN_H-80.f, 240.f, 46.f};

    while (window.isOpen()) {
        sf::Vector2f ms = window.mapPixelToCoords(sf::Mouse::getPosition(window));
        float t = clk.getElapsedTime().asSeconds();

        sf::Event ev{};
        while (window.pollEvent(ev)) {
            if (ev.type == sf::Event::Closed) return {0,0};

            if (ev.type == sf::Event::KeyPressed) {
                auto k = ev.key.code;
                if (k>=sf::Keyboard::Num0 && k<=sf::Keyboard::Num9 && roll_str.size()<12)
                    { roll_str += static_cast<char>('0'+(k-sf::Keyboard::Num0)); error_msg.clear(); }
                if (k>=sf::Keyboard::Numpad0 && k<=sf::Keyboard::Numpad9 && roll_str.size()<12)
                    { roll_str += static_cast<char>('0'+(k-sf::Keyboard::Numpad0)); error_msg.clear(); }
                if (k==sf::Keyboard::BackSpace && !roll_str.empty()) roll_str.pop_back();
                if (k>=sf::Keyboard::Num1 && k<=sf::Keyboard::Num4)
                    party = k - sf::Keyboard::Num0;
                if (k==sf::Keyboard::Return) goto try_launch;
            }

            if (ev.type==sf::Event::MouseButtonPressed &&
                ev.mouseButton.button==sf::Mouse::Left) {

                for (int i=0; i<12; i++) {
                    if (!np[i].b.contains(ms)) continue;
                    int d = np[i].digit;
                    if (d>=0&&d<=9&&roll_str.size()<12)
                        { roll_str+=static_cast<char>('0'+d); error_msg.clear(); }
                    else if (d==10&&!roll_str.empty()) roll_str.pop_back();
                    else if (d==11) roll_str.clear();
                }
                for (int i=0; i<4; i++)
                    if (pc_b[i].contains(ms)) party = i+1;

                if (launch_b.contains(ms)) {
                    try_launch:
                    unsigned rn = 0;
                    bool ok = !roll_str.empty();
                    if (ok) for (char c : roll_str) if (!isdigit((unsigned char)c)) { ok=false; break; }
                    if (ok) { rn = static_cast<unsigned>(std::stoul(roll_str)); if (rn==0) ok=false; }
                    if (!ok)       error_msg = "Enter a valid non-zero roll number!";
                    else if(party<1) error_msg = "Select a party size (1-4)!";
                    else { window.close(); return {rn, party}; }
                }
            }
        }

        /* ══ DRAW ══ */
        window.clear(C_BG);

        /* background grid */
        {
            sf::RectangleShape gl({static_cast<float>(WIN_W), 1.f});
            gl.setFillColor({20,30,60,35});
            for (unsigned gy=0; gy<WIN_H; gy+=32)
                { gl.setPosition(0.f,static_cast<float>(gy)); window.draw(gl); }
            gl.setSize({1.f, static_cast<float>(WIN_H)});
            for (unsigned gx=0; gx<WIN_W; gx+=32)
                { gl.setPosition(static_cast<float>(gx),0.f); window.draw(gl); }
        }

        /* ── Title ── */
        {
            float pulse = 0.5f + 0.5f*std::sin(t*2.f);
            sf::RectangleShape glow({500.f,70.f});
            glow.setOrigin(250.f,35.f); glow.setPosition(WIN_W/2.f,55.f);
            glow.setFillColor({20,60,140, static_cast<sf::Uint8>(25+int(pulse*25))});
            window.draw(glow);

            draw_centered(window, mkt(font,"CHRONO  RIFT",44,C_GOLD,true), WIN_W/2.f, 22.f);
            draw_centered(window, mkt(font,"CS 2006  -  Operating Systems  -  Spring 2026",13,C_DIM),
                          WIN_W/2.f, 76.f);

            sf::RectangleShape rule({static_cast<float>(WIN_W)-80.f,1.f});
            rule.setPosition(40.f,103.f); rule.setFillColor(C_BORDER); window.draw(rule);
        }

        /* ── Roll Number panel ── */
        {
            draw_box(window, 40.f,115.f,400.f,160.f, C_PANEL,C_BORDER);
            draw_corners(window, 40.f,115.f,400.f,160.f);

            auto lbl = mkt(font,"ROLL NUMBER  (RNG SEED)",12,C_DIM);
            lbl.setPosition(54.f,123.f); window.draw(lbl);

            draw_box(window, 54.f,143.f,372.f,44.f, {10,12,28},C_ACCENT,1.5f);
            std::string disp = roll_str.empty() ? "_" : roll_str+"_";
            auto dt = mkt(font, disp, 26, roll_str.empty()?C_DIM:C_WHITE);
            dt.setPosition(62.f,147.f); window.draw(dt);

            auto hint = mkt(font,"Type digits on keyboard, or use the numpad below",11,C_DIM);
            hint.setPosition(54.f,195.f); window.draw(hint);
        }

        /* ── Party Size panel ── */
        {
            draw_box(window, 460.f,115.f,360.f,110.f, C_PANEL,C_BORDER);
            draw_corners(window, 460.f,115.f,360.f,110.f);

            auto lbl2 = mkt(font,"PARTY SIZE  (press 1-4 on keyboard)",12,C_DIM);
            lbl2.setPosition(474.f,123.f); window.draw(lbl2);

            for (int i=0; i<4; i++) {
                bool sel=(party==i+1), hov=pc_b[i].contains(ms);
                sf::Color fill = sel?sf::Color{10,40,90}:hov?sf::Color{15,25,55}:C_PANEL;
                sf::Color bord = sel?C_ACCENT:hov?C_BORDER_HI:C_BORDER;
                draw_box(window,pc_b[i].left,pc_b[i].top,pc_b[i].width,pc_b[i].height,
                         fill,bord,sel||hov?2.f:1.5f);

                auto nt = mkt(font,pc_lbl[i],28,sel?C_ACCENT:hov?C_WHITE:C_DIM,sel);
                sf::FloatRect nb=nt.getLocalBounds();
                nt.setOrigin(nb.left+nb.width/2.f,nb.top);
                nt.setPosition(pc_b[i].left+pc_b[i].width/2.f, pc_b[i].top+4.f);
                window.draw(nt);

                auto st = mkt(font,pc_sub[i],10,sel?C_ACCENT:C_DIM);
                sf::FloatRect sb=st.getLocalBounds();
                st.setOrigin(sb.left+sb.width/2.f,sb.top);
                st.setPosition(pc_b[i].left+pc_b[i].width/2.f,
                               pc_b[i].top+pc_b[i].height-16.f);
                window.draw(st);
            }
        }

        /* ── Numpad panel ── */
        {
            draw_box(window, 40.f,283.f,400.f,258.f, C_PANEL,C_BORDER);
            draw_corners(window, 40.f,283.f,400.f,258.f);

            auto lbl3 = mkt(font,"NUMBER PAD",12,C_DIM);
            lbl3.setPosition(54.f,290.f); window.draw(lbl3);

            for (int i=0; i<12; i++) {
                bool hov = np[i].b.contains(ms);
                draw_box(window,np[i].b.left,np[i].b.top,np[i].b.width,np[i].b.height,
                         hov?sf::Color{18,30,65}:C_PANEL,
                         hov?C_BORDER_HI:C_BORDER, hov?2.f:1.5f);
                auto nt = mkt(font,np[i].lbl,17,hov?C_WHITE:C_DIM);
                sf::FloatRect nb=nt.getLocalBounds();
                nt.setOrigin(nb.left+nb.width/2.f,nb.top+nb.height/2.f);
                nt.setPosition(np[i].b.left+np[i].b.width/2.f,
                               np[i].b.top+np[i].b.height/2.f);
                window.draw(nt);
            }
        }

        /* ── Info panel ── */
        {
            draw_box(window, 460.f,234.f,360.f,307.f, C_PANEL,C_BORDER);
            draw_corners(window, 460.f,234.f,360.f,307.f);

            auto lbl4 = mkt(font,"HOW TO PLAY",12,C_DIM);
            lbl4.setPosition(474.f,242.f); window.draw(lbl4);

            struct IL{const char* ic;const char* tx;sf::Color col;};
            IL lines[]={
                {">","Turn-based multi-process combat",C_WHITE},
                {">","Stamina fills at entity speed",C_WHITE},
                {">","First to full stamina acts first",C_WHITE},
                {"!","Stun: 3s async SIGUSR1 freeze",C_ACCENT},
                {"!","Ultimate: 10s NPC SIGSTOP pause",C_GOLD},
                {"!","Deadlock auto-resolved by arbiter",C_GREEN},
                {"#","Kill 10 enemies to WIN",C_GREEN},
                {"#","All players dead = LOSE",C_RED},
                {"#","Option 0 in terminal = QUIT",C_DIM},
            };
            float iy=264.f;
            for (auto& il:lines){
                auto ic=mkt(font,il.ic,12,il.col); ic.setPosition(474.f,iy); window.draw(ic);
                auto it=mkt(font,il.tx,12,il.col); it.setPosition(492.f,iy); window.draw(it);
                iy+=22.f;
            }
        }

        /* ── Summary / error ── */
        if (!error_msg.empty()){
            float al=200.f+55.f*std::sin(t*6.f);
            auto et=mkt(font,error_msg,14,{220,70,70,static_cast<sf::Uint8>(al)});
            draw_centered(window,et,WIN_W/2.f,WIN_H-108.f);
        } else if (!roll_str.empty()||party>0){
            std::ostringstream ss;
            ss<<"Seed: "<<(roll_str.empty()?"?":roll_str)
              <<"   Party: "<<(party>0?std::to_string(party):"?");
            draw_centered(window,mkt(font,ss.str(),13,C_GOLD_DIM),WIN_W/2.f,WIN_H-108.f);
        }

        /* ── Launch button ── */
        {
            bool ready = !roll_str.empty() && party>=1;
            bool hov   = launch_b.contains(ms);
            float pulse = 0.5f+0.5f*std::sin(t*3.f);

            sf::Color fill = hov&&ready?sf::Color{15,50,120}
                           : ready     ?sf::Color{10,35, 90}
                                       :sf::Color{15,18, 35};
            sf::Color bord;
            if (hov&&ready) bord=C_ACCENT;
            else if (ready) bord={
                static_cast<sf::Uint8>(40 +int(pulse*40)),
                static_cast<sf::Uint8>(120+int(pulse*80)),
                static_cast<sf::Uint8>(200+int(pulse*55))};
            else bord={30,35,60};

            draw_box(window,launch_b.left,launch_b.top,
                     launch_b.width,launch_b.height, fill,bord,2.f);

            auto lt=mkt(font,"LAUNCH  >",20,ready?(hov?C_WHITE:C_ACCENT):C_DIM,ready);
            sf::FloatRect lb=lt.getLocalBounds();
            lt.setOrigin(lb.left+lb.width/2.f,lb.top+lb.height/2.f);
            lt.setPosition(launch_b.left+launch_b.width/2.f,
                           launch_b.top+launch_b.height/2.f);
            window.draw(lt);

            if (!ready){
                auto h=mkt(font,"(enter roll number and select party size first)",11,C_DIM);
                draw_centered(window,h,WIN_W/2.f,WIN_H-28.f);
            }
        }

        draw_scanlines(window);
        window.display();
    }
    return {0,0};
}

/* ════════════════════════════════════════════════════════════════ */
int main()
{
    auto [roll_no, num_players] = run_launcher();
    if (roll_no==0 || num_players==0) { printf("Launch cancelled.\n"); return 0; }

    printf("\nLaunching Chrono Rift [seed=%u, players=%d]...\n\n", roll_no, num_players);

    char roll_str[16], np_str[4];
    snprintf(roll_str,sizeof(roll_str),"%u",roll_no);
    snprintf(np_str,  sizeof(np_str),  "%d",num_players);

    char* args[] = {(char*)"arbiter_bin", roll_str, np_str, nullptr};
    return arbiter_main(3, args);
}
