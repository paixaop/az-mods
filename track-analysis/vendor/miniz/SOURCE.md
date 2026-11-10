# miniz inflate

The three upstream source files are from miniz 3.0.2:
https://github.com/richgel999/miniz/tree/3.0.2.

`miniz_tinfl.c` changes only its include from `miniz.h` to `miniz_tinfl.h`, to build
the independent inflater without compression or archive APIs. `miniz_export.h` is
the local empty export-annotation configuration; mod symbols inherit hidden visibility.
The upstream MIT license is retained in the C source and packaged third-party notices.

SHA-256 of the files as used:

- `miniz_tinfl.c`: `cf38e9484f5f05d5c0972547f7608a65c9a8af64f517c9d2d48e689baed95795`
- `miniz_tinfl.h`: `040aad2d9ca23c487652c150277d950be12e72c5453ae1f7c6713ed2efd16b4e`
- `miniz_common.h`: `8b60e76e726dfcf41a338c3a4c54c70038041a21e6314aa54fd3b7f729ea3412`
