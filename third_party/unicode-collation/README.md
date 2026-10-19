# Unicode collation data (UTS #10, version 16.0.0)

Vendored for the UCA collation feature (`sxml_uca_collation_feature`,
`src/collation_uca.c`, packet 7.15a, D213) under D13 and D208. The version is
pinned in `third_party/unicode-collation.version`. sxml does not compile these
files in: hosts serve `allkeys.txt` through their resource resolver as
`SXML_RESOURCE_KIND_COLLATION_DATA` (D184, D189). The test drivers, the
`sxml` command and the JavaScript package serve this copy.

| File | Source | SHA-256 of the download |
| --- | --- | --- |
| `allkeys.txt` | https://www.unicode.org/Public/UCA/16.0.0/allkeys.txt, unchanged (the DUCET) | `7b47769f4a95615ada83b6812dea1d2a76d74580b1e4d7af48d383eb902017f0` |
| `CollationTest_NON_IGNORABLE_SAMPLE.txt`, `CollationTest_SHIFTED_SAMPLE.txt` | the `_SHORT` files of https://www.unicode.org/Public/UCA/16.0.0/CollationTest.zip | `f5fbccf67524737471b6c35c6f795e4f6de49c8c6101444c48bd57bd560bb817` (the zip) |

The samples keep the header comments and every 20th test line of the
`_SHORT` file, in the file's order (so each line still sorts at or after the
one before it), made for each of NON_IGNORABLE and SHIFTED with:

    python3 - CollationTest_SHIFTED_SHORT.txt CollationTest_SHIFTED_SAMPLE.txt <<'END'
    import sys
    lines = open(sys.argv[1], encoding='utf-8').read().split('\n')
    header = [l for l in lines[:12] if l.startswith('#')]
    tests = [l for l in lines if l and not l.startswith('#')]
    sample = tests[::20]
    with open(sys.argv[2], 'w', encoding='utf-8') as out:
        out.write('\n'.join(header) + '\n')
        out.write('# sxml sample (third_party/unicode-collation/README.md): every 20th test line\n')
        out.write("# of the _SHORT file, %d of %d, in the file's order.\n\n" % (len(sample), len(tests)))
        out.write('\n'.join(sample) + '\n')
    END

`tests/collation_uca_test.c` checks both samples.

License: Unicode License v3 (https://www.unicode.org/terms_of_use.html,
https://www.unicode.org/license.txt).
