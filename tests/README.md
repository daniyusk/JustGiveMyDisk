# Synthetic NTFS fixtures

The parser tests build every input in memory. They do not depend on disk images,
host filesystems, timestamps, randomness, or byte order, so failures are small and
reproducible.

`FileRecordBuilder` creates a 1,024-byte `FILE` record with a three-entry update
sequence array (USA) for two 512-byte sectors. Attributes start at byte `0x38`, are
aligned to eight bytes, and end with the NTFS `0xFFFFFFFF` marker. The builder can
append `$FILE_NAME`, resident `$DATA`, and non-resident `$DATA` attributes.

The focused helpers document the malformed dimension in their names:

- `valid_usa_record` and `invalid_usa_record` exercise sector fixups;
- `positive_run`, `negative_run`, and `sparse_run` encode data-run variants;
- `truncated_record`, `malformed_first_attribute_offset`,
  `malformed_attribute_size`, `malformed_resident_content`, and
  `malformed_non_resident_run_offset` cover bounds validation;
- `invalid_utf16_record` accepts raw UTF-16 code units, including unpaired
  surrogates;
- `sanitized_name_collision` supplies distinct NTFS names (`/` versus `\\`) that
  both become `reports_2026` under the recovery filename sanitizer;
- `overflowing_data_runs_record` and `corrupt_data_runs_record` are regression
  inputs for arithmetic overflow and corrupt run headers.

Run the suite with CTest after configuring a normal build with `BUILD_TESTING=ON`.
