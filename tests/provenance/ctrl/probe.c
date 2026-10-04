/* Controller sampling observation probe. Project-authored (MIT); it uses
 * only the imports named in imports.json and contains no game data.
 *
 * Question: how many SceCtrlData entries do Read and Peek return, and which
 * timestamps do they carry, when a title calls them with a buffer larger
 * than one sample and a varying number of vblanks between calls? Each
 * experiment pre-fills the buffer with a sentinel, waits K vblanks with
 * sceDisplayWaitVblankStart, makes one call, and records what came back. */
typedef unsigned int u32;
typedef struct { u32 stamp, buttons; unsigned char lx, ly, reserved[6]; } pad;

extern int sceIoWrite(int, const void *, unsigned);
extern void sceKernelExitGame(void);
extern int sceDisplayWaitVblankStart(void);
extern int sceDisplayGetVcount(void);
extern int sceCtrlSetSamplingCycle(int);
extern int sceCtrlSetSamplingMode(int);
extern int sceCtrlReadBufferPositive(pad *, int);
extern int sceCtrlPeekBufferPositive(pad *, int);

enum { SLOTS = 72, READ = 0, PEEK = 1 };
#define SENTINEL 0xa5a5a5a5u

static pad buf[SLOTS];
/* Newest timestamp returned by any earlier call; later stamps are printed
 * relative to it so the record does not depend on absolute boot time. */
static u32 base;

static void fill(void) {
    for (unsigned i = 0; i < SLOTS; i++) {
        unsigned char *bytes = (unsigned char *)&buf[i];
        for (unsigned b = 0; b < sizeof buf[i]; b++) bytes[b] = 0xa5;
    }
}

static void emit(const u32 *values, unsigned count) {
    char text[16 * 9 + 1];
    unsigned used = 0;
    const char *digits = "0123456789abcdef";
    for (unsigned i = 0; i < count; i++) {
        for (int shift = 28; shift >= 0; shift -= 4) text[used++] = digits[(values[i] >> shift) & 15];
        text[used++] = i + 1 < count ? ' ' : '\n';
    }
    sceIoWrite(1, text, used);
}

static u32 relative(unsigned i) {
    return buf[i].stamp == SENTINEL ? 0xffffffffu : buf[i].stamp - base;
}

/* id result waited vblanks-during-call highest-written written
 * t0 t1 t2 t3 t[highest-1] buttons0 (lx0<<8|ly0) */
static void record(u32 id, u32 result, u32 waited, u32 during) {
    u32 highest = 0, written = 0, newest = 0;
    for (unsigned i = 0; i < SLOTS; i++) {
        if (buf[i].stamp == SENTINEL) continue;
        highest = i + 1;
        written++;
        if (!newest || (int)(buf[i].stamp - newest) > 0) newest = buf[i].stamp;
    }
    const u32 values[13] = {id, result, waited, during, highest, written,
                            relative(0), relative(1), relative(2), relative(3),
                            highest ? relative(highest - 1) : 0xffffffffu,
                            buf[0].buttons, ((u32)buf[0].lx << 8) | buf[0].ly};
    emit(values, 13);
    if (written) base = newest;
}

static void call(u32 id, int which, int count, unsigned waits) {
    fill();
    for (unsigned i = 0; i < waits; i++) sceDisplayWaitVblankStart();
    const int before = sceDisplayGetVcount();
    const int result = which == READ ? sceCtrlReadBufferPositive(buf, count)
                                     : sceCtrlPeekBufferPositive(buf, count);
    const int after = sceDisplayGetVcount();
    record(id, (u32)result, waits, (u32)(after - before));
}

int module_start(unsigned args, void *argp) {
    (void)args; (void)argp;
    fill();
    record(1, (u32)sceCtrlSetSamplingCycle(0), 0, 0);
    record(2, (u32)sceCtrlSetSamplingMode(1), 0, 0);

    call(10, READ, 64, 0);   /* Drain whatever accumulated during boot. */
    call(11, READ, 10, 0);   /* Nothing unread: does Read wait, and for what? */

    /* Read after K vblanks with room for ten. */
    const unsigned waits[] = {1, 2, 3, 4, 6, 10};
    for (unsigned i = 0; i < 6; i++) call(20 + waits[i], READ, 10, waits[i]);

    /* Fewer slots than unread samples, and more unread than the history. */
    call(40, READ, 1, 3);
    call(41, READ, 2, 3);
    call(42, READ, 64, 70);
    call(43, READ, 10, 70);

    /* Peek: how many it returns, and whether it consumes unread samples. */
    call(50, PEEK, 1, 0);
    call(51, PEEK, 10, 0);
    call(52, PEEK, 64, 0);
    call(53, PEEK, 65, 0);
    call(54, PEEK, 0, 0);
    call(55, READ, 0, 1);
    call(56, READ, 65, 1);
    call(57, PEEK, 10, 3);
    call(58, READ, 10, 0);

    /* The 30 Hz title pattern: two vblanks, then Read with room for ten. */
    for (unsigned i = 0; i < 8; i++) call(60 + i, READ, 10, 2);

    /* A title that reads every vblank. */
    for (unsigned i = 0; i < 4; i++) call(70 + i, READ, 10, 1);

    fill();
    record(0xff, 0, 0, 0);
    sceKernelExitGame();
    return 0;
}
