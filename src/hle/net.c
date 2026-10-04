/* psprecomp — sceNet, sceNetAdhoc, sceNetAdhocctl, sceWlanDrv.
 *
 * There is no radio here, so everything that would need one is refused:
 *
 * - sceNetInit manages a memory pool, not the air interface, so it succeeds
 *   vacuously -- there is nothing to allocate for -- and sceNetTerm likewise
 *   succeeds: terminating nothing is not the out-param lie, since these calls
 *   have no out-parameters. Neither validates its arguments: no source this
 *   project can use says what hardware refuses there, and the game passes
 *   ordinary values, so a rule here would be a guess.
 * - Every other call is refused with SCE_KERNEL_ERROR_NOTIMPLEMENTED. A 0
 *   from Adhoc/adhocctl Init would read as "multiplayer is up" and send the
 *   menu down a flow whose wakeups never arrive -- the success-lie state.md
 *   names as the shape of most bugs here. The network facility's own codes
 *   (0x8041xxxx, module 0x07 adhoc and 0x0b adhocctl per uofw's
 *   include/net/psp_net_error.h) would be more specific, but uofw does not
 *   list the not-initialized or no-address codes, so the generic kernel code
 *   stands in. Out-parameters are left untouched on these paths: the test
 *   seeds guards.
 * - The WLAN switch reads off (0 -- there is no switch), and
 *   sceNetEtherNtostr is pure formatting, so it is implemented: it formats
 *   whatever six bytes the caller passes.
 *
 * Library, NID and name for every entry below are PSPSDK's import stubs
 * (src/net/sceNet.S, sceNetAdhoc.S, sceNetAdhocctl.S, src/wlan/sceWlanDrv.S;
 * BSD), and test_nids_match_names re-derives each NID from its name. The game
 * never calls any of these on a measured path -- no HLE log holds one -- so the
 * refusals are preventive: the multiplayer menu must fail, not hang. Worked
 * under M6, which names exactly this ("registered to fail honestly"), and
 * under the autotests policy's game-bug clause otherwise.
 */

#include "psprecomp/hle.h"
#include "psprecomp/mem.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The net stack is a memory pool, not a radio: succeed vacuously. Terminate is
 * the same with nothing to tear down. */
static void hle_NetInit(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

static void hle_NetTerm(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* Radio firmware that cannot exist, and everything behind it. Out-parameters
 * untouched. */
static void hle_NetNotImpl(void) { psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED); }

/* Pure formatting of the caller's six bytes. Invalid pointers are skipped
 * silently: a void function has no error to report, and faulting the caller
 * is worse. */
static void hle_EtherNtostr(void) {
    const uint8_t *mac = psp_mem_ptr(psp_arg(0), 6);
    char text[18];
    if (!mac || !psp_mem_ptr(psp_arg(1), (uint32_t)sizeof text)) return;
    snprintf(text, sizeof text, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    psp_mem_write_block(psp_arg(1), text, sizeof text);
}

/* There is no switch. Off is the honest reading, and it is what sends a
 * menu down its no-radio path instead of polling forever for an on. */
static void hle_WlanSwitchState(void) { psp_ret(0); }

void psp_net_register(void) {
    psp_hle_register(0x39AF39A6, "sceNet", "sceNetInit",              hle_NetInit);
    psp_hle_register(0x281928A9, "sceNet", "sceNetTerm",              hle_NetTerm);
    psp_hle_register(0x0BF0A3AE, "sceNet", "sceNetGetLocalEtherAddr", hle_NetNotImpl);
    psp_hle_register(0x89360950, "sceNet", "sceNetEtherNtostr",       hle_EtherNtostr);

    psp_hle_register(0xE1D621D7, "sceNetAdhoc", "sceNetAdhocInit",      hle_NetNotImpl);
    psp_hle_register(0xA62C6F57, "sceNetAdhoc", "sceNetAdhocTerm",      hle_NetNotImpl);
    psp_hle_register(0x6F92741B, "sceNetAdhoc", "sceNetAdhocPdpCreate", hle_NetNotImpl);
    psp_hle_register(0xABED3790, "sceNetAdhoc", "sceNetAdhocPdpSend",   hle_NetNotImpl);
    psp_hle_register(0xDFE53E03, "sceNetAdhoc", "sceNetAdhocPdpRecv",   hle_NetNotImpl);
    psp_hle_register(0x7F27BB5E, "sceNetAdhoc", "sceNetAdhocPdpDelete", hle_NetNotImpl);
    psp_hle_register(0x157E6225, "sceNetAdhoc", "sceNetAdhocPtpClose",  hle_NetNotImpl);
    psp_hle_register(0x4DA4C788, "sceNetAdhoc", "sceNetAdhocPtpSend",   hle_NetNotImpl);
    psp_hle_register(0x877F6D66, "sceNetAdhoc", "sceNetAdhocPtpOpen",   hle_NetNotImpl);
    psp_hle_register(0x8BEA2B3E, "sceNetAdhoc", "sceNetAdhocPtpRecv",   hle_NetNotImpl);
    psp_hle_register(0x9DF81198, "sceNetAdhoc", "sceNetAdhocPtpAccept", hle_NetNotImpl);
    psp_hle_register(0xE08BDAC1, "sceNetAdhoc", "sceNetAdhocPtpListen", hle_NetNotImpl);
    psp_hle_register(0xFC6FC07B, "sceNetAdhoc", "sceNetAdhocPtpConnect",hle_NetNotImpl);
    psp_hle_register(0x9AC2EEAC, "sceNetAdhoc", "sceNetAdhocPtpFlush",  hle_NetNotImpl);

    psp_hle_register(0xE26F226E, "sceNetAdhocctl", "sceNetAdhocctlInit",       hle_NetNotImpl);
    psp_hle_register(0x9D689E13, "sceNetAdhocctl", "sceNetAdhocctlTerm",       hle_NetNotImpl);
    psp_hle_register(0x20B317A0, "sceNetAdhocctl", "sceNetAdhocctlAddHandler", hle_NetNotImpl);
    psp_hle_register(0x6402490B, "sceNetAdhocctl", "sceNetAdhocctlDelHandler", hle_NetNotImpl);
    psp_hle_register(0x34401D65, "sceNetAdhocctl", "sceNetAdhocctlDisconnect", hle_NetNotImpl);
    psp_hle_register(0x0AD043ED, "sceNetAdhocctl", "sceNetAdhocctlConnect",    hle_NetNotImpl);
    psp_hle_register(0x75ECD386, "sceNetAdhocctl", "sceNetAdhocctlGetState",   hle_NetNotImpl);
    psp_hle_register(0xE162CB14, "sceNetAdhocctl", "sceNetAdhocctlGetPeerList",hle_NetNotImpl);

    psp_hle_register(0xD7763699, "sceWlanDrv", "sceWlanGetSwitchState", hle_WlanSwitchState);
}
