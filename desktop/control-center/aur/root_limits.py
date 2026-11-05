"""Signed manifest versions select fixed build-root budgets, never client limits.

Output archives, signer allowlists, working tmpfs and install gates are separate.
Version 1 retains its original bounds. Version 2 needs a newly reviewed/signed
manifest; it cannot enlarge the limits of an existing signed version-1 root.
"""
LEGACY = (64, 1024**3, 1024**3, 16384)
BOUNDED_V2 = (256, 1024**3, 2 * 1024**3, 65536)


def root_limits(schema):
    if type(schema) is not int or schema not in (1, 2):
        raise ValueError('Unsupported signed build-root capacity version')
    return LEGACY if schema == 1 else BOUNDED_V2


def package_count_valid(schema, count):
    return type(count) is int and 1 <= count <= root_limits(schema)[0]
