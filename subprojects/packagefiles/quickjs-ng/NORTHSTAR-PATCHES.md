# Northstar's patches to quickjs-ng

Northstar builds quickjs-ng with the patches listed as `diff_files` in
`subprojects/quickjs-ng.wrap`; they live one directory up, in
`subprojects/packagefiles/`.

This directory is the wrap's `patch_directory`, copied over the checkout
before the patches are applied. It holds only this note. It is there
because muon 0.7.0 applies a wrap's `diff_files` only when the wrap also
has a `patch_directory` or `patch_filename`; without it, muon builds
quickjs-ng unpatched and the link fails on the functions the patches add.
