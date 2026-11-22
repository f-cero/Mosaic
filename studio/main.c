/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic Studio — a cross-platform (SDL2) editor: node graph, sequence,
 * scene preview and asset browser. Builds the game object through the same
 * ms_build path the CLI uses. */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "font8x8_basic.h"
#include "ms_platform.h"
#include "mosaic/ms_build.h"
#include "mosaic/ms_cart.h"
#include "mosaic/ms_core.h"
#include "mosaic/ms_game.h"
#include "mosaic/ms_input.h"

#define W 1280
#define H 800
#define MAXL 400
#define CW 256

static char g_lines[MAXL][CW]; static int g_nlines;
static char g_path[256];
static char g_dir[256];
static struct { int id, x, y; char type[24]; char kv[6][64]; int nkv; } g_nodes[128]; static int g_nodes_n;
static struct { int s, d; char sp[12], dp[12]; } g_links[256]; static int g_links_n;
static int g_sel = -1, g_tab = 0; /* 0 graph 1 scene 2 sequence 3 assets */
static int g_drag = -1, g_dx, g_dy, g_wire = -1, g_wx, g_wy, g_panx, g_pany;
static char g_log[512]; static int g_log_ok;
static int g_playing = 1;

static SDL_Window *g_win; static SDL_Renderer *g_ren; static SDL_Texture *g_font;
static SDL_Texture *g_scene;
static uint16_t g_fb[MS_SCREEN_W * MS_SCREEN_H];
static uint8_t g_arena[8192];
static ms_core_t g_core; static ms_game_t g_game; static int g_have_game;
static ms_cart_t g_cart; static uint8_t g_cartbuf[MS_CART_BYTES_MAX]; static size_t g_cartlen;
static uint16_t g_held;

static void setlog(const char *s, int ok){ snprintf(g_log,sizeof(g_log),"%s",s); g_log_ok=ok; }

/* ---------- text ---------- */
static void font_build(void)
{
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, 128*8, 8, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_FillRect(surf, NULL, 0);
    Uint32 *px = surf->pixels;
    for (int c = 0; c < 128; c++) for (int y = 0; y < 8; y++) {
        unsigned char bits = (unsigned char)font8x8_basic[c][y];
        for (int x = 0; x < 8; x++) if (bits & (1u << x)) px[y * surf->w + c*8 + x] = 0xFFFFFFFFu;
    }
    g_font = SDL_CreateTextureFromSurface(g_ren, surf);
    SDL_SetTextureBlendMode(g_font, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(surf);
}
static int textw(const char *s, int sc){ return (int)strlen(s) * 8 * sc; }
static void text(int x, int y, const char *s, int sc, SDL_Color col)
{
    SDL_SetTextureColorMod(g_font, col.r, col.g, col.b);
    SDL_SetTextureAlphaMod(g_font, col.a);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s; if (c >= 128) c = '?';
        SDL_Rect src = { c*8, 0, 8, 8 }, dst = { x, y, 8*sc, 8*sc };
        SDL_RenderCopy(g_ren, g_font, &src, &dst); x += 8*sc;
    }
}
static void fill(int x,int y,int w,int h, SDL_Color c){ SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a); SDL_Rect r={x,y,w,h}; SDL_RenderFillRect(g_ren,&r); }
static void frame(int x,int y,int w,int h, SDL_Color c){ SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a); SDL_Rect r={x,y,w,h}; SDL_RenderDrawRect(g_ren,&r); }
static void line(int x1,int y1,int x2,int y2, SDL_Color c){ SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a); SDL_RenderDrawLine(g_ren,x1,y1,x2,y2); }
static void btn(int x,int y,int w,int h,const char *label,int hi)
{
    SDL_Color bg = hi? (SDL_Color){56,96,160,255} : (SDL_Color){36,42,58,255};
    fill(x,y,w,h,bg); frame(x,y,w,h,(SDL_Color){80,90,115,255});
    text(x+8,y+(h-8)/2,label,1,(SDL_Color){225,230,240,255});
}
static int hit(int mx,int my,int x,int y,int w,int h){ return mx>=x&&my>=y&&mx<x+w&&my<y+h; }

/* ---------- project io ---------- */
static void project_load(const char *path)
{
    snprintf(g_path,sizeof(g_path),"%s",path);
    const char *sl = strrchr(path,'/');
    size_t dl = sl? (size_t)(sl-path):0; memcpy(g_dir,path,dl); g_dir[dl]=0;
    FILE *f=fopen(path,"r"); if(!f) return;
    g_nlines=0; g_nodes_n=0; g_links_n=0;
    while(g_nlines<MAXL && fgets(g_lines[g_nlines],CW,f)){
        char *nl=strpbrk(g_lines[g_nlines],"\r\n"); if(nl)*nl=0;
        char *s=g_lines[g_nlines];
        if(!strncmp(s,"node ",5)){
            int id,n; char ty[24]; int x,y;
            if(sscanf(s+5,"%d %23s %d %d%n",&id,ty,&x,&y,&n)==4){
                g_nodes[g_nodes_n].id=id; g_nodes[g_nodes_n].x=x; g_nodes[g_nodes_n].y=y;
                snprintf(g_nodes[g_nodes_n].type,24,"%s",ty);
                g_nodes[g_nodes_n].nkv=0;
                char *p=s+5+n; while(*p && g_nodes[g_nodes_n].nkv<6){
                    while(*p==' ')p++; if(!*p)break;
                    char *eq=strchr(p,'='); if(!eq)break;
                    int kl=(int)(eq-p), vl=0; char *ve=eq+1;
                    while(*ve && *ve!=' ')ve++; vl=(int)(ve-eq-1);
                    if(kl>15)kl=15; if(vl>31)vl=31;
                    snprintf(g_nodes[g_nodes_n].kv[g_nodes[g_nodes_n].nkv],64,"%.*s=%.*s",kl,p,vl,eq+1);
                    g_nodes[g_nodes_n].nkv++; p=ve;
                }
                g_nodes_n++;
            }
        } else if(!strncmp(s,"link ",5)){
            int a,b; char pa[12],pb[12];
            if(sscanf(s+5,"%d.%11s %d.%11s",&a,pa,&b,pb)==4){
                g_links[g_links_n].s=a; g_links[g_links_n].d=b;
                snprintf(g_links[g_links_n].sp,12,"%s",pa); snprintf(g_links[g_links_n].dp,12,"%s",pb);
                g_links_n++;
            }
        }
        g_nlines++;
    }
    fclose(f);
}
static void project_save(void)
{
    char tmp[300]; snprintf(tmp,sizeof(tmp),"%s.tmp",g_path);
    FILE *f=fopen(tmp,"w"); if(!f)return;
    for(int i=0;i<g_nlines;i++) fprintf(f,"%s\n",g_lines[i]);
    fclose(f); rename(tmp,g_path);
}

/* ---------- build ---------- */
static void do_build(void)
{
    project_save();
    char old[512]; if(!MS_GETCWD(old,sizeof(old))) old[0]=0;
    if(g_dir[0]) (void)!MS_CHDIR(g_dir);
    static ms_build_result_t r;
    int rc = ms_build_project(g_path, &r);
    if(g_dir[0]) (void)!MS_CHDIR(old);
    if(rc!=0){ setlog(r.error,0); return; }
    memcpy(g_cartbuf,r.cart,r.cart_len); g_cartlen=r.cart_len;
    ms_cart_unmount(&g_cart);
    if(ms_cart_mount(&g_cart,g_cartbuf,g_cartlen)!=0){ setlog("cart validation failed",0); g_have_game=0; return; }
    ms_config_t cfg={0}; cfg.audio_hz=MS_AUDIO_HZ; cfg.voices=16; cfg.music_voices=12; cfg.sfx_voices=4;
    ms_core_shutdown(&g_core);
    if(ms_core_init(&g_core,&cfg,g_fb,g_arena,sizeof(g_arena))!=0){ setlog("core init failed",0); g_have_game=0; return; }
    g_have_game = ms_game_load(&g_game,&g_cart,&g_core.ppu,&g_core.synth)==0;
    if(!g_have_game){ setlog(ms_game_error(&g_game),0); return; }
    ms_input_clear(&g_core.input);
    SDL_SetTextureScaleMode(g_scene, SDL_ScaleModeNearest);
    char msg[256]; snprintf(msg,sizeof(msg),"built: %zu bytes, %d entities", r.cart_len, g_game.prog.entity_count);
    setlog(msg,1);
}
static void do_export(void)
{
    if(!g_cartlen){ setlog("build first",0); return; }
    char out[300]; snprintf(out,sizeof(out),"%s/build/game.mcart",g_dir);
    char mk[320]; snprintf(mk,sizeof(mk),"%s/build",g_dir); ms_mkdir_p(mk);
    FILE *f=fopen(out,"wb");
    if(!f){ setlog("cannot write export",0); return; }
    fwrite(g_cartbuf,1,g_cartlen,f); fclose(f);
    char msg[300]; snprintf(msg,sizeof(msg),"exported %s",out); setlog(msg,1);
}

/* ---------- layout helpers for nodes ---------- */
static int node_h(const char *t){ return (!strcmp(t,"if")||!strcmp(t,"update")||!strcmp(t,"start"))?64:40; }
static int node_find(int id){ for(int i=0;i<g_nodes_n;i++) if(g_nodes[i].id==id) return i; return -1; }
static void node_rect(int i,int *x,int *y,int *w,int *h)
{
    *x=g_nodes[i].x+g_panx; *y=g_nodes[i].y+g_pany; *w=150; *h=node_h(g_nodes[i].type);
}
static void add_node(const char *type)
{
    int id=1; for(int i=0;i<g_nodes_n;i++) if(g_nodes[i].id>=id) id=g_nodes[i].id+1;
    char ln[CW]; snprintf(ln,CW,"node %d %s 220 300",id,type);
    snprintf(g_lines[g_nlines],CW,"%s",ln); g_nlines++;
    snprintf(g_nodes[g_nodes_n].type,24,"%s",type);
    g_nodes[g_nodes_n].id=id; g_nodes[g_nodes_n].x=220; g_nodes[g_nodes_n].y=300; g_nodes[g_nodes_n].nkv=0;
    g_sel=g_nodes_n; g_nodes_n++;
    setlog("node added — drag to place, wire from the right pin",1);
}

/* ---------- drawing ---------- */
static const char *palette[]={"update","number","held","pressed","if","addx","addy","setx","sety","wait","playsfx","playsong","random","brightness","bgscrollx","spawn","destroy","signal"};
static void draw_graph(void)
{
    int ox=232, oy=48, ow=W-232-268, oh=H-48-100;
    fill(ox,oy,ow,oh,(SDL_Color){18,20,30,255});
    for(int gx=ox; gx<ox+ow; gx+=32) line(gx,oy,gx,oy+oh,(SDL_Color){26,28,40,255});
    for(int gy=oy; gy<oy+oh; gy+=32) line(ox,gy,ox+ow,gy,(SDL_Color){26,28,40,255});
    for(int i=0;i<g_links_n;i++){
        int si=node_find(g_links[i].s), di=node_find(g_links[i].d);
        if(si<0||di<0)continue;
        int sx,sy,sw,sh,dx,dy,dw,dh;
        node_rect(si,&sx,&sy,&sw,&sh); node_rect(di,&dx,&dy,&dw,&dh);
        int ax=sx+sw, ay=sy+sh-12, bx=dx, by=dy+16;
        if(!strcmp(g_links[i].sp,"then")) ay=sy+10;
        if(!strcmp(g_links[i].sp,"else")) ay=sy+22;
        if(!strcmp(g_links[i].dp,"cond")) by=dy+10;
        SDL_Color lc = !strcmp(g_links[i].sp,"next")||!strcmp(g_links[i].sp,"then")||!strcmp(g_links[i].sp,"else")
                     ? (SDL_Color){230,190,90,255} : (SDL_Color){120,200,255,255};
        int mx=(ax+bx)/2;
        for(int t=0;t<20;t++){
            float u=t/19.0f, v=1-u;
            int x=(int)(v*v*ax+2*v*u*mx+u*u*bx), y=(int)(v*v*ay+2*v*u*ay+u*u*by);
            fill(x,y,2,2,lc);
        }
    }
    for(int i=0;i<g_nodes_n;i++){
        int x,y,w,h; node_rect(i,&x,&y,&w,&h);
        SDL_Color hd = !strcmp(g_nodes[i].type,"update")? (SDL_Color){70,140,110,255}
                     : !strcmp(g_nodes[i].type,"if")? (SDL_Color){150,95,70,255}
                     : !strcmp(g_nodes[i].type,"held")||!strcmp(g_nodes[i].type,"pressed")? (SDL_Color){70,110,150,255}
                     : (SDL_Color){70,75,100,255};
        fill(x,y,w,h,(SDL_Color){32,36,50,255}); frame(x,y,w,h,g_sel==i?(SDL_Color){240,200,120,255}:(SDL_Color){70,80,105,255});
        fill(x,y,w,18,hd);
        text(x+6,y+5,g_nodes[i].type,1,(SDL_Color){240,244,255,255});
        char idbuf[24]; snprintf(idbuf,sizeof(idbuf),"#%d",g_nodes[i].id);
        text(x+w-textw(idbuf,1)-6,y+5,idbuf,1,(SDL_Color){200,210,225,255});
        int exec = !strcmp(g_nodes[i].type,"update")||!strcmp(g_nodes[i].type,"start")||!strcmp(g_nodes[i].type,"if")
                || !strcmp(g_nodes[i].type,"addx")||!strcmp(g_nodes[i].type,"addy")||!strcmp(g_nodes[i].type,"setx")
                || !strcmp(g_nodes[i].type,"sety")||!strcmp(g_nodes[i].type,"wait")||!strcmp(g_nodes[i].type,"playsfx")
                || !strcmp(g_nodes[i].type,"playsong")||!strcmp(g_nodes[i].type,"brightness")||!strcmp(g_nodes[i].type,"bgscrollx")
                || !strcmp(g_nodes[i].type,"spawn")||!strcmp(g_nodes[i].type,"destroy")||!strcmp(g_nodes[i].type,"signal");
        if(exec){ fill(x-4,y+h-16,8,8,(SDL_Color){230,190,90,255}); text(x+6,y+h-18,"in",1,(SDL_Color){170,175,190,255}); }
        else { fill(x-4,y+16,8,8,(SDL_Color){120,200,255,255}); text(x+6,y+22,"value",1,(SDL_Color){170,175,190,255}); }
        if(!strcmp(g_nodes[i].type,"if")){
            fill(x+w-4,y+6,8,8,(SDL_Color){230,190,90,255}); text(x+w-34,y+8,"then",1,(SDL_Color){230,190,90,255});
            fill(x+w-4,y+20,8,8,(SDL_Color){230,190,90,255}); text(x+w-34,y+22,"else",1,(SDL_Color){230,190,90,255});
            fill(x+w-4,y+h-10,8,8,(SDL_Color){230,190,90,255});
            fill(x-4,y+6,8,8,(SDL_Color){120,200,255,255}); text(x+6,y+8,"cond",1,(SDL_Color){120,200,255,255});
        } else if(exec){ fill(x+w-4,y+h-16,8,8,(SDL_Color){230,190,90,255}); text(x+w-30,y+h-18,"next",1,(SDL_Color){230,190,90,255}); }
        else { fill(x+w-4,y+h-14,8,8,(SDL_Color){120,200,255,255}); text(x+w-28,y+h-16,"out",1,(SDL_Color){120,200,255,255}); }
        for(int k=0;k<g_nodes[i].nkv;k++) text(x+8,y+22+k*10,g_nodes[i].kv[k],1,(SDL_Color){210,215,230,255});
    }
}
static void draw_scene(void)
{
    int ox=232, oy=48;
    fill(ox,oy,W-232-268,H-48-100,(SDL_Color){14,16,24,255});
    text(ox+60,oy+20,"SCENE — 320x240 RGB565 (2x preview)",1,(SDL_Color){150,160,180,255});
    SDL_Rect dst={ox+((W-232-268)-640)/2, oy+70, 640, 480};
    frame(dst.x-2,dst.y-2,dst.w+4,dst.h+4,(SDL_Color){70,80,105,255});
    if(g_have_game) SDL_RenderCopy(g_ren,g_scene,NULL,&dst);
    char st[128]; snprintf(st,sizeof(st),"mode %d  entities %u  frame %u  %s",g_core.cfg.mode,g_game.vm.count,g_core.frame,g_playing?"PLAY":"PAUSE");
    text(ox+60,oy+560,st,1,(SDL_Color){190,200,215,255});
    text(ox+60,oy+580,"Arrows/A move  •  Space pause  •  B jump",1,(SDL_Color){130,140,160,255});
}
static void draw_sequence(void)
{
    int ox=232, oy=48;
    fill(ox,oy,W-232-268,H-48-100,(SDL_Color){18,20,30,255});
    text(ox+20,oy+14,"SEQUENCE — compiled exec chain of the selected graph",1,(SDL_Color){150,160,180,255});
    int y=oy+60;
    for(int i=0;i<g_nodes_n && y<oy+520;i++){
        int exec = strcmp(g_nodes[i].type,"number") && strcmp(g_nodes[i].type,"held") && strcmp(g_nodes[i].type,"pressed") && strcmp(g_nodes[i].type,"random");
        if(!exec) continue;
        fill(ox+60,y,220,26,(SDL_Color){36,42,58,255}); frame(ox+60,y,220,26,(SDL_Color){80,90,115,255});
        text(ox+70,y+9,g_nodes[i].type,1,(SDL_Color){225,230,240,255});
        char b[32]; snprintf(b,sizeof(b),"step %d",i+1); text(ox+240,y+9,b,1,(SDL_Color){140,150,170,255});
        if(y+52<oy+520){ line(ox+170,y+26,ox+170,y+52,(SDL_Color){230,190,90,255}); fill(ox+166,y+48,8,6,(SDL_Color){230,190,90,255}); }
        y+=52;
    }
}
static void draw_assets(void)
{
    int ox=232, oy=48;
    fill(ox,oy,W-232-268,H-48-100,(SDL_Color){18,20,30,255});
    text(ox+20,oy+14,"ASSETS",1,(SDL_Color){150,160,180,255});
    char cmd[320]; snprintf(cmd,sizeof(cmd),"ls -1 %s/assets 2>/dev/null",g_dir);
    FILE *p=MS_POPEN(cmd,"r"); char ln[128]; int y=oy+50;
    if(p){ while(fgets(ln,sizeof(ln),p)&&y<oy+500){ char *n=strpbrk(ln,"\r\n"); if(n)*n=0;
        fill(ox+40,y,300,22,(SDL_Color){26,30,42,255}); text(ox+50,y+7,ln,1,(SDL_Color){210,215,230,255});
        const char *ext=strrchr(ln,'.'); 
        text(ox+360,y+7, ext?ext:"",1,(SDL_Color){120,200,255,255});
        y+=28; } MS_PCLOSE(p); }
    text(ox+40,oy+540,"PNG -> SPR2 / level tileset + map, SMF -> music. Run `mosaic convert`.",1,(SDL_Color){130,140,160,255});
}

static void draw_ui(void)
{
    fill(0,0,W,H,(SDL_Color){12,14,20,255});
    fill(0,0,W,40,(SDL_Color){24,27,38,255});
    fill(0,39,W,1,(SDL_Color){60,70,90,255});
    text(12,14,"MOSAIC",2,(SDL_Color){255,210,120,255});
    text(130,18,"STUDIO",1,(SDL_Color){150,160,180,255});
    const char *tabs[]={"Graph","Scene","Sequence","Assets"};
    for(int i=0;i<4;i++){
        int x=260+i*100;
        btn(x,8,92,24,tabs[i],g_tab==i);
        if(g_tab==i) fill(x,31,92,2,(SDL_Color){255,200,110,255});
    }
    btn(700,8,90,24,"Build",0); btn(796,8,90,24,"Export",0); btn(892,8,90,24,g_playing?"Pause":"Play",0); btn(988,8,90,24,"Save",0);
    text(1100,18,"v1.0 • Apache-2.0",1,(SDL_Color){120,130,150,255});

    /* left */
    fill(0,40,228,H-140,(SDL_Color){20,23,33,255});
    text(14,54,"NODE PALETTE",1,(SDL_Color){130,140,160,255});
    for(int i=0;i<(int)(sizeof(palette)/sizeof(palette[0]));i++){
        int x=12+(i%2)*106, y=72+(i/2)*26;
        btn(x,y,100,22,palette[i],0);
    }
    text(14,340,"PROJECT",1,(SDL_Color){130,140,160,255});
    text(14,360,g_path,1,(SDL_Color){200,205,220,255});
    char info[64]; snprintf(info,sizeof(info),"%d nodes  %d links",g_nodes_n,g_links_n);
    text(14,380,info,1,(SDL_Color){150,160,180,255});
    text(14,404,"ENTITY",1,(SDL_Color){130,140,160,255});
    text(14,424,"player  sheet0  176",1,(SDL_Color){200,205,220,255});
    text(14,448,"LIMITS 320x240/60",1,(SDL_Color){150,160,180,255});
    text(14,464,"128 OAM  2 BG  16 v",1,(SDL_Color){150,160,180,255});
    text(14,480,"no PCM • SMF only",1,(SDL_Color){235,170,120,255});

    /* right inspector */
    int rx=W-260;
    fill(rx,40,260,H-140,(SDL_Color){20,23,33,255});
    text(rx+14,54,"INSPECTOR",1,(SDL_Color){130,140,160,255});
    if(g_sel>=0 && g_sel<g_nodes_n){
        text(rx+14,78,g_nodes[g_sel].type,2,(SDL_Color){255,210,120,255});
        char b[64]; snprintf(b,sizeof(b),"node #%d",g_nodes[g_sel].id); text(rx+14,100,b,1,(SDL_Color){170,180,200,255});
        int y=128;
        for(int k=0;k<g_nodes[g_sel].nkv;k++){
            text(rx+14,y,g_nodes[g_sel].kv[k],1,(SDL_Color){215,220,235,255});
            btn(rx+150,y-4,24,18,"-",0); btn(rx+180,y-4,24,18,"+",0);
            y+=28;
        }
        text(rx+14,y+16,"Drag node to move.",1,(SDL_Color){140,150,170,255});
        text(rx+14,y+32,"Drag pin to connect.",1,(SDL_Color){140,150,170,255});
        text(rx+14,y+48,"Del removes node.",1,(SDL_Color){140,150,170,255});
    } else {
        text(rx+14,80,"Select a node",1,(SDL_Color){170,180,200,255});
    }
    text(rx+14,H-180,"TIMELINE",1,(SDL_Color){130,140,160,255});
    fill(rx+14,H-156,232,60,(SDL_Color){26,30,42,255});
    for(int i=0;i<12;i++){ fill(rx+20+i*19,H-130,12,34,(SDL_Color){60+i*8,90+i*5,150,255}); }

    /* bottom log */
    fill(0,H-100,W,100,(SDL_Color){16,18,26,255});
    fill(0,H-101,W,1,(SDL_Color){50,58,75,255});
    text(12,H-88,"CONSOLE",1,(SDL_Color){130,140,160,255});
    text(12,H-70,g_log,1,g_log_ok?(SDL_Color){140,225,170,255}:(SDL_Color){240,150,140,255});
    text(12,H-46,"mosaic build starfall.mpj  •  mosaic run starfall.mcart --screenshot shot.png",1,(SDL_Color){120,130,150,255});
    text(12,H-26,"Apache-2.0 • core: C99, no heap on the hot path • targets ESP32 module clusters over SPI",1,(SDL_Color){100,110,130,255});
}

/* ---------- input ---------- */
static void scene_input(void)
{
    const Uint8 *k=SDL_GetKeyboardState(NULL);
    uint16_t h=0;
    if(k[SDL_SCANCODE_LEFT])h|=MS_BTN_LEFT;
    if(k[SDL_SCANCODE_RIGHT])h|=MS_BTN_RIGHT;
    if(k[SDL_SCANCODE_UP])h|=MS_BTN_UP;
    if(k[SDL_SCANCODE_DOWN])h|=MS_BTN_DOWN;
    if(k[SDL_SCANCODE_Z]||k[SDL_SCANCODE_SPACE])h|=MS_BTN_A;
    if(k[SDL_SCANCODE_X])h|=MS_BTN_B;
    g_held=h;
    if(g_have_game){ ms_input_edge(&g_core.input,h); g_core.input.touch_x=g_core.input.touch_y=-1; }
}
static void update_scene(void)
{
    if(g_have_game && g_playing){ ms_core_run_frame(&g_core,&g_game,&g_core.input);
        SDL_UpdateTexture(g_scene,NULL,g_fb,MS_SCREEN_W*sizeof(uint16_t)); }
}

int main(int argc, char **argv)
{
    const char *proj="examples/starfall/starfall.mpj";
    const char *shot=NULL; const char *view="graph"; int frames=2;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--screenshot")&&i+1<argc) shot=argv[++i];
        else if(!strcmp(argv[i],"--view")&&i+1<argc) view=argv[++i];
        else if(!strcmp(argv[i],"--frames")&&i+1<argc) frames=atoi(argv[++i]);
        else if(argv[i][0]!='-') proj=argv[i];
    }
    if(shot) SDL_SetHint(SDL_HINT_VIDEODRIVER,"dummy");
    if(SDL_Init(SDL_INIT_VIDEO)<0){ fprintf(stderr,"SDL: %s\n",SDL_GetError()); return 1; }
    g_win=SDL_CreateWindow("Mosaic Studio",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,W,H,shot?SDL_WINDOW_HIDDEN:SDL_WINDOW_SHOWN);
    g_ren=SDL_CreateRenderer(g_win,-1,SDL_RENDERER_SOFTWARE);
    if(!g_win||!g_ren){ fprintf(stderr,"window: %s\n",SDL_GetError()); return 1; }
    SDL_SetRenderDrawBlendMode(g_ren,SDL_BLENDMODE_BLEND);
    font_build();
    g_scene=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STREAMING,MS_SCREEN_W,MS_SCREEN_H);
    SDL_SetTextureScaleMode(g_scene,SDL_ScaleModeNearest);
    project_load(proj);
    if(!strcmp(view,"scene")) g_tab=1; else if(!strcmp(view,"sequence")) g_tab=2; else if(!strcmp(view,"assets")) g_tab=3;
    do_build();
    if(!strcmp(view,"scene")){ for(int i=0;i<20;i++){ scene_input(); update_scene(); } }
    if(shot){
        for(int i=0;i<frames;i++){ draw_ui();
            if(g_tab==0)draw_graph(); else if(g_tab==1)draw_scene(); else if(g_tab==2)draw_sequence(); else draw_assets();
            SDL_RenderPresent(g_ren);
            if(g_tab==1) update_scene();
        }
        int ww,hh; SDL_GetRendererOutputSize(g_ren,&ww,&hh);
        SDL_Surface *surf=SDL_CreateRGBSurfaceWithFormat(0,ww,hh,32,SDL_PIXELFORMAT_RGBA32);
        SDL_RenderReadPixels(g_ren,NULL,SDL_PIXELFORMAT_RGBA32,surf->pixels,surf->pitch);
        extern int stbi_write_png(const char*,int,int,int,const void*,int);
        stbi_write_png(shot,ww,hh,4,surf->pixels,surf->pitch);
        SDL_FreeSurface(surf);
        SDL_DestroyRenderer(g_ren); SDL_DestroyWindow(g_win); SDL_Quit();
        return 0;
    }
    int run=1; SDL_Event ev;
    while(run){
        while(SDL_PollEvent(&ev)){
            if(ev.type==SDL_QUIT) run=0;
            if(ev.type==SDL_KEYDOWN && ev.key.keysym.sym==SDLK_ESCAPE) run=0;
            if(ev.type==SDL_KEYDOWN && ev.key.keysym.sym==SDLK_SPACE) g_playing=!g_playing;
            if(ev.type==SDL_KEYDOWN && ev.key.keysym.sym==SDLK_DELETE && g_sel>=0){
                char want[32]; snprintf(want,sizeof(want),"node %d ",g_nodes[g_sel].id);
                for(int i=0;i<g_nlines;i++) if(!strncmp(g_lines[i],want,strlen(want))){ g_lines[i][0]='#'; }
                g_sel=-1; project_save();
                setlog("node removed — reload to refresh view",1);
            }
            if(ev.type==SDL_MOUSEMOTION){
                int mx=ev.motion.x,my=ev.motion.y;
                if(g_drag>=0){ g_nodes[g_drag].x=mx-g_dx-g_panx; g_nodes[g_drag].y=my-g_dy-g_pany; }
                if(g_wire>=0){ g_wx=mx; g_wy=my; }
            }
            if(ev.type==SDL_MOUSEBUTTONDOWN && ev.button.button==1){
                int mx=ev.button.x,my=ev.button.y;
                if(my<40){
                    for(int i=0;i<4;i++) if(hit(mx,my,260+i*100,8,92,24)) g_tab=i;
                    if(hit(mx,my,700,8,90,24)) do_build();
                    if(hit(mx,my,796,8,90,24)) do_export();
                    if(hit(mx,my,892,8,90,24)) g_playing=!g_playing;
                    if(hit(mx,my,988,8,90,24)){ project_save(); setlog("project saved",1); }
                } else if(mx<228){
                    for(int i=0;i<(int)(sizeof(palette)/sizeof(palette[0]));i++){
                        int x=12+(i%2)*106,y=72+(i/2)*26;
                        if(hit(mx,my,x,y,100,22)) add_node(palette[i]);
                    }
                } else if(mx>W-260 && g_sel>=0){
                    /* inspector +/- on props */
                    for(int k=0;k<g_nodes[g_sel].nkv;k++){
                        int y=128+k*28;
                        if(hit(mx,my,W-260+150,y-4,24,18)||hit(mx,my,W-260+180,y-4,24,18)){
                            char key[32]; snprintf(key,sizeof(key),"%s",g_nodes[g_sel].kv[k]);
                            char *eq=strchr(key,'='); if(!eq)continue; *eq=0;
                            int delta=hit(mx,my,W-260+180,y-4,24,18)?1:-1;
                            int val=atoi(eq+1)+delta;
                            char nv[64]; snprintf(nv,sizeof(nv),"%s=%d",key,val);
                            snprintf(g_nodes[g_sel].kv[k],64,"%s",nv);
                            /* write through to the project line, then persist */
                            char want[32]; snprintf(want,sizeof(want),"node %d ",g_nodes[g_sel].id);
                            for(int i=0;i<g_nlines;i++) if(!strncmp(g_lines[i],want,strlen(want))){
                                size_t kl=strlen(key); char *t=g_lines[i];
                                int done=0;
                                while((t=strstr(t,key))!=NULL){
                                    if((t==g_lines[i]||t[-1]==' ')&&t[kl]=='='){
                                        char *sp=strchr(t+kl,' ');
                                        if(sp) snprintf(t,(size_t)(g_lines[i]+CW-t),"%s=%d%s",key,val,sp);
                                        else   snprintf(t,(size_t)(g_lines[i]+CW-t),"%s=%d",key,val);
                                        done=1; break;
                                    }
                                    t+=kl;
                                }
                                if(!done){ /* key not on the line yet: append it */
                                    size_t l=strlen(g_lines[i]);
                                    snprintf(g_lines[i]+l,(size_t)(g_lines[i]+CW-g_lines[i]-l)," %s=%d",key,val);
                                }
                                break;
                            }
                            project_save();
                            setlog("property changed",1);
                        }
                    }
                } else if(g_tab==0){
                    for(int i=g_nodes_n-1;i>=0;i--){
                        int x,y,w,h; node_rect(i,&x,&y,&w,&h);
                        if(hit(mx,my,x+w-8,y+h-20,8,8)||hit(mx,my,x+w-8,y+4,8,10)||hit(mx,my,x+w-8,y+18,8,10)){
                            g_wire=g_nodes[i].id; g_wx=mx; g_wy=my; g_panx=g_panx; break;
                        }
                        if(hit(mx,my,x,y,w,h)){ g_sel=i; g_drag=i; g_dx=mx-x; g_dy=my-y; break; }
                    }
                }
            }
            if(ev.type==SDL_MOUSEBUTTONUP && ev.button.button==1){
                if(g_wire>=0){
                    int mx=ev.button.x,my=ev.button.y;
                    for(int i=0;i<g_nodes_n;i++){
                        int x,y,w,h; node_rect(i,&x,&y,&w,&h);
                        if(hit(mx,my,x-6,y,14,h)){
                            int swi=node_find(g_wire); const char *st=swi>=0?g_nodes[swi].type:"number";
                            int exec_src=strcmp(st,"number")&&strcmp(st,"held")&&strcmp(st,"pressed")&&strcmp(st,"random")&&strcmp(st,"sin")&&strcmp(st,"cos");
                            const char *dp=!strcmp(g_nodes[i].type,"if")?"cond": (strcmp(g_nodes[i].type,"number")&&strcmp(g_nodes[i].type,"held")&&strcmp(g_nodes[i].type,"pressed")&&strcmp(g_nodes[i].type,"random"))?"in":"value";
                            char ln[CW]; snprintf(ln,CW,"link %d.%s %d.%s",g_wire,exec_src?"next":"out",g_nodes[i].id,dp);
                            snprintf(g_lines[g_nlines],CW,"%s",ln); g_nlines++;
                            snprintf(g_links[g_links_n].sp,12,"%s",exec_src?"next":"out");
                            snprintf(g_links[g_links_n].dp,12,"%s",dp);
                            g_links[g_links_n].s=g_wire; g_links[g_links_n].d=g_nodes[i].id; g_links_n++;
                            setlog("link added — Save to persist",1);
                            break;
                        }
                    }
                    g_wire=-1;
                }
                g_drag=-1;
            }
            if(ev.type==SDL_MOUSEWHEEL){
                g_pany+=ev.wheel.y*24;
            }
        }
        scene_input();
        update_scene();
        draw_ui();
        if(g_tab==0) draw_graph(); else if(g_tab==1) draw_scene(); else if(g_tab==2) draw_sequence(); else draw_assets();
        if(g_wire>=0) line(g_wx,g_wy,g_wx+40,g_wy,(SDL_Color){200,220,255,255});
        SDL_RenderPresent(g_ren);
        SDL_Delay(16);
    }
    project_save();
    SDL_DestroyRenderer(g_ren); SDL_DestroyWindow(g_win); SDL_Quit();
    return 0;
}
