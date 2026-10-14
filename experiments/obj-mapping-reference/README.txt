Frozen OBJ mapper from commit 6d712bb, before removal of the fused fallback.

These files support historical mapping probes only. They are not linked into
the production viewer or its loader. The benchmark selection flag lives in
mapping_reference.h so the production ObjPrototypeOptions has no such flag.
The old source and staged face table remain together for controlled comparisons.
