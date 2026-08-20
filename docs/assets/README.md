# Diagram assets

The `.dot` files are source-of-truth Graphviz diagrams. The matching `.svg` files are rendered copies used by the Markdown pages.

Regenerate from this directory with:

```bash
for f in *.dot; do dot -Tsvg "$f" -o "${f%.dot}.svg"; done
```
