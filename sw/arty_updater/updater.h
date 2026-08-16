/*
 * updater.h — the field-update pipeline, public contract.
 *
 * Contract: doc/icd_updater.md. The nine steps live there with their rationale;
 * this header is the API and the failure taxonomy.
 *
 * Composed entirely from the library modules — lwIP, tftp_get(), sha256(),
 * qspi_flash_*(), boot_state_*(), multiboot_to(). Nothing here re-derives any
 * of them.
 */

#ifndef UPDATER_H
#define UPDATER_H

#include "xil_types.h"
#include "lwip/ip_addr.h"
#include "netif/xadapter.h"

/*
 * Every way the pipeline can end, one per abort site. Distinct causes stay
 * distinct: when this aborts, the console has to say WHICH step and WHY, not
 * "update failed". Required by the ICD's error contract.
 */
typedef enum {
	UPD_OK = 0,             /* installed, committed, ready to redirect       */
	UPD_SKIPPED_SAME,       /* step 2c: slot holds it AND the record says so */
	UPD_ERR_MANIFEST_GET,   /* step 2: could not fetch update.sha            */
	UPD_ERR_MANIFEST_PARSE, /* step 2: fetched, but not a manifest we speak  */
	UPD_ERR_DOWNGRADE,      /* step 2b: older than installed, and not forced */
	UPD_ERR_IMAGE_GET,      /* step 3: could not fetch update.bin            */
	UPD_ERR_SHORT,          /* step 3: fewer bytes than the manifest claims  */
	UPD_ERR_TOOBIG,         /* step 3/4: will not fit the slot               */
	UPD_ERR_HASH_DOWNLOAD,  /* step 4: DDR image does not match the manifest */
	UPD_ERR_NOT_BOOTIMAGE,  /* step 4b/2c: hashes fine, not a Zynq boot image*/
	UPD_ERR_INVALIDATE,     /* step 5: could not clear update_present        */
	UPD_ERR_ERASE,          /* step 6: sector erase failed                   */
	UPD_ERR_PROGRAM,        /* step 6: page program failed                   */
	UPD_ERR_READBACK,       /* step 7/2c: could not read the slot back       */
	UPD_ERR_HASH_READBACK,  /* step 7: what landed is not what was sent      */
	UPD_ERR_COMMIT          /* step 8: could not set update_present          */
} upd_result_t;

/* Human-readable form, for the console log. Never NULL. */
const char *upd_result_str(upd_result_t r);

/*
 * Extra detail about the most recent failure, or "" when there is none.
 *
 * EXISTS BECAUSE THE CONSOLE IS NOT THE CHANNEL. This deployment has Ethernet
 * and no serial console -- that sentence is the reason the control channel
 * exists at all (doc/icd_control_channel.md) -- so a diagnosis printed only by
 * xil_printf is a diagnosis nobody in the field will ever read. The manifest
 * parser can distinguish eight failures; without this, all eight reach the wire
 * as "manifest malformed" and the operator cannot tell a stale release script
 * from a corrupt download.
 *
 * Points at static storage owned by this module. Valid until the next
 * updater_run().
 */
const char *updater_last_detail(void);

/*
 * Run steps 1-8. Does NOT redirect -- step 9 is the caller's, so an operator
 * can see the commit land before the board resets itself.
 *
 * On UPD_OK the update slot holds a verified image and boot state says
 * {update_present=1, boot_attempts=0}.
 *
 * On ANY error the slot is either untouched (aborted at or before step 4) or
 * marked invalid (aborted at or after step 5). There is no exit path that
 * leaves update_present set over an unverified slot -- that property is what
 * makes a power cut during this survivable, and it is why step 5 precedes
 * step 6.
 *
 * `netif` is the interface to pump; `server` is the TFTP server; `ddr` is a
 * scratch buffer of `ddr_len` bytes, which must be at least as large as the
 * update slot.
 *
 * `poll` (if non-NULL) is called regularly throughout, including inside the
 * hashing, erase, program and read-back loops. The caller uses it to service
 * whatever must not stall for the ~60 s this takes: the network stack and, in
 * the update role, the watchdog. Same idiom as `kick` below.
 *
 * IT IS NOT OPTIONAL IN PRACTICE. Steps 4 to 8 -- a SHA-256 over the whole
 * image, 66 sector erases, a 4.3 MB program and a 4.3 MB read-back -- take ~20 s
 * with no natural pump point. Without `poll` the board accepts control-channel
 * datagrams into the MAC and delivers them only after the pipeline returns, so a
 * command sent during the update executes tens of seconds later, against state
 * that has since changed. Passing NULL re-creates that.
 */
/*
 * `force` admits the one install a bare UPDATE refuses: a manifest declaring a
 * version older than the one recorded as installed. It does NOT relax any
 * integrity check -- hash, length, boot header and read-back all still apply,
 * and there is deliberately no argument anywhere in this API that can switch
 * those off. It relaxes a POLICY, and policy is the only thing an operator
 * should be able to overrule from the network.
 *
 * It also does not defeat the step-2c skip. Re-installing bytes that are already
 * in the slot cannot be what anyone means by force, and pretending otherwise
 * would just spend a flash-wear cycle on a no-op. The skip requires the boot
 * state to agree that they are installed; where it does not, step 2c commits
 * rather than skips, and no force is needed for that either.
 */
upd_result_t updater_run(struct netif *netif,
                         const ip_addr_t *server,
                         u8 *ddr,
                         u32 ddr_len,
                         int force,
                         void (*poll)(void));

/*
 * Health probe used by the update image to decide whether to commit: the PL
 * heartbeat must be moving and stay moving for `stable_s` seconds.
 *
 * A moving heartbeat proves the bitstream in this image actually configured the
 * PL and is clocked -- which is the thing an update could plausibly get wrong
 * and the reason blinkctl has a free-running counter at all.
 *
 * Calls `kick` (if non-NULL) once per second so the caller can service its
 * watchdog while this blocks.
 */
int updater_payload_healthy(u32 stable_s, void (*kick)(void));

/*
 * The payload's VERSION register, straight off M_AXI_GP0 — what the bitstream
 * in the RUNNING image says it is, which is the only version claim in this
 * system that cannot be stale.
 *
 * Here rather than in the caller so blinkctl's base address stays in exactly one
 * place: the control channel reports it in STATUS and the health probe prints it.
 *
 * Reading it assumes the PL is configured. It is, in both roles: each BOOT.BIN
 * carries its own bitstream and the FSBL loads it before this application runs.
 */
u32 updater_payload_version(void);

#endif /* UPDATER_H */
