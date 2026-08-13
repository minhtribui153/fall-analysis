# Documentation assets

This folder contains editable Graphviz source (`.dot`) and rendered SVG diagrams used by the Markdown documentation.

To regenerate an SVG after editing a diagram:

```bash
dot -Tsvg system-context.dot -o system-context.svg
```

The same command applies to each `.dot` file.
