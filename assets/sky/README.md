# Sky data

## `bright_stars.qsc` — bright star catalogue (ROADMAP R22.2.1)

9096 stars down to V ≈ 8 (complete to V ≈ 6.5, the naked-eye limit): J2000
positions, Johnson V magnitudes and B−V colours, cooked by
[`scripts/cook-star-catalogue.py`](../../scripts/cook-star-catalogue.py)
(the file format is documented there and in
`include/engine/astro/star_catalogue.hpp`). The engine reads it at startup;
nothing is downloaded at runtime.

**Source.** The Yale Bright Star Catalogue, 5th revised edition (Hoffleit &
Warren 1991), catalogue V/50 of the CDS Strasbourg and the NASA
Astronomical Data Center, in the JSON conversion by Bretton Wade
(<https://github.com/brettonw/YaleBrightStarCatalog>, `bsc5.json`, fetched
2026-11-28, SHA-256
`08b019f560a20fdf67565604b2ad04c3e40938f71a4a7c187af2b88fad1a6e8b`).

**Licence.** The catalogue is a compilation of astronomical measurements
published by NASA's ADC and the CDS for unrestricted scientific use; the
fields kept here (positions, magnitudes, colours) are facts and carry no
copyright. It is used as public-domain data, the status the owner asked to
verify. The JSON conversion script that produced the input is MIT-licensed
(© 2016 Bretton Wade); only its output data is used, not its code. 309
stars without a B−V in the catalogue get one derived from the conversion's
colour temperature (Ballesteros 2012 inverted); one star has neither and
gets a solar B−V of 0.65; B−V is clamped to [−0.4, 2.5], the range the
colour model is fitted to (this touches a few peculiar carbon stars).

To re-cook:

```sh
curl -LO https://raw.githubusercontent.com/brettonw/YaleBrightStarCatalog/master/bsc5.json
python3 scripts/cook-star-catalogue.py bsc5.json assets/sky/bright_stars.qsc
```
