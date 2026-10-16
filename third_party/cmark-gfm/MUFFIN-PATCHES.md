# Muffin patches

The vendored parser records whether it created any footnote definitions or
references. When neither occurred, `process_footnotes` skips its two no-op tree
walks. The marker is conservative if nodes are subsequently removed and is
cleared by the existing document reset. References without definitions still
run the original processing, which converts unresolved references to text.

This changes the private `cmark_parser` structure. Muffin builds all cmark core,
extensions and consumers together; do not mix the patched parser with binaries
built against the previous private header.

Autolink's email postprocessor can consume the core's text-consolidation pass
through an optional private `cmark_syntax_extension::postprocess_text_func`.
Only the first postprocessor invocation is eligible. Later invocations retain
their ordinary whole-tree callback, including when a different extension has
changed the tree or the same extension was attached more than once. This also
changes the private extension structure; rebuild core and extensions together.

The fused pass visits decoded, consolidated text outside existing links. Only
literals containing `@` enter the email recognizer. Non-candidates retain the
original owned/C-string contract, so allocations are not deferred into later
`get_literal()` calls. URL/`www` inline recognition and the email matching,
splitting and punctuation rules remain unchanged. The public
`cmark_consolidate_text_nodes` operation still performs ordinary consolidation.

`MuffinParserDefinitionTest` additionally checks the pre-change autolink oracle,
entity-decoded candidates, earlier tree mutations, repeated extension
attachments, parser reuse and byte-wise feeds. See
`docs/benchmarks/cmark-autolink-fused-pass-2026-10-17.md` for the interleaved parse
and full Core preparation measurements.

`MuffinParserDefinitionTest` checks fixed pre-change AST/HTML/source-coordinate
oracles, including repeated references, nested definitions, unresolved
references, disabled footnotes, byte-wise feeds and parser reuse. The original
oracle also includes footnote reference indices, definition counts and links
from references to their definitions. See
`docs/benchmarks/cmark-footnote-fast-path-2026-10-17.md` for measurements and
the separately retained upstream entity-marker instability.
