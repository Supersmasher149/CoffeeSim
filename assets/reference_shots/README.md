# Published reference recipes

These are published target recipes collected as comparison material. They are
not measured shots: no source provides a beverage-mass time series, and none of
these records may be used by `espressolab_cli calibrate`.

Each JSON file intentionally stores two forms of the same information:

- `published`: values and wording as supplied by the source.
- `normalized`: values converted to EspressoLab units. `null` means the source
  did not publish that value; it is never an estimate.

`derived_from_published` lists any normalized values computed from explicitly
published inputs, such as `yield_g = dose_g * brew_ratio`. Every record has its
own URL, retrieval date, and a short source excerpt so the original can be
checked when a recipe changes.

The records are recipe references, not model-ready recipes: the model also
requires basket geometry and an effective particle size, neither of which is
invented here.
