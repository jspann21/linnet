# iPod model catalogue

`tools/inspect/ipod_model_data.py` transcribes the model, original capacity,
generation, color and serial suffix facts in libgpod's `src/itdb_device.c` at
[`4a8a33ef4bc58eee1baca6793618365f75a5c3fa`](https://github.com/fadingred/libgpod/blob/4a8a33ef4bc58eee1baca6793618365f75a5c3fa/src/itdb_device.c).
Source SHA-256: `d5fa52ef884b83eec0533ad14bfc342be6d2aab7578c783a174f0107406ea769`.
Copyright (C) 2002-2007 Jorg Schuler and contributors; LGPL-2.1-or-later.
The license is retained in [COPYING](COPYING).

The transcription excludes iPhone/iPad rows, restores Apple's leading `M`,
retains ambiguous serial suffixes as multiple candidates, and adds the HP
PE435A/PE436A order-number aliases for monochrome 20/40 GB models. This is a
historical catalogue, not a claim to cover every iPod order number ever sold.
New or unlisted models stay unidentified until there is evidence to add them.

Linnet's resolver, regional order-number normalization, and firmware-family
mapping live separately in `tools/inspect/ipod_models.py`. Catalogue metadata
does not attest installed hardware or grant installation support. Disk capacity
is never used to guess a product generation. Live USB and storage identity,
followed by device hardware checks, control installation.
