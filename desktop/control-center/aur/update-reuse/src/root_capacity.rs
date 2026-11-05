// Signed manifest schema 2 selects a bounded larger root. Output package and
// signer limits are unchanged; schema 1 never inherits the larger capacity.
fn root_manifest_capacity_valid(schema: u32, packages: usize) -> bool {
    let maximum = match schema {
        1 => 64,
        2 => 256,
        _ => return false,
    };
    packages > 0 && packages <= maximum
}

#[cfg(test)]
mod root_capacity_tests {
    use super::*;
    #[test]
    fn signed_versions_keep_distinct_finite_bounds() {
        for (schema, count, expected) in [
            (1, 64, true),
            (1, 65, false),
            (2, 65, true),
            (2, 256, true),
            (2, 257, false),
            (2, 0, false),
            (0, 1, false),
            (3, 1, false),
        ] {
            assert_eq!(root_manifest_capacity_valid(schema, count), expected);
        }
    }
}
