#!/bin/sh
#
# solaris-probe.sh: read-only survey of a Solaris or illumos machine, to see
# what the NVIDIA kernel module port can build against and call.
#
#   sudo sh solaris-probe.sh
#
# Everything prints to the terminal and nothing is saved. To keep a copy:
#   sudo sh solaris-probe.sh 2>&1 | tee /var/tmp/probe.txt
#
# The script loads no modules, adds no drivers and edits nothing under /etc.
# It compiles a few tiny files in a private temporary directory and removes
# them on exit. The full prtconf -v dump can hold serial numbers and MAC
# addresses, so it stays in that directory; only the GPU nodes and the PCIe
# root complex nodes are printed, each limited to its own properties. Read
# the output before sharing it. The kernel interface and header scans can
# take a minute or two each.

set -u
PATH=/usr/sbin:/usr/bin:/sbin:$PATH:/usr/gnu/bin

[ "$(uname -s)" = SunOS ] || { echo "solaris-probe: SunOS only" >&2; exit 1; }
[ "$(id -u)" = 0 ] || echo "note: not root; device listings will be incomplete" >&2

TMP=$(mktemp -d /var/tmp/nvprobe.XXXXXX) || exit 1
trap 'rm -rf "$TMP"' 0
trap 'exit 1' 1 2 15

say() { printf '%s\n' "$*"; }
section() { printf '\n==== %s ====\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# runsh "pipeline": print the command, its output and a non-zero exit status.
runsh() {
    printf '$ %s\n' "$1"
    sh -c "$1" 2>&1
    rc=$?
    [ $rc -eq 0 ] || printf '[exit %s]\n' "$rc"
    return 0
}

section "System"
runsh "uname -a"
runsh "isainfo -kv"
runsh "cat /etc/release"
runsh "pagesize -a"
runsh "psrinfo -pv"
runsh "prtconf | egrep 'Memory size|System Configuration'"
have virtinfo && runsh "virtinfo -a | egrep 'Domain role|Domain name|Control domain'"
[ -x /opt/SUNWldm/bin/ldm ] && runsh "/opt/SUNWldm/bin/ldm list"
have beadm && runsh "beadm list"
runsh "pkg list -H '*gcc*' '*header*' '*binutils*' '*assembler*' '*linker*' '*ctf*'"

section "Tools"
for t in gcc cc gmake make ld gld as gas gobjcopy gobjdump gnm ctfconvert ctfmerge ginstall git python3 perl awk ggrep gsed; do
    p=$(command -v $t 2>/dev/null) && say "$t: $p" || say "$t: not found"
done
have gcc && runsh "gcc --version | head -1"
have gcc && runsh "gcc -dumpmachine"
have gcc && runsh "gcc -print-file-name=include"
have cc && runsh "cc -V 2>&1 | head -2"
have ld && runsh "ld -V 2>&1 | head -2"
have gld && runsh "gld --version | head -1"
have gld && runsh "gld --help | egrep -i 'emulations|sparc' | head -5"
have gmake && runsh "gmake --version | head -1"

section "Compiler defaults for 64-bit SPARC"
if have gcc; then
    runsh "gcc -m64 -mcpu=v9 -dM -E -x c /dev/null | egrep -i '__sun|svr4|sparc|arch64|endian|LP64|__unix|SIZEOF_LONG__|SIZEOF_POINTER' | sort"
fi

section "Test compile and link (Oracle's driver flags)"
KFLAGS="-D_KERNEL -m64 -mcpu=v9 -mcmodel=medlow -fno-pic -mno-fpu -ffreestanding -nodefaultlibs"
say "flags: $KFLAGS"
cat > "$TMP/barrier.c" <<'CEOF'
int probe_barrier(int x)
{
    __asm__ __volatile__("membar #StoreStore" ::: "memory");
    __asm__ __volatile__("membar #LoadLoad" ::: "memory");
    __asm__ __volatile__("membar #LoadLoad | #LoadStore | #StoreLoad | #StoreStore" ::: "memory");
    __asm__ __volatile__("nop");
    return x;
}
CEOF
cat > "$TMP/hdr.c" <<'CEOF'
#include <sys/types.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
int probe_hdr(dev_info_t *dip) { return ddi_get_instance(dip); }
CEOF
cat > "$TMP/le.c" <<'CEOF'
unsigned int probe_le(volatile unsigned int *p)
{
    unsigned int v;
    __asm__ __volatile__("lduwa [%1] 0x88, %0" : "=r"(v) : "r"(p) : "memory");
    return v;
}
CEOF
if have gcc; then
    for f in barrier hdr le; do
        printf '$ gcc %s -c %s.c\n' "$KFLAGS" "$f"
        if gcc $KFLAGS -c "$TMP/$f.c" -o "$TMP/$f.o" 2>&1; then
            say "$f.c: compiled"
        else
            say "$f.c: FAILED"
        fi
    done
    if [ -f "$TMP/barrier.o" ]; then
        runsh "ld -r -o '$TMP/t.r' '$TMP/barrier.o' && echo 'ld -r: ok'"
        runsh "ld -ztype=kmod -o '$TMP/t.kmod' '$TMP/barrier.o' && echo 'ld -ztype=kmod: ok'"
        have gld && runsh "gld -r -o '$TMP/t.gr' '$TMP/barrier.o' && echo 'gld -r: ok'"
        have gld && runsh "gld --gc-sections -r -o '$TMP/t.gg' '$TMP/barrier.o' && echo 'gld --gc-sections: ok'"
        have gobjcopy && runsh "gobjcopy --version | head -1"
    fi
else
    say "no gcc, skipped"
fi

section "Kernel headers the port includes (missing ones only)"
INC=/usr/include
: > "$TMP/hdr.missing"
n=0
while read h; do
    [ -n "$h" ] || continue
    n=$((n + 1))
    [ -f "$INC/$h" ] || echo "$h" >> "$TMP/hdr.missing"
done <<'HEOF'
sha2.h
stdint.h
strings.h
sys/acpi/acpi.h
sys/acpica.h
sys/archsystm.h
sys/atomic.h
sys/avl.h
sys/bitmap.h
sys/bootconf.h
sys/buf.h
sys/callb.h
sys/callo.h
sys/cmn_err.h
sys/conf.h
sys/cpuvar.h
sys/cred.h
sys/crypto/api.h
sys/crypto/common.h
sys/ctype.h
sys/ddi.h
sys/ddi_impldefs.h
sys/ddi_intr.h
sys/ddidevmap.h
sys/debug.h
sys/disp.h
sys/errno.h
sys/esunddi.h
sys/fbio.h
sys/fcntl.h
sys/file.h
sys/firmload.h
sys/framebuffer.h
sys/gfx_private.h
sys/i2c/controller.h
sys/id_space.h
sys/int_limits.h
sys/ioccom.h
sys/kd.h
sys/kmem.h
sys/kmem_impl.h
sys/ksynch.h
sys/lgrp.h
sys/list.h
sys/mman.h
sys/modctl.h
sys/model.h
sys/open.h
sys/param.h
sys/pci.h
sys/pci_cap.h
sys/pci_cfgacc.h
sys/pci_impl.h
sys/pcie.h
sys/policy.h
sys/poll.h
sys/proc.h
sys/project.h
sys/random.h
sys/rctl.h
sys/reboot.h
sys/resource.h
sys/sdt.h
sys/semaphore.h
sys/sha2.h
sys/signal.h
sys/smbios.h
sys/stack.h
sys/stat.h
sys/stdbool.h
sys/sunddi.h
sys/sunndi.h
sys/sysmacros.h
sys/systm.h
sys/task.h
sys/taskq.h
sys/taskq_impl.h
sys/thread.h
sys/types.h
sys/uio.h
sys/user.h
sys/utsname.h
sys/vfs_opreg.h
sys/visual_io.h
sys/vmem.h
sys/vmsystm.h
sys/vnode.h
sys/zone.h
vm/as.h
vm/faultcode.h
vm/hat.h
vm/page.h
vm/seg.h
vm/seg_kmem.h
vm/seg_kpm.h
vm/seg_vn.h
HEOF
say "checked $n headers under $INC"
if [ -s "$TMP/hdr.missing" ]; then
    say "MISSING headers: $(wc -l < "$TMP/hdr.missing" | tr -d ' ')"
    cat "$TMP/hdr.missing"
else
    say "all present"
fi

section "Kernel modules the drivers link against"
KISA=$(isainfo -k)
PLAT=$(uname -m)
for m in misc/gfx_private misc/acpica misc/pcie misc/sha2 misc/kcf drv/i2cnex drv/px drv/pcieb; do
    dir=${m%/*}
    base=${m#*/}
    found=
    for k in /kernel /usr/kernel /platform/$PLAT/kernel; do
        [ -f "$k/$dir/$KISA/$base" ] && found="$k/$dir/$KISA/$base"
    done
    say "$m: ${found:-not found}"
done

section "Kernel interfaces the port calls (looked up in module files)"
if ! have nm; then
    say "nm not found, skipped"
else
    echo "  (running nm on every module file; this can take a minute or two)"
    : > "$TMP/modfiles"
    for k in /kernel /usr/kernel /platform/$PLAT/kernel; do
        [ -d "$k" ] && find "$k" -type f 2>/dev/null | grep "/$KISA/" >> "$TMP/modfiles"
    done
    say "module files scanned: $(wc -l < "$TMP/modfiles" | tr -d ' ')"
    : > "$TMP/symtab"
    while read f; do
        nm -p "$f" 2>/dev/null | awk -v m="$f" \
            'NF >= 2 { t = $(NF - 1); if (t ~ /^[ATDBCW]$/) print $NF, m }' >> "$TMP/symtab"
    done < "$TMP/modfiles"
    say "global symbols collected: $(wc -l < "$TMP/symtab" | tr -d ' ')"
    cat > "$TMP/wanted" <<'SEOF'
kobj_open_file
kobj_read_file
kobj_get_filesize
kobj_close_file
vn_open
vn_rdwr
vn_close
page_numtopp
page_numtopp_nowait
page_resv
pci_config_setup
pci_config_teardown
pcie_get_bdf_from_dip
AcpiOsFree
as_delete_callback
as_dup
as_fault
as_findseg
as_free
as_map
as_pagelock
as_rangelock
as_rangeunlock
as_segat
as_setprot
as_unmap
crypto_encrypt
crypto_mech2id
ddi_dev_regsize
ddi_dma_addr_bind_handle
ddi_dma_alloc_handle
ddi_dma_buf_bind_handle
ddi_dma_free_handle
ddi_dma_mem_alloc
ddi_dma_mem_free
ddi_dma_nextcookie
ddi_dma_sync
ddi_dma_unbind_handle
ddi_intr_add_handler
ddi_intr_alloc
ddi_intr_block_disable
ddi_intr_block_enable
ddi_intr_disable
ddi_intr_enable
ddi_intr_free
ddi_intr_get_cap
ddi_intr_get_hilevel_pri
ddi_intr_get_navail
ddi_intr_get_nintrs
ddi_intr_get_pri
ddi_intr_get_supported_types
ddi_intr_remove_handler
ddi_intr_set_pri
ddi_regs_map_free
ddi_regs_map_setup
ddi_umem_unlock
devmap_default_access
devmap_devmem_setup
devmap_setup
devmap_umem_setup
devmap_unload
firmware_close
firmware_get_size
firmware_open
firmware_read
gfxp_alloc_kernel_space
gfxp_fb_attach
gfxp_fb_detach
gfxp_fb_ioctl
gfxp_fb_softc_alloc
gfxp_fb_softc_free
gfxp_free_kernel_space
gfxp_load_kernel_space
gfxp_map_devmem
gfxp_map_kernel_space
gfxp_umem_cookie_destroy
gfxp_umem_cookie_init
gfxp_unload_kernel_space
gfxp_unmap_kernel_space
hat_devload
hat_dup
hat_getpfnum
hat_kpm_pfn2va
hat_memload
hat_unload
i2c_ctrl_io_error
i2c_ctrl_io_success
i2c_ctrl_mod_fini
i2c_ctrl_mod_init
i2c_ctrl_register
i2c_ctrl_register_alloc
i2c_ctrl_register_free
i2c_ctrl_unregister
i2c_prop_info_set_perm
lgrp_mem_size
ndi_devi_enter
ndi_devi_exit
ndi_devi_offline
ndi_hold_devi
ndi_rele_devi
page_create_io
page_create_va
page_destroy
page_downgrade
page_io_unlock
page_iolock_assert
page_lookup
page_numtopp_nolock
page_pptonum
page_retire
page_sub
page_tryupgrade
page_unlock
page_unresv
page_xresv
pci_cfgacc_get16
pci_cfgacc_get32
pci_cfgacc_get8
pci_cfgacc_put16
pci_cfgacc_put32
pci_cfgacc_put8
pf_is_memory
project_hold
project_rele
rctl_decr_locked_mem
rctl_incr_locked_mem
seg_alloc
seg_free
seg_page
segkmem_xfree
smbios_info_chassis
smbios_lookup_type
thread_splitstack
zone_hold
zone_rele
SEOF
    if [ -s "$TMP/symtab" ]; then
        awk 'NR == FNR { want[$1] = 1; next }
             ($1 in want) && !($1 in seen) { seen[$1] = $2 }
             END { for (s in want) {
                       if (s in seen) print "found   " s "  " seen[s]
                       else print "MISSING " s } }' \
            "$TMP/wanted" "$TMP/symtab" | sort > "$TMP/symresult"
        say "interfaces checked: $(wc -l < "$TMP/wanted" | tr -d ' ')"
        say "MISSING: $(grep -c '^MISSING' "$TMP/symresult")"
        grep '^MISSING' "$TMP/symresult"
        say "found (interface, first module that defines it):"
        grep '^found' "$TMP/symresult"
    else
        say "no symbols collected; nm output format may differ"
    fi
fi

section "Alternatives for missing headers and interfaces"
echo "  (searching every .h under /usr, /opt and /platform; this can take a minute or two)"
alt() { printf '%s\n' "$*"; }
G=ggrep
have ggrep || G=egrep
find /usr /opt /platform -xdev -type f -name '*.h' > "$TMP/allhdrs" 2>/dev/null
say "headers found on the system: $(wc -l < "$TMP/allhdrs" | tr -d ' ')"

# 1. Each header the port includes that /usr/include lacks: the same name
#    elsewhere, similarly named headers, and installed packages that list it.
if [ -s "$TMP/hdr.missing" ]; then
    while read h; do
        base=${h##*/}
        stem=${base%.h}
        alt ""
        alt "== $h"
        alt "-- same file name anywhere:"
        $G "/$base\$" "$TMP/allhdrs" | head -10
        alt "-- other headers with '$stem' in the name:"
        $G -i "/[^/]*$stem[^/]*\$" "$TMP/allhdrs" | $G -v "/$base\$" | head -15
        if have pkg; then
            alt "-- installed packages that mention $base:"
            pkg search -l -H "$base" < /dev/null 2>&1 | head -5
        fi
    done < "$TMP/hdr.missing"
fi

# 2. Headers whose names suggest a replacement for a missing facility.
for kw in firm gfx fbio framebuf i2c acpi cfgacc pci_cap pcie_impl pci_impl kobj smbios ddidevmap hvio hsvc visual; do
    alt ""
    alt "== headers with '$kw' in the file name (first 12)"
    $G -i "/[^/]*$kw[^/]*\$" "$TMP/allhdrs" | head -12
done

# 3. Where each interface the port calls is mentioned in /usr/include.
if have ggrep && [ -f "$TMP/wanted" ]; then
    awk 'NF' "$TMP/wanted" > "$TMP/wanted2"
    ggrep -rHnwoF -f "$TMP/wanted2" /usr/include 2>/dev/null |
        awk -F: '{ if (n[$3]++ < 2) print $3 "  " $1 ":" $2 }' | sort > "$TMP/decl"
    alt ""
    alt "== interfaces mentioned in /usr/include (first two places each)"
    cat "$TMP/decl"
    alt ""
    alt "== interfaces mentioned nowhere in /usr/include"
    # FILENAME, not NR == FNR: that test breaks when decl is empty.
    awk 'FILENAME == ARGV[1] { d[$1] = 1; next } !($1 in d)' "$TMP/decl" "$TMP/wanted2"
else
    alt "no ggrep or no interface list; skipped the declaration search"
fi

section "Display and GPU devices"
runsh "egrep -i 'pciex10de|nvidia' /etc/driver_aliases"
runsh "modinfo | egrep -i 'nvidia|gfx_private|i2c|pcieb| px'"
runsh "ls -l /dev/fbs"
runsh "ls -l /kernel/firmware 2>&1 | head"
have prtdiag && runsh "prtdiag 2>&1 | head -80"
have cfgadm && runsh "cfgadm -al 2>&1 | egrep -i 'pci|px' | head -40"
if have prtconf; then
    # The full dump holds serial numbers and MAC addresses. It stays in the
    # private temporary directory; only the excerpts below are printed.
    prtconf -v > "$TMP/prtconf-v.txt" 2>&1
    section "GPU and display nodes from prtconf -v"
    # Each matching node and its own subtree only: stop at the first line
    # that is not indented deeper than the node header.
    awk '/^ *(display|pciex10de,[0-9a-f]+)([ ,(]|$)/ {
             match($0, /^ */); ind = RLENGTH; p = 1; print; next }
         p { match($0, /^ */); if (RLENGTH > ind) print; else p = 0 }' \
        "$TMP/prtconf-v.txt"
    # Each PCIe root complex (px nexus), its own properties only: bus-range
    # and the ranges that map PCI bus addresses to CPU addresses, plus its
    # minor nodes (dev_path names the root). A child node header, which is
    # indented 8 and does not end in a colon, ends the excerpt.
    section "PCIe root complex properties (px nexus)"
    awk 'function flush() { if (buf ~ /SUNW,sun4v-pci/) printf "%s", buf; buf = "" }
         /^    [^ ]/ { flush(); inpx = ($0 ~ /^    pci/); stopped = 0
                       if (inpx) buf = $0 "\n"; next }
         inpx && !stopped {
             if ($0 ~ /^        [^ ]/ && $0 !~ /:[ ]*$/) stopped = 1
             else buf = buf $0 "\n" }
         END { flush() }' "$TMP/prtconf-v.txt"
fi

section "Done"
