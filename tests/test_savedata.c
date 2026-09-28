/* Exercise the real HLE API on a disposable card. No ROM or player data. */
#include "psprecomp/hle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#include <utime.h>
#endif
static const uint32_t p=0x08810000, list=0x08811000, data=0x08812000;
static psp_savedata_view view;
static char root[256];
static uint32_t call(uint32_t nid,uint32_t arg) {
    psp_cpu.r[PSP_REG_A0]=arg; psp_hle_call(nid); return psp_cpu.r[PSP_REG_V0];
}
static void str(uint32_t a,const char *s) {
    do { psp_write8(a++,(unsigned char)*s); } while (*s++);
}
static void setup(unsigned mode,const char *name,const char *value) {
    for (unsigned i=0;i<1536;i++) psp_write8(p+i,0);
    psp_write32(p,1536); psp_write32(p+8,1); psp_write32(p+48,mode);
    str(p+60,"UITEST001"); str(p+76,name); str(p+100,"DATA.BIN");
    psp_write32(p+96,list); str(list,"SLOT00"); str(list+20,"SLOT01"); psp_write8(list+40,0);
    psp_write32(p+116,data); psp_write32(p+120,64); psp_write32(p+124,(uint32_t)strlen(value)+1);
    str(p+128,"Test game"); str(p+256,value); str(p+384,"Pilot name\nAC name\nMission progress");
    psp_write32(p+1480,8); str(data,value);
}
static void start(void) {
    assert(call(0x50C4CD57,p)==0);
    assert(call(0x8874DBE0,0)==1); assert(call(0x8874DBE0,0)==2);
    for (int i=0;i<10;i++) assert(call(0x8874DBE0,0)==2);
    psp_savedata_snapshot(&view);
}
static void update(void) { assert(call(0xD4B95FFB,1)==0); psp_savedata_snapshot(&view); }
static void respond(int action,int index) {
    psp_savedata_snapshot(&view);
    assert(psp_savedata_respond(view.session,view.revision,action,index)); update();
}
static uint32_t finish(void) {
    if (view.active) respond(PSP_SAVEDATA_ACCEPT,0);
    uint32_t result=psp_read32(p+28);
    assert(call(0x8874DBE0,0)==3); assert(call(0x9790B33C,0)==0);
    assert(call(0x8874DBE0,0)==4); assert(call(0x8874DBE0,0)==0);
    return result;
}
static void check_file(const char *name,const char *value) {
    char path[512]; snprintf(path,sizeof path,"%s/ms/PSP/SAVEDATA/UITEST001%s/DATA.BIN",root,name);
    FILE *f=fopen(path,"rb");
    if (!value) { assert(!f); return; }
    assert(f); char content[64]={0}; assert(fread(content,1,sizeof content,f)>0); fclose(f);
    assert(!strcmp(content,value));
}
static void save(const char *name,const char *value) {
    setup(3,name,value); start(); assert(view.stage==PSP_SAVEDATA_CONFIRM);
    respond(PSP_SAVEDATA_ACCEPT,0); assert(view.stage==PSP_SAVEDATA_DONE); assert(finish()==0);
}
static void lifecycle(void) {
    setup(5,"SLOT00","cancelled"); start();
    assert(view.count==2 && view.selected==1); /* LASTEMPTY beats saveName. */
    uint64_t id=view.session,rev=view.revision;
    for (int i=0;i<20;i++) { update(); assert(call(0x8874DBE0,0)==2); }
    check_file("SLOT00",NULL); check_file("SLOT01",NULL);
    assert(call(0x50C4CD57,p)==0x80110001u);
    assert(!psp_savedata_respond(id,rev,PSP_SAVEDATA_SELECT,4));
    respond(PSP_SAVEDATA_SELECT,0);
    assert(!psp_savedata_respond(id,rev,PSP_SAVEDATA_ACCEPT,0));
    respond(PSP_SAVEDATA_CANCEL,0); assert(finish()==1);
    check_file("SLOT00",NULL);
    setup(5,"","shutdown"); start();
    assert(call(0x9790B33C,0)==0); assert(psp_read32(p+28)==1);
    assert(call(0x8874DBE0,0)==4); assert(call(0x8874DBE0,0)==0);
    check_file("SLOT00",NULL);
    assert(!psp_savedata_respond(id,rev,PSP_SAVEDATA_ACCEPT,0));
    /* ACLR does not poll FINISHED after its startup memory-stick query. */
    setup(8,"SLOT00",""); start(); update();
    assert(call(0x8874DBE0,0)==3); assert(call(0x9790B33C,0)==0);
    setup(5,"SLOT00",""); start(); respond(PSP_SAVEDATA_CANCEL,0);
    assert(finish()==1);
}
static void roundtrip(void) {
    setup(5,"SLOT00","first"); start(); respond(PSP_SAVEDATA_SELECT,0);
    respond(PSP_SAVEDATA_ACCEPT,0); assert(view.stage==PSP_SAVEDATA_DONE);
    assert(psp_read8(p+76+5)=='0'); assert(finish()==0);
    setup(5,"SLOT00","second"); start(); assert(view.selected==1);
    respond(PSP_SAVEDATA_ACCEPT,0); assert(finish()==0);
    check_file("SLOT00","first"); check_file("SLOT01","second");
    setup(4,"","sentinel"); start(); assert(view.count==2);
    assert(!strcmp(view.slots[0].title,"first")); assert(strstr(view.slots[0].detail,"Pilot name"));
    respond(PSP_SAVEDATA_SELECT,1); respond(PSP_SAVEDATA_ACCEPT,0);
    assert(psp_read8(data)=='s' && psp_read8(data+1)=='e' && psp_read8(data+2)=='c');
    assert(psp_read8(p+76+5)=='1'); assert(finish()==0);
    setup(5,"","overwrite"); start(); respond(PSP_SAVEDATA_SELECT,0);
    respond(PSP_SAVEDATA_ACCEPT,0); assert(view.stage==PSP_SAVEDATA_CONFIRM);
    check_file("SLOT00","first"); respond(PSP_SAVEDATA_CANCEL,0);
    assert(view.stage==PSP_SAVEDATA_LIST); check_file("SLOT00","first");
    respond(PSP_SAVEDATA_CANCEL,0); assert(finish()==1);
    save("SLOT00","replaced"); check_file("SLOT00","replaced"); check_file("SLOT01","second");
}
static void deletion(void) {
    setup(6,"",""); start(); respond(PSP_SAVEDATA_SELECT,1);
    respond(PSP_SAVEDATA_ACCEPT,0); assert(view.stage==PSP_SAVEDATA_CONFIRM);
    check_file("SLOT01","second"); respond(PSP_SAVEDATA_ACCEPT,0); assert(finish()==0);
    check_file("SLOT00","replaced"); check_file("SLOT01",NULL);
    save("SLOT01","second");
    setup(7,"",""); start(); assert(view.count==2);
    respond(PSP_SAVEDATA_SELECT,0); respond(PSP_SAVEDATA_ACCEPT,0);
    respond(PSP_SAVEDATA_CANCEL,0); respond(PSP_SAVEDATA_CANCEL,0); assert(finish()==1);
    check_file("SLOT00","replaced"); check_file("SLOT01","second");
}
static void errors(void) {
    setup(2,"MISSING",""); start(); assert(view.stage==PSP_SAVEDATA_ERROR);
    assert(finish()==0x80110307u);
    char path[512]; snprintf(path,sizeof path,"%s/ms/PSP/SAVEDATA/UITEST001SLOT01/PARAM.SFO",root);
    FILE *f=fopen(path,"wb"); assert(f); fputs("broken",f); fclose(f);
    setup(4,"",""); start(); assert(view.slots[1].broken);
    respond(PSP_SAVEDATA_SELECT,1); respond(PSP_SAVEDATA_ACCEPT,0);
    assert(view.stage==PSP_SAVEDATA_ERROR); assert(finish()==0x80110306u);
    /* A sibling staging path that is a file forces an actual transaction
     * failure; the previous slot contents must survive. */
    snprintf(path,sizeof path,"%s/ms/PSP/SAVEDATA/.pending-UITEST001SLOT00",root);
    f=fopen(path,"wb"); assert(f); fputs("blocked",f); fclose(f);
    setup(3,"SLOT00","must not replace"); start(); assert(view.stage==PSP_SAVEDATA_ERROR);
    assert(finish()!=0); check_file("SLOT00","replaced"); assert(remove(path)==0);
    /* Simulate termination after moving the old directory aside. */
    char old[512],backup[512];
    snprintf(old,sizeof old,"%s/ms/PSP/SAVEDATA/UITEST001SLOT00",root);
    snprintf(backup,sizeof backup,"%s/ms/PSP/SAVEDATA/.backup-UITEST001SLOT00",root);
    assert(rename(old,backup)==0);
    setup(4,"",""); start(); check_file("SLOT00","replaced");
    respond(PSP_SAVEDATA_CANCEL,0); assert(finish()==1);
    setup(3,"../escape","bad"); assert(call(0x50C4CD57,p)!=0);
    setup(3,"SLOT00","bad"); psp_write32(p,1479); assert(call(0x50C4CD57,p)!=0);
    setup(3,"SLOT00","bad"); psp_write32(p+116,0xDEAD0000); assert(call(0x50C4CD57,p)!=0);
    setup(5,"",""); str(list,"../escape"); start(); assert(view.stage==PSP_SAVEDATA_ERROR); assert(finish()!=0);
    assert(psp_mem_bad_access==0);
}
static void request_safety(void) {
    setup(3,"SLOT00","changed request"); start();
    psp_write32(p+48,9); /* A stale dialog must not become automatic deletion. */
    respond(PSP_SAVEDATA_ACCEPT,0); assert(view.stage==PSP_SAVEDATA_ERROR);
    assert(finish()==0x80110004u); check_file("SLOT00","replaced");
    setup(5,"",""); psp_write32(p+96,0xDEAD0000); start();
    assert(view.stage==PSP_SAVEDATA_ERROR); assert(finish()!=0);
    for (unsigned size=1480;size<=1500;size+=20) {
        setup(1,"VERSION","old ABI"); psp_write32(p,size); psp_write32(p+96,0);
        start(); update(); assert(finish()==0);
    }
    setup(0,"SLOT00",""); psp_write32(p+124,0xFFFFFFFF); /* output field on load */
    start(); update(); assert(finish()==0); assert(psp_read8(data)=='r');
    setup(11,"A?C",""); start(); update(); assert(finish()==0); /* query wildcard */
    assert(psp_mem_bad_access==0);
}
static void headless(void) {
    psp_savedata_set_host(0);
    setup(6,"",""); start(); update(); assert(finish()==1); check_file("SLOT00","replaced");
    setup(0,"SLOT00",""); start(); update(); assert(finish()==0); assert(psp_read8(data)=='r');
    psp_savedata_set_host(1);
}
static void scripts(void) {
    psp_utility_init(); psp_savedata_set_host(0);
    char path[512]; snprintf(path,sizeof path,"%s/dialog.script",root);
    FILE *f=fopen(path,"w"); assert(f);
    fputs("1 4 UITEST001 SLOT00 select\n1 4 UITEST001 SLOT00 accept\n1 4 UITEST001 SLOT00 accept\n",f); fclose(f);
    assert(psp_savedata_set_script(path)==0);
    setup(4,"",""); start(); update(); update(); assert(view.stage==PSP_SAVEDATA_DONE);
    update(); assert(finish()==0); assert(psp_read8(data)=='r');
    setup(6,"",""); start(); update(); assert(finish()==2); check_file("SLOT00","replaced");
    psp_savedata_set_script(NULL); psp_savedata_set_host(1);
}
static void coverage(void) {
    char path[512];
    /* Focus 3 (latest), the value the shipping titles write: the newer of two
     * existing saves is selected regardless of list order. */
#ifndef _WIN32
    snprintf(path,sizeof path,"%s/ms/PSP/SAVEDATA/UITEST001SLOT00",root);
    struct utimbuf older={time(NULL)-600,time(NULL)-600}; assert(utime(path,&older)==0);
#endif
    setup(5,"",""); psp_write32(p+1480,3); start(); assert(view.count==2 && view.selected==1);
    respond(PSP_SAVEDATA_CANCEL,0); assert(finish()==1);
    /* An empty allowed list is a save-family failure, not a delete code. */
    setup(5,"",""); psp_write8(list,0); start(); assert(view.stage==PSP_SAVEDATA_ERROR);
    assert(finish()==0x80110385u);
    /* newData: the game's icon bytes and title for an unused slot, and the
     * request's own PIC1 behind it. Saving there writes both sidecars. */
    const uint32_t nd=0x08813000,icon=0x08814000,title=0x08815000,pic=0x08816000;
    setup(5,"",""); str(list+40,"SLOT02"); psp_write8(list+60,0);
    psp_write32(p+1476,nd); psp_write32(nd,icon); psp_write32(nd+4,8); psp_write32(nd+8,8); psp_write32(nd+16,title);
    str(title,"NEW DATA"); str(icon,"iconpng");
    psp_write32(p+1412,icon); psp_write32(p+1416,8); psp_write32(p+1420,8);
    psp_write32(p+1444,pic); psp_write32(p+1448,16); psp_write32(p+1452,16); str(pic,"pic1 png bytes.");
    start(); assert(view.count==3 && !view.slots[2].exists);
    assert(!strcmp(view.slots[2].title,"NEW DATA"));
    assert(view.new_icon_size==8 && !memcmp(view.new_icon,"iconpng",8));
    assert(view.new_pic1_size==16 && !memcmp(view.new_pic1,"pic1 png bytes.",16));
    respond(PSP_SAVEDATA_SELECT,2); respond(PSP_SAVEDATA_ACCEPT,0); assert(view.stage==PSP_SAVEDATA_DONE);
    assert(finish()==0); check_file("SLOT02","");
    setup(4,"",""); str(list+40,"SLOT02"); psp_write8(list+60,0); start(); assert(view.count==3);
    assert(strstr(view.slots[2].icon_path,"ICON0.PNG") && strstr(view.slots[2].pic1_path,"PIC1.PNG"));
    assert(!view.slots[0].pic1_path[0]);
    respond(PSP_SAVEDATA_CANCEL,0); assert(finish()==1);
    /* ShutdownStart from the confirmation stage must not save. */
    setup(5,"SLOT00","must not land"); start(); respond(PSP_SAVEDATA_SELECT,0);
    respond(PSP_SAVEDATA_ACCEPT,0); assert(view.stage==PSP_SAVEDATA_CONFIRM);
    assert(call(0x9790B33C,0)==0); assert(psp_read32(p+28)==1);
    assert(call(0x8874DBE0,0)==4); assert(call(0x8874DBE0,0)==0);
    check_file("SLOT00","replaced");
    /* A foreign directory with an unrepresentable name is skipped, not fatal. */
    snprintf(path,sizeof path,"%s/ms/PSP/SAVEDATA/FOREIGN_DIRECTORY_WITH_A_VERY_LONG_NAME_INDEED_0123456789",root);
#ifndef _WIN32
    assert(mkdir(path,0700)==0);
#else
    assert(_mkdir(path)==0);
#endif
    setup(5,"",""); start(); assert(view.count==2); respond(PSP_SAVEDATA_CANCEL,0); assert(finish()==1);
    setup(7,"",""); start(); assert(view.count==4); /* SLOT00-02 and VERSION; the foreign name skipped */
    respond(PSP_SAVEDATA_CANCEL,0); assert(finish()==1);
    /* A scripted response naming another slot aborts and writes nothing. */
    psp_utility_init(); psp_savedata_set_host(0);
    snprintf(path,sizeof path,"%s/mismatch.script",root);
    FILE *f=fopen(path,"w"); assert(f); fputs("1 5 UITEST001 SLOT01 accept\n",f); fclose(f);
    assert(psp_savedata_set_script(path)==0);
    setup(5,"SLOT00","mismatch"); start(); update(); assert(finish()==2); check_file("SLOT00","replaced");
    psp_savedata_set_script(NULL); psp_savedata_set_host(1);
}
/* What a PSP on firmware 6.60 does, from tools/hwprobe/saveprobe (run
 * fw660-run1). Each block names the steps it reproduces. */
static void fw660_status(void) {
    /* Status 1, 2, 3 and then 0 at the first poll a frame after
     * ShutdownStart, in every step; 4 only to a poll in the same frame. */
    setup(0,"SLOT00",""); start(); update();
    assert(call(0x8874DBE0,0)==3); assert(call(0x9790B33C,0)==0);
    call(0x984C27E7,0); /* sceDisplayWaitVblankStart */
    assert(call(0x8874DBE0,0)==0);
    assert(psp_read32(p+28)==0);
}
/* A noninteractive request, start to finish. */
static uint32_t run(void) { start(); update(); return finish(); }
static uint32_t le32(const unsigned char *b) {
    return b[0] | (uint32_t)b[1]<<8 | (uint32_t)b[2]<<16 | (uint32_t)b[3]<<24;
}
static long card_file(const char *save,const char *file,unsigned char *out,size_t cap) {
    char path[512]; snprintf(path,sizeof path,"%s/ms/PSP/SAVEDATA/UITEST001%s/%s",root,save,file);
    FILE *f=fopen(path,"rb"); if (!f) return -1;
    long n=(long)fread(out,1,cap,f); fclose(f); return n;
}
static const uint32_t fl=0x08817000, ents=0x08818000;
/* setup() without the save-name list: AUTOSAVE would otherwise move an
 * overwrite to the list's first entry. */
static void hw(unsigned mode,const char *name,const char *value) {
    setup(mode,name,value); psp_write32(p+96,0);
}
/* FILES of save: the three counts, as secure*100 + normal*10 + system. */
static int files_of(const char *save) {
    setup(12,save,"");
    for (unsigned i=0;i<36;i++) psp_write8(fl+i,0);
    psp_write32(fl+0,4); psp_write32(fl+4,4); psp_write32(fl+8,4);
    psp_write32(fl+24,ents); psp_write32(fl+28,ents+320); psp_write32(fl+32,ents+640);
    psp_write32(p+1528,fl);
    assert(run()==0);
    return (int)(psp_read32(fl+12)*100+psp_read32(fl+16)*10+psp_read32(fl+20));
}
static void fw660_sfo(void) {
    /* PARAM.SFO as hardware writes it: 4912 bytes, version 0x101, eight keys
     * sorted by name at 0x94, data at 0x108 (every save step; sfo_model.py). */
    hw(1,"HWSFO","sfo"); psp_write32(p+1408,3); assert(run()==0);
    unsigned char b[8192]; assert(card_file("HWSFO","PARAM.SFO",b,sizeof b)==4912);
    assert(le32(b)==0x46535000u && le32(b+4)==0x101 && le32(b+8)==0x94 && le32(b+12)==0x108 && le32(b+16)==8);
    static const struct { const char *key; unsigned fmt, len, max, off; } want[]={
        {"CATEGORY",0x0204,3,4,0x108},        {"PARENTAL_LEVEL",0x0404,4,4,0x10C},
        {"SAVEDATA_DETAIL",0x0204,36,1024,0x110}, {"SAVEDATA_DIRECTORY",0x0204,15,64,0x510},
        {"SAVEDATA_FILE_LIST",0x0004,3168,3168,0x550}, {"SAVEDATA_PARAMS",0x0004,128,128,0x11B0},
        {"SAVEDATA_TITLE",0x0204,4,128,0x1230}, {"TITLE",0x0204,10,128,0x12B0}};
    for (int i=0;i<8;i++) {
        const unsigned char *e=b+20+i*16;
        assert(!strcmp((const char *)b+0x94+(e[0]|e[1]<<8),want[i].key));
        assert((unsigned)(e[2]|e[3]<<8)==want[i].fmt && le32(e+4)==want[i].len && le32(e+8)==want[i].max);
        assert(0x108+le32(e+12)==want[i].off);
    }
    /* The final directory name, not the transaction's staging name. */
    assert(!strcmp((const char *)b+0x510,"UITEST001HWSFO") && le32(b+0x10C)==3);
    /* An AUTOSAVE lists its data file secure (FILES step 53: secure 1
     * normal 0 system 1) with flags 0x21 at secureVersion 0. */
    assert(!memcmp(b+0x550,"DATA.BIN\0\0\0\0",13) && !b[0x570] && b[0x11B0]==0x21);
    assert(files_of("HWSFO")==101);
    /* secureVersion 1 and 3 write flags 0x01 (A1, A3). */
    hw(1,"HWSFO","v3"); psp_write32(p+1516,3); assert(run()==0);
    assert(card_file("HWSFO","PARAM.SFO",b,sizeof b)==4912 && b[0x11B0]==0x01);
    assert(!memcmp(b+0x550,"DATA.BIN",9) && !b[0x570]);   /* still one entry */
    /* MAKEDATA lists nothing (DPLAIN); MAKEDATASECURE lists the file (DSEC). */
    hw(14,"HWPLAIN","plain"); assert(run()==0);
    assert(card_file("HWPLAIN","PARAM.SFO",b,sizeof b)==4912 && !b[0x550] && b[0x11B0]==0x21);
    assert(files_of("HWPLAIN")==11);
    hw(13,"HWSEC","secure"); assert(run()==0);
    assert(files_of("HWSEC")==101);
    /* WRITEDATA rewrites PARAM.SFO too (DSEC steps 37-38): a plaintext
     * write drops the file from the list, a secure one puts it back. */
    hw(18,"HWSEC","now plain"); str(p+256,"renamed"); assert(run()==0);
    assert(card_file("HWSEC","PARAM.SFO",b,sizeof b)==4912 && !b[0x550]);
    assert(!strcmp((const char *)b+0x1230,"renamed"));
    assert(files_of("HWSEC")==11);
    hw(17,"HWSEC","secure again"); assert(run()==0);
    assert(files_of("HWSEC")==101);
    /* A second secure file joins the list after the first. */
    hw(17,"HWSEC","other"); str(p+100,"OTHER.BIN"); assert(run()==0);
    assert(card_file("HWSEC","PARAM.SFO",b,sizeof b)==4912);
    assert(!strcmp((const char *)b+0x550,"DATA.BIN") && !strcmp((const char *)b+0x570,"OTHER.BIN"));
    assert(files_of("HWSEC")==201);
    /* GETSIZE's entries are the game's input: nothing is written into them. */
    hw(22,"HWSEC",""); psp_write32(p+1532,fl);
    for (unsigned i=0;i<60;i++) psp_write8(fl+i,0);
    psp_write32(fl+0,1); psp_write32(fl+4,1); psp_write32(fl+8,ents); psp_write32(fl+12,ents+24);
    for (unsigned i=0;i<48;i++) psp_write8(ents+i,0xA5);
    assert(run()==0); assert(psp_read32(fl+16)==0x8000u);
    for (unsigned i=0;i<48;i++) assert(psp_read8(ents+i)==0xA5);
}
/* key A = 00..0F, key B = FF..F0 (saveprobe's), 0 = all zero. */
static void key(char which,unsigned version) {
    for (unsigned i=0;i<16;i++)
        psp_write8(p+1500+i,which=='A' ? i : which=='B' ? 0xFF-i : 0);
    psp_write32(p+1516,version);
}
static uint32_t load(const char *save,char which,unsigned version) {
    hw(0,save,""); key(which,version); psp_write32(p+124,0x5A5A); psp_write8(data,0);
    return run();
}
static void fw660_keys(void) {
    /* secureVersion 0 (A0 ... A0LEN1000, steps 5-36) and 2 (A2, 60-63):
     * the right key loads, another is LOAD_DATA_BROKEN, the zero key
     * LOAD_BAD_PARAMS; a refused load leaves dataSize alone. */
    for (unsigned v=0;v<=2;v+=2) {
        hw(1,"HWKEY","keyed"); key('A',v); assert(run()==0);
        assert(load("HWKEY",'A',v)==0 && psp_read32(p+124)==6 && psp_read8(data)=='k');
        assert(load("HWKEY",'B',v)==0x80110306u && psp_read32(p+124)==0x5A5A && !psp_read8(data));
        assert(load("HWKEY",'0',v)==0x80110308u && psp_read32(p+124)==0x5A5A);
    }
    /* secureVersion 1 (A1, 56-59) opens with any key, the zero key too;
     * 3 (A3, 64-67) with any key but the zero one. */
    hw(1,"HWKEY1","v1"); key('A',1); assert(run()==0);
    assert(load("HWKEY1",'B',1)==0 && load("HWKEY1",'0',1)==0 && psp_read32(p+124)==3);
    hw(1,"HWKEY3","v3"); key('A',3); assert(run()==0);
    assert(load("HWKEY3",'B',3)==0 && load("HWKEY3",'0',3)==0x80110308u);
    /* The existence checks come first: a zero key on a missing save is
     * LOAD_NO_DATA (steps 2-4, 47). */
    assert(load("HWNOSUCH",'0',0)==0x80110307u);
    /* The save side is held back (one step, P1), so what saves with the zero
     * key loads with it; another key is still refused. */
    hw(1,"HWZERO","zero"); key('0',0); assert(run()==0);
    assert(load("HWZERO",'0',0)==0 && psp_read8(data)=='z');
    assert(load("HWZERO",'A',0)==0x80110306u);
    /* The rules are for secure files: a MAKEDATA save loads with any key. */
    assert(load("HWPLAIN",'0',0)==0 && load("HWPLAIN",'B',0)==0);
    /* A block without key fields (SDK before 2.00) is not checked. */
    hw(1,"HWSHORT","short"); psp_write32(p,1500); assert(run()==0);
    hw(0,"HWSHORT",""); psp_write32(p,1500); assert(run()==0 && psp_read8(data)=='s');
    assert(load("HWSHORT",'B',0)==0);
}
int main(void) {
#ifndef _WIN32
    snprintf(root,sizeof root,"/tmp/psprecomp-savedata-XXXXXX"); assert(mkdtemp(root));
#else
    snprintf(root,sizeof root,"savedata-test-card"); _mkdir(root);
#endif
    assert(psp_mem_init()==0); psp_cpu_reset(); psp_hle_init(); psp_io_set_root(root); psp_savedata_set_host(1);
    lifecycle(); roundtrip(); deletion(); errors(); request_safety(); headless(); scripts(); coverage();
    fw660_status(); fw660_sfo(); fw660_keys();
    assert(psp_mem_bad_access==0); psp_mem_free();
    printf("savedata: lifecycle, independent slots, writeback, metadata, cancellation, overwrite, deletion, errors, recovery, scripts, focus, new-data artwork and foreign directories passed\n");
    return 0;
}
