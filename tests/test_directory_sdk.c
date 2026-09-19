/* Original host-filesystem tests against BSD PSPSDK declarations. These check
 * the host adapter contract, not emulator-derived firmware error quirks. */
#include "psprecomp/hle.h"
#include "crypto/sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define remove_directory _rmdir
#else
#include <unistd.h>
#define remove_directory rmdir
#endif

static unsigned checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; \
    fprintf(stderr,"line %u: %s\n",__LINE__,#c); } } while (0)
enum { NAME=0x08804000, DATA=0x08805000, ENTRY=0x08806000 };

static uint32_t call(const char *api,uint32_t a,uint32_t b,uint32_t c) {
    psp_cpu.r[PSP_REG_A0]=a;psp_cpu.r[PSP_REG_A1]=b;psp_cpu.r[PSP_REG_A2]=c;
    psp_hle_call(psp_nid(api));return psp_cpu.r[PSP_REG_V0];
}
static uint32_t path_call(const char *api,const char *path,uint32_t b,uint32_t c) {
    psp_mem_write_block(NAME,path,(uint32_t)strlen(path)+1);return call(api,NAME,b,c);
}
static int error(uint32_t result) { return (int32_t)result<0; }

int main(void) {
    if (psp_mem_init()!=0) return 2;
    psp_hle_init();psp_cpu_reset();
    /* CTest gives this process its build directory. Only this named fixture
     * tree is used, and every file is authored below. */
    psp_io_set_root("directory-sdk-fixture");psp_io_reset();
    psp_io_mkdir_all("ms0:/");
    /* A guest mkdir makes the missing parents too (src/hle/iofilemgr.c);
     * what a PSP does is not measured. */
    CHECK(path_call("sceIoMkdir","ms0:/absent/child",0700,0)==0);
    CHECK(path_call("sceIoRmdir","ms0:/absent/child",0,0)==0);
    CHECK(path_call("sceIoRmdir","ms0:/absent",0,0)==0);
    CHECK(path_call("sceIoMkdir","ms0:/sample",0700,0)==0);
    CHECK(error(path_call("sceIoMkdir","ms0:/sample",0700,0)));
    CHECK(path_call("sceIoChdir","ms0:/sample",0,0)==0);
    CHECK(path_call("sceIoMkdir","relative",0700,0)==0);
    CHECK(error(path_call("sceIoChdir","ms0:/missing",0,0)));
    CHECK(path_call("sceIoRmdir","relative",0,0)==0); /* failed chdir kept cwd */

    static const char payload[]="SDK host adapter data";
    psp_mem_write_block(DATA,payload,sizeof payload);
    uint32_t file=path_call("sceIoOpen","payload.bin",0x602,0600);
    CHECK(!error(file));
    CHECK(call("sceIoWrite",file,DATA,sizeof payload)==sizeof payload);
    CHECK(call("sceIoClose",file,0,0)==0);
    CHECK(error(path_call("sceIoChdir","payload.bin",0,0)));
    CHECK(error(path_call("sceIoRmdir","ms0:/sample",0,0)));
    CHECK(error(path_call("sceIoRemove","ms0:/sample",0,0)));

    /* Chstat validates the path and changes nothing on the host
     * (src/hle/iofilemgr.c): nothing reads modes or times back. */
    psp_write32(DATA,0400);
    CHECK(path_call("sceIoChstat","payload.bin",DATA,1)==0);
    char host[1024];psp_io_host_path("payload.bin",host,sizeof host);
    struct stat status;
    CHECK(stat(host,&status)==0);
    CHECK((status.st_mode&0200)!=0);

    uint32_t dir=path_call("sceIoDopen","ms0:/sample",0,0);
    CHECK(!error(dir));
    unsigned seen=0;
    for (unsigned attempts=0;attempts<32;attempts++) {
        unsigned char poison[360];memset(poison,0xcd,sizeof poison);
        psp_mem_write_block(ENTRY-4,poison,sizeof poison);
        uint32_t result=call("sceIoDread",dir,ENTRY,0);
        CHECK(!error(result));
        CHECK(psp_read32(ENTRY-4)==0xcdcdcdcd);
        CHECK(psp_read32(ENTRY+344)==0xcdcdcdcd); /* caller's private pointer */
        CHECK(psp_read32(ENTRY+348)==0xcdcdcdcd);
        CHECK(psp_read32(ENTRY+352)==0xcdcdcdcd);
        if (!result) break;
        char name[256];psp_str(ENTRY+88,name,sizeof name);
        if (!strcmp(name,"payload.bin")) {
            seen++;
            CHECK((psp_read32(ENTRY)&0xf000)==0x2000); /* SDK regular-file mode */
            CHECK(psp_read32(ENTRY+4)==0x20); /* SDK regular-file attributes */
            CHECK(psp_read32(ENTRY+8)==sizeof payload && psp_read32(ENTRY+12)==0);
        }
    }
    CHECK(seen==1);CHECK(call("sceIoDread",dir,ENTRY,0)==0);
    CHECK(call("sceIoDclose",dir,0,0)==0);
    CHECK(error(call("sceIoDclose",dir,0,0)));
    CHECK(error(call("sceIoDread",dir,ENTRY,0)));
    dir=path_call("sceIoDopen","ms0:/sample",0,0);
    CHECK(error(call("sceIoDread",dir,0,0)));
    CHECK(call("sceIoDclose",dir,0,0)==0);

    CHECK(path_call("sceIoRemove","payload.bin",0,0)==0);
    CHECK(error(path_call("sceIoRemove","payload.bin",0,0)));
    CHECK(path_call("sceIoChdir","ms0:/",0,0)==0);
    CHECK(path_call("sceIoRmdir","sample",0,0)==0);
    psp_io_reset();psp_io_set_root(".");
    char ms_root[1100];
    snprintf(ms_root,sizeof ms_root,"directory-sdk-fixture/%s","ms");
    CHECK(remove_directory(ms_root)==0);   /* ms0:/, which the guest cannot rmdir */
    CHECK(remove_directory("directory-sdk-fixture")==0);
    psp_mem_free();
    printf("SDK directory adapter: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
