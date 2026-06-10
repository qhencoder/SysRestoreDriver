[中文版](ARCHITECTURE.md)

---

# Architecture Design

## Overview
This project is a system reboot-restore driver based on the Windows volume filter framework. It redirects all writes to the target volume, storing modifications in free sectors. After a reboot, those modifications are naturally discarded, thus restoring the system to its original state.

Currently only NTFS partitions are supported, and it has been validated on Windows 10/11. A companion MFC user-mode management application `QHEngineUI` provides one-click install / enable / disable protection.

## Core Design Ideas

**Why Volume Filter Instead of File Filter**

File filter drivers (minifilter) can only intercept file-level operations. Volume filter drivers sit below the file system, allowing them to intercept all block-level writes to a volume, including modifications to NTFS metadata (e.g., `$Bitmap`, `$MFT`). In a restore scenario, volume filtering provides more complete protection — all writes are redirected, and the original data remains intact after a reboot.

**Why There Is No Need to Maintain a Custom Free Bitmap**

`$Bitmap` is an NTFS file that records the allocation status of every cluster on the volume. Under the volume filter driver, any writes to `$Bitmap` are also redirected. Therefore, when the driver starts up, the on-disk `$Bitmap` is always a snapshot of "the moment protection was enabled" — it can be used directly to identify free sectors as redirect targets.

## Position in the Device Stack
File System Driver
|
This Driver (Volume Filter Layer)
|
Volume Device
|
Disk Driver

## Key Design Decisions and Evolution

### 1. Simplification of Bitmap Management

**Initial Idea**: Maintain a custom bitmap to track free-sector status. For a 200 GB C drive, the bitmap would require about 35 MB, and larger capacities would cause memory pressure. Thus, the plan was to store the bitmap in disk sectors.

**Final Approach**: The custom bitmap was completely abandoned. Because the volume filter driver redirects all writes (including modifications to `$Bitmap`), the original on-disk `$Bitmap` forever preserves the state it had when protection was enabled. The driver parses `$Bitmap` at startup to obtain cluster-level occupancy and expands it to a sector-level bitmap in memory.

**Reason for Change**: `$Bitmap` itself serves as a natural free-sector index; maintaining a separate one would be redundant.

### 1.1 Bitmap Implementation

The sector-level bitmap is a thin wrapper `QH_BITMAP` built on top of the Windows kernel `RTL_BITMAP` API, allocating one contiguous buffer of `SizeOfBitMap` bits at creation time.

**Reasons for choosing RTL_BITMAP**:
- **Compact code**: `Create/Set/Test/FindNextClear` are direct forwarders to `RtlSetBit` / `RtlClearBit` / `RtlCheckBit` / `RtlFindClearBits`. Since `RtlFindClearBits` has built-in wrap-around semantics, the upper layer needs no "scan again from zero" fallback
- **Sufficient capacity ceiling**: `RTL_BITMAP` indexes with `ULONG`, giving a single-bitmap ceiling of ~4G bits (about 2 TB for 512B sectors, 16 TB for 4 KB sectors), covering nearly all system-disk + data-disk scenarios. If even larger volumes become a requirement, a sparse slotted layer can be added inside `QH_BITMAP` without changing the upper interface
- **Reliance on standard kernel APIs**: `RTL_BITMAP` is a public Microsoft kernel API with documented behavior, eliminating the need to validate the correctness and stability of any custom bit-manipulation algorithm

**Memory cost**: allocating the buffer up front means a 1 TB volume needs roughly 256 MB for `SectorBitmap`. Target scenarios (system disks and data disks) rarely exceed 2 TB, so this is acceptable.

### 2. Data Structure Choice for the Redirect Mapping Table

**Initial Idea**: Use `RTL_DYNAMIC_HASH_TABLE` provided by the Windows kernel as the mapping table, leveraging its O(1) lookup performance.

**Evolution**:
- Attempted to port the open-source `khashl` hash table library to kernel mode
- Found `khashl` unstable in the kernel environment
- Eventually adopted `RTL_GENERIC_TABLE` (a Splay tree) as a transitional solution

**Final Approach**: The current version stores sector offset mappings in `RTL_GENERIC_TABLE`, with O(log N) lookup complexity. A more efficient custom hash table is planned for future implementation after stabilization.

**Reason for Change**: Stability takes precedence over performance. `RTL_GENERIC_TABLE` is a Microsoft-supported kernel data structure with guaranteed reliability.

### 3. Adjustment of Mapping Granularity

**Initial Idea**: Establish mappings at the cluster level (offset cluster → redirected cluster).

**Final Approach**: Establish mappings at the sector level (offset sector → redirected sector).

**Reason for Change**: Cluster-level mappings require a "read original cluster → modify → write new cluster" sequence when only part of the cluster is in use — one extra IO that halves performance. Sector-level mappings match IRP ranges precisely and avoid unnecessary copying.

### 4. The Problem of Redirect Space Exhaustion Due to File Deletion

**Problem Description**:

When protection is enabled, there is a free cluster Z. Original sector X is written to, and the driver redirects it to Z (X → Z). Later, the file system considers Z free and allocates Z to a new file. When the new file writes to Z, the driver has to redirect again (Z → W). When that file is later deleted, the `$Bitmap` (redirected view) marks Z as free, but the driver cannot sense this change; Z remains occupied as the redirect target for X.

Frequent deletions of large files can cause many sectors to become "locked" by the driver and never reclaimed, eventually exhausting the redirect space.

**Current Decision: Not handled for now**

Rationale:
- The scenario that exhausts redirect space requires many modifications to existing protected data combined with frequent file deletions. In typical real-world usage, the amount of writes during protection is limited
- Handling this would require an additional minifilter driver and cross-driver communication, increasing system complexity and instability risk
- If a 50 GB disk is protected and only 30 GB are in use, only modifications to that 30 GB are redirected. Writes to originally free sectors are passed through without creating mappings

This is a known limitation of the current version. It will be addressed in the future (likely via a minifilter intercepting file deletion events) if necessary.

### 5. Persisting the Protection State

After a reboot the driver must know "did the user have protection enabled last time?" This requires a persistence mechanism that **can pierce through redirection**.

**First-generation scheme (abandoned): custom sector**

Pick a sector inside the NTFS free space as a "custom sector"; write `1` / `0` to its first byte to encode enabled / disabled. Record the sector number in the registry.

The problem: NTFS has no idea this sector was hijacked by the driver. On the next boot the file system may already have allocated this sector to some file and written new data into it; the driver reads the sector by its recorded number and gets file content instead — completely wrong state (disable-protection never takes effect).

**Second-generation scheme (current): ProtectRanges + state file at the volume root**

Introduce `ProtectRanges` (a direct-passthrough sector range table): sectors recorded here have their reads and writes routed directly to the original disk location, bypassing redirection entirely. `ProtectRanges` is a small array filled once during initialization and read-only thereafter (capacity cap of 8 entries, typically only 3–4 used), scanned lock-free on the IRP fast path.

When enabling protection, the UI creates `_qh_protect_state.data` (1 MB, hidden + system attributes) at the root of each protected volume, with the first byte set to `1`. During initialization the driver queries the file's cluster layout via `FSCTL_GET_RETRIEVAL_POINTERS` and appends those sectors to `ProtectRanges`.

The invariants this gives us:
- The file is managed by NTFS itself, so it cannot be overwritten by anything else
- The file's sectors are recorded in `ProtectRanges`, so the UI's write passes straight through to the real disk — after disabling and rebooting, the driver reads `0`
- During initialization (`Initialized == FALSE`) every IRP is just passed through to the lower device, so `ZwReadFile` reads real on-disk content
- 1 MB is large enough to force NTFS into non-resident storage; the data is not stuffed into the MFT record (the resident-attribute threshold is ~700 B)

The registry is not involved at all. The same `ProtectRanges` mechanism also passes through `$Volume` (MFT #3, both main and mirror) so the NTFS dirty flag can land on disk — otherwise, after multiple reboots Windows accumulates "unclean shutdown" votes and enters WinRE.

**On why pagefile.sys / hiberfil.sys / bootstat.dat are NOT included**: an early version added these files to the direct-passthrough list, but they may be dynamically grown or shrunk from user mode (especially `pagefile.sys`, which Windows auto-resizes under memory pressure). Newly-allocated sectors would not be in the snapshotted passthrough list — writes to them would be redirected by the driver, the system would later read pagefile and get garbled data, leading to a BSOD or worse. Therefore the current version only includes entities whose sector locations stay static for the lifetime of a protected session (the state file and NTFS metadata).

### 6. Driver Initialization Timing

**Initial Idea**: Perform initialization upon receiving the `IOCTL_VOLUME_ONLINE` control code.

**Final Approach**: Register a `IoRegisterBootDriverReinitialization` callback and initialize after all boot drivers (including NTFS) have loaded.

**Reason for Change**: The state file must be opened through the file system, but the file system is not necessarily ready when `IOCTL_VOLUME_ONLINE` fires. The `BootDriverReinitialization` callback is guaranteed by the system to run after NTFS is mounted, which is the earliest safe time for file operations. For removable media (USB drives, etc.), initialization still runs in the `IOCTL_VOLUME_ONLINE` path, but protection is only activated once `BootReinitDone == 1`.

## Core Data Structures

### Sector Bitmap + Direct-Passthrough Range Table

For each protected volume the driver maintains one `RTL_BITMAP`-backed bitmap and a small array embedded statically in the device extension:

| Structure | Meaning | Source |
|---|---|---|
| `SectorBitmap` (`QH_BITMAP` thin wrapper) | Sector-level occupancy | Expanded from the cluster-level `$Bitmap` |
| `ProtectRanges[8]` (embedded in DEVICE_EXTENSION) | Direct-passthrough sector ranges (state file + `$Volume` MFT#3 main/mirror) | Filled once by `QHPopulateProtectRanges` at init; lock-free read on the IRP path |

### Sector Redirect Mapping Table
- **Implementation**: `RTL_GENERIC_TABLE` (Splay tree)
- **Key**: Original sector index
- **Value**: Redirected sector index
- **Purpose**: Both the "does a redirect exist?" check and the target lookup — read and write paths query this table directly; a miss means "no redirect, use the original location"

## Complete Flow of a Key Operation

### Write IRP Handling

1. Application initiates a write; the IRP reaches the volume device stack and the driver's `IRP_MJ_WRITE` handler intercepts it
2. If `Initialized == FALSE`, pass through to the lower device
3. If not at `PASSIVE_LEVEL`, queue to the work item (NTFS file APIs require `PASSIVE_LEVEL`)
4. Boot-sector write protection: writes at offset < `BytesPerSector` are rejected with `STATUS_ACCESS_DENIED`
5. Range fast-path: if the whole range is in `ProtectRanges` or entirely free, pass through directly, skipping buffer allocation
6. Copy user data into a separate buffer (avoids PFN_LIST_CORRUPT)
7. Classify sector by sector:
   - `ProtectRanges` hit → write the original location (**full passthrough**)
   - `SectorBitmap` says free → write the original location (after reboot `$Bitmap` is restored and the data is naturally discarded)
   - Already-occupied sector → atomically inside `OffsetHashMutex`: "look up → on hit, write target / on miss, allocate a new free sector and insert into the hash table"
8. Physically contiguous sectors are coalesced into a single IO

### Read IRP Handling

1. Fast scan: if the entire range is fully in `ProtectRanges` or has no redirects, pass through directly
2. Otherwise, per sector:
   - `ProtectRanges` hit or hash-table miss → read the original location
   - Hash-table hit → read the target location
3. Contiguous IOs are coalesced; result is copied back to the original buffer

### Reboot Restoration Mechanism

After a reboot, all in-memory bitmaps and the offset table are lost. When NTFS remounts the volume it reads the true on-disk data — and `$Bitmap` has never actually been modified since protection was enabled (all writes were redirected), so the entire NTFS view returns to "the moment protection was enabled". All sectors written during redirection still physically exist on disk, but NTFS does not know they were ever used; they will naturally be overwritten as `$Bitmap` later allocates them.

## Known Limitations

1. **NTFS only**: FAT32, exFAT, and other file systems are not currently supported
2. **Space exhaustion from file deletions not handled**: See Design Decision #4
3. **Not tested on Windows 7/8/8.1**: Theoretically compatible but not verified
4. **Temporary mapping structure**: The current Splay tree is not optimal for lookup performance; a custom hash table is planned
5. **State file can be deleted by the user**: Deleting it is equivalent to disabling protection; no delete-monitoring is in place

## Future Plans

- Implement a high-performance kernel-mode hash table to replace `RTL_GENERIC_TABLE`
- Use a minifilter to monitor the state file and block accidental deletion
- Research NTFS metadata semantics to optimize free-sector management
- Evaluate adding support for FAT32 / exFAT

## Appendix: Optimizations Tried and Abandoned During Development

Notes on the things I tried while tuning write performance during development, so
nobody (including future me) gets fooled by the same ideas twice.

Test setup: CrystalDiskMark 8.0.4, 1 GiB × 5 runs, C: 60GB NVMe SSD, 38% used.

### Starting point

Early-build write performance: 676 MB/s with protection off, dropping to 385 MB/s
with protection on — a 43% loss. Reads were basically untouched. The slow path
was acquiring a fresh `FAST_MUTEX` for every sector, which means up to 8000 lock
acquires per 1 MiB write — looked like the obvious culprit.

### What stuck: whole-range fast-path check at the write entry (+5%)

When a write IRP arrives, peek with `RtlAreBitsClear` and `QHAreSectorsProtected`
to see whether the entire range is either:
- Fully covered by `ProtectRanges` → pass through
- Fully free in `SectorBitmap` → mark the range used and pass through

When either condition hits, we skip the 1 MiB `qhalloc` + `memcpy` and the slow
per-sector loop entirely. About 30 lines added at the top of `QHIrpDispatchWrite`.

Measured at +5% — much less than expected, but the code is simple and the risk
is low, so it stayed.

### What I tried and reverted

**Whole-range fast-path check on the read path too** — symmetric idea to the write side.
SEQ Q1 Read regressed by 5–11% and I never pinned down why; maybe
`RtlAreBitsClear` over a large range is actually slower than the bit-by-bit
`Test` in the low-redirect-density case. Reads only lose a few percent to begin
with, so optimizing them wasn't worth the uncertainty. Reverted.

**Lock consolidation in the slow path** — collapse the 3-4 `BitmapMutex`
acquires per sector into one big critical section. Should be a clear win on
paper. Made zero difference in the CDM benchmark. `FAST_MUTEX` is just too cheap
on this machine for the lock loop to be the bottleneck. Added complexity for
nothing, reverted.

**Async IO pipeline** (this is where I spent the most time) — built a batch
struct with a completion routine, reference counting, and slot limiting
(MAX_INFLIGHT=16) so that multiple non-contiguous segments within one write IRP
could ride the storage queue in parallel. The theory says total latency drops
from sum(per-segment) to max(per-segment). Result: +5%.

Why so little? Because `LastScanIndex` makes successive redirect targets
contiguous on disk, so after IO merging there's typically only one segment per
IRP. Nothing to run in parallel. CDM's "preallocate then rewrite" pattern is
particularly hostile to this optimization.

I don't think this path is dead — workloads like "unzip thousands of small
files" are genuinely multi-segment and could benefit — but it needs dedicated
stability testing before shipping, and CDM can't show that anyway, so I removed
it for now.

### A few things I figured out

1. **CDM SEQ writes don't represent all write workloads.** Their
   characteristics make control-flow optimizations essentially useless:
   - 1 MiB chunks → IO merging is nearly perfect
   - File is preallocated, then rewritten 5 times → from round 2 onwards
     everything hits the slow path
   - Redirect targets are contiguous → physically still sequential writes

2. **The real cost of "protection on" writes isn't control flow.** Of the
   remaining latency, the big chunks are:
   - The defensive 1 MiB `qhalloc` + `memcpy`
   - Each segment's synchronous `IoCallDriver + KeWait` round trip

   Optimizing locks and branches without touching these two doesn't help.

3. **The buffer scheme cannot be touched.** I tried removing the defensive
   `qhalloc + RtlCopyMemory` and passing the parent IRP's MDL system address
   directly down — BSOD. That path is closed, don't try it again.

### Directions I haven't tried

- Replace the Splay tree with a per-bucket-locked chained hash table (already in
  Future Plans). Should be better for concurrent writes, but CDM single-threaded
  can't show it
- Swap `FAST_MUTEX` for `ERESOURCE` so the bitmaps support concurrent readers —
  might help Q8 multi-threaded reads
- Tune the `LastScanIndex` strategy to keep redirects more compact — but this
  could break IO merging, careful
- Rewrite the whole thing as a proper IRP-pipelined async model — large
  engineering effort, don't even start without WinDbg-class debugging tools

