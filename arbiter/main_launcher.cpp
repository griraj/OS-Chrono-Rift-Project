/*
 * main_launcher.cpp — SFML graphical launcher (SFML 2.x compatible)
 * Splash screen unchanged (4-5s animated title).
 * Launcher: clean dark green-shaded single rectangle UI.
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

/* ── Palette (shared) ── */
static const sf::Color C_BG       {  8,  10,  20 };
static const sf::Color C_GOLD     {220, 185,  80 };
static const sf::Color C_WHITE    {230, 235, 245 };
static const sf::Color C_DIM      {100, 110, 130 };
static const sf::Color C_ACCENT   { 60, 200, 255 };
static const sf::Color C_SCAN     {  0,  20,  60,  18 };

static const unsigned WIN_W = 860;
static const unsigned WIN_H = 620;

/* ── Splash palette ── */
static const sf::Color C_BORDER   { 40,  80, 160 };
static const sf::Color C_PANEL    { 16,  18,  35 };

/* ── Launcher palette ──────────────────────────────────────────────
 * Background : deep midnight indigo with animated purple hex-grid
 * Window border / corners : warm copper-amber + magenta corner sparks
 * Login card  : dark forest green (kept green as requested)
 * ──────────────────────────────────────────────────────────────── */
// Background
static const sf::Color G_DARK      {  6,   8,  22 };   // deep midnight indigo
// Window outer border — copper-amber
static const sf::Color WIN_BDR     {190, 110,  30 };
static const sf::Color WIN_BDR_HI  {255, 175,  60 };
static const sf::Color WIN_CORNER  {255,  80, 160 };   // magenta-pink corner sparks
// Login card — forest green
static const sf::Color G_PANEL     {  8,  32,  16 };
static const sf::Color G_BORDER    { 28, 100,  50 };
static const sf::Color G_BORDER_HI { 55, 190,  95 };
static const sf::Color G_TITLE     { 70, 210, 110 };
static const sf::Color G_DIM       { 40,  85,  55 };
static const sf::Color G_FIELD_BG  {  5,  18,  10 };
static const sf::Color G_FIELD_BD  { 38, 150,  75 };
static const sf::Color G_SEL       {  9,  50,  22 };
static const sf::Color G_LAUNCH    { 12,  62,  30 };
static const sf::Color G_LAUNCH_HI { 18,  82,  40 };
static const sf::Color G_LAUNCH_BD { 48, 170,  85 };
static const sf::Color G_RED       {220,  70,  70 };
static const sf::Color G_GOLD_HI   {220, 185,  80 };

/* ── Letter colours for animated splash title ── */
static const sf::Color LETTER_COLORS[] = {
    {255, 80,  80},  {255,140,  0},  {255,220,  0},
    { 80,220, 80},  { 60,200,255},  {120,100,255},
    {220, 60,220},  {255, 80,160},  {255,140,  0},
    {255,220,  0},  { 80,220, 80},
};

/* ════════════════════════════════════
 * Shared helpers
 * ════════════════════════════════════ */
static sf::Text mkt(const sf::Font& f, const std::string& s,
                    unsigned sz, sf::Color col, bool bold=false)
{
    sf::Text t; t.setFont(f); t.setString(s);
    t.setCharacterSize(sz); t.setFillColor(col);
    if (bold) t.setStyle(sf::Text::Bold);
    return t;
}

static void draw_centered(sf::RenderTarget& rt, sf::Text t, float cx, float y)
{
    sf::FloatRect b = t.getLocalBounds();
    t.setOrigin(b.left+b.width/2.f, b.top);
    t.setPosition(cx, y);
    rt.draw(t);
}

static void draw_box(sf::RenderTarget& rt,
                     float x, float y, float w, float h,
                     sf::Color fill, sf::Color border, float thick=1.5f)
{
    sf::RectangleShape r({w,h}); r.setPosition(x,y);
    r.setFillColor(fill); r.setOutlineColor(border);
    r.setOutlineThickness(thick); rt.draw(r);
}

static void draw_scanlines(sf::RenderTarget& rt)
{
    sf::RectangleShape line({static_cast<float>(WIN_W),1.f});
    line.setFillColor(C_SCAN);
    for (unsigned y=0; y<WIN_H; y+=4){ line.setPosition(0.f,(float)y); rt.draw(line); }
}

/* Gold corner brackets */
static void draw_corners(sf::RenderTarget& rt,
                          float x, float y, float w, float h,
                          sf::Color col, float sz=12.f)
{
    sf::RectangleShape r; r.setFillColor(col);
    auto seg=[&](float px,float py,float pw,float ph){
        r.setSize({pw,ph}); r.setPosition(px,py); rt.draw(r); };
    seg(x,    y,    sz, 2.f); seg(x,    y,    2.f, sz);
    seg(x+w-sz,y,   sz, 2.f); seg(x+w-2.f,y, 2.f, sz);
    seg(x,    y+h-2.f,sz,2.f); seg(x,   y+h-sz,2.f,sz);
    seg(x+w-sz,y+h-2.f,sz,2.f); seg(x+w-2.f,y+h-sz,2.f,sz);
}

/* Animated multi-colour title (used in both splash and launcher header) */
static void draw_colored_title(sf::RenderTarget& rt, const sf::Font& font,
                                float cx, float y, unsigned char_size,
                                sf::Uint8 alpha, float time_val)
{
    const std::string TITLE = "CHRONO RIFT";
    const int N = (int)TITLE.size();
    float total_w = 0.f;
    for (int i=0; i<N; ++i){
        sf::Text t; t.setFont(font); t.setString(std::string(1,TITLE[i]));
        t.setCharacterSize(char_size); t.setStyle(sf::Text::Bold);
        total_w += t.getLocalBounds().width + 2.f;
    }
    float x = cx - total_w/2.f;
    int ci = 0;
    for (int i=0; i<N; ++i){
        char ch = TITLE[i];
        if (ch==' '){
            sf::Text sp; sp.setFont(font); sp.setString(" ");
            sp.setCharacterSize(char_size); sp.setStyle(sf::Text::Bold);
            x += sp.getLocalBounds().width+2.f; continue;
        }
        float bob = std::sin(time_val*2.5f + ci*0.55f)*6.f;
        sf::Color col = LETTER_COLORS[ci%(sizeof(LETTER_COLORS)/sizeof(LETTER_COLORS[0]))];
        col.a = alpha;
        sf::Text glow; glow.setFont(font); glow.setString(std::string(1,ch));
        glow.setCharacterSize(char_size+4); glow.setStyle(sf::Text::Bold);
        glow.setFillColor({col.r,col.g,col.b,(sf::Uint8)(alpha/5)});
        glow.setPosition(x-2.f, y+bob-2.f); rt.draw(glow);
        sf::Text letter; letter.setFont(font); letter.setString(std::string(1,ch));
        letter.setCharacterSize(char_size); letter.setStyle(sf::Text::Bold);
        letter.setFillColor(col); letter.setPosition(x, y+bob); rt.draw(letter);
        x += letter.getLocalBounds().width+2.f; ++ci;
    }
}

/* ════════════════════════════════════
 * SPLASH SCREEN  (unchanged)
 * ════════════════════════════════════ */
static bool run_splash(const sf::Font& font)
{
    sf::RenderWindow window(sf::VideoMode(WIN_W,WIN_H),"Chrono Rift",
                            sf::Style::Titlebar|sf::Style::Close);
    window.setFramerateLimit(60);

    sf::Texture bg_tex; bool has_bg = bg_tex.loadFromFile("splash.png");
    if (!has_bg) has_bg = bg_tex.loadFromFile("assets/splash.png");
    sf::Sprite bg_sprite;
    if (has_bg){
        bg_tex.setSmooth(true); bg_sprite.setTexture(bg_tex);
        float sx=(float)WIN_W/bg_tex.getSize().x, sy=(float)WIN_H/bg_tex.getSize().y;
        bg_sprite.setScale(sx,sy);
    }

    const float SPLASH_DURATION = 4.5f;
    sf::Clock clk;

    while (window.isOpen()){
        sf::Event ev{};
        while (window.pollEvent(ev)){
            if (ev.type==sf::Event::Closed) return false;
            if (ev.type==sf::Event::KeyPressed||ev.type==sf::Event::MouseButtonPressed)
                goto splash_done;
        }
        float t = clk.getElapsedTime().asSeconds();
        if (t>=SPLASH_DURATION) break;

        float alpha_f=1.f;
        if (t<0.8f) alpha_f=t/0.8f;
        else if (t>SPLASH_DURATION-0.6f) alpha_f=(SPLASH_DURATION-t)/0.6f;
        alpha_f=std::max(0.f,std::min(1.f,alpha_f));
        sf::Uint8 alpha=(sf::Uint8)(alpha_f*255.f);

        window.clear(C_BG);
        if (has_bg){ bg_sprite.setColor({255,255,255,(sf::Uint8)(alpha_f*200.f)}); window.draw(bg_sprite); }
        sf::RectangleShape vignette({(float)WIN_W,(float)WIN_H});
        vignette.setFillColor({0,0,0,(sf::Uint8)(alpha_f*130.f)}); window.draw(vignette);

        { sf::RectangleShape rl1({WIN_W*0.75f,2.f}); rl1.setPosition(WIN_W*0.125f,WIN_H/2.f-72.f);
          rl1.setFillColor({220,185,80,alpha}); window.draw(rl1);
          sf::RectangleShape rl2({WIN_W*0.75f,2.f}); rl2.setPosition(WIN_W*0.125f,WIN_H/2.f+68.f);
          rl2.setFillColor({220,185,80,alpha}); window.draw(rl2); }

        draw_colored_title(window,font,WIN_W/2.f,WIN_H/2.f-32.f,74,alpha,t);

        if (t>1.0f){
            float haf=std::min(1.f,(t-1.0f)/0.5f)*alpha_f;
            float pulse=0.5f+0.5f*std::sin(t*4.f);
            sf::Uint8 ha=(sf::Uint8)(haf*(140+int(pulse*60)));
            sf::Text hint=mkt(font,"Press any key to continue",13,{150,160,180,ha});
            draw_centered(window,hint,WIN_W/2.f,WIN_H-36.f);
        }
        draw_scanlines(window);
        window.display();
    }
splash_done:
    window.close(); return true;
}

/* ════════════════════════════════════
 * LAUNCHER  — green-shaded rectangle
 * ════════════════════════════════════ */
static std::pair<unsigned,int> run_launcher(const sf::Font& font)
{
    sf::RenderWindow window(sf::VideoMode(WIN_W,WIN_H),"Chrono Rift — Launch",
                            sf::Style::Titlebar|sf::Style::Close);
    window.setFramerateLimit(60);

    std::string roll_str;
    int         party    = 0;
    std::string error_msg;
    sf::Clock   clk;

    /* ── Layout constants ── */
    // Central card dimensions
    const float CARD_W  = 520.f;
    const float CARD_H  = 420.f;
    const float CARD_X  = (WIN_W - CARD_W) / 2.f;
    const float CARD_Y  = (WIN_H - CARD_H) / 2.f - 20.f;

    // Roll-number field inside card
    const float FLD_X   = CARD_X + 30.f;
    const float FLD_Y   = CARD_Y + 116.f;
    const float FLD_W   = CARD_W - 60.f;
    const float FLD_H   = 52.f;

    // Party size buttons
    const float PC_Y    = CARD_Y + 220.f;
    const float PC_H    = 56.f;
    const float PC_W    = (CARD_W - 60.f - 3*10.f) / 4.f;
    sf::FloatRect pc_b[4];
    for (int i=0; i<4; i++)
        pc_b[i] = {FLD_X + i*(PC_W+10.f), PC_Y, PC_W, PC_H};
    const char* pc_lbl[4]  = {"1","2","3","4"};
    const char* pc_sub[4]  = {"SOLO","DUO","TRIO","SQUAD"};

    // Launch button
    const float BTN_W   = 240.f;
    const float BTN_H   = 50.f;
    sf::FloatRect launch_b = {CARD_X+(CARD_W-BTN_W)/2.f, CARD_Y+CARD_H-70.f, BTN_W, BTN_H};

    while (window.isOpen()){
        sf::Vector2f ms = window.mapPixelToCoords(sf::Mouse::getPosition(window));
        float t = clk.getElapsedTime().asSeconds();

        sf::Event ev{};
        while (window.pollEvent(ev)){
            if (ev.type==sf::Event::Closed) return {0,0};

            if (ev.type==sf::Event::KeyPressed){
                auto k = ev.key.code;
                if (k>=sf::Keyboard::Num0 && k<=sf::Keyboard::Num9 && roll_str.size()<12)
                    { roll_str+=(char)('0'+(k-sf::Keyboard::Num0)); error_msg.clear(); }
                if (k>=sf::Keyboard::Numpad0 && k<=sf::Keyboard::Numpad9 && roll_str.size()<12)
                    { roll_str+=(char)('0'+(k-sf::Keyboard::Numpad0)); error_msg.clear(); }
                if (k==sf::Keyboard::BackSpace && !roll_str.empty()) roll_str.pop_back();
                if (k>=sf::Keyboard::Num1 && k<=sf::Keyboard::Num4) party=k-sf::Keyboard::Num0;
                if (k==sf::Keyboard::Return) goto try_launch;
            }

            if (ev.type==sf::Event::MouseButtonPressed &&
                ev.mouseButton.button==sf::Mouse::Left){
                // party cards
                for (int i=0; i<4; i++)
                    if (pc_b[i].contains(ms)) party=i+1;
                // launch
                if (launch_b.contains(ms)){
                    try_launch:
                    unsigned rn=0; bool ok=!roll_str.empty();
                    if (ok) for (char c:roll_str) if (!isdigit((unsigned char)c)){ok=false;break;}
                    if (ok){ rn=(unsigned)std::stoul(roll_str); if(rn==0) ok=false; }
                    if (!ok)      error_msg="Enter a valid non-zero roll number!";
                    else if(party<1) error_msg="Select a party size (1-4)!";
                    else { window.close(); return {rn,party}; }
                }
                // click inside field (just focus, nothing needed)
            }
        }

        /* ══ DRAW ══ */
        window.clear(G_DARK);

        /* ── Midnight indigo background with animated diagonal purple grid ── */
        {
            // Horizontal lines
            sf::RectangleShape gl({(float)WIN_W, 1.f});
            gl.setFillColor({30, 20, 70, 28});
            for (unsigned gy=0; gy<WIN_H; gy+=36){
                gl.setPosition(0,(float)gy); window.draw(gl);
            }
            // Vertical lines
            gl.setSize({1.f,(float)WIN_H});
            for (unsigned gx=0; gx<WIN_W; gx+=36){
                gl.setPosition((float)gx,0); window.draw(gl);
            }
            // Diagonal shimmer lines drifting over time
            float drift = fmod(t*18.f, 72.f);
            sf::RectangleShape diag({(float)(WIN_W+WIN_H), 1.f});
            diag.setFillColor({80, 50, 160, 18});
            for (int d=-10; d<22; d++){
                float off = d*72.f + drift;
                diag.setRotation(32.f);
                diag.setPosition(off - WIN_H*0.6f, 0.f);
                window.draw(diag);
            }
        }

        /* ── Outer copper-amber window frame (decorative, inset 8px) ── */
        {
            float pulse = 0.5f + 0.5f*std::sin(t*1.4f);
            sf::Uint8 ba = (sf::Uint8)(180 + int(pulse*55));
            sf::Color bdr = {WIN_BDR.r, WIN_BDR.g, (sf::Uint8)(WIN_BDR.b + int(pulse*20)), ba};

            // Top bar
            sf::RectangleShape bar({(float)WIN_W-16.f, 2.f}); bar.setFillColor(bdr);
            bar.setPosition(8.f, 8.f); window.draw(bar);
            // Bottom bar
            bar.setPosition(8.f, (float)WIN_H-10.f); window.draw(bar);
            // Left bar
            bar.setSize({2.f,(float)WIN_H-16.f}); bar.setPosition(8.f,8.f); window.draw(bar);
            // Right bar
            bar.setPosition((float)WIN_W-10.f, 8.f); window.draw(bar);

            // Magenta-pink corner sparks (L-shaped, 28px arms)
            sf::Color cc = {WIN_CORNER.r, WIN_CORNER.g, WIN_CORNER.b, ba};
            sf::RectangleShape arm; arm.setFillColor(cc);
            float M=8.f, S=28.f, T=3.f;
            auto corner=[&](float cx,float cy,float dx,float dy){
                arm.setSize({S,T}); arm.setPosition(cx,cy); window.draw(arm);
                arm.setSize({T,S}); arm.setPosition(cx+(dx<0?-T:0),cy); window.draw(arm);
                (void)dy;
            };
            corner(M,   M,    1, 1);
            corner(WIN_W-M-S, M,    -1, 1);
            corner(M,   WIN_H-M-T, 1,-1);
            corner(WIN_W-M-S, WIN_H-M-T,-1,-1);
        }

        /* ── Ambient purple glow behind card ── */
        {
            float pulse=0.5f+0.5f*std::sin(t*1.8f);
            sf::RectangleShape glow({CARD_W+100.f,CARD_H+100.f});
            glow.setPosition(CARD_X-50.f,CARD_Y-50.f);
            glow.setFillColor({50,20,110,(sf::Uint8)(20+int(pulse*18))});
            window.draw(glow);
            // inner warmer copper glow right behind card
            glow.setSize({CARD_W+30.f,CARD_H+30.f});
            glow.setPosition(CARD_X-15.f,CARD_Y-15.f);
            glow.setFillColor({120,60,10,(sf::Uint8)(12+int(pulse*10))});
            window.draw(glow);
        }

        /* ── Central green card ── */
        draw_box(window,CARD_X,CARD_Y,CARD_W,CARD_H, G_PANEL,G_BORDER,2.f);
        // Card corners use the copper-amber colour for contrast against the green card
        draw_corners(window,CARD_X,CARD_Y,CARD_W,CARD_H, WIN_BDR_HI, 18.f);

        /* Inner subtle gradient overlay */
        {
            sf::RectangleShape inner({CARD_W-4.f,CARD_H/2.f});
            inner.setPosition(CARD_X+2.f,CARD_Y+2.f);
            inner.setFillColor({20,60,30,20}); window.draw(inner);
        }

        /* ── Title inside card ── */
        {
            /* decorative rule above title */
            sf::RectangleShape rl({CARD_W-60.f,1.f});
            rl.setPosition(CARD_X+30.f, CARD_Y+22.f); rl.setFillColor(G_DIM); window.draw(rl);

            draw_colored_title(window,font,CARD_X+CARD_W/2.f,CARD_Y+28.f,30,255,t);

            sf::RectangleShape rl2({CARD_W-60.f,1.f});
            rl2.setPosition(CARD_X+30.f,CARD_Y+74.f); rl2.setFillColor(G_DIM); window.draw(rl2);
        }

        /* ── Roll Number label + field ── */
        {
            auto lbl=mkt(font,"ROLL NUMBER  /  RNG SEED",12,G_TITLE);
            lbl.setPosition(FLD_X,CARD_Y+88.f); window.draw(lbl);

            bool field_hov = sf::FloatRect{FLD_X,FLD_Y,FLD_W,FLD_H}.contains(ms);
            sf::Color field_bd = field_hov ? G_BORDER_HI : G_FIELD_BD;
            draw_box(window,FLD_X,FLD_Y,FLD_W,FLD_H,G_FIELD_BG,field_bd,2.f);

            /* blinking cursor */
            bool cur_vis = fmod(t,1.0f)<0.55f;
            std::string disp = roll_str + (cur_vis ? "|" : " ");
            auto dt=mkt(font,disp,26,roll_str.empty()?G_DIM:C_WHITE);
            dt.setPosition(FLD_X+12.f,FLD_Y+10.f); window.draw(dt);

            /* hint below field */
            auto hint=mkt(font,"Type digits on keyboard",11,G_DIM);
            hint.setPosition(FLD_X,FLD_Y+FLD_H+5.f); window.draw(hint);
        }

        /* ── Party size label + cards ── */
        {
            auto lbl2=mkt(font,"PARTY SIZE",12,G_TITLE);
            lbl2.setPosition(FLD_X,PC_Y-20.f); window.draw(lbl2);

            for (int i=0;i<4;i++){
                bool sel=(party==i+1), hov=pc_b[i].contains(ms);
                sf::Color fill = sel?G_SEL : hov?sf::Color{8,44,18}:G_PANEL;
                sf::Color bord = sel?G_BORDER_HI : hov?G_BORDER_HI : G_BORDER;
                float thick = (sel||hov)?2.5f:1.5f;
                draw_box(window,pc_b[i].left,pc_b[i].top,pc_b[i].width,pc_b[i].height,fill,bord,thick);

                /* number */
                auto nt=mkt(font,pc_lbl[i],28,sel?G_BORDER_HI:hov?G_TITLE:G_DIM,sel);
                sf::FloatRect nb=nt.getLocalBounds();
                nt.setOrigin(nb.left+nb.width/2.f,nb.top);
                nt.setPosition(pc_b[i].left+pc_b[i].width/2.f,pc_b[i].top+4.f);
                window.draw(nt);

                /* sub label */
                auto st=mkt(font,pc_sub[i],10,sel?G_BORDER_HI:G_DIM);
                sf::FloatRect sb=st.getLocalBounds();
                st.setOrigin(sb.left+sb.width/2.f,sb.top);
                st.setPosition(pc_b[i].left+pc_b[i].width/2.f,pc_b[i].top+pc_b[i].height-16.f);
                window.draw(st);

                /* selected glow */
                if (sel){
                    sf::RectangleShape glow({pc_b[i].width,3.f});
                    glow.setPosition(pc_b[i].left,pc_b[i].top);
                    glow.setFillColor(G_BORDER_HI); window.draw(glow);
                    glow.setPosition(pc_b[i].left,pc_b[i].top+pc_b[i].height-3.f);
                    window.draw(glow);
                }
            }
        }

        /* ── Error / summary ── */
        if (!error_msg.empty()){
            float al=200.f+55.f*std::sin(t*6.f);
            auto et=mkt(font,error_msg,13,{220,70,70,(sf::Uint8)al});
            draw_centered(window,et,CARD_X+CARD_W/2.f,PC_Y+PC_H+14.f);
        } else if (!roll_str.empty()||party>0){
            std::ostringstream ss;
            ss<<"Seed: "<<(roll_str.empty()?"?":roll_str)
              <<"   Party: "<<(party>0?std::to_string(party):"?");
            auto st=mkt(font,ss.str(),12,G_DIM);
            draw_centered(window,st,CARD_X+CARD_W/2.f,PC_Y+PC_H+14.f);
        }

        /* ── Launch button ── */
        {
            bool ready = !roll_str.empty() && party>=1;
            bool hov   = launch_b.contains(ms);
            float pulse = 0.5f+0.5f*std::sin(t*3.f);

            sf::Color fill = hov&&ready ? G_LAUNCH_HI : ready ? G_LAUNCH : G_PANEL;
            sf::Color bord;
            if (hov&&ready) bord=G_BORDER_HI;
            else if (ready) bord={
                (sf::Uint8)(30+int(pulse*30)),
                (sf::Uint8)(150+int(pulse*50)),
                (sf::Uint8)(60+int(pulse*20))};
            else bord={20,50,28};

            draw_box(window,launch_b.left,launch_b.top,launch_b.width,launch_b.height,
                     fill,bord,2.f);

            /* glowing top edge when ready */
            if (ready){
                sf::RectangleShape glow({launch_b.width,2.f});
                glow.setPosition(launch_b.left,launch_b.top);
                glow.setFillColor({(sf::Uint8)(60+int(pulse*60)),
                                   (sf::Uint8)(180+int(pulse*40)),
                                   (sf::Uint8)(80+int(pulse*20)),
                                   (sf::Uint8)(180+int(pulse*75))});
                window.draw(glow);
            }

            auto lt=mkt(font,"LAUNCH  >",20,ready?(hov?C_WHITE:G_BORDER_HI):G_DIM,ready);
            sf::FloatRect lb=lt.getLocalBounds();
            lt.setOrigin(lb.left+lb.width/2.f,lb.top+lb.height/2.f);
            lt.setPosition(launch_b.left+launch_b.width/2.f,
                           launch_b.top+launch_b.height/2.f);
            window.draw(lt);

            if (!ready){
                auto h=mkt(font,"enter roll number and select party size",11,G_DIM);
                draw_centered(window,h,CARD_X+CARD_W/2.f,launch_b.top+launch_b.height+8.f);
            }
        }

        draw_scanlines(window);
        window.display();
    }
    return {0,0};
}

/* ════════════════════════════════════
 * Entry point
 * ════════════════════════════════════ */
int main()
{
    sf::Font font;
    const char* fpaths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf", nullptr
    };
    for (int i=0; fpaths[i]; ++i)
        if (font.loadFromFile(fpaths[i])) break;

    if (!run_splash(font)){ printf("Launch cancelled.\n"); return 0; }

    auto [roll_no, num_players] = run_launcher(font);
    if (roll_no==0||num_players==0){ printf("Launch cancelled.\n"); return 0; }

    printf("\nLaunching Chrono Rift [seed=%u, players=%d]...\n\n",roll_no,num_players);

    char roll_str[16], np_str[4];
    snprintf(roll_str,sizeof(roll_str),"%u",roll_no);
    snprintf(np_str,  sizeof(np_str),  "%d",num_players);
    char* args[] = {(char*)"arbiter_bin", roll_str, np_str, nullptr};
    return arbiter_main(3, args);
}
