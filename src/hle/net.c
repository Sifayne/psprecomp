/* psprecomp — sceNet, sceNetAdhoc, sceNetAdhocctl, sceWlanDrv.
 *
 * There is no radio here. The refusal is split where hardware splits it:
 *
 * - sceNetInit manages a memory pool, not the air interface, so it
 *   validates its arguments exactly the way PPSSPP documents hardware doing
 *   (poolSize 0 is ILLEGAL_MEMSIZE, priorities outside 0x08..0x77 are
 *   ILLEGAL_PRIORITY) and otherwise succeeds vacuously -- there is nothing
 *   to allocate for. sceNetTerm likewise succeeds: terminating nothing is
 *   not the out-param lie, since these calls have no out-parameters.
 * - Adhoc/adhocctl Init bring up radio firmware, which cannot exist here,
 *   so they are refused outright (SCE_KERNEL_ERROR_NOTIMPLEMENTED). A 0
 *   there would read as "multiplayer is up" and send the menu down a flow
 *   whose wakeups never arrive -- the success-lie state.md names as the
 *   shape of most bugs here.
 * - Everything behind those inits reports the prerequisite it will never
 *   meet: NOT_INITIALIZED in its own family's code (0x80410712 adhoc,
 *   0x80410b08 adhocctl). Out-parameters are left untouched on these paths,
 *   the way the Refer*Status captures require: the test seeds guards.
 * - Two calls return values rather than errors, and both have honest ones:
 *   the WLAN switch reads off (0 -- there is no switch), and
 *   sceNetGetLocalEtherAddr answers 0x80410180, the code hardware gives
 *   when no address is available. sceNetEtherNtostr is pure formatting, so
 *   it is implemented: it writes a zero MAC, which is what this radio has.
 *
 * NID-to-name mapping is read out of PPSSPP's HLE tables
 * (Core/HLE/sceNetAdhoc.cpp, sceNet.cpp) and the values out of its
 * ErrorCodes.h (hrydgard/ppsspp, GPL-2.0 -- names and values only, no
 * code). The game never calls any of these on a measured path -- no HLE
 * log holds one -- so the refusals are preventive: the multiplayer menu
 * must fail, not hang. Worked under M6, which names exactly this
 * ("registered to fail honestly"), and under the autotests policy's
 * game-bug clause otherwise.
 */

#include "psprecomp/hle.h"
#include "psprecomp/mem.h"

#include <stddef.h>
#include <string.h>

/* The net stack is a memory pool, not a radio: validate like hardware
 * (PPSSPP pins both rules against real init calls), then succeed
 * vacuously. Terminate is the same with nothing to tear down. */
static void hle_NetInit(void) {
    if (psp_arg(0) == 0) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE); return; }
    if (psp_arg(1) < 0x08 || psp_arg(1) > 0x77 ||
        psp_arg(3) < 0x08 || psp_arg(3) > 0x77) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_PRIORITY);
        return;
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_NetTerm(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* Radio firmware that cannot exist. */
static void hle_NetNotImpl(void) { psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED); }

/* Behind an init that can never succeed. Out-parameters untouched. */
static void hle_AdhocNotInit(void)    { psp_ret(SCE_NET_ADHOC_ERROR_NOT_INITIALIZED); }
static void hle_AdhocctlNotInit(void) { psp_ret(SCE_NET_ADHOCCTL_ERROR_NOT_INITIALIZED); }

/* No address is available, and none will become so: the code hardware gives
 * for exactly this situation. The caller's buffer is left alone. */
static void hle_GetLocalEtherAddr(void) { psp_ret(SCE_NET_ERROR_NO_ADDRESS); }

/* Pure formatting over whatever MAC the caller points at; ours is zeros.
 * Invalid pointers are skipped silently, the way PPSSPP guards them: a void
 * function has no error to report, and faulting the caller is worse. */
static void hle_EtherNtostr(void) {
    static const char zero[] = "00:00:00:00:00:00";
    void *dst = psp_mem_ptr(psp_arg(1), (uint32_t)sizeof zero);
    if (!dst) return;
    memcpy(dst, zero, sizeof zero);
}

/* There is no switch. Off is the honest reading, and it is what sends a
 * menu down its no-radio path instead of polling forever for an on. */
static void hle_WlanSwitchState(void) { psp_ret(0); }

void psp_net_register(void) {
    psp_hle_register(0x39AF39A6, "sceNet", "sceNetInit",              hle_NetInit);
    psp_hle_register(0x281928A9, "sceNet", "sceNetTerm",              hle_NetTerm);
    psp_hle_register(0x0BF0A3AE, "sceNet", "sceNetGetLocalEtherAddr", hle_GetLocalEtherAddr);
    psp_hle_register(0x89360950, "sceNet", "sceNetEtherNtostr",       hle_EtherNtostr);

    psp_hle_register(0xE1D621D7, "sceNetAdhoc", "sceNetAdhocInit",      hle_NetNotImpl);
    psp_hle_register(0xA62C6F57, "sceNetAdhoc", "sceNetAdhocTerm",      hle_AdhocNotInit);
    psp_hle_register(0x6F92741B, "sceNetAdhoc", "sceNetAdhocPdpCreate", hle_AdhocNotInit);
    psp_hle_register(0xABED3790, "sceNetAdhoc", "sceNetAdhocPdpSend",   hle_AdhocNotInit);
    psp_hle_register(0xDFE53E03, "sceNetAdhoc", "sceNetAdhocPdpRecv",   hle_AdhocNotInit);
    psp_hle_register(0x7F27BB5E, "sceNetAdhoc", "sceNetAdhocPdpDelete", hle_AdhocNotInit);
    psp_hle_register(0x157E6225, "sceNetAdhoc", "sceNetAdhocPtpClose",  hle_AdhocNotInit);
    psp_hle_register(0x4DA4C788, "sceNetAdhoc", "sceNetAdhocPtpSend",   hle_AdhocNotInit);
    psp_hle_register(0x877F6D66, "sceNetAdhoc", "sceNetAdhocPtpOpen",   hle_AdhocNotInit);
    psp_hle_register(0x8BEA2B3E, "sceNetAdhoc", "sceNetAdhocPtpRecv",   hle_AdhocNotInit);
    psp_hle_register(0x9DF81198, "sceNetAdhoc", "sceNetAdhocPtpAccept", hle_AdhocNotInit);
    psp_hle_register(0xE08BDAC1, "sceNetAdhoc", "sceNetAdhocPtpListen", hle_AdhocNotInit);
    psp_hle_register(0xFC6FC07B, "sceNetAdhoc", "sceNetAdhocPtpConnect",hle_AdhocNotInit);
    psp_hle_register(0x9AC2EEAC, "sceNetAdhoc", "sceNetAdhocPtpFlush",  hle_AdhocNotInit);

    psp_hle_register(0xE26F226E, "sceNetAdhocctl", "sceNetAdhocctlInit",       hle_NetNotImpl);
    psp_hle_register(0x9D689E13, "sceNetAdhocctl", "sceNetAdhocctlTerm",       hle_AdhocctlNotInit);
    psp_hle_register(0x20B317A0, "sceNetAdhocctl", "sceNetAdhocctlAddHandler", hle_AdhocctlNotInit);
    psp_hle_register(0x6402490B, "sceNetAdhocctl", "sceNetAdhocctlDelHandler", hle_AdhocctlNotInit);
    psp_hle_register(0x34401D65, "sceNetAdhocctl", "sceNetAdhocctlDisconnect", hle_AdhocctlNotInit);
    psp_hle_register(0x0AD043ED, "sceNetAdhocctl", "sceNetAdhocctlConnect",    hle_AdhocctlNotInit);
    psp_hle_register(0x75ECD386, "sceNetAdhocctl", "sceNetAdhocctlGetState",   hle_AdhocctlNotInit);
    psp_hle_register(0xE162CB14, "sceNetAdhocctl", "sceNetAdhocctlGetPeerList",hle_AdhocctlNotInit);

    psp_hle_register(0xD7763699, "sceWlanDrv", "sceWlanGetSwitchState", hle_WlanSwitchState);
}
