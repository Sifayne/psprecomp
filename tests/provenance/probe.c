typedef unsigned int u32;
extern int sceIoWrite(int fd, const void *data, unsigned int count);
extern void sceKernelExitGame(void);
extern void rng_seed(u32 value);
extern void rng_seed_prefixed(u32 value,u32 prefix);
extern void rng_read_controls(u32 *values);
extern u32 rng_next(void);
extern void rng_read(u32 *state);
extern void rng_write(const u32 *state);
extern u32 rng_revision(void);

extern void rng_draw_0_0(u32 prefix,u32 *values);
extern void rng_draw_0_1(u32 prefix,u32 *values);
extern void rng_draw_0_2(u32 prefix,u32 *values);
extern void rng_draw_0_3(u32 prefix,u32 *values);
extern void rng_draw_1_0(u32 prefix,u32 *values);
extern void rng_draw_1_1(u32 prefix,u32 *values);
extern void rng_draw_1_2(u32 prefix,u32 *values);
extern void rng_draw_1_3(u32 prefix,u32 *values);
extern void rng_draw_2_0(u32 prefix,u32 *values);
extern void rng_draw_2_1(u32 prefix,u32 *values);
extern void rng_draw_2_2(u32 prefix,u32 *values);
extern void rng_draw_2_3(u32 prefix,u32 *values);
static void (*const draw[])(u32,u32*) = {rng_draw_0_0, rng_draw_0_1, rng_draw_0_2, rng_draw_0_3, rng_draw_1_0, rng_draw_1_1, rng_draw_1_2, rng_draw_1_3, rng_draw_2_0, rng_draw_2_1, rng_draw_2_2, rng_draw_2_3};

static char line[256];
static unsigned used;
static void hex(u32 v) {
    static const char digits[] = "0123456789abcdef";
    for (int s = 28; s >= 0; s -= 4) line[used++] = digits[(v >> s) & 15];
    line[used++] = ' ';
}
static void output(u32 kind, u32 id, const u32 *before, u32 value, const u32 *after) {
    used = 0; hex(kind); hex(id);
    for (unsigned i=0;i<8;i++) hex(before[i]);
    hex(value);
    for (unsigned i=0;i<8;i++) hex(after[i]);
    line[used++] = '\n'; sceIoWrite(1,line,used);
}
int module_start(unsigned args, void *argp) {
    (void)args; (void)argp;
    const u32 seeds[] = {0, 1, 0x12345678, 0xffffffff, 0x80000000};
    u32 before[8], after[8], injected[8];
    rng_read(before);
    u32 initial_value = rng_next();
    rng_read(after);
    output(11,rng_revision(),before,initial_value,after);
    for (unsigned seed=0;seed<5;seed++) {
        rng_seed(seeds[seed]);
        for (unsigned step=0;step<12;step++) {
            rng_read(before);
            u32 value = rng_next();
            rng_read(after);
            output(seed,step,before,value,after);
        }
    }
    for (unsigned bit=0;bit<256;bit++) {
        for (unsigned i=0;i<8;i++) injected[i]=0;
        injected[bit/32]=1u<<(bit%32);
        rng_write(injected);
        rng_read(before);
        u32 value = rng_next();
        rng_read(after);
        output(5,bit,before,value,after);
    }
    u32 input = 0x5192026;
    for (unsigned test=0;test<4096;test++) {
        for (unsigned i=0;i<8;i++) {
            input = input*1664525u + 1013904223u;
            injected[i] = input;
        }
        rng_write(injected); rng_read(before);
        u32 value = rng_next(); rng_read(after);
        output(6,test,before,value,after);
    }
    for (unsigned test=0;test<128;test++) {
        input = input*1664525u + 1013904223u;
        rng_seed(input); rng_read(before);
        u32 value = rng_next(); rng_read(after);
        output(7,input,before,value,after);
    }
    /* Threshold interventions distinguish carry models that differ by only
     * a few low bits and are indistinguishable on ordinary random inputs. */
    const u32 edges[] = {0,1,2,3,4,7,0x7fffffff,0x80000000,0xfffffffe,0xffffffff};
    for (unsigned a=0;a<10;a++) for (unsigned b=0;b<10;b++) for (unsigned c=0;c<10;c++) {
        for (unsigned i=0;i<8;i++) injected[i]=((edges[c]>>(4*i))&15)<<16;
        injected[2] |= edges[a]&65535; injected[6] |= edges[a]>>16;
        injected[3] |= edges[b]&65535; injected[7] |= edges[b]>>16;
        rng_write(injected); rng_read(before);
        u32 value=rng_next(); rng_read(after);
        output(8,a*100+b*10+c,before,value,after);
    }
    for (unsigned shape=0;shape<12;shape++) for (unsigned prefix=0;prefix<4096;prefix++) {
        u32 values[4], controls[3];
        rng_seed(0x12345678); rng_read(before);
        draw[shape](prefix,values); rng_read(after); rng_read_controls(controls);
        used=0;hex(9);hex((shape<<12)|prefix);
        for (unsigned i=0;i<8;i++) hex(before[i]);
        for (unsigned i=0;i<4;i++) hex(values[i]);
        for (unsigned i=0;i<8;i++) hex(after[i]);
        for (unsigned i=0;i<3;i++) hex(controls[i]);
        line[used++]='\n';sceIoWrite(1,line,used);
    }
    for (unsigned test=0;test<116;test++) {
        u32 prefix;
        if (test<32) prefix=(test&3)|((test&4)<<6)|((test&8)<<9)|((test&16)<<12);
        else if (test<52) prefix=1u<<(test-32);
        else { input=input*1664525u+1013904223u;prefix=input&0xfffff; }
        rng_seed_prefixed(0xbf400001,prefix); rng_read(before);
        u32 value=rng_next();rng_read(after);
        output(10,prefix,before,value,after);
    }
    sceKernelExitGame();
    return 0;
}
