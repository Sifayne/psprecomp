/* Host savedata UI: SDL owns input/font rasterization; the GL owner receives
 * a complete copied image. PSP status and all file mutations belong to HLE. */
#include "psprecomp/host/save_dialog.h"
#include "psprecomp/savedata.h"
#include <SDL2/SDL_ttf.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 1024
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#if !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#endif
#include "../../third_party/stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static psp_savedata_view ui;
static SDL_Surface *canvas;
static TTF_Font *f_title, *f_banner, *f_body, *f_small;
static SDL_Texture *texture;
static uint64_t uploaded;
static int initialized,active,armed,focused=1,pending,held_direction,held_side,key_direction,confirm_no;
static Uint64 opened_at;
static uint64_t repeat_at;
/* The icon column slides: the drawn selection chases the real one. */
static double scroll_pos, scroll_target;
static Uint64 scroll_tick;
static int scrolling;
static pthread_mutex_t pixels_lock=PTHREAD_MUTEX_INITIALIZER;
static uint32_t pixels[SAVE_DIALOG_W*SAVE_DIALOG_H];
static uint64_t pixels_revision;
static int pixels_active;
static char init_error[256];

/* ---- decoded images ------------------------------------------------------
 * ICON0 per visible slot and the selected slot's PIC1 are decoded on the SDL
 * thread and cached by session, slot and source, so cursor moves and stage
 * changes never re-read a file they already decoded. */
typedef struct { uint64_t session; int index; char key[1024]; SDL_Surface *s; } image_slot;
enum { ICON_CACHE=8 };
static image_slot icons[ICON_CACHE], pic_file, pic_new, icon_new;
static unsigned icon_next;

static SDL_Surface *decode_png(const unsigned char *bytes,size_t size) {
    if (!bytes || !size) return NULL;
    int w,h,n; unsigned char *rgba=stbi_load_from_memory(bytes,(int)size,&w,&h,&n,4);
    if (!rgba) return NULL;
    SDL_Surface *s=SDL_CreateRGBSurfaceWithFormatFrom(rgba,w,h,32,w*4,SDL_PIXELFORMAT_RGBA32), *out=NULL;
    if (s) { out=SDL_ConvertSurfaceFormat(s,SDL_PIXELFORMAT_RGBA32,0); SDL_FreeSurface(s); }
    stbi_image_free(rgba);
    return out;
}
static SDL_Surface *decode_file(const char *path) {
    unsigned char *bytes=NULL; SDL_Surface *s=NULL;
    FILE *f=fopen(path,"rb");
    if (!f) return NULL;
    if (!fseek(f,0,SEEK_END)) {
        long n=ftell(f);
        if (n>0 && n<=1024*1024 && !fseek(f,0,SEEK_SET)) {
            bytes=malloc((size_t)n);
            if (bytes && fread(bytes,1,(size_t)n,f)==(size_t)n) s=decode_png(bytes,(size_t)n);
        }
    }
    fclose(f); free(bytes);
    return s;
}
static void image_reset(image_slot *e) {
    if (e->s) SDL_FreeSurface(e->s);
    memset(e,0,sizeof *e); e->index=-1;
}
/* The game's own new-data icon, when the request supplied one. */
static SDL_Surface *new_icon_surface(void) {
    if (icon_new.session!=ui.session) {
        image_reset(&icon_new); icon_new.session=ui.session;
        icon_new.s=decode_png(ui.new_icon,ui.new_icon_size);
    }
    return icon_new.s;
}
static SDL_Surface *icon_for(int index) {
    psp_savedata_slot *slot=&ui.slots[index];
    if (!slot->exists) return new_icon_surface();
    if (!slot->icon_path[0]) return NULL;
    for (int i=0;i<ICON_CACHE;i++)
        if (icons[i].session==ui.session && icons[i].index==index && !strcmp(icons[i].key,slot->icon_path))
            return icons[i].s;
    image_slot *e=&icons[icon_next++%ICON_CACHE];
    image_reset(e); e->session=ui.session; e->index=index;
    snprintf(e->key,sizeof e->key,"%s",slot->icon_path);
    e->s=decode_file(slot->icon_path);
    return e->s;
}
static SDL_Surface *backdrop_for_selected(void) {
    if (!ui.count) return NULL;
    psp_savedata_slot *slot=&ui.slots[ui.selected];
    if (slot->exists) {
        if (!slot->pic1_path[0]) return NULL;
        if (pic_file.session!=ui.session || strcmp(pic_file.key,slot->pic1_path)) {
            image_reset(&pic_file); pic_file.session=ui.session;
            snprintf(pic_file.key,sizeof pic_file.key,"%s",slot->pic1_path);
            pic_file.s=decode_file(slot->pic1_path);
        }
        return pic_file.s;
    }
    if (pic_new.session!=ui.session) {
        image_reset(&pic_new); pic_new.session=ui.session;
        pic_new.s=decode_png(ui.new_pic1,ui.new_pic1_size);
    }
    return pic_new.s;
}
static void images_free(void) {
    for (int i=0;i<ICON_CACHE;i++) image_reset(&icons[i]);
    image_reset(&pic_file); image_reset(&pic_new); image_reset(&icon_new);
}

/* ---- soft raster ---------------------------------------------------------
 * The canvas is 2x PSP resolution. Fills, frames and glyphs are drawn per
 * pixel with coverage anti-aliasing and source-over blending, so the dialog
 * scales cleanly in both presentations. Colours are r,g,b,a bytes. */
typedef struct { unsigned char r,g,b,a; } rgba;
static const rgba WHITE={255,255,255,255}, SHADOW={0,0,0,150}, GREY={205,205,208,255};

static inline void px(int x,int y,rgba c,int coverage) {
    int a=(c.a*coverage+127)/255;
    if (a<=0 || x<0 || y<0 || x>=SAVE_DIALOG_W || y>=SAVE_DIALOG_H) return;
    unsigned char *p=(unsigned char *)canvas->pixels+(size_t)y*canvas->pitch+(size_t)x*4;
    if (a>=255) { p[0]=c.r; p[1]=c.g; p[2]=c.b; p[3]=255; return; }
    int ia=255-a;
    p[0]=(unsigned char)((c.r*a+p[0]*ia+127)/255);
    p[1]=(unsigned char)((c.g*a+p[1]*ia+127)/255);
    p[2]=(unsigned char)((c.b*a+p[2]*ia+127)/255);
    p[3]=(unsigned char)(a+(p[3]*ia+127)/255);
}
static void fill(int x,int y,int w,int h,rgba c) {
    for (int j=y;j<y+h;j++) for (int i=x;i<x+w;i++) px(i,j,c,255);
}
static inline int coverage(double d) { /* signed distance -> 0..255, one pixel wide edge */
    double v=0.5-d; return v<=0 ? 0 : v>=1 ? 255 : (int)(v*255.0+0.5);
}
static double box_distance(double px_,double py_,double x,double y,double w,double h,double r) {
    double cx=x+w/2,cy=y+h/2,qx=fabs(px_-cx)-(w/2-r),qy=fabs(py_-cy)-(h/2-r);
    double ox=qx>0?qx:0,oy=qy>0?qy:0,inside=qx>qy?qx:qy;
    return sqrt(ox*ox+oy*oy)+(inside<0?inside:0)-r;
}
static void rounded(int x,int y,int w,int h,int r,rgba c) {
    for (int j=y;j<y+h;j++) for (int i=x;i<x+w;i++)
        px(i,j,c,coverage(box_distance(i+0.5,j+0.5,x,y,w,h,r)));
}
static void rounded_ring(int x,int y,int w,int h,int r,double thick,rgba c) {
    for (int j=y-1;j<=y+h;j++) for (int i=x-1;i<=x+w;i++)
        px(i,j,c,coverage(fabs(box_distance(i+0.5,j+0.5,x,y,w,h,r))-thick/2));
}
static void ring(double cx,double cy,double r,double thick,rgba c) {
    int lo=(int)floor(r+thick)+1;
    for (int j=(int)cy-lo;j<=(int)cy+lo;j++) for (int i=(int)cx-lo;i<=(int)cx+lo;i++) {
        double dx=i+0.5-cx,dy=j+0.5-cy;
        px(i,j,c,coverage(fabs(sqrt(dx*dx+dy*dy)-r)-thick/2));
    }
}
static double segment_distance(double x,double y,double ax,double ay,double bx,double by) {
    double vx=bx-ax,vy=by-ay,t=((x-ax)*vx+(y-ay)*vy)/(vx*vx+vy*vy);
    t=t<0?0:t>1?1:t; double dx=x-(ax+vx*t),dy=y-(ay+vy*t);
    return sqrt(dx*dx+dy*dy);
}
static void cross(double cx,double cy,double half,double thick,rgba c) {
    int lo=(int)ceil(half+thick)+1;
    for (int j=(int)cy-lo;j<=(int)cy+lo;j++) for (int i=(int)cx-lo;i<=(int)cx+lo;i++) {
        double x=i+0.5,y=j+0.5;
        double d1=segment_distance(x,y,cx-half,cy-half,cx+half,cy+half);
        double d2=segment_distance(x,y,cx-half,cy+half,cx+half,cy-half);
        px(i,j,c,coverage((d1<d2?d1:d2)-thick/2));
    }
}
/* Bilinear resample of an RGBA32 surface into a destination box, letterboxed
 * to the source aspect and centred. Nearest blits would leave 144x80 icons
 * uneven at the small size. */
static void blit_fit(SDL_Surface *s,int x,int y,int w,int h) {
    if (!s || s->w<=0 || s->h<=0) return;
    double scale=(double)w/s->w; if (scale*s->h>h) scale=(double)h/s->h;
    int dw=(int)(s->w*scale+0.5),dh=(int)(s->h*scale+0.5);
    if (dw<1 || dh<1) return;
    int dx=x+(w-dw)/2,dy=y+(h-dh)/2;
    const unsigned char *src=s->pixels; int pitch=s->pitch;
    if (dw==2*s->w && dh==2*s->h) {
        /* PIC1 is authored at PSP size: an exact 2x copy, cheap enough per frame. */
        for (int j=0;j<dh;j++) {
            const unsigned char *row=src+(size_t)(j/2)*pitch;
            for (int i=0;i<dw;i++) { const unsigned char *p=row+(size_t)(i/2)*4; px(dx+i,dy+j,(rgba){p[0],p[1],p[2],p[3]},255); }
        }
        return;
    }
    for (int j=0;j<dh;j++) {
        double sy=(j+0.5)/scale-0.5; int y0=(int)floor(sy); double fy=sy-y0;
        int y1=y0+1; if (y0<0) y0=0; if (y1>s->h-1) y1=s->h-1; if (y0>s->h-1) y0=s->h-1;
        for (int i=0;i<dw;i++) {
            double sx=(i+0.5)/scale-0.5; int x0=(int)floor(sx); double fx=sx-x0;
            int x1=x0+1; if (x0<0) x0=0; if (x1>s->w-1) x1=s->w-1; if (x0>s->w-1) x0=s->w-1;
            const unsigned char *a=src+(size_t)y0*pitch+(size_t)x0*4,*b=src+(size_t)y0*pitch+(size_t)x1*4;
            const unsigned char *c=src+(size_t)y1*pitch+(size_t)x0*4,*d=src+(size_t)y1*pitch+(size_t)x1*4;
            rgba o;
            unsigned char *out=&o.r;
            for (int k=0;k<4;k++)
                out[k]=(unsigned char)((a[k]*(1-fx)+b[k]*fx)*(1-fy)+(c[k]*(1-fx)+d[k]*fx)*fy+0.5);
            px(dx+i,dy+j,o,255);
        }
    }
}

/* ---- text -----------------------------------------------------------------
 * White type with a one-PSP-pixel dark shadow, as the utility draws it over
 * the save's own PIC1 artwork. */
static const char *fold(const char *text,char *display,size_t cap,TTF_Font *font) {
    /* Some English PSP saves use full-width punctuation (e.g. 0％). Keep
     * their metadata intact, but use the ASCII equivalent when the bundled
     * font lacks that compatibility glyph. Other UTF-8 bytes stay unchanged. */
    size_t in=0,out=0;
    while (text[in] && out+1<cap) {
        const unsigned char *p=(const unsigned char *)text+in;
        if (p[0]==0xEF && p[1] && p[2] && (p[1]==0xBC || p[1]==0xBD) &&
            (p[2]&0xC0)==0x80) {
            unsigned cp=((p[0]&15u)<<12)|((p[1]&63u)<<6)|(p[2]&63u);
            if (cp>=0xFF01 && cp<=0xFF5E && !TTF_GlyphIsProvided(font,(Uint16)cp)) {
                display[out++]=(char)(cp-0xFEE0); in+=3; continue;
            }
        }
        display[out++]=text[in++];
    }
    display[out]=0;
    return display;
}
enum { ALIGN_LEFT, ALIGN_CENTER };
static SDL_Surface *render_text(TTF_Font *font,const char *text,int wrap,int align,SDL_Color color) {
    char display[2048]; fold(text,display,sizeof display,font);
#if SDL_TTF_VERSION_ATLEAST(2,20,0)
    TTF_SetFontWrappedAlign(font,align==ALIGN_CENTER?TTF_WRAPPED_ALIGN_CENTER:TTF_WRAPPED_ALIGN_LEFT);
#else
    (void)align;
#endif
    return TTF_RenderUTF8_Blended_Wrapped(font,display,color,(Uint32)(wrap>0?wrap:0));
}
/* Draw text at x,y (x is the centre when align is ALIGN_CENTER), wrapped to
 * `wrap` pixels (0 = single line), clipped to `maxh`. Returns the height. */
static int text(TTF_Font *font,int x,int y,int wrap,int maxh,const char *str,rgba c,int align) {
    if (!str || !*str || !font) return 0;
    SDL_Color face={c.r,c.g,c.b,255},dark={0,0,0,255};
    SDL_Surface *s=render_text(font,str,wrap,align,face);
    if (!s) return 0;
    SDL_Surface *sh=render_text(font,str,wrap,align,dark);
    int h=s->h<maxh?s->h:maxh, left=align==ALIGN_CENTER ? x-s->w/2 : x;
    SDL_Rect src={0,0,s->w,h};
    if (sh) {
        SDL_SetSurfaceAlphaMod(sh,SHADOW.a);
        SDL_Rect dst={left+2,y+2,s->w,h}; SDL_BlitSurface(sh,&src,canvas,&dst); SDL_FreeSurface(sh);
    }
    SDL_SetSurfaceAlphaMod(s,c.a);
    SDL_Rect dst={left,y,s->w,h}; SDL_BlitSurface(s,&src,canvas,&dst);
    SDL_FreeSurface(s);
    return h;
}
static int text_width(TTF_Font *font,const char *str) {
    char display[2048]; int w=0,h=0;
    TTF_SizeUTF8(font,fold(str,display,sizeof display,font),&w,&h);
    return w;
}
/* One line in the largest of the given fonts that fits `wrap`. */
static int text_fit(TTF_Font *const *fonts,int n,int x,int y,int wrap,const char *str,rgba c) {
    TTF_Font *font=fonts[n-1];
    for (int i=0;i<n;i++) if (text_width(fonts[i],str)<=wrap) { font=fonts[i]; break; }
    return text(font,x,y,0,TTF_FontLineSkip(font)+4,str,c,ALIGN_LEFT);
}

/* ---- layout ---------------------------------------------------------------
 * Geometry follows real-hardware captures of the utility (Load screens of two
 * titles), in PSP pixels doubled: the selected save's PIC1 fills the screen
 * at full brightness; a column of small ICON0s stacks above and below the
 * framed native-size selected icon on the left; the game title, date line,
 * save title and detail text sit in a column at x=178; the operation name
 * top-left; Cross then Circle prompts centred at the bottom. The game
 * supplies every image and string; the host draws only the chrome. */
enum {
    COL_X=356, COL_W=584,
    SEL_X=54, SEL_Y=194, SEL_W=288, SEL_H=160, FRAME=4,
    SMALL_X=96, SMALL_W=162, SMALL_H=90, SMALL_STRIDE=90,
    BOX_X=356, BOX_Y=240, BOX_W=584, BOX_H=200, BOX_R=12,
    HINT_Y=504, BACKDROP_DIM=0
};
static const char *operation(void) {
    if (ui.mode==3 || ui.mode==5) return "Save";
    if (ui.mode==2 || ui.mode==4) return "Load";
    return "Delete";
}
/* `1/9/2015  7:40 PM  544 KB`: the utility's date line for an existing save. */
static void date_line(const psp_savedata_slot *slot,char *out,size_t size) {
    time_t stamp=(time_t)slot->modified; struct tm tm;
    unsigned long long kb=(unsigned long long)((slot->bytes+1023)/1024);
    if (!slot->modified || !localtime_r(&stamp,&tm)) { snprintf(out,size,"%llu KB",kb); return; }
    int hour=tm.tm_hour%12; if (!hour) hour=12;
    snprintf(out,size,"%d/%d/%d  %d:%02d %s  %llu KB",tm.tm_mon+1,tm.tm_mday,tm.tm_year+1900,
             hour,tm.tm_min,tm.tm_hour<12?"AM":"PM",kb);
}
static void backdrop(void) {
    SDL_Surface *pic=backdrop_for_selected();
    if (pic) blit_fit(pic,0,0,SAVE_DIALOG_W,SAVE_DIALOG_H);
    else for (int y=0;y<SAVE_DIALOG_H;y++) {
        int t=y*255/(SAVE_DIALOG_H-1);
        rgba c={(unsigned char)(16+(36-16)*t/255),(unsigned char)(16+(36-16)*t/255),(unsigned char)(20+(42-20)*t/255),255};
        fill(0,y,SAVE_DIALOG_W,1,c);
    }
    if (BACKDROP_DIM) { rgba dim={0,0,0,(unsigned char)BACKDROP_DIM}; fill(0,0,SAVE_DIALOG_W,SAVE_DIALOG_H,dim); }
}
static void memory_stick(int x,int y) {
    rgba dark={0,0,0,110};
    rounded(x+2,y+2,18,24,3,dark);
    rounded(x,y,18,24,3,WHITE);
    fill(x+4,y+16,10,3,dark);
    fill(x+4,y+5,10,7,(rgba){0,0,0,70});
}
/* An icon box: the game's ICON0, its new-data icon, or a plain placeholder. */
static void icon_box(int index,int x,int y,int w,int h) {
    SDL_Surface *s=ui.slots[index].broken ? NULL : icon_for(index);
    rgba plate={44,44,48,255},edge={85,85,90,255};
    fill(x,y,w,h,plate);
    if (s) blit_fit(s,x,y,w,h);
    else {
        rounded_ring(x,y,w,h,1,2,edge);
        if (ui.slots[index].broken) cross(x+w/2.0,y+h/2.0,h/5.0,2,(rgba){150,150,155,255});
    }
}
/* The slot an entry occupies at integer distance r from the selection. */
static void slot_rect(int r,int *o) {
    if (r==0) { o[0]=SEL_X; o[1]=SEL_Y; o[2]=SEL_W; o[3]=SEL_H; }
    else if (r<0) { o[0]=SMALL_X; o[1]=SEL_Y-6-SMALL_STRIDE*(-r); o[2]=SMALL_W; o[3]=SMALL_H; }
    else { o[0]=SMALL_X; o[1]=SEL_Y+SEL_H+6+SMALL_STRIDE*(r-1); o[2]=SMALL_W; o[3]=SMALL_H; }
}
/* Ease the drawn position toward the selection: about 60 ms to cover most of
 * a step, so held repeats (110 ms apart) chain into one continuous slide. */
static void advance_scroll(void) {
    Uint64 now=SDL_GetTicks64();
    double d=scroll_target-scroll_pos;
    if (fabs(d)<0.004) { scroll_pos=scroll_target; scrolling=0; return; }
    double dt=scrolling ? (double)(now-scroll_tick)/1000.0 : 0.0;
    scroll_tick=now; scrolling=1;
    scroll_pos+=d*(1.0-exp(-dt/0.06));
}
static void icon_column(void) {
    advance_scroll();
    int lo=(int)floor(scroll_pos)-3,hi=(int)ceil(scroll_pos)+3;
    /* Farthest entries first; the selected one last, with its frame riding
     * along as it slides and grows into the big slot. */
    int box[4]={0,0,0,0};
    for (int ring=3;ring>=0;ring--) for (int i=lo;i<=hi;i++) {
        if (i<0 || i>=ui.count) continue;
        double r=i-scroll_pos; if ((int)floor(fabs(r)+0.5)!=ring) continue;
        double fl=floor(r),fr=r-fl; int a[4],b[4];
        slot_rect((int)fl,a); slot_rect((int)fl+1,b);
        for (int k=0;k<4;k++) box[k]=(int)lrint(a[k]+(b[k]-a[k])*fr);
        if (i!=ui.selected) icon_box(i,box[0],box[1],box[2],box[3]);
    }
    {
        double r=ui.selected-scroll_pos,fl=floor(r),fr=r-fl; int a[4],b[4];
        slot_rect((int)fl,a); slot_rect((int)fl+1,b);
        for (int k=0;k<4;k++) box[k]=(int)lrint(a[k]+(b[k]-a[k])*fr);
    }
    rgba frame={255,255,255,230};
    fill(box[0]-FRAME,box[1]-FRAME,box[2]+2*FRAME,FRAME,frame);
    fill(box[0]-FRAME,box[1]+box[3],box[2]+2*FRAME,FRAME,frame);
    fill(box[0]-FRAME,box[1],FRAME,box[3],frame);
    fill(box[0]+box[2],box[1],FRAME,box[3],frame);
    icon_box(ui.selected,box[0],box[1],box[2],box[3]);
}
static void info_column(void) {
    psp_savedata_slot *slot=&ui.slots[ui.selected];
    TTF_Font *const fonts[]={f_title,f_banner,f_body};
    int y=236;
    if (ui.title[0]) text_fit(fonts,3,COL_X,y,COL_W,ui.title,WHITE);
    if (slot->exists) { char line[128]; date_line(slot,line,sizeof line); text(f_small,COL_X,282,0,40,line,WHITE,ALIGN_LEFT); }
    text(f_body,COL_X,326,COL_W,40,slot->broken?"Corrupted Data":slot->title,WHITE,ALIGN_LEFT);
    if (slot->exists && !slot->broken) text(f_small,COL_X,368,COL_W,4*TTF_FontLineSkip(f_small),slot->detail,WHITE,ALIGN_LEFT);
}
static void hints(void) {
    const char *left=ui.confirm_circle?"Back":"Enter",*right=ui.confirm_circle?"Enter":"Back";
    int glyph=22,gap=10,space=48;
    int total=glyph+gap+text_width(f_small,left)+space+glyph+gap+text_width(f_small,right);
    int x=(SAVE_DIALOG_W-total)/2,cy=HINT_Y+TTF_FontLineSkip(f_small)/2;
    cross(x+2+glyph/2.0,cy+2,7.5,3,SHADOW); cross(x+glyph/2.0,cy,7.5,3,WHITE);
    x+=glyph+gap; x+=text(f_small,x,HINT_Y,0,40,left,WHITE,ALIGN_LEFT) ? text_width(f_small,left) : 0;
    x+=space;
    ring(x+2+glyph/2.0,cy+2,8,2.5,SHADOW); ring(x+glyph/2.0,cy,8,2.5,WHITE);
    x+=glyph+gap; text(f_small,x,HINT_Y,0,40,right,WHITE,ALIGN_LEFT);
}
static void message_box(void) {
    rgba plate={0,0,0,158},edge={255,255,255,90};
    rounded(BOX_X,BOX_Y,BOX_W,BOX_H,BOX_R,plate);
    rounded_ring(BOX_X,BOX_Y,BOX_W,BOX_H,BOX_R,2,edge);
    SDL_Surface *m=render_text(f_body,ui.message,BOX_W-48,ALIGN_CENTER,(SDL_Color){255,255,255,255});
    int mh=m?m->h:0; if (m) SDL_FreeSurface(m);
    int top=ui.stage==PSP_SAVEDATA_CONFIRM ? BOX_Y+70-mh/2 : BOX_Y+BOX_H/2-mh/2-(ui.result?16:0);
    if (top<BOX_Y+12) top=BOX_Y+12;
    text(f_body,BOX_X+BOX_W/2,top,BOX_W-48,BOX_H-40,ui.message,WHITE,ALIGN_CENTER);
    if (ui.stage==PSP_SAVEDATA_CONFIRM) {
        const char *options[2]={"Yes","No"}; int cx[2]={BOX_X+BOX_W/2-60,BOX_X+BOX_W/2+60},y=BOX_Y+138;
        for (int i=0;i<2;i++) {
            int w=text_width(f_body,options[i]),h=TTF_FontLineSkip(f_body);
            if (i==confirm_no) rounded(cx[i]-w/2-22,y-5,w+44,h+10,10,(rgba){255,255,255,64});
            text(f_body,cx[i],y,0,h+4,options[i],WHITE,ALIGN_CENTER);
        }
    } else if (ui.result) {
        char code[32]; snprintf(code,sizeof code,"(%08X)",ui.result);
        text(f_small,BOX_X+BOX_W/2,top+mh+8,0,40,code,GREY,ALIGN_CENTER);
    }
}
static void paint(void) {
    backdrop();
    if (ui.count && ui.selected>=0 && ui.selected<ui.count) {
        icon_column();
        if (ui.stage==PSP_SAVEDATA_LIST) info_column();
    }
    /* The operation name sits over the icon stack, as the utility draws it. */
    memory_stick(28,12);
    text(f_banner,60,8,0,48,operation(),WHITE,ALIGN_LEFT);
    if (ui.stage!=PSP_SAVEDATA_LIST) message_box();
    hints();
    pthread_mutex_lock(&pixels_lock);
    for (int y=0;y<SAVE_DIALOG_H;y++) memcpy(pixels+(size_t)y*SAVE_DIALOG_W,
        (unsigned char *)canvas->pixels+(size_t)y*canvas->pitch,SAVE_DIALOG_W*4);
    pixels_active=1; pixels_revision++;
    pthread_mutex_unlock(&pixels_lock);
}
int save_dialog_init(void) {
    if (initialized) return 0;
    memset(&ui,0,sizeof ui);
    armed=pending=held_direction=held_side=key_direction=confirm_no=0; focused=1;
    init_error[0]=0;
    if (TTF_Init()) { snprintf(init_error,sizeof init_error,"SDL_ttf: %s",TTF_GetError()); return -1; }
    const char *requested=getenv("PSPRECOMP_UI_FONT");
    const char *paths[]={requested,"/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf","/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "C:/Windows/Fonts/segoeui.ttf",NULL};
    const char *path=NULL;
    for (unsigned i=0;i<sizeof paths/sizeof paths[0];i++) {
        if (!paths[i]) continue;
        f_body=TTF_OpenFont(paths[i],26);
        if (f_body) { path=paths[i]; break; }
        if (i==0 && requested && *requested) break;
    }
    if (path) { f_small=TTF_OpenFont(path,24); f_banner=TTF_OpenFont(path,28); f_title=TTF_OpenFont(path,32); }
    canvas=SDL_CreateRGBSurfaceWithFormat(0,SAVE_DIALOG_W,SAVE_DIALOG_H,32,SDL_PIXELFORMAT_RGBA32);
    if (!f_body || !f_small || !f_banner || !f_title || !canvas) {
        snprintf(init_error,sizeof init_error,"font/overlay unavailable (%s): %s",
                 requested && *requested ? requested : "no usable system font",TTF_GetError());
        fprintf(stderr,"savedata UI: %s\n",init_error);
        save_dialog_shutdown(); return -1;
    }
    initialized=1; psp_savedata_set_host(1); return 0;
}
const char *save_dialog_error(void) { return init_error[0] ? init_error : NULL; }
void save_dialog_shutdown(void) {
    psp_savedata_set_host(0);
    if (texture) SDL_DestroyTexture(texture);
    images_free();
    if (canvas) SDL_FreeSurface(canvas);
    if (f_body) TTF_CloseFont(f_body);
    if (f_small) TTF_CloseFont(f_small);
    if (f_banner) TTF_CloseFont(f_banner);
    if (f_title) TTF_CloseFont(f_title);
    texture=NULL; canvas=NULL; f_body=f_small=f_banner=f_title=NULL;
    initialized=active=0; uploaded=0;
    pthread_mutex_lock(&pixels_lock); pixels_active=0; pixels_revision++; pthread_mutex_unlock(&pixels_lock);
    TTF_Quit();
}
int save_dialog_active(void) { return active; }
/* Nothing the dialog reads is held: keys, mouse buttons and pad buttons.
 * Stick rest positions are deliberately not part of this -- a worn stick
 * must never keep the game deaf after the dialog closes. */
int save_dialog_input_neutral(SDL_GameController *pad) {
    int n; const Uint8 *keys=SDL_GetKeyboardState(&n);
    for (int i=0;i<n;i++) if (keys[i]) return 0;
    if (SDL_GetMouseState(NULL,NULL)) return 0;
    if (pad) for (int i=0;i<SDL_CONTROLLER_BUTTON_MAX;i++)
        if (SDL_GameControllerGetButton(pad,(SDL_GameControllerButton)i)) return 0;
    return 1;
}
static void send(int action,int index) {
    if (pending || !armed || !focused) return;
    pending=psp_savedata_respond(ui.session,ui.revision,action,index);
}
static void move(int delta) {
    if (ui.stage!=PSP_SAVEDATA_LIST || !ui.count) return;
    int next=ui.selected+delta;
    if (next<0) next=0;
    if (next>=ui.count) next=ui.count-1;
    if (next!=ui.selected) send(PSP_SAVEDATA_SELECT,next);
}
static void choose(int no) {
    if (ui.stage!=PSP_SAVEDATA_CONFIRM || !armed || !focused || no==confirm_no) return;
    confirm_no=no; paint();
}
static void accept(void) {
    send(ui.stage==PSP_SAVEDATA_CONFIRM && confirm_no ? PSP_SAVEDATA_CANCEL : PSP_SAVEDATA_ACCEPT,ui.selected);
}
void save_dialog_update(SDL_GameController *pad) {
    if (!initialized) return;
    uint64_t session=ui.session;
    int state=psp_savedata_snapshot(&ui),was=active;
    active=state!=0;
    if (!active) {
        if (was) { pthread_mutex_lock(&pixels_lock); pixels_active=0; pixels_revision++; pthread_mutex_unlock(&pixels_lock); }
        return;
    }
    if (!was || ui.session!=session) {
        armed=0; pending=0; held_direction=held_side=key_direction=0;
        scroll_pos=scroll_target=ui.selected; scrolling=0;
        opened_at=SDL_GetTicks64();
    }
    /* Arm once nothing is held, so the press that opened the dialog cannot
     * act on it -- or after a moment regardless. Accept and cancel act on
     * press edges, which a button held since before the dialog cannot
     * produce, so the delay risks only a held stick scrolling; without it a
     * pad that keeps reporting something (a thumb resting on a Steam
     * Controller's trackpad reads as a mouse button) never arms and the
     * dialog ignores every press, which is how the first live test went. */
    if (!armed && focused && (save_dialog_input_neutral(pad) || SDL_GetTicks64()-opened_at>=300)) armed=1;
    if (state==2) {
        pending=0; confirm_no=0; scroll_target=ui.selected;
        if (ui.stage!=PSP_SAVEDATA_LIST) { scroll_pos=scroll_target; scrolling=0; }
        paint();
    } else if (scrolling) paint();
    /* Stick repeats are host time; they never advance guest pad polls. */
    int direction=0,side=0;
    if (pad && focused && armed) {
        int y=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_LEFTY),x=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_LEFTX);
        direction=y>16000 ? 1 : y< -16000 ? -1 : 0;
        side=x>16000 ? 1 : x< -16000 ? -1 : 0;
        if (SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_UP)) direction=-1;
        if (SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_DOWN)) direction=1;
        if (SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_LEFT)) side=-1;
        if (SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) side=1;
    }
    if (!direction && focused && armed) direction=key_direction;
    Uint64 now=SDL_GetTicks64();
    if (direction && direction!=held_direction) { move(direction); repeat_at=now+350; }
    else if (direction && now>=repeat_at) { move(direction); repeat_at=now+110; }
    held_direction=direction;
    if (side && side!=held_side) choose(side>0);
    held_side=side;
}
int save_dialog_event(const SDL_Event *e,SDL_JoystickID controller) {
    if (e->type==SDL_WINDOWEVENT) {
        if (e->window.event==SDL_WINDOWEVENT_FOCUS_LOST) { focused=0; armed=0; key_direction=0; }
        if (e->window.event==SDL_WINDOWEVENT_FOCUS_GAINED) { focused=1; armed=0; }
    }
    if (e->type==SDL_CONTROLLERDEVICEREMOVED) { armed=0; held_direction=held_side=0; }
    if (!active) return 0;
    if (e->type==SDL_KEYDOWN || e->type==SDL_KEYUP) {
        /* Movement keys hold a direction that save_dialog_update repeats on the
         * pad's clock; the desktop's own key repeat is ignored. */
        SDL_Keycode k=e->key.keysym.sym;
        int dir=(k==SDLK_UP || k==SDLK_w) ? -1 : (k==SDLK_DOWN || k==SDLK_s) ? 1 : 0;
        if (dir) {
            /* The press edge moves at once; a hold repeats from save_dialog_update
             * on the pad's clock, and the desktop's own key repeat is ignored. */
            if (e->type==SDL_KEYDOWN && !e->key.repeat) {
                move(dir); key_direction=held_direction=dir; repeat_at=SDL_GetTicks64()+350;
            } else if (e->type==SDL_KEYUP && key_direction==dir) key_direction=0;
        }
        else if (e->type==SDL_KEYDOWN && !e->key.repeat) {
            int accept_key=k==SDLK_RETURN || k==SDLK_KP_ENTER ||
                (ui.confirm_circle ? k==SDLK_x : (k==SDLK_z || k==SDLK_SPACE));
            int cancel=k==SDLK_ESCAPE || k==SDLK_BACKSPACE ||
                (ui.confirm_circle ? (k==SDLK_z || k==SDLK_SPACE) : k==SDLK_x);
            if (k==SDLK_LEFT || k==SDLK_a) choose(0);
            else if (k==SDLK_RIGHT || k==SDLK_d) choose(1);
            else if (accept_key) accept();
            else if (cancel) send(PSP_SAVEDATA_CANCEL,ui.selected);
        }
    } else if (e->type==SDL_CONTROLLERBUTTONDOWN && e->cbutton.which==controller) {
        int b=e->cbutton.button;
        if (b==(ui.confirm_circle?SDL_CONTROLLER_BUTTON_B:SDL_CONTROLLER_BUTTON_A)) accept();
        else if (b==(ui.confirm_circle?SDL_CONTROLLER_BUTTON_A:SDL_CONTROLLER_BUTTON_B)) send(PSP_SAVEDATA_CANCEL,ui.selected);
    }
    return e->type==SDL_KEYDOWN || e->type==SDL_KEYUP || e->type==SDL_MOUSEMOTION ||
        e->type==SDL_MOUSEBUTTONDOWN || e->type==SDL_MOUSEBUTTONUP ||
        e->type==SDL_CONTROLLERBUTTONDOWN || e->type==SDL_CONTROLLERBUTTONUP || e->type==SDL_CONTROLLERAXISMOTION;
}
void save_dialog_draw_software(SDL_Renderer *renderer) {
    if (!active || !canvas) return;
    if (!texture) {
        texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STREAMING,SAVE_DIALOG_W,SAVE_DIALOG_H);
        if (texture) SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
        uploaded=0;
    }
    if (texture) {
        if (uploaded!=pixels_revision) { SDL_UpdateTexture(texture,NULL,canvas->pixels,canvas->pitch); uploaded=pixels_revision; }
        SDL_RenderCopy(renderer,texture,NULL,NULL);
    }
}
int save_dialog_copy_pixels(uint32_t *out,uint64_t *revision) {
    pthread_mutex_lock(&pixels_lock);
    int on=pixels_active;
    if (on && *revision!=pixels_revision) {
        memcpy(out,pixels,sizeof pixels); *revision=pixels_revision;
    }
    pthread_mutex_unlock(&pixels_lock);
    return on;
}
int save_dialog_capture(const char *path) { return canvas ? SDL_SaveBMP(canvas,path) : -1; }
