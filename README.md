# JustGiveMyDisk

JustGiveMyDisk is an experimental, terminal-only C++20 recovery helper for corrupted NTFS partitions. It scans surviving NTFS MFT `FILE` records into SQLite and can recover indexed directory trees by reading resident and non-resident `$DATA` attributes.

## Warnings

- Experimental software. Validate results before trusting them.
- The source device is opened read-only with `O_RDONLY | O_CLOEXEC`.
- This tool does not repair NTFS and does not write to the source partition.
- Do not run recovery experiments against the only copy of important data. Image the device first when possible.
- Recovered files are best-effort. Corrupt records, unsupported attributes, encrypted data, compressed data, and unreadable runs are skipped.

## Features In This Milestone

- Scans a block device or image for 1024-byte NTFS MFT records with the `FILE` signature.
- Applies NTFS USA/fixup validation and restoration.
- Parses resident `$FILE_NAME` attributes (`0x30`).
- Extracts guessed record id, byte offset, parent reference, filename, namespace, flags, allocated size, real size, and likely directory status.
- Converts UTF-16LE filenames to UTF-8.
- Stores scan results in SQLite.
- Supports exact-name lookup and a basic indexed tree view.
- Recursively recovers indexed directory trees into a destination directory.
- Supports resident and non-resident unnamed `$DATA` attributes (`0x80`).
- Parses NTFS data runs and copies only each file's real size.
- Provides a `--dry-run` recovery preview.

## Dependencies

A C++20 compiler, CMake 3.20 or newer, Git, and a standard build tool are required.
CLI11, fmt, FTXUI, and the SQLite amalgamation are fetched by CMake from immutable,
checksum-verified revisions declared in `cmake/Dependencies.cmake`.

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

Enable warnings-as-errors and run the smoke tests with:

```sh
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON -DJGMD_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The scanner, recovery, parser, database, and UTF conversion code live in the
`JustGiveMyDisk::core` library. Tests link that library directly and do not start
or link through the interactive TUI entry point.

## Usage

Open the TestDisk-style terminal UI:

```sh
./build/JustGiveMyDisk tui
```

The TUI detects Linux block devices and partitions, asks for a `scan.db` path, scans read-only using the same scanner as the CLI, then provides indexed search, parent-chain/tree previews, dry-run recovery, and confirmed recovery.

Scan a damaged partition read-only and write the index to `scan.db`:

```sh
sudo ./build/JustGiveMyDisk scan /dev/nvme0n1p3 --out scan.db --target "FolderName"
```

Find exact filename matches:

```sh
./build/JustGiveMyDisk find scan.db "FolderName"
```

Print children below a database row id:

```sh
./build/JustGiveMyDisk tree scan.db --id 12345
```

The `tree` command uses the selected row's `record_id_guess` as the parent reference for child lookup. On heavily corrupted volumes this is only a best-effort view.

Preview recovery below a known `record_id_guess` without writing files:

```sh
sudo ./build/JustGiveMyDisk recover /dev/nvme0n1p3 scan.db --id 1189298 --dest /media/bkcpdisco/RECUP_IN_ROMANCE --dry-run
```

Recover the indexed tree into a separate destination:

```sh
sudo ./build/JustGiveMyDisk recover /dev/nvme0n1p3 scan.db --id 1189298 --dest /media/bkcpdisco/RECUP_IN_ROMANCE
```

The `recover` command uses `scan.db` to recursively collect children whose `parent_ref` matches the current directory `record_id_guess`. It then scans the source for MFT `FILE` records and matches the record number stored in the MFT header before reading `$DATA`; it does not assume that `record_id_guess * 1024` is the source offset.

## SQLite Schema

```sql
CREATE TABLE records(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  record_id_guess INTEGER,
  offset INTEGER NOT NULL,
  parent_ref INTEGER,
  name TEXT,
  namespace INTEGER,
  flags INTEGER,
  allocated_size INTEGER,
  real_size INTEGER,
  is_directory INTEGER
);
```

Indexes are created on `name`, `parent_ref`, and `record_id_guess`.
