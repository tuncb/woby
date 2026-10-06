# Diagnostic campaigns

Each `stage-*.json` records a contemporaneous legacy comparison. The `before`
column changes between experiments as shown below; it is **not always** the
original prototype. Three rotated fresh processes were used per variant/model.
Inspect each file's executable hashes, metrics and worker counts before combining
results. Absolute times across campaigns are not independent optimization gains.

| File | `before` | Candidate / purpose | Disposition |
| --- | --- | --- | --- |
| baseline-sweep | Original prototype | Block caps 256 KiB, 1 MiB, 4 MiB, 8 MiB | Diagnostic |
| stage-a | Original | Balanced medium blocks, bulk tuple copy | Retained |
| stage-b | Original | Common position decoder on A | Retained |
| stage-c | Original | Serial scalar construction | Rejected |
| stage-d | Original | Input-buffer pooling, scalar construction removed | Pooling rejected |
| stage-e | Original | One-point and ordinary-primitive dispatch | Dispatch retained |
| stage-f | Original | Complete-line shortcut, pooling removed | Initial candidate |
| corpus-f | Original | F across 12 models, five repeats | Superseded by final corpus |
| viewer-f-summary | Legacy viewer | F inside the viewer, six models, three repeats | Exposed remaining cloud regression |
| worker-sweep-f | Legacy | F with 8, 12, 16 workers | Keep default cap 16 |
| stage-g | F | Typed position chunks, bulk vector construction | Retained |
| stage-h | G | Alternate newline search and outlined ordinary helper | Rejected |
| stage-i | G | Another input-buffer pooling attempt | Rejected |
| stage-k | G | In-place position decoding, flatter complete-line loop, in-place sparse records | Retained; J was a build step without a separate campaign |
| stage-l | K | Persistent double-buffered native file ranges | Retained |
| stage-m | L | Unbuffered ranges, read sharing | Rejected |
| stage-n | L | Unbuffered ranges aligned to whole read blocks | Rejected |
| stage-o | L | Position construction within merge worker pool | Retained |
| stage-p | O | Plain positive-index triangle/quad decoder | Retained |
| corpus-p-before-error-guard | Original | P across all 12 models, five repeats | All medians below legacy; superseded by final source validation |
| viewer-p-before-error-guard | Legacy viewer | P on quads/lines/cloud, three repeats | Parser medians below legacy; final source additionally fixes an early limit diagnostic |
| corpus-p-after-error-guard | Original | P plus retained early-limit diagnostic | Exposed 7.5% negative-attribute-index gap; other 11 models meet the gate |
| stage-q | P plus diagnostic fix | Direct UV/normal arity decoding | Retained; final candidate |

The original prototype is the saved executable built at `de7ac08`. The first
corpus appeared to meet the median gate, but a viewer check and quieter runs
revealed remaining regressions. The main report uses the later final corpus.
The initial viewer summary includes validated geometry counts, capacities, GPU input sizes
and decoded screenshot fingerprints. Original per-run traces remain under
`build/parser-fixed-workflow` in the worktree.

Later campaigns add a two-sample CPU gate. G used 10%; H used 20%; I and K–P used
15% (also Q). These checks limit background activity before launch, not every transient
load during parsing. Unbuffered legacy I/O sometimes varied dramatically even
when the gate passed. Per-worker read/decode sums help distinguish those runs.

An aborted H attempt produced no useful measurements and is not included. Its
temporary fixture folder remained after forced termination; automatic approval
review blocked the subsequent verified cleanup attempt.
