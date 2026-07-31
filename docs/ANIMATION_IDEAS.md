# Animation ideas — design specs for the two-ring glasses

This is a **design document**, not code. It describes a batch of new effects in
enough detail to implement each one directly against the existing engine, with
no guesswork about geometry, speed, colour or state. Nothing here has been
compiled or wired into the firmware yet — it is the spec you hand to whoever
writes the `render_*()` functions (that may well be future-you).

Everything is written against the real contract in `src/led_animations.h`, so
the pseudocode uses the drawing API (`led_ring_set`, `led_all_set`,
`led_px_set`), the speed helpers (`led_speed_delay`, `led_speed_step`), the
colour helpers (`led_hsv_to_rgb`, `led_scale`) and the auto-colour boundary
(`led_cycle_color`) exactly as they exist today. If a symbol appears in the
pseudocode, it is a real function you can call — except a couple of clearly
labelled *proposed helpers* in [§2](#2-shared-building-blocks), which are tiny
and worth adding once so several effects share them.

**Physical picture.** The two rings are the two eyes of a pair of glasses. Ring
0 and ring 1 are equal-size loops (typically 16–24 px each). Each ring is
calibrated with its own `top` ("12 o'clock") and winding `dir`, so a logical
position `p` means "the same clock position on that eye" regardless of how the
strip was physically soldered. Draw in logical positions and the two eyes stay
visually aligned; `led_all_set()` with opposite per-ring `dir` gives a true
left/right mirror. This is what makes twin-eye effects read as symmetric.

**The one-colour constraint, and the trick around it.** The engine has a single
global colour (`f->r/g/b`, set over BLE with `C`). That is deliberate — the UI
has one colour picker. But an effect is free to *synthesise* extra colours at
render time: the rainbow already does (it ignores the global colour entirely and
generates its own hues). So "one global colour" does **not** mean "one colour on
screen." The pattern used throughout this doc: **ring 0 is anchored to the
user's global colour; ring 1 derives a *randomised* companion palette from it**
(a rotated hue, a random gradient span), refreshed at each lap boundary. The two
eyes relate but never quite match — which is exactly the effect the user asked
for.

---

## 1. How to read each spec

Every effect below is written to the same shape so it maps cleanly onto the
three-step "adding an effect" checklist in the README / `led_animations.h`:

1. **Add the enum value** to `led_effect_t` in `src/led_effects.h` and bump
   `EFFECT_MAX`. Suggested indices are noted per effect (current max is
   `EFFECT_WORM = 4`).
2. **Write `render_<name>()`** in `src/led_animations.c` from the pseudocode.
3. **Add the `case`** to the `switch` in `led_render()`.
4. Update the `E<0-N>` row in `README.md` and `PROTOCOL.md` (the effect table).

Each spec lists:

- **What you see** — the visual, in one breath.
- **Rings** — how the two eyes differ (or mirror).
- **Gradient / sparkle mechanics** — the actual maths.
- **Colour** — how it uses the global colour and `auto_color`.
- **Speed** — what `f->speed` should control and the delay range.
- **State** — the file-scope `static`s the renderer keeps between frames.
- **Return** — the inter-frame delay.
- **Tuning knobs** — the two or three constants worth exposing.
- **Caveats** — anything that bites (RAM, wrap seams, brownout).

The headline sparkle effect ([§3](#3-sparkle-comet--headline-sparkle--gradient))
is specified in the most depth; later effects lean on the shared blocks and
stay terser.

---

## 2. Shared building blocks

These are patterns and (a few) proposed helpers that recur across the effects.
Implement the proposed helpers once in `led_animations.c` (file-scope `static`,
above the renderers) and every effect gets simpler.

### 2.1 Per-pixel persistent state and the "max pixels" question

Effects that decay pixels over time (sparkle, fireflies, comet trails) need
per-pixel state that survives between frames. Like any animation phase, that
lives in a file-scope `static` — but now it is an **array**, one slot per
pixel, and it must be sized to a compile-time maximum because C statics cannot
be sized to the runtime `f->count`.

The engine sizes its own pixel buffer to `STRIP_MAX_PIXELS` (the DTS
`chain_length`), but that macro is private to `led_effects.c`. Two clean options:

- **Preferred:** promote the ceiling to a shared constant. Add to
  `led_animations.h`:
  ```c
  /* Compile-time ceiling for per-effect per-pixel state arrays. Must be >= the
   * DTS chain_length; the engine's own pixel buffer uses that same length. */
  #ifndef LED_MAX_PIXELS
  #define LED_MAX_PIXELS 256
  #endif
  ```
  and size sparkle/decay arrays `[LED_MAX_PIXELS]`. 256 bytes/array is nothing.
- **Alternative:** keep a local `#define SPARKLE_MAX 72` in the renderer and
  clamp `count` to it. Simplest, but silently caps the effect on a longer chain.

All per-pixel specs below assume the shared `LED_MAX_PIXELS`.

### 2.2 Randomness

`sys_rand32_get()` from `<zephyr/random/random.h>` is already used by the engine
(`led_effects_cycle_color`). Include that header in `led_animations.c` and use
it freely for sparkle placement, jitter and the randomised second palette.
Cheap idiom for "roughly every Nth frame, do X":
`if ((sys_rand32_get() % N) == 0) { ... }`.

### 2.3 Proposed helper — RGB linear interpolate (a real gradient primitive)

A gradient "around the ring" between two arbitrary colours needs a lerp. The
engine has `led_scale` (channel × brightness) but no two-colour blend. Add:

```c
/* Blend a→b by t/255 (0 = all a, 255 = all b), per channel. */
static inline struct led_rgb led_lerp(struct led_rgb a, struct led_rgb b, uint8_t t)
{
    struct led_rgb o;
    o.r = ((uint16_t)a.r * (255U - t) + (uint16_t)b.r * t) / 255U;
    o.g = ((uint16_t)a.g * (255U - t) + (uint16_t)b.g * t) / 255U;
    o.b = ((uint16_t)a.b * (255U - t) + (uint16_t)b.b * t) / 255U;
    return o;
}
```

With `led_lerp` + `led_hsv_to_rgb` you can build any of the gradients below.

### 2.4 Deriving a randomised companion palette from the global colour

The recurring "ring 1 is a randomised cousin of the global colour" trick. The
global colour is RGB; to rotate its *hue* cheaply without a full RGB→HSV
conversion, keep a private hue accumulator (same approach as
`led_effects_cycle_color`) rather than trying to read the hue back out of
`f->r/g/b`:

```c
static uint8_t companion_hue;          /* file scope */
/* at a lap boundary: */
companion_hue += 40U + (sys_rand32_get() % 176U);   /* +40..215, always visibly different */
uint8_t cr, cg, cb;
led_hsv_to_rgb(companion_hue, &cr, &cg, &cb);        /* ring 1's anchor colour */
```

Ring 0 uses `f->r/g/b` (the true global colour); ring 1 uses `cr/cg/cb`. Refresh
`companion_hue` only at the cycle boundary so ring 1's colour is stable within a
lap and only *changes* when the motion wraps — the same discipline
`led_cycle_color` uses.

### 2.5 Speed, and the refresh floor

Reuse the two-part speed story the existing effects use:

- `led_speed_delay(speed, fast_ms, slow_ms)` sets the frame delay you return.
- Once the delay bottoms out at the ~12 ms refresh floor, `led_speed_step(speed,
  max_step)` moves the animation *further per frame* so top speed is genuinely
  fast. Keep `max_step` small (a handful) or fast motion turns choppy.

Pick `fast_ms`/`slow_ms` per effect from the tables below; they are tuned to the
feel (a lazy aurora wants ~120 ms slow, a chase wants ~60 ms).

### 2.6 Cycle boundaries and `auto_color`

Call `led_cycle_color(f)` **once**, at the instant the motion returns to its
start (a completed lap, a finished breath), and ideally when the strip is
momentarily dark so the swap is invisible. It is a no-op when `auto_color` is
off, so it is always safe to call. Effects that never read the global colour
(the pure-rainbow-style ones) simply never call it.

---

## 3. Sparkle Comet — headline sparkle + gradient

> **The one the user asked for:** a sparkly band that travels around each ring,
> laid over a colour gradient, with ring 1's gradient randomised.

**Suggested id:** `EFFECT_SPARKLE = 5`.

**What you see.** Each eye carries a smooth colour gradient wrapped once around
the ring. A brighter "comet" band sweeps around continuously, and *within and
just behind* that band, individual pixels flare to near-white and fade out —
glints catching the light as the band passes. Ring 0's gradient is built from
the user's colour; ring 1's is a randomised companion, so the two eyes shimmer
in related-but-different hues. It is calm at low speed (a slow drifting
sparkle-haze) and energetic at high speed (a fast glittering chase).

**Rings.** Both rings run the same *structure* but different *palettes*:
- Ring 0 gradient: from `f->r/g/b` at the head to a dimmed/hue-shifted version
  around the loop.
- Ring 1 gradient: from the randomised `companion` colour (§2.4) to its own
  dimmed end.
Both comets share the same head position `head` (logical), so the sparkle band
sits at the same clock angle on both eyes — the symmetry reads as intentional
while the colours differ.

**Gradient mechanics.** For a ring of length `len`, pixel at logical position
`p` gets a base gradient colour. Two good options:

- *Hue sweep (vivid):* `hue = base_hue + p * 256 / len`, then `led_hsv_to_rgb`.
  Ring 0 `base_hue` tracks the global colour's hue accumulator; ring 1 uses
  `companion_hue`. This is the "second gradient can be randomised" reading —
  ring 1 gets its own random `base_hue` and, optionally, its own random *span*
  (e.g. half a wheel instead of a full one) refreshed each lap.
- *Two-colour lerp (on-brand):* `led_lerp(anchor, tail_colour, p * 255 / len)`
  where `anchor` is the ring's colour and `tail_colour` is `anchor` scaled to
  ~25% (a gradient into shadow). Keeps ring 0 unmistakably "the user's colour."

Pick the hue-sweep for a rainbow-ish shimmer, the lerp for a monochrome-glow
shimmer. The pseudocode below shows the lerp; swapping in the hue sweep is two
lines.

**Sparkle mechanics.** Keep a per-pixel brightness buffer `spark[LED_MAX_PIXELS]`
(§2.1), one byte per *physical* pixel (index the whole chain, not per ring — it
is simpler and the two rings occupy disjoint physical ranges anyway). Each
frame:
1. **Decay** every `spark[i]` toward 0 by a fixed `SPARK_DECAY` (e.g. 24). This
   is the fade-out tail.
2. **Seed** new sparkles biased to the comet band: for a few positions near the
   head (say `head-2 .. head+1`), with probability `SPARK_CHANCE` set that
   pixel's `spark` to 255. Seeding *near the head* is what makes the glitter
   travel with the comet instead of twinkling everywhere.
3. **Compose:** final pixel = gradient colour, then add the sparkle as a push
   toward white scaled by `spark[i]` (`led_lerp(gradient, WHITE, spark[i])`),
   then apply master `f->brightness`.

**Colour.** Ring 0 reads the global colour every frame (so dragging the picker
updates it live). Ring 1's companion palette is refreshed at each lap. Call
`led_cycle_color(f)` when `head` wraps — with `auto_color` on, the global colour
(and thus ring 0) drifts lap by lap, and ring 1's companion re-randomises in the
same beat.

**Speed.** `head` advances by `led_speed_step(f->speed, 4)` per frame; return
`led_speed_delay(f->speed, 6, 70)`. Low speed → slow drift, few sparkles moving
lazily; high speed → fast chase. Optionally scale `SPARK_CHANCE` up with speed so
faster looks denser.

**State.**
```c
static uint16_t head;                    /* comet head, logical position */
static uint8_t  spark[LED_MAX_PIXELS];   /* per-physical-pixel glint brightness */
static uint8_t  companion_hue;           /* ring 1's randomised anchor hue */
```

**Pseudocode.**
```c
static uint32_t render_sparkle(const struct led_frame *f)
{
    const uint16_t len = led_ring_len(f);
    if (len == 0U) return 50U;

    const struct led_rgb WHITE = { 255, 255, 255 };
    const uint8_t SPARK_DECAY  = 24;
    const uint8_t SPARK_CHANCE = 40;   /* higher = fewer (1-in-N per seed slot) */

    /* 1. decay existing glints (whole active chain) */
    for (uint16_t i = 0; i < f->count; i++)
        spark[i] = (spark[i] > SPARK_DECAY) ? spark[i] - SPARK_DECAY : 0;

    /* 2. per-ring gradient + comet, drawn in logical positions */
    for (int ring = 0; ring < led_ring_count(f); ring++) {
        /* ring anchor colour: ring 0 = global, ring 1 = randomised companion */
        struct led_rgb anchor;
        if (ring == 0) { anchor = (struct led_rgb){ f->r, f->g, f->b }; }
        else {
            uint8_t cr, cg, cb;
            led_hsv_to_rgb(companion_hue, &cr, &cg, &cb);
            anchor = (struct led_rgb){ cr, cg, cb };
        }
        struct led_rgb tail = { anchor.r/4, anchor.g/4, anchor.b/4 };  /* into shadow */

        for (uint16_t p = 0; p < len; p++) {
            /* base gradient around the loop */
            struct led_rgb col = led_lerp(anchor, tail, (uint8_t)(p * 255U / len));

            /* comet: brighten a short arc around the head, wrap-aware */
            uint16_t d = (p + len - (head % len)) % len;   /* distance behind head */
            if (d < 5U) {
                uint8_t boost = 255U - d * 51U;            /* 255,204,...,51 */
                col = led_lerp(col, anchor, boost);        /* head pops toward full anchor */
            }

            /* map this ring's logical p to a physical index and seed sparkles there */
            /* (led_ring_set does the mapping for drawing; for the sparkle seed we
             *  reuse the same head-relative window) */
            if (d < 4U && (sys_rand32_get() % SPARK_CHANCE) == 0U) {
                /* seed via a raw write is awkward; instead mark by drawing bright now
                 * and letting the persistent buffer keep it — see note below */
            }

            /* apply persistent sparkle + master brightness, then draw */
            /* NB: sparkle is indexed physically; get the phys index from the ring */
            col.r = led_scale(col.r, f->brightness);
            col.g = led_scale(col.g, f->brightness);
            col.b = led_scale(col.b, f->brightness);
            led_ring_set(f, ring, p, col);
        }
    }

    /* 3. sparkle overlay, indexed physically over the whole active chain */
    for (uint16_t i = 0; i < f->count; i++) {
        if ((sys_rand32_get() % 60U) == 0U) spark[i] = 255;   /* ambient twinkle */
        if (spark[i]) {
            struct led_rgb cur = f->pixels[i];                 /* read gradient back */
            f->pixels[i] = led_lerp(cur, WHITE, led_scale(spark[i], f->brightness));
        }
    }

    /* advance + lap boundary */
    uint16_t prev = head;
    head = (uint16_t)((head + led_speed_step(f->speed, 4U)) % len);
    if (head < prev) {
        companion_hue += 40U + (sys_rand32_get() % 176U);   /* re-randomise ring 1 */
        led_cycle_color(f);                                 /* drift global if auto */
    }
    return led_speed_delay(f->speed, 6U, 70U);
}
```

> **Implementation note on sparkle indexing.** The cleanest structure is: (a)
> draw the gradient+comet with `led_ring_set` (logical), then (b) run the
> sparkle overlay as a *second pass* over physical indices `0..f->count`, reading
> each pixel back out of `f->pixels[i]` and pushing it toward white. That is what
> the pseudocode's step 3 does, and it sidesteps the logical↔physical mismatch —
> the sparkle biasing toward the comet head can be dropped in favour of uniform
> ambient twinkle (shown) for a first cut, then reintroduced by seeding
> `spark[led__ring_phys(...)]` if you want the glints to visibly travel with the
> band. Start with ambient; it already looks great.

**Tuning knobs.** `SPARK_DECAY` (tail length — lower = longer trails),
`SPARK_CHANCE` / the ambient `% 60` (density), comet arc length (`d < 5`),
gradient tail depth (`/4`).

**Caveats.** The two-pass approach reads `f->pixels` back, so it must run *after*
the gradient pass and before the engine flush (it is — it is the last thing the
renderer does). `spark[]` is `LED_MAX_PIXELS` bytes of BSS; fine. If you skip
`led_lerp`, you can approximate the sparkle by `MAX(channel, spark[i])` per
channel — cheaper, slightly harsher glint.

---

## 4. Aurora Drift — slow flowing gradient

**Suggested id:** `EFFECT_AURORA = 6`.

**What you see.** Soft bands of colour drift slowly around both eyes, like
northern lights — no hard edges, no discrete pixels, just a breathing gradient
that never repeats quite the same way. Meditative; the "ambient" effect.

**Rings.** Ring 1 runs the same field as ring 0 but **phase-shifted** (offset the
sample position by `len/3`) so the two eyes are never in lockstep — the drift
looks organic rather than mechanical.

**Gradient mechanics.** Sum two slow sine-ish waves of different wavelengths over
position and time to get a smooth, non-repeating hue/brightness field. Zephyr has
no `sinf` guarantee on this build without enabling the math lib, so use a small
256-entry sine LUT (a `static const uint8_t`) or a triangle wave — both read fine
at this size. Hue at pixel `p`, time `t`:
`hue = base + wave(p*W1 + t) + wave(p*W2 - t*2)`, then `led_hsv_to_rgb`. Anchor
`base` to the global colour's hue accumulator so the user's picker tints the
whole aurora; ring 1 adds a random `+companion` offset.

**Colour.** Tinted by the global colour (via `base`). Call `led_cycle_color(f)`
every ~256 time-steps (one slow full cycle) if you want it to wander with
`auto_color`.

**Speed.** Advance `t += led_speed_step(f->speed, 3)`; return
`led_speed_delay(f->speed, 20, 120)`. This one wants to stay *slow* — cap the
fast end higher (20 ms) than a chase.

**State.** `static uint16_t t;` plus the const sine LUT.

**Tuning knobs.** Wavelengths `W1`,`W2` (how many bands), the ring-1 phase
offset, the LUT amplitude (contrast).

**Caveats.** Keep amplitudes modest or the hue wraps too fast and the "soft
band" look becomes a rainbow. No per-pixel state needed — it is a pure function
of `p` and `t`, so it is cheap.

---

## 5. Twin Meteors — comets crossing the eyes

**Suggested id:** `EFFECT_METEOR = 7`.

**What you see.** A bright meteor with a fading tail shoots around each ring. The
two meteors travel in **opposite directions** and meet/cross at the top and
bottom each lap — a satisfying "blink" as they pass. Dark background.

**Rings.** This is the mirror primitive taken literally: ring 0's meteor runs
forward from `head`, ring 1's runs backward from `len-head`. (Same idea as
`render_worm`, but with a longer, smoother tail and a brighter head.)

**Gradient mechanics.** The tail *is* the gradient: pixel at distance `d` behind
the head gets brightness `255 - d*(255/TAIL)`, so the head is white-hot and the
tail fades to black. Colour: ring 0 uses the global colour, ring 1 the randomised
companion — two differently-coloured meteors.

**Colour.** `led_clear(f)` first (only a few pixels lit). `led_cycle_color(f)` on
lap wrap.

**Speed.** `head += led_speed_step(f->speed, 4)`; `led_speed_delay(f->speed, 8,
80)` — same envelope as the worm, which feels right for a chase.

**State.** `static uint16_t head; static uint8_t companion_hue;`

**Tuning knobs.** `TAIL` length (8–12 looks good on a 16–24 px ring), head
over-brightness (blend head toward white).

**Caveats.** Tail length near `len` overlaps itself across the wrap; keep `TAIL
< len`. If you want *multiple* meteors per ring, space N heads by `len/N` — no
extra state, just a loop.

---

## 6. Heartbeat — the lub-dub pulse

**Suggested id:** `EFFECT_HEARTBEAT = 8`.

**What you see.** Both eyes pulse in the global colour with a real cardiac
rhythm: a quick strong beat, a quick softer beat, then a rest — *lub-dub … …
lub-dub … …* Solid fill, no motion around the ring, just brightness. Reads as
"alive," great for a wearable.

**Rings.** Identical on both eyes (`led_all_set` / whole-strip fill). Symmetry is
the point.

**Gradient mechanics.** None spatial — it is a brightness *envelope* over time.
Precompute or compute a 2-pulse envelope: a short tall Gaussian-ish bump, a gap,
a shorter bump, a long gap, repeat. Easiest as a small `static const uint8_t
beat[N]` LUT you step through; `bri = beat[phase]`.

**Colour.** Pure global colour × envelope × `f->brightness`. `led_cycle_color(f)`
at the end of the rest (envelope == 0, strip dark → seamless swap).

**Speed.** Steps through the `beat[]` LUT; `led_speed_step` sets how many entries
per frame (BPM), `led_speed_delay(f->speed, 4, 30)` the frame rate. Default speed
≈ resting 60–70 bpm; max speed = racing heart.

**State.** `static uint16_t phase;` + the `beat[]` LUT.

**Tuning knobs.** The envelope shape (bump heights, gap lengths) — this is where
the "realness" lives. A subtle deep-red default colour sells it.

**Caveats.** Keep the two bumps clearly unequal (dub ≈ 60% of lub) or it reads as
a plain double-blink rather than a heartbeat.

---

## 7. Fireflies — drifting independent twinkles

**Suggested id:** `EFFECT_FIREFLY = 9`.

**What you see.** A dark field with a handful of soft points that each fade *in*,
glow, and fade *out* at their own pace and position — fireflies on a summer
night. No chase, no gradient sweep; gentle, sparse, random. The calm cousin of
the Sparkle Comet.

**Rings.** Both rings share one physical-indexed field, so fireflies appear
independently on either eye — no imposed symmetry, which suits the organic look.

**Gradient mechanics.** Per-pixel state again, but with *two* phases per pixel:
each active firefly has a brightness that ramps up then down (a triangle over its
lifetime). Store `fly[LED_MAX_PIXELS]` as a signed-ish ramp, or reuse the sparkle
buffer with a *slower* decay and *rarer, softer* seeding (seed to a random 120–255
rather than always 255). Colour each firefly either the global colour, or —
nicer — a slight random hue jitter around it: `hue = base + rand(±20)`.

**Colour.** Tinted by global colour. `auto_color`: call `led_cycle_color(f)` on a
timer (e.g. every ~300 frames) since there is no natural "lap."

**Speed.** Controls seed rate and decay speed together: faster = more, quicker
fireflies. `led_speed_delay(f->speed, 15, 90)` — this wants to breathe slowly.

**State.** `static uint8_t fly[LED_MAX_PIXELS];` (+ optional per-pixel hue jitter
array if you want coloured fireflies, another `LED_MAX_PIXELS` bytes).

**Tuning knobs.** Seed probability (density), decay rate (lifetime), max
brightness of a seed (softness), hue jitter range.

**Caveats.** Two `LED_MAX_PIXELS` arrays if you add hue jitter — still cheap.
Keep density low (a few lit at once) or it stops reading as *fireflies* and
becomes noise.

---

## 8. Pinwheel — rotating gradient wedges

**Suggested id:** `EFFECT_PINWHEEL = 10`.

**What you see.** Each eye is split into a few equal wedges of colour that rotate
steadily, like a spinning pinwheel or a loading spinner. Crisp and graphic —
the opposite of the aurora's softness.

**Rings.** Ring 1 rotates the **opposite way** to ring 0 (counter-rotating
pinwheels), which looks striking on two eyes. Optionally offset ring 1's wedge
colours by the randomised companion hue.

**Gradient mechanics.** Divide the ring into `SEG` segments (`SEG = 3` or 4).
Pixel `p` is in segment `s = ((p + rot) % len) * SEG / len`. Colour each segment
by `led_hsv_to_rgb(base_hue + s * (256/SEG))` — evenly spaced hues around the
wheel, anchored to the global colour's hue on ring 0, companion hue on ring 1.
For softer edges, `led_lerp` between adjacent segment colours across a couple of
boundary pixels.

**Colour.** Anchored to global via `base_hue`. `led_cycle_color(f)` each full
rotation.

**Speed.** `rot += led_speed_step(f->speed, 4)`; `led_speed_delay(f->speed, 8,
70)`.

**State.** `static uint16_t rot; static uint8_t companion_hue;`

**Tuning knobs.** `SEG` (2 = yin-yang split, 4 = quarters), edge softness, the
counter-rotation on/off.

**Caveats.** Hard segment edges can strobe at high speed on a small ring; soften
the boundaries or cap `max_step` at 3.

---

## 9. Confetti — random colour pops

**Suggested id:** `EFFECT_CONFETTI = 11`.

**What you see.** Random pixels pop on in random vivid colours and fade out —
festive, busy, celebratory. A classic (it is a staple of FastLED demos) and a
good stress-test of the per-pixel decay machinery.

**Rings.** Whole-chain, physical-indexed; both eyes twinkle independently.

**Gradient mechanics.** Per-pixel decay buffer for brightness *and* a per-pixel
hue buffer (so each pop keeps its own colour as it fades). Each frame: decay all;
then with some probability pick a random pixel, give it a random hue
(`sys_rand32_get()`), full brightness. Render each lit pixel as
`led_hsv_to_rgb(hue[i])` scaled by `bright[i]`.

**Colour.** Ignores the global colour entirely (like the rainbow) — the pops are
self-coloured, so `auto_color` is irrelevant and it never calls
`led_cycle_color`. Optionally add a mode flag to bias hues near the global colour
for a themed confetti.

**Speed.** Pop rate + decay: `led_speed_delay(f->speed, 8, 60)`, and scale the
pop probability with speed.

**State.** `static uint8_t c_bri[LED_MAX_PIXELS]; static uint8_t
c_hue[LED_MAX_PIXELS];`

**Tuning knobs.** Pop probability, decay rate, whether hues are global-themed or
fully random.

**Caveats.** Two `LED_MAX_PIXELS` arrays. Very high pop-rate + slow decay
saturates the ring to near-white; keep decay brisk.

---

## 10. Effect quick-reference

| # (proposed) | Name          | Sparkle | Gradient        | Ring relationship          | Uses global colour | Per-pixel state |
|--------------|---------------|:-------:|-----------------|----------------------------|:------------------:|:---------------:|
| 5  | **Sparkle Comet** | ✔ | sweep + comet   | shared motion, split palette | ring 0 = global, ring 1 random | `spark[]` |
| 6  | Aurora Drift      |   | soft flowing    | phase-shifted              | tint               | none            |
| 7  | Twin Meteors      |   | tail = gradient | mirrored, opposite dir     | ring 0 = global, ring 1 random | none |
| 8  | Heartbeat         |   | brightness env. | identical (symmetric)      | yes                | none            |
| 9  | Fireflies         | ✔ | —               | independent                | tint               | `fly[]`         |
| 10 | Pinwheel          |   | wedge hues      | counter-rotating           | anchor             | none            |
| 11 | Confetti          | ✔ | —               | independent                | ignored            | `bri[]`,`hue[]` |

**If you only build one:** the Sparkle Comet ([§3](#3-sparkle-comet--headline-sparkle--gradient))
is the direct answer to the brief — sparkle + travelling gradient + a randomised
second ring. **If you build two,** pair it with the Aurora Drift for a calm
counterpoint (no per-pixel state, pure function, cheap) or Twin Meteors for a
symmetric showpiece.

---

## 11. Implementation order & checklist

Recommended order — each step reuses the last:

1. **Add `led_lerp` and `LED_MAX_PIXELS`** to `led_animations.h` (§2.1, §2.3).
   Nothing depends on the engine; verify it still builds.
2. **Twin Meteors (7)** — no per-pixel state, exercises the randomised companion
   palette and mirror geometry. Lowest-risk first real effect.
3. **Sparkle Comet (5)** — the headline; builds on the companion palette from
   step 2 and adds the `spark[]` buffer. Start with ambient twinkle, add
   head-biased seeding once it looks right.
4. **Aurora Drift (6)** — introduces the sine LUT; pure function, easy to tune.
5. Cherry-pick Heartbeat / Fireflies / Pinwheel / Confetti as desired.

For each effect, the mechanical wiring (unchanged from the README's recipe):

- [ ] enum value in `led_effect_t` + bump `EFFECT_MAX` (`src/led_effects.h`)
- [ ] `render_<name>()` in `src/led_animations.c`
- [ ] `case` in `led_render()`
- [ ] `E<0-N>` row + effect list in `README.md` **and** `PROTOCOL.md`
- [ ] eyeball on real hardware at low/default/high speed and at low brightness
      (the first-boot brightness is deliberately ~64 — sparkle glints must still
      read at that level)

**Two cross-cutting reminders from the engine:**

- Effects run on one thread, one at a time, so all the file-scope `static`s above
  are safe with no locking.
- The engine blanks pixels past `f->count`, so every renderer only ever fills
  `pixels[0 .. count-1]` — never assume a fixed ring size; read `led_ring_len(f)`
  / `led_ring_count(f)` so one effect works on a 16-, 20- or 24-px ring and on a
  degenerate single-ring build.
