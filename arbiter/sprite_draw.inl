

static const float SPRITE_W = 20.f;
static const float SPRITE_H = 20.f;

// spx
static void spx(sf::RenderTarget& rt, float bx, float by,
                int col, int row, float pw, float ph, sf::Color c)
{
    sf::RectangleShape r({pw - 0.5f, ph - 0.5f});
    r.setPosition(bx + col * pw, by + row * ph);
    r.setFillColor(c);
    rt.draw(r);
}

// draw_player_sprite
static void draw_player_sprite(sf::RenderTarget& rt, float x, float y, sf::Color tint)
{
    const float PW = 2.f, PH = 2.f;
    sf::Color hi = {(sf::Uint8)std::min(255,(int)tint.r+60),
                    (sf::Uint8)std::min(255,(int)tint.g+60),
                    (sf::Uint8)std::min(255,(int)tint.b+20)};
    sf::Color sh = {(sf::Uint8)(tint.r/2),(sf::Uint8)(tint.g/2),(sf::Uint8)(tint.b/2)};

    spx(rt,x,y,4,0,PW,PH,hi); spx(rt,x,y,5,0,PW,PH,hi);
    spx(rt,x,y,3,1,PW,PH,hi); spx(rt,x,y,4,1,PW,PH,tint);
    spx(rt,x,y,5,1,PW,PH,tint); spx(rt,x,y,6,1,PW,PH,hi);

    for(int c=2;c<=7;c++) spx(rt,x,y,c,2,PW,PH,tint);
    spx(rt,x,y,2,2,PW,PH,hi); spx(rt,x,y,7,2,PW,PH,hi);
    for(int c=1;c<=8;c++) spx(rt,x,y,c,3,PW,PH,tint);
    spx(rt,x,y,1,3,PW,PH,hi); spx(rt,x,y,8,3,PW,PH,sh);

    for(int c=1;c<=8;c++) spx(rt,x,y,c,4,PW,PH,tint);
    spx(rt,x,y,2,4,PW,PH,sh); spx(rt,x,y,3,4,PW,PH,sh);
    spx(rt,x,y,6,4,PW,PH,sh); spx(rt,x,y,7,4,PW,PH,sh);
    for(int c=1;c<=8;c++) spx(rt,x,y,c,5,PW,PH,tint);

    for(int c=0;c<=9;c++) spx(rt,x,y,c,6,PW,PH,tint);
    spx(rt,x,y,0,6,PW,PH,hi); spx(rt,x,y,9,6,PW,PH,sh);
    for(int c=0;c<=9;c++) spx(rt,x,y,c,7,PW,PH,tint);

    spx(rt,x,y,1,8,PW,PH,tint); spx(rt,x,y,2,8,PW,PH,tint);
    spx(rt,x,y,7,8,PW,PH,tint); spx(rt,x,y,8,8,PW,PH,tint);
    for(int c=2;c<=7;c++) spx(rt,x,y,c,9,PW,PH,sh);
}

// draw_enemy_sprite
static void draw_enemy_sprite(sf::RenderTarget& rt, float x, float y, sf::Color tint)
{
    const float PW = 2.f, PH = 2.f;
    sf::Color hi = {(sf::Uint8)std::min(255,(int)tint.r+40),
                    (sf::Uint8)std::min(255,(int)tint.g+20),
                    (sf::Uint8)std::min(255,(int)tint.b+10)};
    sf::Color dk = {(sf::Uint8)(tint.r/3),(sf::Uint8)(tint.g/4),(sf::Uint8)(tint.b/4)};
    sf::Color eye = {255,80,0};

    spx(rt,x,y,1,0,PW,PH,hi); spx(rt,x,y,8,0,PW,PH,hi);
    spx(rt,x,y,1,1,PW,PH,hi); spx(rt,x,y,2,1,PW,PH,tint);
    spx(rt,x,y,7,1,PW,PH,tint); spx(rt,x,y,8,1,PW,PH,hi);
    spx(rt,x,y,2,2,PW,PH,tint); spx(rt,x,y,3,2,PW,PH,tint);
    spx(rt,x,y,6,2,PW,PH,tint); spx(rt,x,y,7,2,PW,PH,tint);

    for(int c=2;c<=7;c++) spx(rt,x,y,c,3,PW,PH,tint);
    spx(rt,x,y,2,3,PW,PH,hi);
    for(int c=1;c<=8;c++) spx(rt,x,y,c,4,PW,PH,tint);
    spx(rt,x,y,1,4,PW,PH,hi);
    for(int c=1;c<=8;c++) spx(rt,x,y,c,5,PW,PH,tint);

    for(int c=1;c<=8;c++) spx(rt,x,y,c,6,PW,PH,tint);
    spx(rt,x,y,2,6,PW,PH,dk); spx(rt,x,y,3,6,PW,PH,eye);
    spx(rt,x,y,6,6,PW,PH,eye); spx(rt,x,y,7,6,PW,PH,dk);

    for(int c=1;c<=8;c++) spx(rt,x,y,c,7,PW,PH,tint);
    spx(rt,x,y,4,7,PW,PH,dk); spx(rt,x,y,5,7,PW,PH,dk);
    spx(rt,x,y,1,8,PW,PH,tint); spx(rt,x,y,2,8,PW,PH,tint);
    spx(rt,x,y,3,8,PW,PH,dk); spx(rt,x,y,6,8,PW,PH,dk);
    spx(rt,x,y,7,8,PW,PH,tint); spx(rt,x,y,8,8,PW,PH,tint);
    for(int c=2;c<=7;c++) spx(rt,x,y,c,9,PW,PH,dk);
}

// draw_dead_sprite
static void draw_dead_sprite(sf::RenderTarget& rt, float x, float y)
{
    const float PW = 2.f, PH = 2.f;
    sf::Color c = {80,50,50};
    for(int i=1;i<=8;i++){
        spx(rt,x,y,i,i,PW,PH,c);
        spx(rt,x,y,9-i,i,PW,PH,c);
    }
}

// draw_stun_sprite
static void draw_stun_sprite(sf::RenderTarget& rt, float x, float y)
{
    const float PW = 2.f, PH = 2.f;
    sf::Color c = {180,100,255};
    spx(rt,x,y,5,0,PW,PH,c); spx(rt,x,y,6,0,PW,PH,c);
    spx(rt,x,y,4,1,PW,PH,c); spx(rt,x,y,5,1,PW,PH,c);
    spx(rt,x,y,3,2,PW,PH,c); spx(rt,x,y,4,2,PW,PH,c);
    spx(rt,x,y,5,2,PW,PH,c); spx(rt,x,y,6,2,PW,PH,c); spx(rt,x,y,7,2,PW,PH,c);
    spx(rt,x,y,4,3,PW,PH,c); spx(rt,x,y,5,3,PW,PH,c); spx(rt,x,y,6,3,PW,PH,c);
    spx(rt,x,y,5,4,PW,PH,c); spx(rt,x,y,6,4,PW,PH,c);
    spx(rt,x,y,7,4,PW,PH,c); spx(rt,x,y,8,4,PW,PH,c);
    spx(rt,x,y,4,5,PW,PH,c); spx(rt,x,y,5,5,PW,PH,c); spx(rt,x,y,6,5,PW,PH,c);
    spx(rt,x,y,3,6,PW,PH,c); spx(rt,x,y,4,6,PW,PH,c);
    spx(rt,x,y,4,7,PW,PH,c); spx(rt,x,y,5,7,PW,PH,c);
    spx(rt,x,y,3,8,PW,PH,c); spx(rt,x,y,4,8,PW,PH,c);
    spx(rt,x,y,3,9,PW,PH,c);
}
