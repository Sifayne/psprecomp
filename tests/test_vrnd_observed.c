/* Compare the runtime with observations from our separately compiled PSP
 * instruction probe. The observations, not an emulator implementation, are
 * the oracle. See tests/provenance/README.md for generation and limitations. */
#include "psprecomp/cpu.h"
#include "psprecomp/vfpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures, checks;
static void check_word(unsigned line, const char *field, unsigned index,
                       uint32_t actual, uint32_t expected) {
    checks++;
    if (actual == expected) return;
    if (failures++ < 12)
        fprintf(stderr, "line %u %s[%u]: %08X != %08X\n", line,field,index,actual,expected);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr,"usage: test_vrnd_observed observations.txt\n"); return 2; }
    FILE *file = fopen(argv[1],"r");
    if (!file) { perror(argv[1]); return 2; }
    char line[512]; unsigned number=0, cases=0;
    while (fgets(line,sizeof line,file)) {
        number++;
        if (line[0]=='#' || line[0]=='\n') continue;
        uint32_t words[25]; unsigned count=0;
        char *p=line;
        while (count<25) {
            char *end; unsigned long value=strtoul(p,&end,16);
            if (end==p) break;
            words[count++]=(uint32_t)value; p=end;
        }
        if (count<2 || count!=(words[0]==9 ? 25u:19u)) {
            fprintf(stderr,"invalid observation on line %u\n",number); fclose(file); return 2;
        }
        psp_cpu_reset();
        psp_vfpu_reset();
        const unsigned kind=words[0], id=words[1];
        if (kind==11) {
            check_word(number,"revision",0,psp_mfvc(7),id);
            for (unsigned i=0;i<8;i++) check_word(number,"reset",i,psp_mfvc(8+i),words[2+i]);
        }
        if ((kind<5 && id==0) || kind==7 || kind==10) {
            const uint32_t seeds[]={0,1,0x12345678,0xffffffff,0x80000000};
            psp_mtv(0,kind==7 ? id:kind==10 ? 0xbf400001:seeds[kind]);
            if (kind==10) {
                psp_mtv(32,0x40000000); psp_mtv(64,0x40400000); psp_mtv(96,0x40800000);
                psp_vfpu_set_prefix(0,id);
            }
            psp_vrnds(0, 1);
            for (unsigned i=0;i<8;i++) check_word(number,"seed",i,psp_mfvc(8+i),words[2+i]);
        }
        for (unsigned i=0;i<8;i++) psp_mtvc(8+i,words[2+i]);
        unsigned state_offset;
        if (kind==9) {
            const unsigned shape=id>>12;
            if (shape>=12) { fclose(file); return 2; }
            for (unsigned i=0;i<4;i++) psp_mtv(i*32,0x4abcdef0);
            psp_vfpu_set_prefix(0,0xfffff); psp_vfpu_set_prefix(1,0xfffff); psp_vfpu_set_prefix(2,id&4095);
            psp_vrnd(0,shape/4,1+shape%4);
            for (unsigned i=0;i<4;i++) check_word(number,"lane",i,psp_mfv(i*32),words[10+i]);
            for (unsigned i=0;i<3;i++) check_word(number,"prefix",i,psp_mfvc(i),words[22+i]);
            state_offset=14;
        } else {
            psp_vrnd(0,0,1);
            check_word(number,"result",0,psp_mfv(0),words[10]);
            state_offset=11;
        }
        for (unsigned i=0;i<8;i++) check_word(number,"state",i,psp_mfvc(8+i),words[state_offset+i]);
        cases++;
    }
    const int read_error=ferror(file); fclose(file);
    printf("vrnd observations: %u cases, %u word checks, %u failures\n",cases,checks,failures);
    return read_error || !cases || failures ? 1:0;
}
