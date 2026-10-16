"""Check that interval and record accounting keeps overlap and boundaries."""
import io
from analyze import union_ms, origin, engine_values
from census import counts

assert union_ms([(0, 4), (2, 6), (9, 10), (6, 8)]) == 9
assert union_ms([]) == 0
try:
    union_ms([(4, 1)])
    raise AssertionError("Reversed intervals must fail")
except ValueError:
    pass
assert origin({"observer_epoch_qpc_ns": 1000000000,
    "trace": [{"kind": "metadata", "steady_epoch_nanoseconds": 1010000000},
              {"kind": "mark", "name": "model_add_requested", "time_ms": 7}]}) == 17
assert engine_values([{"instance": "pid_123_engtype_3D", "value": 25},
                      {"instance": "pid_456_engtype_3D", "value": 90}], 123, "3D") == [25]
text = b"v 0 0 0\nvt\t.5 .5\nv 1 0 0\n# v 9 9 9\nf 1/1 2/1 1/1\nusemtl mat\n"
for size in (1, 2, 3, 4, 7, 16, 64):
    records, _ = counts(io.BytesIO(text), size)
    assert records["v"] == 2, (size, records)
    assert records["vt"] == 1, (size, records)
    assert records["f"] == 1, (size, records)
    assert records["usemtl"] == 1, (size, records)
print("Loading analysis accounting checks passed")
