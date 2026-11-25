---
name: Pastel Lavender Serenity
colors:
  surface: '#faf8ff'
  surface-dim: '#dbd9e0'
  surface-bright: '#faf8ff'
  surface-container-lowest: '#ffffff'
  surface-container-low: '#f5f3fa'
  surface-container: '#efedf4'
  surface-container-high: '#e9e7ee'
  surface-container-highest: '#e3e1e8'
  on-surface: '#1b1b20'
  on-surface-variant: '#494551'
  inverse-surface: '#2f3035'
  inverse-on-surface: '#f2f0f7'
  outline: '#7a7582'
  outline-variant: '#cbc4d3'
  surface-tint: '#674eaa'
  primary: '#533994'
  on-primary: '#ffffff'
  primary-container: '#6b52ae'
  on-primary-container: '#e5d9ff'
  inverse-primary: '#cfbdff'
  secondary: '#605c6d'
  on-secondary: '#ffffff'
  secondary-container: '#e5dff4'
  on-secondary-container: '#666274'
  tertiary: '#503a96'
  on-tertiary: '#ffffff'
  tertiary-container: '#6853af'
  on-tertiary-container: '#e4d9ff'
  error: '#ba1a1a'
  on-error: '#ffffff'
  error-container: '#ffdad6'
  on-error-container: '#93000a'
  primary-fixed: '#e9ddff'
  primary-fixed-dim: '#cfbdff'
  on-primary-fixed: '#22005d'
  on-primary-fixed-variant: '#4f3590'
  secondary-fixed: '#e5dff4'
  secondary-fixed-dim: '#c9c4d8'
  on-secondary-fixed: '#1c1a28'
  on-secondary-fixed-variant: '#484555'
  tertiary-fixed: '#e7deff'
  tertiary-fixed-dim: '#cdbdff'
  on-tertiary-fixed: '#1f005f'
  on-tertiary-fixed-variant: '#4c3692'
  background: '#faf8ff'
  on-background: '#1b1b20'
  surface-variant: '#e3e1e8'
typography:
  headline-xl:
    fontFamily: Plus Jakarta Sans
    fontSize: 3.5rem
    fontWeight: '700'
    lineHeight: 3.75rem
    letterSpacing: -0.03em
  headline-xl-mobile:
    fontFamily: Plus Jakarta Sans
    fontSize: 2.75rem
    fontWeight: '700'
    lineHeight: 3rem
    letterSpacing: -0.02em
  headline-lg:
    fontFamily: Plus Jakarta Sans
    fontSize: 2rem
    fontWeight: '600'
    lineHeight: 2.5rem
    letterSpacing: -0.02em
  headline-sm:
    fontFamily: Plus Jakarta Sans
    fontSize: 1.125rem
    fontWeight: '600'
    lineHeight: 1.5rem
    letterSpacing: -0.01em
  body-lg:
    fontFamily: Plus Jakarta Sans
    fontSize: 1rem
    fontWeight: '500'
    lineHeight: 1.5rem
  body-md:
    fontFamily: Plus Jakarta Sans
    fontSize: 0.875rem
    fontWeight: '500'
    lineHeight: 1.25rem
  body-sm:
    fontFamily: Plus Jakarta Sans
    fontSize: 0.75rem
    fontWeight: '400'
    lineHeight: 1rem
  label-md:
    fontFamily: Plus Jakarta Sans
    fontSize: 0.8125rem
    fontWeight: '600'
    lineHeight: 1rem
    letterSpacing: 0.01em
  label-sm:
    fontFamily: Plus Jakarta Sans
    fontSize: 0.6875rem
    fontWeight: '500'
    lineHeight: 0.875rem
    letterSpacing: 0.02em
rounded:
  sm: 0.5rem
  DEFAULT: 1rem
  md: 1.5rem
  lg: 2rem
  xl: 3rem
  full: 9999px
spacing:
  gutter: 0.875rem
  margin: 1rem
  space-xs: 0.25rem
  space-sm: 0.5rem
  space-md: 0.875rem
  space-lg: 1.25rem
  space-xl: 1.75rem
---

## Brand & Style

This design system embodies a calm, meditative, and sophisticated aesthetic tailored for glanceable information, ambient utility widgets, and lifestyle utilities. It merges modern minimalism with tactile softness—subtle rounded cards, gentle lavender tinting, and airy whitespace create an effortless, stress-free atmosphere.

### Core Attributes
- **Calm & Atmospheric:** Soft lilac washes, translucent surfaces, and gentle tones replace harsh pure whites and heavy contrasts.
- **Micro-Precision & Clarity:** Every metric, icon, and label is spaced intentionally, allowing high-density data (such as hourly forecasts and multi-metric matrices) to breathe effortlessly.
- **Friendly Sophistication:** Deep muted violet-indigo replaces harsh carbon blacks for text and line graphics, maintaining crisp legibility while preserving the soothing twilight mood.

## Colors

The palette revolves around ethereal lavender and muted wisteria tones contrasted against crisp deep indigo neutrals. 

- **Primary (`#6b52ae` / `#7c67c5`):** The signature lavender-violet accent used for active highlights, key weather icons, focused timeline bars, and primary interaction targets.
- **Secondary (`#e9e3f8`):** A soft, pale lilac container tint used for active segment pills, hero weather card backdrops, and interactive state indicators.
- **Tertiary (`#7c67c5`):** A lighter, vibrant wisteria used for secondary graphical nodes, weather trend curves, and fine line charts.
- **Neutral Canvas (`#f5f3fa`):** A gentle, tinted off-white base canvas that prevents glare and binds the cards together into a seamless ecosystem.
- **Card Surfaces (`#ffffff`):** Crisp pure white applied to individual utility cards to maintain high legibility and soft contrast against the tinted canvas.
- **Text & Stroke Neutral (`#231d36` to `#756e87`):** Instead of neutral grays or harsh pure blacks, typography and line icons leverage deep slate-violet tones for softer contrast.

## Typography

The typography leverages **Plus Jakarta Sans** for its friendly geometry, open apertures, and crisp readability at compact sizes. 

- Large hero numeric values (`headline-xl`) carry confident weights with slight negative letter tracking for a tight, polished widget appearance.
- Sub-labels and metadata pairings (`label-sm`, `body-sm`) maintain generous line heights and balanced weights (500/600) to ensure high visibility against both white cards and tinted lavender containers.
- Monospace styling is not needed; tabular numeric figures (`tnum`) should be enabled for seamless horizontal alignment across timelines and forecast columns.

## Layout & Spacing

The layout follows a modular widget card structure suited for dashboard grids, multi-column metrics, and horizontal scrolling timelines.

### Layout Rhythm
- **Card Margins & Gutters:** A consistent `0.875rem` (14px) gutter separates independent card modules, keeping the canvas unified while preserving distinct content boundaries.
- **Card Padding:** Standard cards utilize `1.25rem` (20px) inner padding for hero modules and `1rem` (16px) for secondary multi-cell grids.
- **Responsive Adaptations:**
  - **Desktop / Tablet:** Hero primary forecast and multi-parameter stats sit side-by-side in a 2-column split (e.g., 5:7 ratio), followed by full-width 24-hour and 7-day timeline rows.
  - **Mobile:** The split collapses vertically; timeline scrubbers transition into horizontal smooth-scrolling rails with touch snap points.

## Elevation & Depth

Visual hierarchy is established primarily through **tonal layering** and **whisper-soft ambient diffusion** rather than heavy drop shadows:

- **Base Layer:** The outer container or application background utilizes the neutral lavender-tinted wash (`#f5f3fa`).
- **Surface Elevation (Cards):** Standard cards sit on crisp white (`#ffffff`) with an ultra-soft ambient shadow:
  - `box-shadow: 0 4px 20px -2px rgba(107, 82, 174, 0.05), 0 1px 3px 0 rgba(107, 82, 174, 0.03);`
  - A subtle 1px border (`rgba(107, 82, 174, 0.08)`) encases each card for crisp edge definition on lower-contrast screens.
- **Accent Containers (Hero Surface):** Hero cards (such as the main current weather display) use a saturated lavender wash (`#e9e3f8`) with no harsh shadow, creating a soft, tinted focal point.
- **Scrubber / Selection Highlight:** Focused segments and active timeline states use a gentle translucent pill (`#dfd7f5` or `rgba(107, 82, 174, 0.12)`) paired with a smooth micro-glow on active data points.

## Shapes

The design language favors soft, organic, and ultra-smooth silhouettes:

- **Widget Cards:** Styled with generous `1.5rem` to `1.75rem` corner radiuses, producing an approachable, modern pebble aesthetic.
- **Selection Pills & Badges:** Use full pill radiuses (`9999px`) or `1rem` rounded rectangles for segment indicators, daytime chips, and interactive buttons.
- **Icon Badges & Avatars:** Utilizes organic scalloped medallions or soft squircular backdrops (e.g., 12-lobed flower/star badge for current weather state icon) to introduce playful tactile personality.

## Components

### 1. Weather & Statistic Metric Cards
- **Structure:** Crisp white surface with rounded corners (`1.75rem`). Interior items organized in symmetrical 3-column or 2-row grids.
- **Typography Pairing:** Upper sub-label in muted slate-violet (`body-sm`), primary value directly beneath in medium-bold weight (`body-lg` or `headline-sm`).
- **Icons:** Minimal line-based icons (1.5px stroke width) aligned inline with the metric header.

### 2. Timeline & Chart Scrubbers (Hourly & Weekly)
- **Column Units:** Segmented columns with timestamp on top, weather icon in the center, temperature/metric directly below, and precip probability at the base.
- **Selected State:** Active segment is wrapped in a full-height rounded pill container (`#dfd7f5` background, rounded `1rem`), with bolded active text.
- **Line Trends:** A continuous smooth bezier curve (`#7c67c5`, 2px stroke) threads across the columns with solid violet circular node anchors at each time slice.

### 3. Icon Badges & Medallions
- **Hero Icon Container:** Fluted scalloped medallion shape in solid primary lavender (`#6b52ae`) housing an inset white vector icon (e.g., crescent moon, sun).

### 4. Navigation Headers & Segment Controls
- **Section Headers:** Clean, unadorned titles with an accompanying icon, paired with compact chevron controls (`<` `>`) and calendar trigger buttons on the right margin.
- **Buttons & Icon Links:** Micro-pill buttons with light lavender hover states (`rgba(107, 82, 174, 0.08)`) and muted slate-violet stroke icons.