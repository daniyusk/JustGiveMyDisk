# JustGiveMyDisk

JustGiveMyDisk is an experimental, terminal-only C++20 recovery helper for corrupted NTFS partitions. The first milestone only scans and indexes surviving NTFS MFT `FILE` records into SQLite.

## Warnings

- Experimental software. Validate results before trusting them.
- The source device is opened read-only with `O_RDONLY`.
- This tool does not repair NTFS and does not write to the source partition.
- Do not run recovery experiments against the only copy of important data. Image the device first when possible.
- This milestone does not recover file contents yet. It only builds an index from surviving MFT records.

## Features In This Milestone

- Scans a block device or image for 1024-byte NTFS MFT records with the `FILE` signature.
- Applies NTFS USA/fixup validation and restoration.
- Parses resident `$FILE_NAME` attributes (`0x30`).
- Extracts guessed record id, byte offset, parent reference, filename, namespace, flags, allocated size, real size, and likely directory status.
- Converts UTF-16LE filenames to UTF-8.
- Stores scan results in SQLite.
- Supports exact-name lookup and a basic indexed tree view.

## Dependencies

Arch/EndeavourOS:

```sh
sudo pacman -S cmake ninja gcc sqlite fmt cli11
```

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

## Usage

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
