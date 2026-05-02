---

# Concerto EQ — DSP Research Reference

Prepared for the Concerto (formerly PLYR) desktop audio player. This is reference material to consult during implementation; no code is produced here.

Primary sources are cited inline and summarized in Section 12. Where a source is unreliable or undocumented (notably Apple's iTunes preset numeric values), that is called out explicitly — do not treat consensus blog posts as gospel.

---

## 1. Biquad Filter Fundamentals

### 1.1 The building block

A biquad is a second-order IIR section with transfer function

```
        b0 + b1*z^-1 + b2*z^-2
H(z) = -------------------------
        a0 + a1*z^-1 + a2*z^-2
```

Usually `a0` is normalized to 1 by dividing all six coefficients by `a0` at the end of coefficient calculation. The recurrence (see 1.2) then uses five multiplies and four adds per sample per biquad.

A parametric EQ is N biquads in series, one per band. Each band's biquad is configured with its own (freq, gain, Q) and runs on the same audio stream, output-of-one feeds input-of-next. For stereo, you need **independent state per channel**; coefficients are shared.

### 1.2 The difference equation

The canonical Direct Form I difference equation (assuming a0 already folded in):

```
y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2]
                - a1*y[n-1] - a2*y[n-2]
```

The minus signs in front of `a1, a2` are a convention baked into the RBJ formulas — the denominator polynomial's coefficients are stored with that sign implicit in the recurrence. If you instead keep the sign explicit (i.e. store the polynomial coefficients verbatim and use `+ a1*y[n-1] + a2*y[n-2]` but with negated `a1, a2`), coefficient tables from different cookbooks will silently disagree. Pick one convention project-wide and document it.

### 1.3 The four classic forms

There are four algebraically equivalent forms with different numerical behavior:

- **Direct Form I (DF1)** — stores last two `x` and last two `y` samples (4 state words). Single summation point at the output. Only place numbers can overflow in fixed point is at the final sum. This is the form most textbooks present first.
- **Direct Form II (DF2)** — canonical form: merges the two delay lines into one intermediate signal `w[n]`, saving 2 state words. Downside: the intermediate `w` can be much larger in magnitude than the input or output (the poles boost it, the zeros then cancel that boost), so fixed-point DF2 overflows internally where DF1 would not.
- **Transposed Direct Form I (TDF1)** — rare, not discussed further.
- **Transposed Direct Form II (TDF2)** — derived by reversing the signal flow graph of DF2. Two state words, like DF2, but the intermediate sums are organized so that each addition in the recurrence mixes numbers of similar magnitude. That matters in floating point, where adding a tiny number to a large one loses the tiny number's low bits (catastrophic cancellation on the next subtraction).

### 1.4 Which form to use

Consensus across Julius Smith (CCRMA), Nigel Redmon (EarLevel), the CMSIS-DSP library, and musicdsp.org:

- **Fixed point**: Direct Form I. No internal overflow risk, and most fixed-point DSPs have an extended-precision accumulator that tolerates the intermediate sum.
- **Floating point (our case)**: **Transposed Direct Form II**. Superior floating-point accuracy because intermediate sums combine numbers of similar magnitude, minimizing swamping. Also has the nice side effect that under parameter modulation, the two state variables act as a mild smoother (one-pole-ish behavior) between old and new coefficient values, reducing click/pop artifacts compared to DF1.

TDF2 recurrence (per sample):

```
y[n]  = b0*x[n] + s1
s1    = b1*x[n] - a1*y[n] + s2
s2    = b2*x[n] - a2*y[n]
```

`s1, s2` are the two state variables carried from sample to sample.

### 1.5 Low-frequency numerical problems

Biquads get unstable and inaccurate at low frequencies relative to sample rate, for a structural reason. The pole radius approaches the unit circle as `f0 / Fs` approaches zero, and the coefficients `a1, a2` approach `-2` and `+1`. Small quantization errors in `a1, a2` produce large pole-position errors, which at low Q produces audible detuning and at high Q produces outright instability.

Quantitative guidance from Redmon (EarLevel): "24-bit coefficients and memory work well for most filters, but start to become unstable below about 300 Hz at 48 kHz." Below ~1/100 of Fs the problem becomes severe.

**Implication for Concerto**: at 44.1/48 kHz, the lowest ISO band (31.25 Hz) is near the edge. At 96 or 192 kHz, it's well into the problem zone. Mitigations in order of effectiveness:

1. **Use double-precision state variables** even if audio samples are `float`. Coefficients can be `double`; recurrence in `double`; convert to `float` only at the boundary with the audio callback. This is the single most effective fix, and on modern x86-64 the cost of `double` vs `float` for a simple biquad is negligible (both SIMD pipelines handle both cleanly).
2. **Double-precision coefficients** (this is automatic if you compute them in `double`, which you should).
3. For the lowest band specifically at high sample rates, consider a State Variable Filter (SVF) topology (Chamberlin / Zavalishin TPT). SVFs place their poles via sin/cos tables and do not suffer the low-frequency coefficient precision problem. Recommended only if you observe artifacts; otherwise a double-precision TDF2 biquad is fine.

### 1.6 Why double-precision state matters even with float audio

The recurrence feeds output back into input. Roundoff from one sample accumulates into the next. In a high-Q filter the state sits near the pole, which can be enormously amplified relative to the output sample. A float state variable that's 10^6 times bigger than the output will have its low 20 or so bits below the output's LSB — those bits *are* the filter's memory of quiet/resonant content, so losing them produces noise and detuning. Double gives you 52 mantissa bits vs 23 — plenty of headroom for audio biquads at any reasonable Q and frequency.

---

## 2. The RBJ Audio EQ Cookbook Coefficient Formulas

Source: Robert Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter coefficients," hosted authoritatively at W3C (audio-eq-cookbook). This is the de facto industry standard; the Web Audio API `BiquadFilterNode` uses these exact formulas, as do countless plugins, DAWs, and hardware DSPs.

### 2.1 Inputs and intermediate variables

Inputs to the coefficient calculation:

- `f0` — center/corner frequency in Hz
- `Fs` — sample rate in Hz
- `dBgain` — gain in dB (for peaking and shelving only)
- Exactly one of: `Q`, `BW` (bandwidth in octaves), or `S` (shelf slope)

Intermediate values:

```
A     = 10^(dBgain/40)          (peaking & shelving only; note /40 not /20,
                                 because it's voltage amplitude of half-gain)
w0    = 2 * pi * f0 / Fs        (normalized angular frequency)
cos_w0 = cos(w0)
sin_w0 = sin(w0)
```

Alpha (three forms — the caller picks which is most natural):

```
alpha = sin_w0 / (2*Q)                                           [Q case]

alpha = sin_w0 * sinh( (ln(2)/2) * BW * w0 / sin_w0 )            [BW case, digital]

alpha = (sin_w0 / 2) * sqrt( (A + 1/A) * (1/S - 1) + 2 )         [S case, shelving only]
```

Conversions between Q and BW:

```
1/Q = 2 * sinh( (ln(2)/2) * BW * w0 / sin_w0 )       (digital filter)
1/Q = 2 * sinh( (ln(2)/2) * BW )                     (analog prototype)
```

Conversion between S and "Q at A" used by shelving:

```
1/Q = sqrt( (A + 1/A) * (1/S - 1) + 2 )
2 * sqrt(A) * alpha = sin_w0 * sqrt( (A^2 + 1) * (1/S - 1) + 2*A )
```

When `S = 1`, the shelf is as steep as possible without the magnitude response becoming non-monotonic (no overshoot bump before the shelf).

### 2.2 Meaning of Q, BW, S

From RBJ's own notes:

- **Q** is the standard EE Q factor. For peaking EQ there is a subtlety: the Q parameter in the peaking formula is "half-gain" Q (the gain is `dBgain/2` at the `-BW` frequencies). A boost of N dB with some Q followed by a cut of N dB with the same Q and f0 gives a flat unity response — this is the "matched" or "compensating" property and it's the reason RBJ's peaking formulation is the one everyone uses.
- **BW** (bandwidth in octaves) is measured:
  - between the -3 dB frequencies for BPF and notch,
  - between the half-gain (`dBgain/2`) frequencies for peaking EQ.
- **S** (shelf slope) applies only to shelving. `S=1` is the steepest monotonic slope; slope in dB/octave scales with S.

### 2.3 Coefficients for each filter type

All formulas use the intermediates above and produce raw coefficients. You then normalize by dividing all six by `a0` (or keep `a0` and divide at runtime — normalizing once at coefficient-update time is cheaper).

**Low-Pass Filter (LPF)**
```
b0 = (1 - cos_w0) / 2
b1 =  1 - cos_w0
b2 = (1 - cos_w0) / 2
a0 =  1 + alpha
a1 = -2 * cos_w0
a2 =  1 - alpha
```

**High-Pass Filter (HPF)**
```
b0 =  (1 + cos_w0) / 2
b1 = -(1 + cos_w0)
b2 =  (1 + cos_w0) / 2
a0 =  1 + alpha
a1 = -2 * cos_w0
a2 =  1 - alpha
```

**Band-Pass Filter — constant skirt gain (peak gain = Q)**
```
b0 =  sin_w0 / 2           (= Q * alpha)
b1 =  0
b2 = -sin_w0 / 2           (= -Q * alpha)
a0 =  1 + alpha
a1 = -2 * cos_w0
a2 =  1 - alpha
```

**Band-Pass Filter — constant 0 dB peak gain**
```
b0 =  alpha
b1 =  0
b2 = -alpha
a0 =  1 + alpha
a1 = -2 * cos_w0
a2 =  1 - alpha
```

**Notch**
```
b0 =  1
b1 = -2 * cos_w0
b2 =  1
a0 =  1 + alpha
a1 = -2 * cos_w0
a2 =  1 - alpha
```

**All-Pass (APF)**
```
b0 =  1 - alpha
b1 = -2 * cos_w0
b2 =  1 + alpha
a0 =  1 + alpha
a1 = -2 * cos_w0
a2 =  1 - alpha
```

**Peaking EQ (bell)** — the workhorse of parametric EQ
```
b0 =  1 + alpha * A
b1 = -2 * cos_w0
b2 =  1 - alpha * A
a0 =  1 + alpha / A
a1 = -2 * cos_w0
a2 =  1 - alpha / A
```

**Low Shelf**
```
b0 =     A * ( (A+1) - (A-1)*cos_w0 + 2*sqrt(A)*alpha )
b1 = 2 * A * ( (A-1) - (A+1)*cos_w0 )
b2 =     A * ( (A+1) - (A-1)*cos_w0 - 2*sqrt(A)*alpha )
a0 =         ( (A+1) + (A-1)*cos_w0 + 2*sqrt(A)*alpha )
a1 =    -2 * ( (A-1) + (A+1)*cos_w0 )
a2 =         ( (A+1) + (A-1)*cos_w0 - 2*sqrt(A)*alpha )
```

**High Shelf**
```
b0 =      A * ( (A+1) + (A-1)*cos_w0 + 2*sqrt(A)*alpha )
b1 = -2 * A * ( (A-1) + (A+1)*cos_w0 )
b2 =      A * ( (A+1) + (A-1)*cos_w0 - 2*sqrt(A)*alpha )
a0 =          ( (A+1) - (A-1)*cos_w0 + 2*sqrt(A)*alpha )
a1 =      2 * ( (A-1) - (A+1)*cos_w0 )
a2 =          ( (A+1) - (A-1)*cos_w0 - 2*sqrt(A)*alpha )
```

### 2.4 Constant-skirt vs constant-peak BPF — clarification

There is no "band-pass EQ" in the RBJ lexicon (see §3 for terminology). There are two BPF variants:

- **Constant skirt gain** — the bandpass has peak gain = Q. Useful when Q is interpreted as gain (classic analog BPF behavior).
- **Constant 0 dB peak gain** — normalized so the peak is always 0 dB regardless of Q. Useful as a "signal extractor" that doesn't change loudness as you change Q.

For an EQ application that uses bell/peaking shapes, neither BPF is what you want — use peaking EQ. BPF has two zeros at DC and Nyquist (it attenuates everything away from f0), whereas peaking EQ has a flat response away from f0 and only a bump or dip at f0.

---

## 3. Parametric vs Graphic vs "Band-Pass" EQ

### 3.1 Terminology and tradeoffs

**Parametric EQ**: every band has three controls — frequency, gain, Q/bandwidth. Filter types are peaking (bell) for interior bands, plus shelves at the extremes and optional LPF/HPF. Professional mix/mastering tool. Band count is typically 4–8 for full parametrics (e.g. FabFilter Pro-Q, Waves SSL E-channel) but music-player tone controls are usually 3 (bass/mid/treble, which is really two shelves + one bell).

**Semi-parametric** (or "sweepable"): frequency and gain are adjustable, Q is fixed. Common in guitar amp channel strips and car stereos.

**Graphic EQ**: N fixed bands at preset center frequencies (usually ISO octave or 1/3-octave), each band has only a gain control. The collective slider positions *look* like the EQ curve (hence "graphic"). Classic 10-band consumer EQ, 31-band pro EQ.

**"Band-pass EQ"** — not a standard term. The user's hunch is correct: when people say "band-pass EQ" in the context of iTunes-style tone shaping, they almost always mean a graphic EQ implemented as **a series of fixed-Q peaking biquads at preset frequencies**. Strictly speaking RBJ's BPF (the attenuate-everywhere-except-f0 filter) is not what's running under those sliders — it's peaking EQ with a fixed Q. The confusion arises because each slider "affects a band of frequencies," which sounds like a band-pass operation. Avoid the ambiguous term in code and docs; use "graphic EQ" or "fixed-frequency peaking EQ."

Under the hood, both a graphic EQ and a parametric EQ can use the same cascade of RBJ peaking biquads. The only difference is what the UI exposes and, often, whether Q is fixed or user-adjustable.

### 3.2 Constant-Q vs proportional-Q (per Rane)

Rane Note 101 and 154 (the canonical industry references — Dennis Bohn) lay this out:

- **Proportional-Q** (aka reciprocal-Q, variable-Q): bandwidth widens as you move the slider toward 0 dB and narrows at extreme boost/cut. Older gyrator-based analog graphic EQs did this naturally because the passive analog topology forced it. The filter is "truly 1/3-octave wide only at extreme boost/cut; at modest settings the effective bandwidth exceeds one octave." This makes the front-panel sliders misrepresent what's actually happening.
- **Constant-Q**: bandwidth stays fixed regardless of slider position. The RBJ peaking formulas are inherently constant-Q, which is one reason they won out in digital. Downside: adjacent boosted bands show visible ripple between peaks (because each band really is as narrow as the spec says), whereas proportional-Q naturally "smooths out" summed curves.

**Recommendation for Concerto**: use constant-Q. It's what you get for free from RBJ peaking biquads, it matches what the UI sliders show, and modern users (esp. those coming from Equalizer APO, VST plugins, Spotify's tone control, etc.) expect constant-Q behavior.

### 3.3 Which style for a music-player use case

Given the requirements (users primarily pick presets, occasionally tweak sliders):

Recommendation: **10-band constant-Q graphic EQ** on the ISO octave set (see §4), implemented internally as a cascade of 10 RBJ peaking biquads at fixed frequency and fixed Q, with only the gain per band changing at runtime. Plus a **preamp gain** control (see §7).

Reasons:
- Presets are trivially representable as 10 dB values (or 10 gain values + preamp). Every existing preset library for iTunes/Winamp/Foobar/XMMS maps directly.
- User mental model is dead simple — slider up = boost that band.
- Zero ambiguity about "what is Q here" — Q is an implementation detail, fixed and hidden.
- Fully constant-Q behavior is what users expect.

If you later want a power-user mode, a separate 5-band parametric with freq/gain/Q per band costs the same underlying biquad code; it's purely UI. You don't need to choose now.

You could alternatively do a 3-band shelving/bell tone stack (low-shelf + peaking-mid + high-shelf, like a classic channel strip) — simpler UI, but then presets that rely on non-adjacent-band contouring (e.g. Loudness V-curves, Dance's scooped mids) can't be represented. 10-band is the sweet spot.

---

## 4. Standard ISO Frequency Bands

The relevant standards are **ISO 266:1997** (preferred frequencies for acoustical measurements) and **ANSI S1.11-2004** (octave-band filters). These specify the "preferred numbers" used for labeling; the actual center frequencies are calculated as `1000 Hz · 10^(n/10)` for base-10 octave filters or `1000 Hz · 2^(n/3)` for base-2.

### 4.1 10-band octave graphic EQ (ISO-preferred)

Canonical "hi-fi" labeling, used by almost every consumer 10-band EQ (including iTunes):

```
 31.5 Hz   (sometimes labeled 31 or 32)
 63   Hz   (sometimes labeled 64)
125   Hz
250   Hz
500   Hz
  1   kHz
  2   kHz
  4   kHz
  8   kHz
 16   kHz
```

Note: iTunes labels the low two bands as "32" and "64" rather than "31.5" and "63"; these are rounding conventions only, the actual filter frequencies differ negligibly from the ISO preferred values. For Concerto, use the ISO-preferred labels (31.5, 63, 125, ...) in UI strings but document that the internal filter f0 values match the ISO labels exactly.

### 4.2 31-band 1/3-octave pro graphic EQ

Used in live-sound ringing-out / room-tuning:

```
20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500,
630, 800, 1k, 1.25k, 1.6k, 2k, 2.5k, 3.15k, 4k, 5k, 6.3k, 8k, 10k, 12.5k, 16k, 20k
```

Overkill for a music player. Don't ship this as a default view; if power users want finer control, parametric is the right answer, not 31-band.

### 4.3 Historical player band choices

- **iTunes** (classic, all versions): 10 bands, ISO octave labeling as 32, 64, 125, 250, 500, 1k, 2k, 4k, 8k, 16k. dBgain range ±12 dB. Plus a preamp slider with the same ±12 dB range.
- **Winamp** (classic 2.x, 5.x): 10 bands, labeled as 60, 170, 310, 600, 1k, 3k, 6k, 12k, 14k, 16k in newer versions; older versions showed 70, 180, 320, 600, 1k, 3k, 6k, 12k, 14k, 16k. These are **not ISO-standard frequencies** — they're Winamp's own choice, slightly bass-heavy relative to ISO. Preset files (.EQF) use a 0–63 byte range mapping to +12 to -12 dB (inverted: byte 0 = +12 dB, byte 31 = 0 dB, byte 63 = -12 dB), plus one preamp byte with the same mapping.
- **Foobar2000** (built-in EQ): **18 bands**, not 10. Frequencies at 55, 77, 110, 156, 220, 311, 440, 622, 880, 1.25k, 1.75k, 2.5k, 3.5k, 5k, 7k, 10k, 14k, 20k Hz (quarter-octave roughly). Built-in FIR-based FFT equalizer, not IIR — different implementation entirely, not a biquad cascade.
- **XMMS / Audacious / classic Linux players**: same 10 bands as Winamp (they literally imported .EQF files).
- **Android system equalizer (AudioFX)**: typically 5 bands, frequencies vary by device.

**Recommendation for Concerto**: match iTunes' ISO-octave 10-band layout. It's the standard hi-fi layout, it maximizes compatibility with published presets, and it's what users with any graphic-EQ experience will recognize.

---

## 5. Preset Curves

### 5.1 Important reality check

The top-line finding: **Apple has never published the exact dB values for iTunes' built-in presets**. They are "known" only through people reading the sliders visually and publishing approximations. So anything labeled "the iTunes Classical preset values" that you find on blogs is someone's squint-reading of the screen, not Apple documentation. The values are roughly reproducible but not authoritative.

**Winamp's presets, by contrast, are fully documented** — they ship as a binary file (`winamp.q1`) that has been reverse-engineered (schollz/Winamp-Original-Presets on GitHub, captbaritone/webamp's `winamp-eqf` package). Those values are the real thing.

**Recommendation**: use the Winamp preset set as Concerto's "published" preset baseline. They are well-documented, openly available in a machine-readable form, and ship the same 10-band octave-ish model. Document in the UI that the preset curves are "inspired by" or "based on" the Winamp set — do not claim they are iTunes' presets unless you've actually measured them from a current Music.app install.

### 5.2 Winamp original presets (authoritative, from schollz/Winamp-Original-Presets and captbaritone/winamp-eqf parser)

Bands are Winamp's native 10 frequencies (60, 170, 310, 600, 1k, 3k, 6k, 12k, 14k, 16k — though functionally equivalent to ISO when remapped). Values are in dB (values of the form `-1.11022e-15` are floating-point zeros — treat as 0 dB). Preamp is typically 0 for these.

```
Preset         60    170   310   600   1k    3k    6k    12k   14k   16k
--------------------------------------------------------------------------
Classical        0     0     0     0     0     0  -7.2  -7.2  -7.2  -9.6
Club             0     0   3.2   5.6   5.6   5.6   3.2     0     0     0
Dance          9.6   7.2   2.4     0     0  -5.6  -7.2  -7.2     0     0
Full Bass      9.6   9.6   9.6   5.6   1.6  -4.0  -8.0 -10.4 -11.2 -11.2
Full B & T     7.2   5.6     0  -7.2  -4.8   1.6   8.0  11.2  12.0  12.0
Full Treble   -9.6  -9.6  -9.6  -4.0   2.4  11.2  16.0  16.0  16.0  16.8
Headphones     4.8  11.2   5.6  -3.2  -2.4   1.6   4.8   9.6  12.8  14.4
Large Hall    10.4  10.4   5.6   5.6     0  -4.8  -4.8  -4.8     0     0
Live          -4.8     0   4.0   5.6   5.6   5.6   4.0   2.4   2.4   2.4
Party          7.2   7.2     0     0     0     0     0     0   7.2   7.2
Pop           -1.6   4.8   7.2   8.0   5.6     0  -2.4  -2.4  -1.6  -1.6
Reggae           0     0     0  -5.6     0   6.4   6.4     0     0     0
Rock           8.0   4.8  -5.6  -8.0  -3.2   4.0   8.8  11.2  11.2  11.2
Ska           -2.4  -4.8  -4.0     0   4.0   5.6   8.8   9.6  11.2   9.6
Soft           4.8   1.6     0  -2.4     0   4.0   8.0   9.6  11.2  12.0
Soft Rock      4.0   4.0   2.4     0  -4.0  -5.6  -3.2     0   2.4   8.8
Techno         8.0   5.6     0  -5.6  -4.8     0   8.0   9.6   9.6   8.8
```

These values come from the raw Winamp `.EQF` files after applying the inverse of the byte-to-dB mapping `dB = 12 - byte*(24/63)` (byte range 0..63). The ±12 dB range is Winamp's native; when mapped onto an ISO-layout EQ you may want to scale slightly since the frequencies don't match 1:1.

### 5.3 Genre rationale (what the curves are actually doing)

- **Classical**: cuts only treble (from 6k up by ~7 dB, 16k by ~10 dB). Assumes acoustic recording with natural air and no need for bass boost; trims top to reduce harshness on hot digital masters. Very gentle.
- **Rock**: V-curve — boost the lows (+8 at 60 Hz), scoop 310–1k (about -5 to -8 dB), big treble lift (+8 to +11 above 6 kHz). Classic smile curve for electric guitar material.
- **Pop**: mid-centric — peaks at 310–600 Hz (+7 to +8), gently cut highs. Pulls vocals and kick drums forward.
- **Jazz** (not in Winamp's 18, but commonly derived): gentle U-curve — mild low and high boost, flat mids. Intent is "airy but warm."
- **Dance/Techno**: boosted sub and low bass (+8 to +10 at 60–170 Hz), scooped mids, boosted highs. Emphasizes kick and cymbal.
- **Bass Boost / Full Bass**: low-shelf-like — roughly +10 dB at 60/170/310, gradual rolloff above. Like toggling a loudness button.
- **Treble Boost / Full Treble**: inverse — suppress everything below ~600 Hz, big shelf at top.
- **Vocal / Spoken Word** (common on players other than Winamp): boost 1–3 kHz (speech intelligibility band), gentle cut below 200 Hz and above 8 kHz. The Ska and Soft Rock curves above are close.
- **Live / Large Hall**: simulated hall response — boosted lows (some proximity effect) and cut upper mids (absorption by the air and audience).
- **Loudness** / equal-loudness (not in Winamp's set but iTunes-style): pure V-curve — symmetric bass and treble boost with flat mids. Compensates for Fletcher-Munson at low listening volumes.

### 5.4 Vinyl / Warm curve

There is no standardized "vinyl preset." Several distinct things get called vinyl/warm:

1. **RIAA playback curve** — this is the de-emphasis curve applied by a phono preamp to the raw needle signal. It is NOT something you'd apply to already-ripped audio; the audio file is already post-RIAA. Including a "RIAA-style" preset that applies this curve *again* to a normal digital file would double-emphasize bass and cut treble absurdly. Only relevant if you're designing a phono preamp emulator.
2. **Vintage mastering color** — what people actually mean by "vinyl warmth" on a digital file. Typical recipe documented across audio-engineering sources (MusicRadar, Sweetwater, etc.):
   - Low shelf +2 to +4 dB below ~100 Hz (subtle bass fullness)
   - Gentle peak boost around 200–400 Hz, ~+2 to +3 dB, Q ~0.7 (the "low-mid warmth" bump)
   - Mild upper-mid cut around 2–4 kHz, -1 to -2 dB (removes digital harshness)
   - High shelf rolloff above ~10 kHz, -3 to -6 dB (the "rolled-off highs" signature)
   - Net effect on a 10-band layout: roughly +2, +3, +3, +2, 0, -1, -2, -3, -4, -5.
3. **78rpm / pre-RIAA curves** — completely different from RIAA; various label-specific (Columbia, Decca FFRR, etc.) pre-emphasis curves. Not relevant to a music player.

Recommendation: ship a "Warm" or "Vinyl" preset matching the #2 recipe; avoid labeling it "RIAA." If you want to be clever, a slightly rolled-off high shelf plus a +2–3 dB low-mid peak at ~250 Hz with Q~0.9 is the one thing people consistently describe as "vinyl-like."

### 5.5 Consensus on genre presets

There is no AES standard, no audiophile consensus, no mastering-engineer agreement on "the correct EQ for jazz." Genre presets are folklore. Every major player and every forum publishes its own. The practical truth:

- They're useful as **starting points** and as a way of letting users pick a "flavor" without learning EQ.
- They do not represent scientific optimization of anything.
- Users listening on identical gear to identical tracks will disagree on which preset is best.
- The presets' biggest real-world function is *speaker/headphone compensation*: a bass-lifted preset sounds "right" on small speakers that can't produce bass naturally.

For Concerto, provide maybe 12–16 presets covering the common genres plus a "Flat" (all zero, preamp 0) as the no-op default, and let users save their own. Do not overthink the preset values; Winamp's are a fine baseline.

---

## 6. Q Factor Choices for Fixed-Frequency Graphic EQ

### 6.1 The octave math

For a peaking filter centered on `f0` with bandwidth `BW` octaves, measured between the -3 dB points (for BPF convention) or half-gain points (for peaking EQ convention per RBJ):

```
Q = f0 / BW_Hz                                  (analog convention)
Q = 1 / (2 * sinh(ln(2)/2 * BW))                (for BW in octaves, analog)
```

Common values:
- **BW = 1 octave** → Q ≈ 1.4142 (= √2). This is the "cover one full octave at -3 dB" answer.
- **BW = 1/2 octave** → Q ≈ 2.87
- **BW = 1/3 octave** → Q ≈ 4.32
- **BW = 2 octaves** → Q ≈ 0.667

### 6.2 Why √2 for a 10-band octave EQ

If the bands are spaced one octave apart, a Q of √2 makes the -3 dB skirt of one band just reach the center of its neighbors. That's the "kissing" design where the octave bands together cover the spectrum without gaps or huge overlap.

In practice though, constant-Q with Q=√2 when you boost two adjacent bands by 6 dB gives a visible dip (the "ripple") between them. If you want adjacent-band sums to add smoothly, you widen Q slightly (Q ≈ 0.9–1.1). If you want tighter surgical control at the cost of inter-band ripple, you narrow Q (Q ≈ 1.8–2.5).

### 6.3 Practical Q choice

Three schools of thought:

1. **RBJ's default, Q ≈ √2 (1.41)** for octave bands — mathematically clean, textbook answer.
2. **Q ≈ 0.9–1.0** — what iTunes appears to use empirically, and what most hi-fi consumer EQs do. Bands "sum" more smoothly, preset curves look less ripply on a spectrum analyzer.
3. **Q ≈ 1.8–2.0** — what some pro graphic EQs (constant-Q Rane style) do. Less interaction between adjacent sliders; sounds more "precise" but sum-of-adjacent-bands shows ripple.

**Recommendation for Concerto**: start with **Q = 1.0** (slightly wider than one octave). It matches consumer-player feel, hides inter-band ripple, and the preset curves will look closer to what users expect from iTunes/Winamp-style EQs. Make it a compile-time constant initially; if you later expose a "precision" / "smooth" toggle, it's a one-line change.

Excessively narrow Q on a music-player graphic EQ is perceptually bad — the ear hears narrow peaking as "ringing" or "resonance" rather than tonal balance. Stay wider.

Excessively wide Q (Q < 0.5) defeats the purpose of having 10 separate bands, because adjacent bands become indistinguishable.

---

## 7. Gain Staging and Headroom

### 7.1 The problem

Summing multiple boosted peaking bands can produce peaks well above 0 dBFS, which then hard-clip in the output path. The worst case: all 10 bands at +12 dB on a signal with broadband content — that's theoretically +32 dB ((10 bands each at +12 don't sum to +120 on a broadband input, because each filter's boost is frequency-selective, but worst-case correlated signals can stack substantially).

In practice on music content, even 3–4 adjacent bands boosted by +6 to +12 dB is enough to clip quiet-mastered recordings, let alone modern loudness-war masters that already peak at -0.1 dBFS.

### 7.2 How the big players handle it

- **iTunes**: explicit **Preamp** slider (±12 dB) above the band sliders. Convention: if you boost, manually pull the preamp down. Apple does not auto-compensate. Users are expected to learn this.
- **Winamp**: same — a Preamp slider is part of the EQ UI, stored alongside the band values in `.EQF`. Most Winamp presets ship with preamp ≈ 0; the user adjusts down as needed.
- **Foobar2000**: similar manual preamp.
- **Equalizer APO**: automatically calculates a "Peak gain" (from frequency-response analysis) and warns when it exceeds 0 dB; does not auto-attenuate.
- **Modern DAWs** (FabFilter Pro-Q etc.): automatic gain compensation ("Auto Gain" toggle) that estimates a matching attenuation from the filter response. Off by default in most, but ships as a convenience.

### 7.3 Recommendation for Concerto

Two-layer approach:

1. **Ship a visible preamp slider** (±12 dB, default 0) in the EQ UI. Stored with each preset. This is the familiar and expected affordance.
2. **Soft-clip the output** of the EQ chain as a safety net. A simple `tanh`-based soft saturator kicks in only when levels exceed some threshold (e.g. -1 dBFS), adding mild nonlinearity that's far less offensive than hard-clip buzz. Apply at the very end of the EQ chain, after the preamp has already been subtracted, as a last line of defense.
3. (Optional) Show a peak-level meter on the EQ tab so users can see when they're clipping.
4. (Optional, future) "Auto preamp" toggle that estimates peak response gain from the coefficient set and pre-attenuates. Off by default. Non-trivial to do correctly — requires response-curve computation or online peak tracking.

Do **not** auto-attenuate silently. Users who boost bass and expect it to be louder will be confused if the thing gets quieter. Keep it explicit.

---

## 8. Zipper Noise and Parameter Smoothing

### 8.1 The problem

When a user drags a gain slider, the EQ's host code recomputes coefficients. If you naively swap old coefficients for new coefficients once per audio block (e.g. every 256 samples), the signal experiences a discontinuous change in filter response — and because biquads have state, the new coefficients operating on the old state can produce transient glitches (clicks, pops, "zipper noise" when continuous dragging produces a fast train of discontinuities).

### 8.2 Strategies, cheapest to most robust

**(a) Block-rate coefficient swap, no smoothing** — cheap, glitches audible. Not recommended.

**(b) One-pole smoothing of the user parameter (recommended)**:
- Incoming slider change sets a target for gain_dB (or Q, or f0).
- A per-parameter one-pole lowpass, ticking at the audio rate or the block rate, chases the target: `smoothed += (target - smoothed) * alpha` where `alpha` is set to give a time constant of 10–50 ms.
- Coefficients are recomputed from the smoothed parameters, either every sample (expensive but glitch-free) or every N samples (e.g. 32 or 64) with linear interpolation of coefficients between recomputations.
- Cheap, bounded CPU overhead, glitch-free for typical UI use.

**(c) Linear interpolation of coefficients themselves** between the previous and the new set, ramped across one audio block. Similar result to (b) but operates on already-computed coefficients. Gotcha: linearly interpolating coefficients does NOT produce a filter that is linearly between the two frequency responses — it produces some in-between response that *might* briefly be unstable (poles outside the unit circle). Safe for small frequency changes, risky for big jumps. The DAFX paper "smooth and safe parameter interpolation" (Väänänen & Schimmel 2006) specifically warns about this and proposes "sliding edges" (see d).

**(d) Crossfade between two filter instances**: run two parallel biquad states, fade from old-coefficient-filter to new-coefficient-filter over N samples. Mathematically clean, glitch-proof. Costs 2× CPU during the crossfade window only. Used in some pro plugins for large parameter jumps (e.g. preset recall).

**(e) Pole-zero domain interpolation**: decompose biquads into (pole, zero) pairs, interpolate those, reconstruct coefficients. The "most correct" approach but expensive and complex.

### 8.3 Recommendation for Concerto

**Use (b) — parameter smoothing**:
- One-pole smoother on each band's gain_dB parameter. Time constant 20–30 ms (alpha corresponding to `1 - exp(-1/(tau*Fs))` per-sample or per-block).
- Recompute coefficients at block rate (or every 16–32 samples for very small blocks) from the smoothed parameter values.
- Use TDF2 biquads — they're naturally less click-prone under coefficient modulation than DF1.

For preset recall (big parameter jumps) specifically, consider (d) crossfading between old and new filter states over ~50 ms, because a 20 ms smoother may still produce audible motion on a large simultaneous change of all 10 bands.

Never recompute coefficients at audio-sample rate unless profile shows you can afford it. Block-rate with smoothing is fine for EQ.

---

## 9. Denormals

### 9.1 The problem

When audio input falls to silence, a biquad's state feeds back through itself and decays exponentially toward zero. It eventually hits subnormal (denormal) float values (smaller than ~1e-38 for float32, ~1e-308 for float64). On x86, operating on denormals can be 50–100× slower than on normal floats — one moderate biquad cascade with denormal state can consume the entire audio budget on older CPUs (worst-case stall was famously observed on Pentium 4, where a simple decay caused complete CPU lockup). Modern CPUs (Sandy Bridge onward, Intel; Zen onward, AMD) have much smaller penalties but are not immune.

### 9.2 Mitigations, in practice

**(a) FTZ/DAZ flags** — preferred. On x86 SSE2, set two bits in the MXCSR register:
- **FTZ (Flush to Zero)**: denormal *outputs* become zero.
- **DAZ (Denormals Are Zero)**: denormal *inputs* are treated as zero.

```c
#include <xmmintrin.h>
#include <pmmintrin.h>  /* for DAZ */
_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
_MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
```

These flags are **per-thread** in the CPU state. You must set them in the actual audio callback thread (or early in its startup), not just in the main thread. Save the previous MXCSR on entry and restore on exit if you might be sharing the thread with code that cares about denormals (rare in practice for audio pipelines).

On ARM/ARM64, the equivalent is setting the FZ bit in the FPCR register. Most real-time audio frameworks (AudioUnits, Core Audio, JACK, JUCE, most VST hosts) do this for you. Qt's audio abstractions may or may not — for Concerto, **set it explicitly** in your audio thread entry.

**(b) Adding a tiny constant to signal** — a defensive trick if FTZ can't be relied upon or for maximum portability:
- Add something like `1e-18f` (-360 dB, below the float32 noise floor) to the biquad input each sample.
- To prevent DC accumulation, alternate the sign: add `+1e-18` one sample, `-1e-18` next, or add to only every 8th sample.
- Effective and extremely cheap (one add per sample per biquad).
- Note: Nigel Redmon's EarLevel actually recommends `1e-15f` (-300 dB) as a safer value — well below audibility (-120 dB is the float32 noise floor anyway for a signal at 0 dBFS) but large enough to guarantee non-denormal recirculation.

**(c) JUCE-style macro** `x += 1.0; x -= 1.0;` — the `+ 1.0` forces the value up into normal range, the `- 1.0` restores it. Works, but adding 1.0 destroys any small-signal information below the unit-ULP level, which for `float` is on the order of 1e-7 — audible noise floor if the filter is supposed to carry reverb-tail-style low signal. Don't use this in audio paths.

### 9.3 Recommendation for Concerto

1. Set FTZ and DAZ at the top of your audio thread, once. This is the clean solution.
2. Ensure your build targets x86-64 (SSE2 built-in) or ARM64 (NEON with FZ). Don't bother with x86-32 fallback.
3. As belt-and-suspenders, optionally add a `1e-18f` dither alternating sign to the biquad inputs. It's cheap and insures against the case where some third-party audio stack resets MXCSR between your init and your callback.

Never rely only on (b) without (a). FTZ is the authoritative fix.

---

## 10. Processing Architecture

### 10.1 State organization

For N biquads × C channels:
- Shared (N × 6 coefficients) — all channels use the same filter.
- Per-channel (N × 2 state values for TDF2, N × 4 for DF1).
- Per-band smoothing state (N × 1 current smoothed gain, plus target).
- Plus preamp state.

Typical memory for a 10-band stereo EQ in TDF2 with `double` state: `10 bands × 2 channels × 2 state × 8 bytes = 320 bytes` for filter state. Negligible.

### 10.2 Block vs sample processing

Three loop organizations:

**(a) Band-outer, sample-inner** (serial, obvious):
```
for each sample in block:
    x = input[sample]
    for each band:
        x = biquad_step(band, x)
    output[sample] = x * preamp
```

**(b) Sample-outer, band-inner** (slightly better cache locality):
```
for each band:
    for each sample in block:
        x = band_input[sample]
        band_output[sample] = biquad_step(band, x)
    swap input/output buffers
```
Version (b) keeps one band's coefficients hot in registers for the whole block. Typically 2–3× faster than (a) on modern CPUs at block size ≥ 64.

**(c) SIMD multi-channel**: process L and R simultaneously in one xmm register. Biquads don't vectorize *across bands* because each band's output feeds the next, but they vectorize cleanly *across channels*. Expect another 1.5–2× from this on stereo.

### 10.3 CPU cost

Rule-of-thumb numbers for a modern x86-64 CPU:
- One TDF2 biquad in `double` costs about **5 multiplies + 4 adds ≈ 5 ns per sample** on a 4 GHz Skylake-class core (unvectorized).
- 10 bands × 2 channels = ~100 ns per sample pair, which at 48 kHz is 100 ns × 48000 ≈ **4.8 ms of CPU time per second of audio**, or **~0.5% of one core**. Completely negligible.
- Even 31-band EQ at 192 kHz in stereo is well under 5% of a single core.

EQ is not a CPU-bound problem. Optimize for correctness and zipper-free behavior first, SIMD only if profiling shows a need.

### 10.4 Recommended architecture

- C DSP layer: `eq_state` struct holding coefficients (shared, updated by UI thread), per-channel state arrays, and per-band parameter smoothers.
- Process function: takes `(eq_state*, float* in, float* out, int nframes, int nchannels)`. Internally calls a per-block coefficient update (if parameters changed), then iterates band-outer sample-inner, handling channels via an inner `for (c=0; c<nchannels; c++)`.
- Coefficient updates happen in a thread-safe way — either a lock-free atomic swap of a coefficient bundle pointer, or a simple `atomic<bool> dirty` flag that the audio thread checks at the top of the block and pulls fresh coefficients from a shared struct. Avoid locking the audio thread.
- Qt UI never touches the DSP state directly — it calls `eq_set_gain(band, dB)` etc., which updates a UI-side parameter store that the audio thread reads.

---

## 11. A Few More Gotchas Worth Flagging

- **Initialization**: clear all biquad state to zero at startup. Do NOT leave it uninitialized — uninitialized floats may be denormal or NaN, which will stall FTZ-less systems and make debugging a nightmare.
- **Sample rate changes**: every biquad's coefficients depend on Fs. If the audio device renegotiates sample rate (common on some Qt audio backends when moving between devices), you must recompute all coefficients. Tie coefficient computation to a `set_sample_rate(Fs)` entry point and call it from the audio setup path.
- **Preset save/load format**: save the preset as JSON or similar with `{ name, preamp_dB, bands: [dB, dB, ...] }`. Don't bake in binary format specifics like Winamp's `.EQF` byte scaling — store real dB values. If you want to *import* `.EQF` files, the conversion is `dB = 12 - byte*(24/63)` for all 10 band bytes and the preamp byte.
- **Bypass**: the EQ must have a hard-bypass that skips processing entirely (not a "flat curve" that still runs 10 biquads returning unity). Users expect A/B comparison.
- **Per-channel state mismatch**: if you ever add mid/side processing or surround support, each channel strictly needs its own state. Don't share.
- **Float denormals in coefficient space**: at extreme settings (f0 = 20 Hz, Fs = 192 kHz, Q = 0.1), `alpha` can be very small. Compute coefficients in `double` always; this is separate from the state-precision discussion and matters equally.

---

## 12. References and Further Reading

Primary / canonical sources to cite during implementation:

- **Robert Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter coefficients"**. The definitive coefficient source. Authoritative versions:
  - W3C hosted: https://www.w3.org/TR/audio-eq-cookbook/
  - Webaudio group: https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html
  - musicdsp.org mirror: https://www.musicdsp.org/en/latest/Filters/197-rbj-audio-eq-cookbook.html
- **Julius O. Smith III, "Introduction to Digital Filters with Audio Applications" (CCRMA online book)**: https://ccrma.stanford.edu/~jos/filters/ — and the companion "Physical Audio Signal Processing" at https://ccrma.stanford.edu/~jos/pasp/ — and the numerical section at https://ccrma.stanford.edu/~jos/fp/ . Free, rigorous, authoritative.
- **Udo Zölzer (ed.), DAFX — Digital Audio Effects** (Wiley, 2nd ed. 2011). Chapter on filters covers biquad implementation, parameter interpolation, and stability. Companion site http://www.dafx.de/ has MATLAB.
- **Will Pirkle, Designing Audio Effect Plug-Ins in C++** (2nd ed., Focal Press 2019). Very practical, C++-oriented. Chapters on biquads, parametric EQ, and numerical issues are directly applicable.
- **Nigel Redmon, EarLevel Engineering**:
  - "Biquads" intro: https://www.earlevel.com/main/2003/02/28/biquads/
  - "Floating point denormals": https://www.earlevel.com/main/2019/04/19/floating-point-denormals/
  - "A note about de-normalization": https://www.earlevel.com/main/2012/12/03/a-note-about-de-normalization/
  - Biquad C++ source + calculator: https://www.earlevel.com/main/2012/11/26/biquad-c-source-code/ and https://www.earlevel.com/main/2013/10/13/biquad-calculator-v2/
- **musicdsp.org archive** — a community collection of small, tested DSP snippets: https://www.musicdsp.org/ . Filters section: https://www.musicdsp.org/en/latest/Filters/
- **Rane Notes (Dennis Bohn)** — classic graphic EQ design history:
  - Note 101 "Constant-Q Graphic Equalizers": https://www.ranecommercial.com/legacy/note101.html
  - Note 154 "Perfect-Q": https://www.ranecommercial.com/legacy/note154.html
- **Väänänen & Schimmel, "Smooth and Safe Parameter Interpolation" (DAFx-06)**: https://www.dafx.de/paper-archive/2006/papers/p_057.pdf — the paper on clickless biquad parameter modulation.
- **ARM CMSIS-DSP biquad reference** (for reference implementation comparison):
  - DF1: https://arm-software.github.io/CMSIS-DSP/main/group__BiquadCascadeDF1.html
  - DF2T: https://arm-software.github.io/CMSIS-DSP/main/group__BiquadCascadeDF2T.html
- **Winamp preset archive** (reverse-engineered, for preset values):
  - schollz/Winamp-Original-Presets: https://github.com/schollz/Winamp-Original-Presets
  - captbaritone/winamp-eqf (EQF parser): https://github.com/captbaritone/webamp/tree/master/packages/winamp-eqf
- **Wikipedia "Digital biquad filter"**: https://en.wikipedia.org/wiki/Digital_biquad_filter — decent overview, especially for the four forms compared side-by-side.
- **Web Audio BiquadFilterNode spec** (uses RBJ directly, worth skimming for interface ideas): https://www.w3.org/TR/webaudio/#biquadfilternode
- **RIAA equalization (Wikipedia)** — for context on why you should NOT apply RIAA as a "vinyl" preset: https://en.wikipedia.org/wiki/RIAA_equalization

Sources consulted but lower priority / informational:
- https://kirkville.com/fine-tune-your-music-playback-with-itunes-equalizer/ (iTunes UI walkthrough)
- https://www.kvraudio.com/forum/viewtopic.php?t=492285 (biquad process differences)
- https://www.kvraudio.com/forum/viewtopic.php?t=309908 (time-varying biquad params)
- https://forum.juce.com/t/resolving-denormal-floats-once-and-for-all/8241 (denormal handling tactics)

---

## Summary of Concrete Choices for Implementation

When you start writing code, these are the defaults this research recommends. Each is defensible and can be changed later with isolated impact.

| Decision | Choice | Why |
|---|---|---|
| Filter topology | RBJ peaking biquad per band | Industry standard, well-tested, simple |
| Biquad form | Transposed Direct Form II (TDF2) | Best floating-point accuracy, mildly click-resistant |
| Coefficient precision | `double` | Avoids low-frequency numerical issues, free on x86-64 |
| State precision | `double` | Essential for high-Q low-frequency stability |
| Bands | 10 | Matches iTunes/Winamp/XMMS convention |
| Frequencies | ISO 266 preferred: 31.5, 63, 125, 250, 500, 1k, 2k, 4k, 8k, 16k Hz | Standard, widely compatible |
| Q per band | Fixed Q = 1.0 | Smooth adjacent-band summing, matches consumer-player feel |
| Filter type | Peaking (bell) for all 10 bands, or peaking-mid + low-shelf on band 1 + high-shelf on band 10 if you want cleaner extremes | Parametric EQ canonical |
| Gain range | ±12 dB per band | Standard, matches iTunes/Winamp |
| Preamp | ±12 dB, saved per preset | Required for headroom management |
| Headroom protection | Visible preamp + final soft-clip (tanh) after preamp | Belt-and-suspenders |
| Parameter smoothing | One-pole on each gain dB, ~20 ms tau; recompute coeffs every block | Glitch-free, cheap |
| Preset recall | Short crossfade (~50 ms) between old and new filter states | Smooth on large jumps |
| Denormal protection | FTZ + DAZ set in audio thread init | Authoritative mitigation |
| Processing | Band-outer, sample-inner loop; per-channel state, shared coefficients | Good cache behavior; negligible CPU |
| Preset library baseline | Winamp's 17 documented presets, renamed/adjusted as needed | Fully documented, legal to reference |
| Vinyl/warm preset | Recipe #2 in §5.4 (low-mid bump + high rolloff), NOT RIAA | Correct understanding of what "vinyl" means for digital audio |

Sources:

- [Audio EQ Cookbook (W3C)](https://www.w3.org/TR/audio-eq-cookbook/)
- [Audio EQ Cookbook (WebAudio)](https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html)
- [RBJ Audio-EQ-Cookbook mirror (musicdsp.org)](https://www.musicdsp.org/en/latest/Filters/197-rbj-audio-eq-cookbook.html)
- [Introduction to Digital Filters with Audio Applications — Julius O. Smith](https://ccrma.stanford.edu/~jos/filters/)
- [Transposed Direct Forms — Julius O. Smith, CCRMA](https://ccrma.stanford.edu/~jos/fp/Transposed_Direct_Forms.html)
- [Direct Form II — Julius O. Smith, CCRMA](https://ccrma.stanford.edu/~jos/fp/Direct_Form_II.html)
- [Biquads — EarLevel Engineering](https://www.earlevel.com/main/2003/02/28/biquads/)
- [Floating point denormals — EarLevel Engineering](https://www.earlevel.com/main/2019/04/19/floating-point-denormals/)
- [A note about de-normalization — EarLevel Engineering](https://www.earlevel.com/main/2012/12/03/a-note-about-de-normalization/)
- [Digital biquad filter — Wikipedia](https://en.wikipedia.org/wiki/Digital_biquad_filter)
- [CMSIS-DSP Biquad Cascade DF2T](https://arm-software.github.io/CMSIS_5/DSP/html/group__BiquadCascadeDF2T.html)
- [Constant-Q Graphic Equalizers — Rane Note 101 (Dennis Bohn)](https://www.ranecommercial.com/legacy/note101.html)
- [Perfect-Q, the Next Step in Graphic EQ Design — Rane Note 154](https://www.ranecommercial.com/legacy/note154.html)
- [Smooth and Safe Parameter Interpolation — Väänänen & Schimmel, DAFx-06](https://www.dafx.de/paper-archive/2006/papers/p_057.pdf)
- [Mitigating clicks in modulated DSP filters — KVR](https://www.kvraudio.com/forum/viewtopic.php?p=8487299)
- [Resolving denormal floats once and for all — JUCE forum](https://forum.juce.com/t/resolving-denormal-floats-once-and-for-all/8241)
- [Winamp Original Presets (GitHub)](https://github.com/schollz/Winamp-Original-Presets)
- [captbaritone/webamp winamp-eqf parser](https://github.com/captbaritone/webamp/tree/master/packages/winamp-eqf)
- [XMMS Equalizer Presets from WinAmp (dave king wiki)](https://wiki.daveking.com/index.php/XMMS_Equalizer_Presets_from_WinAmp)
- [Fine-Tune Your Music Playback with iTunes' Equalizer — Kirkville](https://kirkville.com/fine-tune-your-music-playback-with-itunes-equalizer/)
- [iTunes COM IITEQPreset reference (archived)](http://www.joshkunz.com/iTunesControl/interfaceIITEQPreset.html)
- [Parametric EQ vs Graphic EQ — Audio University](https://audiouniversityonline.com/parametric-eq-vs-graphic-eq/)
- [RIAA equalization — Wikipedia](https://en.wikipedia.org/wiki/RIAA_equalization)
- [How to EQ for vintage analogue warmth — MusicRadar](https://www.musicradar.com/tuition/tech/how-to-eq-for-vintage-analogue-warmth-571572)
- [Designing Audio Effect Plug-Ins in C++ — Will Pirkle (2nd ed.)](https://www.willpirkle.com/)
- [DAFX: Digital Audio Effects — Udo Zölzer](https://onlinelibrary.wiley.com/doi/book/10.1002/9781119991298)
