# textedit

Text-file operations independent of model-facing toolsets. This initial package
provides advisory UTF-8 inspection; editing and reading tools will be separate
consumers of the package.

Link `textedit_lib` and include `textedit/utf8_probe.hpp`:

```cpp
const auto result = textedit::probe_utf8_file(path);
if (result.likelihood == textedit::Utf8TextLikelihood::Unlikely) {
    // Attach a hint: "This file may not be UTF-8 text."
}
if (result.error) {
    // Report an inspection failure, not an encoding warning.
}
```

The default sample is the first 8192 bytes, with one extra byte to determine
whether the file continues. Callers can select 1 through 1048576 sample bytes.
Memory and inspected content are bounded by this limit. IO is synchronous.
Regular files are accepted, including symlink targets; other file types are
refused. Relative paths use the process working directory. The probe never
writes file contents, but normal filesystem access-time updates can occur.

`Likely` means the sampled prefix is compatible with UTF-8 text, not that the
file is proven to be text or entirely UTF-8. ASCII, empty files and a UTF-8 BOM
are accepted. `Unlikely` means malformed UTF-8 or suspicious ASCII controls
were found. `Unknown` with an error code means inspection failed. None of these
results prohibits a caller from reading or editing the file.

Validation rejects stray continuation bytes, overlong encodings, surrogates,
values above U+10FFFF and incomplete sequences at EOF. A potentially valid
sequence cut off by the sample limit sets `incomplete_suffix` instead of an
encoding error. Already-invalid partial sequences are still rejected. The
first invalid sequence's byte offset is reported when present. NUL, DEL and
ASCII controls other than TAB, LF, CR and FF independently produce a warning;
legitimate text containing those characters can therefore receive a warning.

This is deliberately a heuristic, not encoding detection. ASCII-only binary
data and some legacy-encoded data can pass. Invalid bytes after the sample are
not examined. No conversion, normalization or full-file scan occurs. Files
changing during inspection are not a consistent snapshot, and this result must
not serve as a write precondition or security decision. Allocation failures can
throw; invalid sample limits throw `std::invalid_argument`.
