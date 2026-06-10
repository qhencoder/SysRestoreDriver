[中文版](README.md)

---

# SysRestoreDriver

An independently developed Windows volume-level **reboot-restore driver** based on copy-on-write redirection. While protection is enabled, every write to the system disk is redirected to a free sector — **after a reboot the disk automatically returns to the state it was in when protection was turned on**. The same idea as the classic "restore card" used in internet cafés and classrooms, implemented purely in software.

A companion MFC user-mode management application `QHEngineUI` is included. It embeds the driver files as resources in the exe and provides one-click install, volume selection, and enable/disable protection.

![Application UI](images/mfc_ui.png)

> ## ⚠️ Data Safety Warning
>
> This project is a **kernel-mode filter driver that attaches directly to the volume device stack** and redirects the target of every disk write while protection is active. Please read before using:
>
> - **Always test in a virtual machine or on a disposable test machine first** — do not deploy on a machine with important data on first try
> - **Take a full backup before trying** (system image + important data). An unsigned kernel driver may BSOD on untested hardware / AV / security software combinations
> - **Disabling protection requires a reboot** to take effect (see FAQ Q4). Never force-power-off while protection is active — `$Bitmap` is in the redirected view and a hard cut may corrupt the file system
> - **Do not manually delete the state file `_qh_protect_state.data`**. Deleting it is equivalent to disabling protection, and all writes made during the protected session are lost on the next boot
> - **If the next boot lands in the WinRE recovery environment**, use the commands in the "Recovery if the reboot lands in WinRE" section below
> - This project is licensed under the Apache License 2.0 and provided **"AS-IS"** — no liability for any data loss or hardware failure arising from its use

## Use Cases

- **Shared machines**: internet cafés, classrooms, libraries, demo units at trade shows — every reboot returns to a clean state
- **Software trial sandbox**: install random software, mess with the registry, delete system files — reboot wipes it all
- **Misuse protection**: a machine you give to elderly parents or children — no matter what they do, a reboot fixes it

NTFS partitions only. Tested on Windows 10 / 11. This project does not reference any Microsoft driver samples and was built from scratch using WDK header files only. Open-sourced under the Apache License 2.0.

## Project Status

Current version: v0.1 (first release; core functionality complete and tested: enable/disable protection, multi-volume selection, reboot-restore all working end to end)

## Dependencies

**Zero third-party dependencies, pure WDK.** The driver links only against the kernel libraries shipped with the Windows Driver Kit (`ntoskrnl`, `hal`, etc.) and pulls in no third-party open-source components — an earlier attempt to port klib's khashl hash table was dropped due to stability issues, which also incidentally avoided any potential third-party license contamination.

The project has no transitive dependencies and can be safely integrated into any commercial or open-source product.

## Supported Platforms

- Windows 10/11: tested
- Windows 8.1/7: theoretically compatible, not yet tested
- NTFS partitions only

## Build Environment

- Microsoft Visual Studio Enterprise 2022 (64-bit) 17.14.31 (April 2026)
- Windows SDK 10.0.26100.0
- Windows Driver Kit (WDK) 10.0.26100.0

## Build Steps

1. Open `SysRestoreDriver.sln`
2. Select Release / x64
3. Build the solution

Build artifacts:
- `SysRestoreDriver.sys` — the driver binary
- `QHEngineUI.exe` — MFC user-mode management application (with .sys and .inf embedded as resources)

## Deployment and Loading

1. Enable test signing mode (administrator terminal): `bcdedit /set testsigning on`
   > The current build is not code-signed and can only load in test mode. A "Test Mode" watermark will appear at the bottom-right of the desktop — this is expected. Production deployment requires an EV code signing certificate for kernel driver signing.
2. Reboot
3. Run `QHEngineUI.exe` as administrator
4. Select volumes to protect in the list, then click "Enable Protection"
5. Reboot — protection is now active

To disable: click "Disable Protection" in `QHEngineUI.exe`, then reboot. Normal read-write access is restored.

### Recovery if the reboot lands in WinRE

When enabling protection, the UI calls `bcdedit` to configure the boot failure policy to avoid entering WinRE. If that configuration fails or you somehow end up at the Windows Recovery Environment, run the following from a WinRE command prompt to recover:

```
bcdedit /set {default} bootstatuspolicy DisplayAllFailures
bcdedit /set {default} recoveryenabled Yes
```

## Protection State File Scheme

To solve the problem of persisting protection state when the volume filter driver is active, this project uses a **file-based scheme**:

- When enabling protection, the UI creates `_qh_protect_state.data` (1 MB, hidden + system attributes) at the root of each protected volume, with the first byte set to `1`
- This file is added to the driver's `ProtectRanges` (direct-passthrough sector list); reads and writes to it pass straight through to the real disk and are never redirected
- On boot, the driver reads the file's first byte to decide whether to activate protection: `1` = enabled, `0` = disabled, file missing = not configured
- When disabling, the UI overwrites the first byte to `0`; after reboot the driver reads `0` and skips activation

The protection state does not depend on the Windows registry. See section 5 of [Architecture Design](ARCHITECTURE_EN.md) for the full rationale.

## Test Records

| System Version | NTFS Version | Scenario | Result |
|---|---|---|---|
| Win10 22H2 | 3.1 | Normal write then reboot | Passed |
| Win11 23H2 | 3.1 | Simulated unexpected power loss then reboot | Passed |
| Win11 23H2 | 3.1 | Enable/disable protection reboot cycle | Passed |

## FAQ

### Q1: How big is the performance hit?

Measured with CrystalDiskMark 8.0.4 on Win10 22H2 / NVMe SSD / 60 GB system disk (1 GiB × 5 runs):

| Test | Protection OFF | Protection ON | Change |
|---|---|---|---|
| SEQ Q8 1MiB **Read** | 1753 MB/s | 1510 MB/s | −14% |
| SEQ Q1 1MiB **Read** | 1170 MB/s | 685 MB/s | **−41%** |
| RND Q32 4KiB **Read** | 18.3 MB/s | 18.5 MB/s | +1% (noise) |
| RND Q1 4KiB **Read** | 11.2 MB/s | 10.8 MB/s | −4% |
| SEQ Q8 1MiB **Write** | 673 MB/s | 436 MB/s | −35% |
| SEQ Q1 1MiB **Write** | 916 MB/s | 373 MB/s | **−59%** |
| RND Q32 4KiB **Write** | 19.2 MB/s | 10.1 MB/s | −47% |
| RND Q1 4KiB **Write** | 10.9 MB/s | 10.2 MB/s | −6% |

<details>
<summary><b>Reproduction</b></summary>

- Tool: CrystalDiskMark 8.0.4 x64, Profile = Default, Test = 1 GiB × 5 runs, 5-second interval
- Target volume: system disk C: (NVMe SSD), used capacity 39% before / 38% after (essentially identical)
- Procedure:
  1. With protection disabled, reboot the system, wait 60 seconds after desktop is ready, then run the first measurement
  2. Enable protection via `QHEngineUI`, reboot the system, wait 60 seconds after desktop is ready, then run the second measurement
- Both runs were executed as Administrator with antivirus real-time scanning disabled to avoid extra IO interference

</details>

**Conclusions**:
- **Read path**: Low-queue-depth sequential reads take the biggest hit (SEQ Q1 about −41%), bottlenecked on the per-sector Splay-tree lookup that decides redirection. At higher queue depths concurrency hides the cost, shrinking the loss to −14%
- **Write path**: Sequential large writes take a notable hit (about −35%), bottlenecked on the defensive 1 MiB buffer copy and the synchronous wait for each IO segment
- **Small IOs are essentially lossless**: Q1 4KiB read/write both < 6%

No comparison against commercial counterparts (Deep Freeze / PowerShadow / etc.) is provided — these vendors do not publish benchmark data and third-party independent reviews are scarce, so no reliable reference exists. If you have reproducible comparison numbers, feedback is welcome.

Future optimization directions: several were tried during development (lock consolidation, whole-range fast-path check on reads, asynchronous IO pipeline) — none of them moved the needle for CDM SEQ workloads. See the [ARCHITECTURE.md appendix](ARCHITECTURE_EN.md#appendix-optimizations-tried-and-abandoned-during-development) for the full story. The real bottlenecks are the defensive buffer copy and synchronous IO waits — those are where future work needs to focus.

### Q2: How much memory does it use?

Memory usage depends on **volume size** and **actual write volume**:

- **Sector bitmap** (`SectorBitmap`): a thin wrapper over the Windows kernel `RTL_BITMAP`, with the entire buffer allocated up front based on the volume's total sector count
  - Per 1 TB volume ≈ 256 MB (at 512B sectors, one bit per sector)
  - Per 100 GB volume ≈ 25 MB
  - 60 GB system disk ≈ 15 MB
- **Direct-passthrough range table** (`ProtectRanges`): a small array embedded statically in DEVICE_EXTENSION, holding only the sectors of `_qh_protect_state.data` and `$Volume` MFT#3 (main + mirror). Fixed ≤ 128 bytes
- **Offset mapping table** (Splay tree): about 40 bytes per entry. 1 million first-time redirects ≈ 40 MB
- **Typical scenario** (200 GB system disk, 5 GB written during a session): total memory ≈ **70–100 MB**

All driver memory is allocated from `NonPagedPool` (non-pageable), which is precious kernel memory. Be mindful in extreme write workloads.

### Q3: Can it protect non-system drives or multiple volumes?

**Yes**. The driver registers as a Volume Upper Filter (see `FilterClass = Volume` in [SysRestoreDriver.inf](SysRestoreDriver/SysRestoreDriver.inf)) and attaches to every NTFS volume mounted by the system:

- System disks, data disks, external drives, USB sticks — all supported
- Each volume is evaluated independently: protection is activated only if the volume root contains `_qh_protect_state.data` with first byte = 1
- Removable media (USB drives, etc.) is initialized separately via the `IOCTL_VOLUME_ONLINE` path
- The UI lists all volumes for selection; unselected volumes are passed through unchanged

**Note**: NTFS only. FAT32 / exFAT volumes are skipped (the driver can't parse `$Bitmap`).

### Q4: Can protection be disabled without rebooting?

**Current version: no. Disabling protection requires a reboot to take effect.**

The reason is rooted in the architecture itself:
- While protection is active, on-disk metadata (`$Bitmap`, `$MFT`, etc.) is "frozen" at the moment protection was enabled
- The in-memory offset table records which sectors are redirected to where; the file system and applications see the merged view
- If the driver were unloaded at runtime → the file system would suddenly see the true on-disk data (the snapshot from when protection was enabled) → all currently open files become corrupted → **guaranteed BSOD**
- The only safe disable path: UI writes 0 to the state file's first byte → reboot → driver reads 0 during init → does not activate → normal R/W on the whole volume

In theory, an "online commit" path could be implemented — write all in-memory redirects back to original sectors and then unload. But this would bring:
- Full-volume IO pause (essentially scheduled downtime)
- Power loss during commit would corrupt the file system
- Once committed, no rollback is possible

Given the target use cases (internet cafés, classrooms, software trial sandboxes) explicitly market "reboot for a clean slate", reboot-to-disable is a deliberate product design choice. Online commit is not planned.

*Note: The above tests were performed in a fixed-configuration virtual machine and lasted less than 10 minutes. They do not constitute any stability or compatibility guarantee.

## Known Limitations

- NTFS partitions only; other file systems (FAT32, exFAT, etc.) are not yet supported
- The issue of redirect space exhaustion caused by file deletion is not yet handled
- **The state file `_qh_protect_state.data` can be deleted by the user** — deleting it is equivalent to disabling protection; no delete protection is in place (a minifilter-based monitor is being considered)
- Currently uses a Splay tree (`RTL_GENERIC_TABLE`) for sector mapping storage, which is not optimal for lookup performance; a custom hash table replacement is planned

## Copyright and License

This project is open-sourced under the Apache License 2.0, copyright owned by Xuhui Jiang.
See the [LICENSE](LICENSE) file for the full license text and the [NOTICE](NOTICE) file for project attribution information.

## Contact

Questions, feedback, and pull requests are welcome via GitHub Issues.

## Related Documents

- [Architecture Design](ARCHITECTURE_EN.md)
