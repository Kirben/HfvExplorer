HFVExplorer 2.0.0
-----------------
Target: Windows 7 SP1 with KB2533623 or a superseding update; Windows 8/8.1,
10 and 11 x64. Build with pinned v143 14.44 tools and matching static MFC/CRT.
Build and run checks with build-x64.ps1. Older Windows versions still await
VM execution tests. The original readme
below describes the historical release and its obsolete drivers.

Per-window browser sessions
---------------------------

Each browser document owns a `BrowserSession`, its native HFS+/HFSX/MFS
controller, mounts and folder navigation. Tree, list, preview and
status updates resolve through the receiving session rather than the
application's current-pane pointers. Classic HFS/Windows locations, selection
and sort settings stay with their document and views; Finder layout and classic
icon tables are independent too. Storage-backed directory jobs receive their
owning frame explicitly, including during startup before MFC sets `m_pMainWnd`.

Tools > New browser (Ctrl+Enter, also Ctrl+N) creates an independent top-level
window with its own document and panes. Menus, accelerators, file/image opening,
launchers, folder navigation and view commands target the receiving window.
Closing a browser closes only that window; closing the first window transfers
application ownership to a surviving frame. File > Exit closes all windows.
Window titles show the current Windows folder path or Mac disk/folder followed
by HFVExplorer, independently in each browser window, including Unicode names.
Ordinary Copy/Paste and folder drops now run on a separate copy queue with a
non-modal progress window. The conversion-mode picker is also non-modal; it
owns captured source/destination requests and supports OK to all without
changing an active job's conversion settings. Closed classic/floppy readers
reopen when their Macintosh entry is selected after copying. Windows folders and unrelated mounted HFS+/HFSX/MFS
images remain browsable in the same window. Navigation on a reserved Mac volume
shows a stable status message and resumes after the transfer; navigating elsewhere
cancels that pending navigation. Directory copies retain overall/file progress,
and single-file copies retain one progress bar. Overwrite decisions affect the
copy window while browser windows remain enabled.

Volume reservations cover writes, finalization and move-source cleanup.
Ordinary floppy copies retain the original fast path. A floppy Cut/move reads
back changed sectors before removing its source, then finalizes any source-floppy
deletion separately. All classic adapters close before destination finalization.
Non-conflicting same-volume HFS/HFS+/HFSX moves rename catalog entries in place,
preserving their IDs and avoiding a data copy; overwrite/merge handling is retained.
Queued browser requests retain the selected floppy's media-change counter, so
a reported media swap during conversion questions rejects the old request.
Classic HFS uses explicit mount/error/undo contexts, and converters keep their
mutable settings, callbacks, CRC state and temporary filenames private to each
executing thread. Captured classic adapters retain their contexts across worker
migration. Exports use absolute destination paths without changing the process
working directory. CD/floppy caches and descriptors have per-device locks.

Up to four file-operation jobs can execute concurrently on unrelated resources. Requests
that conflict with a reserved volume remain queued without occupying a worker;
all participating resources are acquired together. Classic HFS/physical-media
navigation defers only for the selected busy volume. Windows directories and
unrelated classic/HFS+/HFSX/MFS images remain browsable. New browser is available
during transfers; its initial tree retains busy images without opening them.
Each copy keeps its own progress/overwrite/cancellation UI. The taskbar combines
active job progress per browser, so completing one job does not clear another.

Command availability now follows the selected or chosen volume rather than
whether any background job exists. Options and source/destination pickers remain
available. Eject/reload require the selected device to have no active readers or
writer; imaging, formatting, legacy converter helpers and CD recording acquire
atomic source/destination reservations before accessing their media. Recorder
aliases use the same drive identity as browsing and eject/reload. Each operation
rechecks at execution, including conflicts that arise while a dialog is open.

Unrelated image opening extends the existing tree while work is active. Refresh
and media scans leave busy mounts intact and skip only busy drives. Legacy
association searches also skip reserved images. Releasing a CD closes idle native
readers in every browser, including backends retained by the application clipboard;
other listings remain intact. Released clipboard sources must be selected again.
File pickers preserve the process working directory. Name/file selection and
legacy device/format wait dialogs retain their existing modal behavior; this does
not make every storage command a non-modal job.

Windows-file reservations cover affected files and directory subtrees rather
than their entire drive. Independent files and folders can run concurrently,
including known-name copies into the same folder. Parent rename/deletion,
resource companions and temporary publication files remain protected. Canonical
paths, file IDs, hard links and junction aliases share reservations; mounted
images also conflict with raw access to their image file or containing folder.
Directory browsing can refresh sibling changes during an active copy. Path
identification runs on workers, and command checks use cached browser identities.
Legacy conversions with unresolved output names reserve their output folder;
unresolvable identities fall back to conservative host/device reservations.
Mac filesystems and physical media retain volume/device-level coordination.

`--verify-windows-path-coordination NEW_DIRECTORY` checks same-drive parallel
copies and mutations, parent/child exclusion, aliases, publication and staging
protection, targeted refresh, slow path identification, failure and cancellation.
It uses hidden synthetic browser windows and files, without physical media.

`--verify-per-volume-commands NEW_DIRECTORY` tests real multi-window command and
menu-update routing during a gated copy, late source/destination conflicts,
recorder aliases, simulated eject/imaging/floppy writes, resource cleanup, idle CD
reader release, skipped busy scan targets and incremental image opening. Test
builds use only injected probes and never scan physical user drives.

Windows browsing uses independent metadata readers and generic icons
during copying. Browser windows can close independently while copies continue;
progress/taskbar ownership moves to a surviving browser. Physical-reader
resources that could block on active I/O are retained without a window until
the copies are idle, then released on the UI thread. Every main browser X/Alt+F4 close asks for confirmation. Closing the last browser
while copying asks before cancelling outstanding copies and waits for safe
cleanup before exiting. File > Exit still cancels all copies and closes every
browser after cleanup. Legacy emulator
temporary-file helpers remain synchronous because their callers need completed
files immediately. Copy jobs do not hold browser/document pointers, and Cut is
cleared only after successful completion.

Copy/Cut/Paste and their Append commands share an application clipboard between
HFVExplorer windows. Captured locations own their backend references or absolute
paths and source identity; closing/navigating the source window does not change
the clipboard. Completed cuts are consumed only after successful transfer;
cancelled, skipped and failed pastes retain them. Escape clears the clipboard.

Drag selected files/folders from a list into another HFVExplorer window's tree
folder, list folder or list background. Dropping defaults to moving on the same
volume and copying across volumes; Ctrl forces copy and Shift forces move.
Read-only sources can be copied, and read-only destinations reject drops. Escape
or capture loss cancels a drag without changing the clipboard. Existing local
icon positioning remains available when copying is idle; folder drops use the
same captured background jobs. Drop highlights do
not navigate the destination, and are removed on release/cancellation.

Transfers use the existing independent jobs, overwrite prompts, fork/metadata
preservation, volume coordination and refresh notifications. Legacy HFS/Windows
import/export conversion preferences remain active; same-format transfers use
the coordinated adapters, including classic volumes with identical labels.
Converted PC-to-HFS imports also ask before replacing the resolved Mac filename,
including embedded MacBinary/BinHex names. Skip/Cancel retain the old file.
Windows source lookup resolves only the selected entry, so unrelated junctions
in its parent folder cannot prevent copying or overwrite prompts. Selected
reparse points remain rejected; those errors now identify the offending path.
Selecting a removable drive explicitly probes for classic HFS before Windows
folder enumeration, even when startup floppy detection is disabled.

This clipboard and cross-window drag support are internal to HFVExplorer; they
do not add Windows Explorer/OLE clipboard interchange.

X/Alt+F4 confirmation is limited to file-operation progress/copy-mode dialogs
and every main browser window, defaulting to No. About, Options and other
ordinary dialogs close normally. Completed progress windows also close without
confirmation. Confirming an active progress X requests cancellation and keeps
progress open until cleanup finishes. Copy-mode X confirms cancellation before
I/O begins. Explicit OK/Cancel buttons keep their existing behavior, and
non-cancellable waits keep their X disabled. Internal shutdown cleanup does not
open additional confirmation questions.

`--verify-background-window-close NEW_DIRECTORY` tests immediate first/main and
successive browser closure during a stalled clipboard Cut, command routing,
progress/taskbar migration, cancellation/overwrite/error handling after closure,
copy-mode lifetime, physical-reader retirement while a device read is blocked,
and final-window cancellation of running, queued and mode-pending work. It also
checks Yes/No title-bar close decisions on idle/non-last/last browsers and active
copy progress/mode dialogs, plus normal closure without confirmation on
About/General/Options property-sheet and completed progress windows. No physical devices or
computer control are used.

`--verify-independent-storage NEW_DIRECTORY` interleaves real classic raw and
MacBinary converters, BinHex encoders, classic undo contexts, browser navigation
and simulated physical drives. It tests distinct mount/settings/callback/CRC
state, resource forks, stable process paths, adapter migration, same-volume
exclusion, seven conflicting queued jobs without worker starvation, independent
cancellation, New browser during copying and aggregate taskbar progress. Tests
use hidden synthetic windows and injected devices; no physical media is opened.

`--verify-floppy-copy-regressions NEW_DIRECTORY` uses real hidden browser
clipboard commands and injected floppy APIs. It covers lock/write/short-write/
flush/readback failures, reported media changes, source/Cut retention, Cancel
during readback, native and same-floppy folder moves (including resource forks),
physical-source move cleanup, per-file mode cancellation and closing the last
browser while a conversion question is pending. No physical drives are opened.

`--verify-background-copies NEW_DIRECTORY` exercises a gated slow read while
browsing Windows/unrelated images in the same window; busy-volume deferral and
stale-navigation cancellation; overwrite and Cut completion; read errors,
replacement rollback, queued cancellation, safe frame close; and simulated
physical floppy write/readback/unlock. Tests use hidden synthetic windows and
image fixtures, without physical drives or computer control.

`--verify-browser-transfers NEW_DIRECTORY` covers shared clipboard routing,
mixed-format append, overwrite decisions, source-window closure, native forks
and Finder metadata, tree/folder/background drops, same-volume moves, read-only
volumes, stale selections, descendant rejection and replaced classic images.

`--verify-browser-windows NEW_DIRECTORY` tests actual hidden frames, documents,
menus/accelerators, explicit command routing, failed startup, busy guards,
closing the first/main frame, surviving-window commands and final shutdown.

`--verify-browser-sessions NEW_DIRECTORY` in the Test build exercises two hidden
sessions with conflicting application aliases, native navigation and clipboard,
Windows folders, same-named classic images, slow reads, error recovery, pane
ownership and teardown. It also creates a real hidden frame and splitters before
the application main-window pointer is assigned. No physical devices are used.

Independent file-operation jobs
-------------------------------

Copy, rename and delete commands submit value-based `FileOperations::Request`
objects to independent coordinated workers. Requests own their paths, IDs, conversion
settings and backend references; they do not retain browser/window/control
pointers. Closing a submitting session cannot invalidate queued work. Progress,
overwrite decisions and cancellation belong to the job; a progress dialog only
observes it. An application-wide pending registry allows another consumer to
find jobs/conflicts, and application shutdown cancels pending jobs.

Classic MacBinary/BinHex/text/AppleDouble converters remain active, with import
and export settings captured per request and restored after execution. Native
fork import/export, safe host deletion and classic HFS mutations use the same
submission path. Cut sources are removed after successful destination
finalization and revalidation; skipped copies retain their sources. Completion
records distinguish incomplete work and include potentially changed resources
without keeping completed jobs' volume handles alive. Classic HFS rows carry
captured full paths so operations do not reconstruct paths from a later tree
selection.

Copying, renaming and deleting use non-modal background jobs. Browsing unrelated
volumes and Windows folders remains available while they run. Rename/delete
progress has one activity bar, a Cancel button and operation-specific errors.
Displayed entries refresh from the filesystem after completion, including partial
deletions. Cancel stops before the next mutation; completed deletions are not undone
and an atomic rename already in progress may finish.
`--verify-background-mutations NEW_DIRECTORY` tests slow rename/delete, cancellation,
failures and partial deletion, owner closure, volume exclusion, refresh, captured
Windows paths/identities, long names/resource companions and classic HFS commands.
Same-volume writers remain mutually exclusive; this does not enable concurrent
uncoordinated access to one filesystem.
Folder creation, Properties reads, classic Finder-property edits, label/icon-position
updates and native resource-fork imports also use independent background jobs.
Properties windows are modeless and can outlive the submitting browser. HFS+/HFSX
and Windows Properties expose extended attributes and Mac ACL metadata; classic HFS retains
its editable property fields. Applying an editor rejects stale metadata and
combines a name change and attribute edits in one runtime undo transaction.
Label and icon-position jobs patch only their own fields. Folder creation keeps
its captured parent path/catalog identity and rejects replaced Windows parents;
directory timestamp changes alone do not invalidate the parent. Folder-name/file
selection prompts still precede submission. Formatting keeps its existing path.
`--verify-background-metadata NEW_DIRECTORY` covers slow/error/canceled operations,
modeless result lifetime, stale editors/destinations, resource-fork preservation,
classic transaction collisions, simulated floppy finalization and last-window
cancellation. Device calls already in progress still need to return before cleanup.

Run the Test build with `--verify-file-operations NEW_DIRECTORY` for detached
session lifetime, queued values/settings, slow/error reads, conflicts, sticky
cancellation, stale rename rejection, successful/skipped moves, partial deletes,
real journaled HFS+/HFSX, classic HFS and Windows operations. Tests include hidden
progress/owner restoration and classic recursive-delete cancellation, with no
physical media access. `--verify-shared-browser` also exercises actual classic
HFS and Windows view rename/delete adapters with unrelated tree selection.

Volume coordination and session refresh
---------------------------------------

File-operation requests acquire all participating volumes together: sources are
read resources, destinations/moved sources are write resources, and duplicate
resources merge to the stronger access mode. Conflicts fail as busy before any
mutation. The copy scheduler runs unrelated volumes concurrently, while same-resource
requests wait for atomic reservations. General synchronous storage work retains
its separate worker.

Images use Windows file identity, including case/path and hard-link aliases.
Windows folders use their host-volume identity; physical media use the device
identity. Native HFS+/HFSX/MFS image browsers share a weakly cached backend in
this process, preserving the existing cross-process writable-image exclusion
and avoiding separate stale native caches. The final browser/job reference
releases the backend; the coordinator does not hold an idle writer open.

Completed operations publish revision changes for resources that may have been
modified, including on partial failure. Each browser session has a private
message-only mailbox: worker threads never call a browser object. Notifications
coalesce, defer during storage dialogs/writers/editing, and refresh affected
active views on the UI thread without progress popups or focus changes. Snapshots acknowledge the revision actually read,
so an explicit originating-window refresh avoids duplicate work without losing
changes arriving between reading and subscribing. Native views retain their
folder, surviving selection and sort order; a deleted current folder falls back
to a surviving ancestor. Classic HFS cached readers are invalidated/reopened,
and legacy Windows/classic views use the same notification delivery. Inactive
legacy roots discard stale child paths and refill when selected; read errors
clear the affected listing and retry on a subsequent change.

This is an in-process operation/refresh service, not an external filesystem
watcher. Independent browser windows use this service. Test with
`--verify-volume-coordination NEW_DIRECTORY`; the suite covers access conflicts,
shared aliases, coalescing, busy deferral, late subscriptions, partial failures,
folder rename/deletion, Windows/classic refresh and closed sessions with queued
messages. All browser windows used by tests are hidden; no physical media is
accessed. Startup tests also cover disabled floppy detection, choosing a fixed
Windows root instead of probing an empty removable drive automatically.

Storage ownership
-----------------
- Writable HFS+/HFSX image handles and classic HFS image sessions deny other
  writers through Windows file sharing, including alternate paths and hard links.
  Conflicting HFVExplorer image sessions fail as busy; they are not silently
  downgraded into a stale read-only view. Read-only image sessions can share with
  other readers. Classic HFS operations retain their lock for the mounted session;
  the classic GUI's separate preview/listing handle is not a writable session.
- Ordinary floppy writes, formatting and full-image writes share a cross-process
  guard keyed by the resolved device, acquired before probing/opening a writer.
  The native Windows volume lock remains mandatory before dismount or writes.
  Another HFVExplorer writer fails without touching media; locks release on close
  or process termination. This does not make interrupted writes crash-atomic.
- Tests include actual child-process image conflicts and simulated floppy APIs;
  the exclusivity test never opens a physical floppy. Run the standalone GUI test
  build with --verify-storage-exclusive NEW_OUTPUT_DIRECTORY, or build-x64.ps1 -Test.

Shared browser presentation
---------------------------
- Classic HFS, Windows, HFS+, HFSX and MFS listings use the same typed row model,
  renderer and sorter. Sorting uses exact fork sizes and FILETIME precision,
  rather than parsing rounded KB strings. Dates share a local-time display format.
- Classic HFS and Windows directory backends prepare snapshots on the serialized
  storage worker; failed reads do not publish a partial list. Classic HFS browsing
  uses read-only enumeration. Windows folders no longer open every child directory
  just to count its entries. Validated Windows AppleDouble fork/Finder metadata
  uses the same descriptor as cross-format transfers.
- Backend bindings preserve classic Finder icons/layouts, MacBinary/BinHex copy
  modes, legacy properties and emulator launching. These operations retain their
  existing format-specific rules. Read-only directory/entry capabilities are
  supplied by backends and checked by the shared view and edit-menu policy.
- --verify-shared-browser in the test build exercises real hidden tree/list
  controls, classic HFS and sparse 6 GiB Windows files, exact size/date sorting,
  read-only browsing, slow/error snapshots, owner restoration and classic
  conversion round trips, without discovery scans or physical media access.

Current filesystem support
--------------------------
- Classic HFS: logical forks up to 2 GiB minus one byte, and image volumes up
  to 2 TiB minus 512 bytes. Host image offsets remain 64-bit. Larger forks
  belong on HFS+; oversized classic imports are rejected before creation.
- Classic HFS metadata operations, fork shrinking, file close, explicit flush and
  clean-unmount markers have runtime undo for reported write/short-write and
  storage-flush failures. Failed file closes retain pending views for retry.
  Undo is bounded to 32 MiB of original sectors per operation; an operation that
  needs more is rejected before overwriting uncaptured sectors. Whole copies and
  ordinary in-place file data writes are not atomic.
  If rollback also fails, further writes are disabled
  and original sectors are saved in an HFSUNDO1 recovery file in the Windows temp
  folder when possible; the error gives its path. These files require manual
  source/media verification before recovery and are not automatically applied.
  Runtime undo is not a persistent journal and does not protect against power loss.
- HFS+ disk images: format, browse, import/export data and resource forks,
  overwrite files, create folders, rename entries and delete files or
  folder trees. File > Format HFS+ / HFSX image creates a new image (8 MiB minimum),
  with journaling enabled by default. File > Open volume mounts HFS+ / HFSX and
  writable MFS images in the main tree and list alongside classic HFS and Windows
  folders. Unicode names, 64-bit fork sizes and filesystem capabilities use a
  shared presentation/transfer model; ordinary navigation, Copy/Paste/Cut, folder
  creation, rename, delete, search, sorting and properties use the main interface.
  File > Import file and raw data/resource fork export remain available.
  Replace resource fork preserves the existing data fork. These adapters transfer
  both forks, Finder information and creation/modification dates; they do not
  add BinHex/MacBinary conversion to HFS+ / HFSX. Windows transfers use validated
  AppleDouble %filename companions with staged publication/rollback. The companion
  requires a Windows-compatible name of at most 254 UTF-16 units and a resource
  fork below 4 GiB; raw fork export has neither AppleDouble restriction.
  Folder operations are not whole-tree atomic. Moves revalidate the source
  before deletion and retain source items when copying/finalization fails.
- HFSX v5 supports formatting, browsing and read/write in both case-sensitive
  and case-insensitive modes, with or without journaling. Case-sensitive names
  preserve ignorable characters and distinguish Readme from README. Catalog
  comparison mode, volume version and directory counts survive all writes.
  Apple_HFSX partitions are supported; HFSX inside an HFS wrapper is rejected.
  HFSX shares the main browser and the general limitations listed below.
- HFS+ and HFSX internal journals support read/write transactions and replay. A read-only
  open replays into memory without changing the image. Non-journaled writes
  are supported but cannot provide journaled recovery after interruption.
- External HFS+/HFSX journals support read/write transactions and read-only
  replay from a separate GPT journal disk image. Normal Open volume asks for
  the matching disk image when required; the journal partition type and unique
  UUID must match the journal info block. Both GPT copies, table/header CRCs,
  partition bounds and overlaps are validated before journal access. A missing,
  mismatched or corrupt journal is refused; it is never silently ignored.
  Format Mac image can create a separate external journal image (64 MiB sparse
  GPT disk image). Keep the filesystem image and journal image together when
  backing up or moving them; close the volume first. Each image has exclusive writable access, and
  background operations coordinate both files. Journal enlargement uses a
  disjoint region in that dedicated partition and a durable JIB switch; it
  fails safely if there is insufficient room. External journal selection
  currently requires a whole GPT disk image with the native journal UUID;
  raw journal-only files and older signature-only devices are not supported.
- Expanded timestamps support reading/writing existing volumes and the optional
  "Expanded timestamps (1970-2106)" formatting choice. The native unsigned Unix
  epoch is distinguished from legacy 1904-based fields per catalog record;
  mixed-record volumes are supported. Browser display, sorting, compression
  conversion and Windows/native file copies keep canonical 64-bit dates. Out-of-range
  dates are rejected before writes rather than wrapping. Classic HFS/MFS and
  legacy-timestamp HFS+ destinations reject unrepresentable dates before moving
  or copying data. The expanded mode is intended for systems supporting Apple's
  non-classic timestamp extension, not older classic Mac OS compatibility.
- Selecting a CD drive automatically detects classic HFS, HFS+ and HFSX data
  tracks in the main tree/list, including Apple-partitioned hybrids, mixed-mode
  discs and completed multisession discs. Detection chooses the newest supported
  Mac data track, falling back to an older one if newer tracks contain only other
  filesystems. The Windows drive entry remains available separately. Both forks,
  compressed files and hard links use the same readers as disk images. Media
  changes invalidate cached listings; Refresh reopens the disc at its root.
  Internal journal replay stays in memory; no device write is issued.
  Tools > Dump CD to file offers a session/data-track picker. The default saves
  that track's exact 2048-byte sectors, including its partition map and hybrid
  contents. "Save Mac volume only" extracts the filesystem as a portable raw
  image, useful when a later session's partition map uses disc-absolute offsets.
  Track reads and cache read-ahead stop at track/session boundaries, excluding
  audio tracks and inter-session gaps. If the drive cannot supply the full
  session table, only safely bounded tracks in the latest session are exposed;
  ambiguous layouts are rejected. Audio extraction, BIN/CUE, raw 2352/2324-byte
  sectors and native ISO/UDF browsing are not implemented. The drive must expose
  cooked 2048-byte data sectors. Tests use injected layouts/sources and fixture
  files; physical mixed-mode/multisession hardware testing remains outstanding.
- Raw, Apple partition map and classic HFS-wrapped HFS+ images are recognized.
  Physical HFS+ / HFSX floppy access, GPT filesystem images and compressed DMG
  containers are not implemented. GPT support below is for external journals only.
- Compressed-file reads decode decmpfs stored data (1/9/10), zlib (3/4), LZVN
  (7/8), LZFSE (11/12) and LZBITMAP (13/14), from attributes or resource-fork blocks. Listings show
  logical sizes; exports produce decompressed data. Compression storage is hidden
  from user resource-fork exports; genuine user resource forks are preserved.
  Other unrecognized compression methods are refused explicitly.
- File and directory hard links resolve for browsing/export, including compressed
  backing files. Private inode directories are hidden. Broken links are marked
  unreadable; directory navigation returns to the visible parent. Creating,
  deleting or editing hard links/backing inodes remains unsupported.
- HFS+/HFSX extended attributes and Mac ACLs support reading, creation, replacement
  and removal on ordinary files and folders. Rename and fork-only overwrite retain
  existing metadata. Deletion reclaims fork-backed attributes. Attributes use binary,
  case-sensitive keys even on case-insensitive volumes, with incremental B-tree
  updates and the same internal journal/recovery path as other metadata.
  Properties > Extended attributes lists named values; small values can be edited
  as hexadecimal bytes, and Import/Export value handles larger binary values.
  Set value stages an edit; Apply saves all staged changes. The editor rejects stale
  snapshots and malformed com.apple.system.Security ACL records. Owner/group UUIDs,
  ACE order, rights and inheritance flags are retained as Mac metadata; they are not
  translated to or enforced as Windows permissions. ACL editing uses binary values,
  not a Windows-style permissions or account-name editor.
  Copies between HFS+/HFSX and Windows preserve attributes/ACLs in validated AppleDouble
  ATTR extensions, including directory companions. New copied directories receive
  source attributes; merging into an existing directory retains that directory's
  attributes. Transfers to classic HFS or other destinations unable to preserve
  attributes fail before copying/removing sources. Logical compressed-file exports
  omit compression storage attributes; hard-link exports omit private link topology.
  Limits: 64 MiB per attribute and 128 MiB per entry; HFS+ attribute names have at most
  127 UTF-16 units. AppleDouble export also requires names within 127 UTF-8 bytes and
  its attribute directory within 64 KiB. Existing larger attributes cannot be edited
  through this implementation. Raw fork exports intentionally contain only that fork.
- HFS+/HFSX compressed files can be overwritten, renamed, deleted and have their
  ordinary attributes/ACLs edited. Overwrites re-encode logical contents as zlib
  decmpfs, regardless of the original supported compression method. Properties
  offers background Compress/Decompress actions on writable ordinary files.
  New compression uses type 3 inline data (3802-byte attribute limit) or type 4
  resource storage with independently encoded 64 KiB units. Incompressible units
  use the stored-data marker; compression is not guaranteed to save space.
  Finder information, dates and ordinary attributes/ACLs survive conversion.
  A genuine resource fork is preserved: explicit compression is refused if it
  needs that fork, while an ordinary overwrite/import uses uncompressed storage
  when necessary to retain both forks. Conversion uses copy-on-write allocation
  and the existing journal commit/replay path. It requires temporary free space;
  type-4 compressed resource offsets remain limited to 32 bits. No other new
  compression encoders are included. Encryption remains
  unsupported; private compression/encryption/link attributes cannot be created
  through the ordinary attribute editor.
- Catalog and extents B-trees are updated incrementally, including node splits,
  merges and linked allocation maps. There is no fixed catalog-record or
  single-header-map ceiling. Catalog entries are still loaded into memory.
  Internal journals grow automatically when a metadata transaction needs more
  space; growth requires a suitable contiguous free region on the image.
  The extents-overflow file retains the format's eight-inline-extent requirement.
- Classic-HFS copy options include BinHex 5.0 (MacBinary I), exporting .bin
  files with both forks, type/creator, dates, icon position, protection and the
  original high-byte Finder flags. Automatic .bin imports accept I and II.
  BinHex 4.0 (.hqx) and MacBinary II remain separate options; version I has no
  header CRC or low-byte Finder flags. Legacy headers require zero reserved
  bytes, and declared fork lengths are checked before replacing a destination.
  HFS+/HFSX main-browser transfers use both-fork copies or explicit raw fork export.
- MFS images support formatting, read/write of both forks, overwrite, rename,
  deletion, file versions and Finder metadata in the common main browser. File >
  Format Mac image offers raw MFS and Disk Copy 4.2 choices alongside HFS+/HFSX.
  Raw formatting supports 128 KiB-20 MiB in 512-byte increments; DC42 formatting
  uses standard 400/800/720/1440 KiB sizes. MacRoman file names retain up to 255
  bytes; volume names retain 27. Same-name files with different versions are
  separate entries, displayed with version labels, with version-specific overwrite
  checks. Properties edits name, version, lock state, UTC dates and the original
  16-byte Finder information. Non-MacRoman names, folders, extended attributes and
  extended Finder fields that MFS cannot represent are rejected before transfer.
  MFS is flat: importing files is enabled, while New folder remains disabled.
  Native MFS-to-MFS transfers preserve file versions; other filesystems do not
  acquire MFS version semantics. Physical MFS media access is not implemented.
  Native classic HFS CD/floppy access retains its existing media restrictions.
- MFS mutations snapshot the bounded image and validate the proposed directory/map
  before writing. Reported write/short-write/flush errors restore touched bytes.
  If rollback fails, further access is disabled and an original complete image is
  saved in a Windows temporary recovery file when possible. The error gives its
  path; verify the source before manual recovery. This runtime rollback is not a
  persistent journal and does not protect against power loss. A full data area or
  fixed directory rejects the operation before writing. Inconsistent allocation
  counts, unowned blocks, locked volumes and invalid DC42 checksums stay read-only.
  DC42 data/tag checksums are updated; scavenger hints for changed sectors are
  cleared rather than retaining stale file identities. Unchanged tags survive.

`--verify-mfs-write-browser NEW_DIRECTORY` checks background import, versioned
copy/overwrite, modeless Properties editing, stable identities, deletion, two-window
refresh, exclusive handles and rejection of unrepresentable input. The retained
`mfs-readonly.exe` test helper also runs formatter/writer, directory-reservation,
highest block-ID, full-disk, checksum, failure rollback and recovery-backup tests.

`--verify-extended-attributes NEW_DIRECTORY` checks native/Windows metadata and
ACL roundtrips, directory companions, unsupported destination rejection, actual
modeless Properties editing, stale snapshots and binary value import/export. Core
HFS+ tests cover inline/fork-backed values, fragmented attribute overflow extents,
B-tree split/merge, journal enlargement and recovery at each commit boundary.

Tests: .\build-x64.ps1 -Test runs the full suite, including real HFS+ reads and
writes beyond 2 GiB in both forks of HFS+ and HFSX images. -TestHfsPlus runs
only the HFS+ / HFSX checks. -TestBinHex runs the small BinHex 5 suite.
Optional independent checks use tests\test-hfsplus-interop.ps1 with WSL and
hfsprogs installed; neither is needed to run HFVExplorer. Tested on Windows 11;
older Windows execution tests remain pending. No custom storage driver is used.

HFS+ implementation reference: Apple's HFS Plus Volume Format (TN1150),
https://developer.apple.com/library/archive/technotes/tn/tn1150.html
Unicode normalization uses Unicode 3.2 data; see licenses/Unicode-DFS.txt.
No third-party filesystem engine is included. The static compression codecs
are zlib 1.4 (inflate/deflate), Apple's BSD-licensed LZFSE/LZVN sources and MIT-licensed
libzbitmap; see src/compression
and licenses for provenance. Compression tests include retained macOS-written
fixtures and compare exports against macOS readback hashes.

HFV/DSK Explorer v. 1.3.0 -- a tool for vMac and Executor(DOS/Win32) users.
Experimental freeware software.


Disclaimer
----------
  THIS SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND,
  IN HOPE THAT IT WILL BE USEFUL BUT WITH NO GUARANTEES WHATSOEVER.
  ANYGRAAF DISCLAIMS ALL WARRANTIES, EITHER EXPRESS OR IMPLIED,
  INCLUDING THE WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
  PURPOSE. IN NO EVENT SHALL ANYGRAAF BE LIABLE FOR ANY DAMAGES
  WHATSOEVER INCLUDING DIRECT, INDIRECT, INCIDENTAL, CONSEQUENTIAL, 
  LOSS OF BUSINESS PROFITS OR SPECIAL DAMAGES. USE AT YOUR OWN RISK. 



Installation of HFVExplorer
---------------------------
  1. Extract all of the files to the directory of your choice.
     HFVExplorer initially shows all HFV and DSK files from the directory
     of the executable, so it is convenient to place them into a directory
     where you have your HFV and DSK files.

  2. CDENABLE.VXD is needed under Windows 95 to give access to
     HFS/hybrid CD:s. Must be *moved* to "\Windows\System" directory.

     CDENABLE.SYS is needed under Windows NT 4.0 to give access to
     HFS/hybrid CD:s. Must be *moved* to "\Winnt\System32\Drivers" directory.
     After moving the driver, the program must be run once with system administrator
     rights. Later normal user rights suffice.

  3. Start HFVExplorer and go to the options dialog (View menu).
     Check all of the property pages to suit your needs.




Features of HFVExplorer
-----------------------
- A read/write, Explorer-like view to ARDI HFV files and
  vMac DSK files (the same thing essentially).
- A tree representation of the corresponding HFS file structure.
- Direct access to floppies, works in both Windows 95 and NT 4.0.
- Direct access to CDs, works under Windows 95, 98, NT 4, NT 5.
- You can launch the applications or documents by double-clicking them.
- After you have run the program once, you can start it by
  double-clicking HFV file name.
- Drag&drop a HFV file to open it.
- Drag&drop file copy/move. If target volume is the same as source,
  the file will be moved, otherwise it will copied. Override by
  holding down Shift or Control keys - again, just like in Explorer.
  Drop item on top of the "up" button and it goes to parent directory.
- Rename by clicking the highlighted name, or by pressing F2.
  Just like in Windows Explorer.
- Delete by pressing the "Delete" button.
- Create new folder: right mouse click in an empty window area.
- Press "Tab" to switch between list and tree views.
- Press F5 to update screen, for example, after inserting new floppy disk.
- Enter or double-click opens Application/Document/Folder.
- Format floppies. File menu.
- Commands to dump HFV/DSK files to floppies, and create them from
  floppies.
- Create and format new HFS/DSK files. File menu.
- AppleDouble support. Special characters in names not handled.
- Automatic type conversions when moving between FAT and HFS.
  This code is directly from HFS tools.
  fat -> hfs:
    *.bin (Mac Binary II) -> data & resource forks
    *.hqx (binhex) -> data & resource forks
  hfs -> fat:
    data & resource forks -> *.bin (Mac Binary II)
  txt, c, h:
    CR/LF conversion, file name extension
- File copy mode selection for HFS -> FAT and FAT -> HFS.
  Selected from Options dialog (new property page).
  Maybe in future there will be file type mapping.
- Multiple item selection -- move, copy, delete.
  Works as in Explorer (or in any multi-selection list control).
  Shift-click, control-click or drag a marquee.
- "Create shortcut" command implemented for Executor/DOS,
  Executor/Win32 and vMac, including a launcher app for vMac.
  Places an icon on the Windows desktop, pointing to either a document 
  or an application. Automatic Mac icon extraction. 
  If you want the special characters  to be shown in icon names, 
  change the desktop icon font (Control Panel/Display/
  Appearance/Icon "Font") to something else than MS Sans Serif, 
  perhaps Arial, which may be ugly but has more visible characters.
  Icons and parameter  files are placed into subdirectory "Links".
  Helper directory "vMacLink" contains a link to a vMac installation.
- "Open with" supports the tree emulators (E/DOS, E/Win32, vMac).
  *** IMPORTANT *** : DO NOT try to copy/move/delete/create new files 
  in HFVExplorer when the emulator is running. MacOS doesn't like
  anything happening behind its back. The file accesses are not properly
  synchronized, and if unlucky, you may suffer data loss.
  HFVExplorer tries to protect you from this, but its protection
  schemes are far from perfect. It takes care that its own data
  structures are always in sync, but the emulators don't do this.
  After the emulator terminates, HFVExplorer checks if it needs to
  update the file structures and if so, it does this. That's the
  reason why it takes a while when emulator terminates -- if one bit
  of data has changed in a volume file, the only safe way is to
  reload the whole volume.
  Pressing return or double-clicking now opens Executor/Win32 by default.
  You can change this behavior from the options dialog.
- You should normally use the alias launching method with vMac,
  and only try the Apple events method if this fails. Executor users
  should not be concerned with the launching method since appropriate
  way is selected automatically. Just make sure that you don't
  claim in the setup dialog that your Windows Executor is DOS
  or vice versa -- this information have to be correct.
- Possibility to hide/show "invisible" files.
- Automatic file type mappings. Mappings are easily exported and imported.


CD support:
-----------
ISO9660 CD:s of course. HFS. Hybrid HFS/ISO. No High Sierra.
ISO with Apple extensions is shown as DOS CD, with no additional
resources, that is, icons and types/creators.

CDENABLE.VXD is needed under Windows 95 to give access to
HFS/hybrid CD:s. Must be in either Windows or Windows\System directory.

When HFVExplorer is running, DOS Executor is also able to see HFS
CD:s, using protected mode drivers. No MSCDEX is needed.
If you close HFVExplorer, Executor will continue to see the
current CD, but will not see the next one inserted.

NEW note: released a separate utility, CDENABLE.EXE,
to allow people who do not need HFVExplorer to use
protected mode drivers in Executor.
This file is now shipped with Executor and is not available
from me any more (to avoid unnecessary version conflicts).

TECHNICAL NOTE: the way CDEnable.vxd is implemented is incompatible
with future Windows versions. It works (at least) with original
Windows 95, OEM Service Release 2 (OSR2, "Windows 95 B") and
Windows 98 release build.

CD auto insert notification supported, just pop in the CD and it
should appear in the tree view, if it contains recognizable volume.
In some machines, the auto-insertion/arrival messages
take long time to reach applications. HFVExplorer can't
help it, press F5 if you don't want to wait.

Starting from version 1.1.1i, full CD support under Windows NT 4.0, too.
CDENABLE.SYS must be copied to your System32\Drivers directory.
After copying the driver, the program must be run once with system
administrator rights. Later normal user rights are enough.

Hook1508 released. A tool to help DOS Executor see the HFS (non-hybrid)
CD's under Windows NT 4.0. Not included in this archive.
If you intend to use Hook1508 with HFVExplorer shortcuts and/or
launching, you should either create a .bat or modify AUTOXEC.NT,
or make a custom version AUTOXEC.NT. The procedure is described
in Hook1508 read me file.


Tips:
-----
Use shortest possible paths. Application launching
imposes some limits to path lengths. There is nothing
I can do for it. It depends on system and the program to
be launched only.

All icons from the icon cache file HFVICONS.DAT are always 
loaded into the memory when HFVExplorer starts. You may save 
some memory and time by deleting the icon cache file HFVICONS.DAT 
once the startup gets intolerably slow.


Platforms:
----------
Microsoft Windows 95, Windows NT 4.0, Windows NT 5.0 beta 1 and beta 2, 
Windows 98.



Availability:
-------------
http://www.nic.fi/~lpesonen/HFVExplorer/

Could be in a subdirectory below. File
is named for example "Hdexp130.zip" for
version 1.3.0, etc.

There is no web page, just the zip file.


Documentation:
--------------
This file.


License:
--------
Freeware. See the disclaimer.


Credits:
--------
- Marcus Better, for "HFS tools". Much better than
  my own HFS code. Probably I will discard my
  own code altogether.
  HFS tools can be found:
  http://www.student.nada.kth.se/~f96-bet/hfsutils/
  email: Marcus.Better@abc.se
  Originally Copyright (C) 1996, 1997 by Robert Leslie, email: rob@mars.org
- ARDI (http://www.ardi.com/). Executor is great. HFV Executor uses a snippet
  from ARDI Browser code; resource bundle hash table.
- vMac team (http://www.vmac.org/). vMac is great, too.
- Paul Hargrove. HFS CD partition table handling was taken
  from Paul's Linux sources. GNU GPL as well.
- Mr. Noda (noda@lyra.vega.or.jp) for his help in Japanese language support.
- Peter van Sebille; vxd sector read routine is inspired by his
  Win95 Linux FSD.
- Everybody else I forgot to mention.

  GNU GPL (General Public License) now. See "Copying".
  Source code available by request, email: lpesonen@nic.fi



Copyrights
----------
TODO: put here a comprehensive copyright list (with urls).

Anygraaf
Apple
ARDI (Executor)
Bauer, Christian (Shapeshifter)
Better, Marcus
Cummins, Philip (vMac) (and other members)
Hargrove, Paul 
Leslie, Robert
Microcode solutions (Fusion)
Microsoft
Ortmann, Mathias (WinUAE)
Schmidt, Bernd (UAE)

- more?



E-mail:
------
"lpesonen@nic.fi" or "lauri.pesonen@anygraaf.fi"



History:
--------
Things fixed/changed (oldest first):


Versions 1.1.0-1.0.4
--------------------
- 32 MB limit in HFV files fixed
  (Misunderstanding between logical sector size and
  physical sector size).

- Better support for special characters when launching
  applications. Requires Executor version 2.0n or later.

- Default viewer for types "TEXT", "ttro" and "ttxt
  is now "System:Shareware:Tex-Edit:Tex-Edit"

- 1.0.1 crashed when opened dos volumes.
  (Visual Studio 5.0 optimization bug)

- AppleDouble launching: new versions of Executor
  seem to want drive letter to be lowercase,
  otherwise you get always browser only.
  Both programs changed to respect this feature.

- Old: lists *.HFV. Now: *.HFV and *.DSK

- about box: version number -> 1.0.4



Version 1.1.0:
--------------
- Copy / Move files or folders
    - Drag & drop: drop to tree view (left pane of the
      window), or a folder in the right pane.
    - No multi-selection so far.
    - Control & shift à la Windows Explorer
      to force copy or move; otherwise auto-selection
        mode. That is, move inside same volume, copy
        if volume changes.
        Cursor shows a "+" when copy will be performed.
    - can copy between fat/hfs
    - auto type convert when copying between fat & hfs:
        fat -> hfs:
            *.bin (Mac Binary II) -> data & resource forks
            *.hqx (binhex) -> data & resource forks
        hfs -> fat:
            data & resource forks -> *.bin (Mac Binary II)
        txt, c, h:
          CR/LF conversion, file name extension
        Conversion code directly from HFS tools.
        Will be customized later.
    - drag on top of "Up" button -> copy/move to parent directory

- Direct access to floppies. After floppy change, press F5
  to force update of the screen.

- delete file / folder (all contents!), asks for confirmation

- create folder: right mouse click context menu.

- rename file / folder: click the (already) selected name
  under icon -> wait 1 sec -> edit the name. Just like
  in Explorer.

- Format New Volume
    - Sizes the file, creates HFS structures and Desktop
    - menu command
    - create New document with Explorer context menu.
      double-click -> will ask name/size and format.
    - if you want to format a floppy, select from
      combo the appropriate drive

- Now contains *two* HFS routine sets - my own and
  Hfsutils' (see credits).
  I will eventually discard my own code.

- "File" menu contains two commands to dump a volume
  to a floppy / create a volume from a floppy.

- HFS code now taken from Hfsutils by Marcus Better:
  http://www.student.nada.kth.se/~f96-bet/hfsutils/
  email: Marcus.Better@abc.se
  Originally Copyright (C) 1996, 1997 by Robert Leslie, email: rob@mars.org

- GNU GPL (General Public License) now. See "Copying".
  Source code available by request, email: lpesonen@nic.fi

- MFC42.DLL not needed anymore.



Version 1.1.1:
--------------
- No fatal bugs reported since 1.1.0, so I assume that disk
  writing functions are ok.

- Deleting from fat volume was broken (simply didn't delete!)
  in 1.1.0, fixed.

- Fixed a bug that caused HFS -> HFS copied files
  to be shown with standard icons only.
  (Failed to clear the "inited" finder flag after copying)

- Creating own "Desktop" (after format) was a bad idea.
  Does not work, but prevents Finder from copying stuff
  to Desktop later. Removed.
  Erroneous Desktops should be deleted. Affects
  vMac only, Executor seems not use Desktop.

- CD support. HFS cd's and hybrid ISO/HFS.
    - No support for High Sierra.
    - No support for ISO with Apple Extensions. They are
      shown as FAT volumes, with icon information missing.
    - CD auto notification supported, just pop in the CD
      and it should appear in the tree view.
      In some machines, the auto-insertion/arrival messages
      take long time to reach applications. There
      is nothing HFVExplorer can do about it, press
      F5 if you don't want to wait.
  - Small memory leak (custom icons).
    Windows 95:
      - Seems to work ok now. Requires that CDENABLE.VXD is copied to
        your Windows directory. System dir is ok too, as any directory
        within path. Do NOT copy to HFVExplorer directory.
      - ALSO when HFVExplorer is running, DOS Executor (and any other
        program) is able to see cd's, too - and using protected mode
        drivers! No MSCDEX required. In some machines, you had to boot
    to MS-DOS mode to see CD drive.
    NT 4.0:
      More limited so far, does not open all cd's.

- AppleDouble now supported when copying/deleting files
  to HFS volumes. From HFS volumes creates MacBinary II.

- About box: credits edited. Version updated 1.1.1

- DSK filter added to "Open" dialog.

- Some missing tool tips added.

- NEW icon in tool bar opens now the Format dialog.

- New way to create volumes: in Windows Explorer,
  right-click and select New/HFVExp Document.
  Creates empty file, double-click it to open
  it in HFV Explorer -> will ask volume name and size.

- Option dialog: first page is now functional.



Version 1.1.1b & c
------------------
- Attempted to fix cache bug.
- Bundle bit was lost when copying HFS -> HFS
  causing missing icons.

Version 1.1.1d
--------------
- Fixed a bug in reading from CD.
  (caused missing icons and the previous "Known bug: sometimes
  has problems opening items in the root directory of the CD."


Version 1.1.1e
--------------
- Fixed a bug in reading from CD.
  Failed to mount some CD's with partition
  located at very high addresses, near end of disk.


Version 1.1.1f
--------------
- Following performance improvements:
  - Read-ahead code for CD's.
  - Larger cache.
  - Does not update screen so often any more.
    Could be still better.
- Memory leaks fixed.


Version 1.1.1g
--------------
- Properties dialog. Click file/folder
  with right mouse button (or Alt-Enter).
  Select properties. Be careful in what you change.
- Name locked items cannot be renamed any more, without
  removing the lock first (properties).
- Tool bar buttons to eject/reload all CD trays.


Version 1.1.1h
--------------
- Failed to mount hybrid CD's with boot block zeroed out.
  Fixed. For example, Executor 2 CD.


Version 1.1.1i
--------------
- Support for pure Mac CD's under Windows NT.
  CDENABLE.SYS must be copied to your System32\Drivers
  directory. After copying the driver, the program
  must be run once with system administrator rights.
  Later normal user rights suffice.


Version 1.2.0
-------------
- Faster directory browsing. Especially large volumes
  and directories work better, and CD's. The performance
  hit was tremendous if the volume had files more than
  a specific threshold.

- Multiple item selection -- move, copy, delete.
  Works as in Explorer (or in any multi-selection list control).

- File copy mode selection for HFS -> FAT and FAT -> HFS.
  Selected from Options dialog (new property page).
  Maybe in future there will be file type mapping.

- "Create shortcut" command implemented for Executor/DOS,
  Executor/Win32 and vMac, including a launcher app for vMac,
  and an automatic alias creator.
  Places an icon on the Windows desktop, pointing to either a document
  or an application. Automatic Mac icon extraction. 
  If you want the special characters to be shown in icon names, 
  change the desktop icon font (Control Panel/Display/
  Appearance/Icon "Font") to something else than MS Sans Serif, 
  perhaps Arial, which may be ugly but has more visible characters.
  Icons and parameter files are placed into subdirectory "Links".
  Helper directory "vMacLink" contains a link to the vMac installation.
  Click the button "Install files required by vMac" to set up this
  directory.

- "Open with" supports the tree emulators (E/DOS, E/Win32, vMac).
  *** IMPORTANT *** : DO NOT try to copy/move/delete/create new files 
  in HFVExplorer when the emulator is running. MacOS doesn't like
  anything happening behind its back. The file accesses are not properly
  synchronized, and if unlucky, you may suffer data loss.
  HFVExplorer tries to protect you from this, but its protection
  schemes are far from perfect. It takes care that its own data
  structures are always in sync, but the emulators don't do this.
  After the emulator terminates, HFVExplorer checks if it needs to
  update the file structures and if so, it does this. That's the
  reason why it takes a while when emulator terminates -- if one bit
  of data has changed in a volume file, the only safe way is to
  reload the whole volume.

- You should normally use the alias launching method with vMac,
  and only try Apple events method if this fails. Executor users
  should not be concerned with the launching method since appropriate
  way is selected automatically. Just make sure that you don't
  claim in the setup dialog that your Windows Executor is DOS
  or vice versa -- this information has to be correct.

- Icon corruption bug fixed. To get rid of the old
  corrupted icons, the icon cache file "HFVICONS.DAT" must be deleted.
  HFVExplorer will ask you whether it's ok to delete this file.
  After deleting the file, you may notice that some icons,
  typically various preference files but some documents as well,
  do not have proper icons. This is normal, since HFVExplorer 
  does not load icons from MacOS Desktop file or Desktop database, 
  but uses its own system. The icons will eventually reappear after
  you browse through the folder containing the creator applications
  which has the bundle resources and icons.

- Resolves folder custom icons.

- Resolves (some) alias types icons.

- HFS and FAT default icons are now in different colors.
  The new plain icons are best shown if you have window color something
  different than white (Control Panel/Display/Appearance/item "Window")

- Folder "Icon\x00D" file is copied out to FAT volumes as "Icon~"
  This file contains the custom icons of a folder, if any.

- CD's were mounted as R/W file systems, causing strange error
  messages even when copying succeeded.

- Accelerator F2 to edit file name.

- Obsolete tool buttons removed, new buttons added
  to read a volume file from floppy and write one to a floppy.

- Some more tool tips added.

- Possibility to limit the time spent in creating DOS icons.
  In options dialog.

- Mac vs. ANSI character set fixes. Some characters may still
  be incorrect, but see the note in "Create shortcut" above.

- Uses Arial font in most places now. Yes I know it's ugly,
  but MS Sans Serif does not have some important special
  characters.

- Possibility to hide/show "invisible" files.

- Pressing return or double-clicking now opens Executor/Win32 by default.
  You can change this behavior from the options dialog.

- Changing of "is alias" enabled in properties dialog.

- Many miscellaneous bug fixes.
  
- Hook1508 -- a tool to help DOS Executor see the HFS (non-hybrid) 
  CD's under Windows NT 4.0. Not included in this archive.
  If you intend to use Hook1508 with HFVExplorer shortcuts and/or
  launching, you should either create a .bat or modify AUTOXEC.NT,
  or make a custom version AUTOXEC.NT. The procedure is described
  in Hook1508 read me file.


Version 1.2.0 -- additon to same version
-------------
- "Icon\x00D" is now treated as invisible file even if the 
  corresponding flag is not set.

- Explained the netbios problem under NT.


Version 1.2.1
-------------
- Configurable font and point size (Options/General)
- Remembers window position and size
- Mac floppies are now detected when "A:\" is clicked.
- Bugs in emulator launching code fixed
- Minor cosmetic fixes
- List view now always shows the selection
- Some special characters fixed in AppleDouble names
- CD support for Windows 98, was previously a separate download.
  (Check the size of \Windows\System\cdenable.vxd -- should be 10400 bytes.
  Some users experienced file corruption when downloading the separate vxd.)


Version 1.2.2
-------------
- Japanese font support, thanks to Mr. Noda (noda@lyra.vega.or.jp).
- Japanese users please check the option "Font character set is SJIS (Japanese)"
  in "General" page of the options, change Font (for example "MS gothic")
  and restart HFVExplorer.


Version 1.2.3
-------------
- File type conversion table DOS extension <--> MacOS type/creator.
  See "Options/Copying Files/Raw copy file type mappings".
  Click the "?" button for help.
  Initially, the program comes with no preset mappings, but once you
  have typed in some they can be easily shared with other people with
  export/import commands. 
- New right-click popup menu command to prefill the mapping dialog
  with currently selected item(s), either DOS or HFS files will do.
  Function key F4 does the same thing.
- Copying files between HFS and DOS does not change ' ' <-> '_' any more.
  Some other character mapping fixes in file names. 


Version 1.2.4
-------------
- File type conversion table (1.2.3): fixed a bug which caused types and
  creators that contain special characters to be ignored.

- In report (details) list view, columns may now be sorted by clicking
  the column header. Click again to reverse the order (asdending/descending).

- Creating new volume file is much faster now.

- Yet another experimental feature: "Finder view mode" tool button. 
  Shows the files and folders in original positions and uses Finder "magic rectangle"
  to reposition moved icons. Use a small font point size.

  Works under any System version, but positioning new items works only under
  System 7.1.1 or later. 

  Note that Finder relocates items only when it "sees" them; if some application
  (including Finder itself; i.e. "Recent" folders) has installed new files 
  and you have never opened the window in Finder, the files may be layered
  on top of each other. HFVExplorer tries to place them "somewhere".

  If you copy new files into a folder and do not move them in "Finder view mode",
  they may not be shown exactly in same positions in Finder and HFVExplorer.
  This is because HFVExplorer does not try to mimic the Finder automatic
  placement function. Either move them in HFVExplorer or visit the folder in Finder.

  "Go up..." button is not available in Finder view, there is no good place for it.
  Use the new keyboard shortcut Backspace to move upwards (used to work when the 
  focus was in tree view, now works always).

  (Check "http://developer.apple.com/technotes/tb/tb_42.html" for a discussion
  on the problems when positioning Finder icons.)



Version 1.2.5 (in-house)
------------------------
- DOS report view said that all folders have "0 items". Fixed.
- AppleDouble report view displayed an incorrect number for folder item count. Fixed.
- A message "Failed to set item properties" was generated every time
  when trying to move within a window in read-only media, when "Finder view"
  was selected. Fixed.
- Launch support for WinUAE/Shapeshifter (please see the bug list).
- Launch support for Fusion demo (not completed).



Version 1.2.6
-------------
- Sorting by HFS creation/modification date was intolerably slow. Fixed.
- Some icons crashed the program. Now they shouldn't any more, but they
  are shown garbled. Not perfect, but will do for now.
- Some popup menu reorderings.
- Fixed some problems with WinUAE/Shapeshifter launching.
- Launch support for Fusion finished.
- Alias launching for vMac, Fusion and ShapeShifter: optionally, installs
  a tiny Mac app into Startup folder (HFVExplorer Remover). It's only purpose
  is to wait in background for 5 seconds and then delete the alias.
  This is safer than the old method which deleted the alias under Windows.
  Check "Remove alias" in the corresponding emulator dialog to activate this feature.
- Apple events launching should not be used any more.



Version 1.2.6 (addition)
------------------------
- Updated bug list.



Version 1.2.7 (icon facelift release)
-------------------------------------
- Compressed icons are not shown any more (result was garbage).
  Generic icons are used instead. Decompression algorithm is not documented, see:
  http://developer.apple.com/technotes/tb/tb_555.html
- Icons 16 colors -> 256 colors. Both in list view and shortcuts.
  Requires "thousands of colors" display mode at minimum. 15, 16, 24 and 32 bits supported.
  To show the icons correctly in Windows desktop, enable in Control panel: 
  "Display" / "Plus!" / "Show icons using all possible colors".
- "Save icon..." command (right click menu), works for folders too.
- Finder label colors. Displayed as superimposed colored pedestals right below 
  the icons. Not perfect (will probably change later), but the way MacOS shows 
  the labels of colored icons is (arguably) even worse.
  Right click items to change labels.
  Uses default colors and texts (Red=Hot, Orange=Essential etc).



Version 1.2.8
-------------
- It turned out that Toast allows writing of CD's with very long volume
  names. Although it correctly cuts the name at 27 characters in MDB, 
  the entry in Catalog File is longer which causes problems for
  HFVExplorer. To mount such CD's, edit the file "HFVExplorer.ini" in
  your Windows directory as shown below:

  [Setup]
  UsePartialNames=1 ; default value is 0

  If there is entry "UsePartialNames" already, change the value to be "1",
  otherwise create new entry.

  This has the effect that any requests to the hfsutils core are made
  with partial file paths instead of full file paths:
     ":MyFolder:Myfile.xxx"
  instead of
     "MyMacVolume:MyFolder:Myfile.xxx"
  Once I'm sure that this works always, I may turn it on by default.

- Support for volume files named as HFx (where x is anything).



Version 1.2.9 (bug fixes)
-------------------------
- Fixed some special character issues when copying files
  between HFS and FAT file systems.



Version 1.2.10 (bug fixes)
--------------------------
- Folder custom icons were lost when copying from HFS -> FAT and back again.
- A bug in deletion fixed (HFS & FAT); sometimes, one had to try delete twice
  before a nested folder was completely deleted.
- If a folder name had any special characters, then the creation of a subfolder
  below it failed (when copying a deep  directory structure). 
  This was a new bug in 1.2.9.



Version 1.2.11 (clipboard release)
----------------------------------
- "Cut", "Copy" & "Paste" -- these work mostly like in Explorer.
- "Cut Append" & "Copy Append" -- the selected items are appended to the
  clipboard file list. You can even mix "Cut Append" and "Copy Append"
  commands so that one "Paste" command copies some files and cuts some.
- Escape empties the clipboard ("Cut" and "Copy" also empty it before a refill)
- Added more checks for illegal copy attempts. The destination folder may 
  not be a subfolder of the source folder, etc.
- "Select All" command. Control-A.
- When moving directory structures between HFS and FAT, not all files
  were deleted but empty directories was left behind. Fixed.
- The default action changed when the "A:\" or "B:\" floppy symbols are clicked.
  Starting from version 1.2.1, Mac floppies were detected too, but this
  is not the case any more. There seems to be some very nasty bug related
  to this feature, and therefore it's only accessible by manually editing
  HFVExplorer.ini. If you really want that feature back, change the following:

      [Setup]
      DisableAutoFloppy=0

  Normally, you should press F5 to refresh the screen after
  inserting/changing/removing floppies.



Version 1.2.12
--------------
- Tool buttons for "Cut Append" & "Copy Append".
- One comment was left out about 1.2.11 "Append" command
  description: note that you can append from different 
  folders, even from different volumes and medias (but do 
  not change the floppy or cd when collecting files).



Version 1.2.13
--------------
- Launching Mac documents with Windows applications (New "emulator").
  The documents are copied to the Windows temporary folder and opened.
  When the application (not document) is closed, the user is prompted
  whether to update the changes, if there are any. 
  Multiple documents can be open at the same time. The changes are tracked
  file by file.

  If the original document does not have an extension, a file type
  mapping must be created; a dialog box will open automatically.

  Under Windows NT, launching to 16 bit Windows apps does not 
  work properly. The document opens ok, but HFVExplorer does not
  recognize when the application is terminated.

- Default emulator (after first time installation) is now the one 
  described above.

- Copying HFS->FAT: file type mappings are not enforced
  any more if the path already contains an(y) extension.



Version 1.3.0 (beta 1 - beta 3: in-house)


Version 1.3.0
-------------
- Support for Mac formatted removable media (Zip and others).
  No ASPI manager needed. For Windows 9x, cdenable.vxd must 
  be updated. For Windows NT, no driver update is needed.
  The removable drive must have a drive letter in Windows.
  If you use any HFS file system driver (MacOpener or others),
  do not try to modify the media simultaneously in HFVExplorer
  and via a drive letter.
  Auto-insert notification works with Zip disks under Windows 9x.
  Under Windows NT, you need to use Refresh command (F5) after
  changing media.

- Write-back cache code for all writable media. Consequently,
  some performance boost in floppy write throughput.

- None of the newly supported media can be formatted, yet.

- Some enhancements in read ahead cache code.

- Fixed CD eject problems (HP7110e & NT, maybe some others)

- CD eject and reload now manipulate the selected drive only.

- Eject and reload work for removable disks too, if possible
  (for example, Zip's can only eject).

- New accelerators: Ctrl-E to eject selected media, Ctrl-R to reload it.

- Attempted to fix a conflict between Norton AV 5.0 under
  Windows 9x (reportedly freezed when accessing floppy drive 
  with no floppy disk present).

- Fixed a crash when ejecting, and the selected item
  was not a drive (i.e. the "Configurations" icon).

- Removed the "Configurations" icon from the tree view.
  It had no function (never had).

- Fixed strange error messages when copying from Zip drive
  to elsewhere. Strangely enough, HFVExplorer actually copied
  allright, but all the same believed that there was an error
  an reported nonsense things. The same bug once pestered my
  CD code in its infancy, and now the same bug reappeared here.

- Removed (undocumented) hard disk support. I have no chance
  to test it properly now, and I should before I let anyone else 
  try it out.

- Fixed a bug in floppy writing code.

- Removable media and floppy write code was disabled in beta 2,
  should work now.

- Fixed a bug in "New folder" code.

- Supports HPBurner. Menu command and tool button for burning cd.
  You'll need HPBurner.exe to write to CDR/CDRW media.
   ONLY works with HP CD-Writer+ 7100 (and compatibles).

- Fixed reload media crash.

- execut95.exe is not supported any more and was removed from the 
  distribution archive.




Version 1.3.1
-------------
Fixed some cd burning problems.




Bug list
--------
- Copying when the file already exists: behaves badly,
  does not ask for confirmation to overwrite. If you copy
  a file and there is a *folder* with same name -> not
  even error message is given, and the copying fails.
  As for now, I decided that this is a "feature" :)

- In report view, file dates seem to be wrong (again/still?).
  I thought that it was fixed. oh well.

- Properties and file mappings with F4 does not work for AppleDouble files.

- Root folder of DOS volumes: two first items are overlapped (in some systems).
  Could be a bug in Common dialog control libraries, since not all systems
  have this problem.

- AppleDouble launching for Executor DOS works only with short file names.
  Not much I can do for it.

- vMac launch has problems under NT 5 beta 2. Seems to be a beta bug,
  cannot resolve shortcuts properly.

- Finder view mode: in some systems, the scroll bars always appear and
  the window is scrolled a few pixels. Very annoying. Strangely enough, 
  I cannot repeat this on my dev machine. I can repeat it with my backup 
  machine so there's hope to get it eventually fixed. Could be a bug in MFC 
  libraries which is already fixed.

- Options dialog: some "browse" buttons outsmart themselves when a directory
  name contains spaces.

- Shapeshifter launching crashes sometimes if "Remove alias" is on. Hard to fix
  since it can be repeated by manually launching two aliases in SS within
  narrow enough time frame. If this happens to you, do not use "Remove alias".



Wish list
---------
- Support for Services for Macintosh (SFM) under Windows NT.
  There is now minimal support (showing icons), copying is not yet implemented.

- Unimplemented: "Find"

- Unimplemented: Preview window (I forgot the whole thing...)

- If you open a second browser window (Control-double
  click to folder, control-enter) it can't access HFS 
  floppies (under NT at least). "Second browser" is quite
  useless feature anyway, until one can drop between views.

- OLE drag and drop.

- Expanding Finder view mode to a full-blown Finder replacement???
  This would require kicking out the MFC CListView code, it's too restricted.

- Icon cache code should be rewritten.

- Should upgrage to Rob's latest hfsutils source tree (w/ blessed folder).
  Would allow making bootable volume file.

- Should follow folder aliases ('fdrp'), or launch them.

- HFS volume browse dialog (no big deal)

- After delete, tree view still shows the deleted folders...

- Copy progress dialog

- F4 should have menu equivalent; and the right-click commands
  should be in menu too.

- What should be done when copying FAT files starting with a dot.
  Traditionally Mac driver names only, Apple says that no normal
  file name should start with a dot.

- Compressed Mac resources.

- PC link floppies.

- Perhaps copying files should always update the icon locations;
  trying to keep the original order, but avoiding any overlaps.
  It is confusing that (in Finder view mode) you need to move
  the icons again in order to permanently record the positions,
  so that real Finder honours them.

- Copyright list



3/12/1999
Lauri Pesonen
lpesonen@nic.fi
