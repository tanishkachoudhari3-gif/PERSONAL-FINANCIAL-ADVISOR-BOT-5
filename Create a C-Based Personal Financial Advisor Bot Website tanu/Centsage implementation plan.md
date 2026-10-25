# Centsage implementation plan

## Product scope

Centsage is a privacy-first, educational personal finance advisor. It uses a small C HTTP server to render the page, calculate a monthly budget snapshot, and return rule-based savings/advisor responses. There is no login, database, bank connection, external finance API, or persistent storage. User inputs live only for the request and are not written to disk.

## Implementation approach

- `src/main.c` is a dependency-free C11 HTTP server listening on `0.0.0.0:3000` (or `PORT`). It serves the homepage, CSS, and two POST actions: `/api/plan` for the budget snapshot and `/api/advice` for conversational rule-based guidance.
- The server parses `application/x-www-form-urlencoded` form values, validates non-negative money inputs, calculates totals and rates with transparent helper functions, and HTML-escapes any user-provided text before rendering it back.
- `public/styles.css` provides the only presentation layer: a responsive, accessible, warm editorial dashboard with clear labels, visible focus states, high contrast, and mobile-friendly layout.
- `public/manus-routes.json` declares the single page route required by Webdev.
- `Makefile` builds the C binary with `gcc -std=c11 -Wall -Wextra -Wpedantic`; `Dockerfile` provides a minimal production container using Debian's compiler toolchain.
- The frontend intentionally avoids JavaScript. Standard HTML forms POST to the C server and receive a complete, server-rendered result, keeping the core product in C and making the interaction usable with keyboard navigation and scripts disabled.

## Design direction

### Three possible styles

- **Quiet editorial ledger** — 0.06: warm paper, ink, and one bright savings accent with asymmetrical information blocks.
- **Neo-bank glass** — 0.07: cool gradients, translucent cards, and dense app chrome.
- **Civic utility** — 0.04: high-contrast navy, orange, and structured public-service forms.

### Chosen style: Quiet editorial ledger

- **Design Movement:** Contemporary editorial finance tools, borrowing the calm hierarchy of a well-made annual report rather than a fintech trading screen.
- **Core Principles:** Make the numbers legible; keep every recommendation explainable; reduce financial shame; make privacy visible.
- **Color Philosophy:** A soft parchment background lowers visual stress, deep ink anchors reading, sage signals stability, and a single citrus accent marks action without implying guaranteed outcomes.
- **Layout Paradigm:** A left rail establishes the “ledger” identity while the main column moves from inputs to snapshot to conversation; wide budget cards alternate with narrow notes so the page does not feel like a centered grid.
- **Signature Elements:** A vertical “C” mark in the brand rail, pill-shaped metric labels, and thin ruled dividers that echo a paper ledger.
- **Interaction Philosophy:** Inputs are plain and explicit; results appear in familiar money language; every recommendation includes a “why this appears” explanation.
- **Animation:** No required motion. Preserve a stable layout for reduced-motion users and keep form feedback immediate through normal server responses.
- **Typography System:** Use Georgia for the display wordmark and headings, with system sans-serif for labels, controls, and metric values. Large headings are compact; body copy stays at a comfortable reading width.
- **Brand Essence:** “A calmer first conversation with your money” for people who want a useful starting point without handing over their accounts; thoughtful, grounded, encouraging.
- **Brand Voice:** Headlines are direct and humane. Example lines: “See what your month can carry.” / “Start with the next useful move.”
- **Wordmark & Logo:** A stacked “C / S” ledger monogram inside a narrow rounded rectangle beside the Centsage wordmark.
- **Signature Brand Color:** Citrus chartreuse `#d9ef72`, used sparingly for the primary action and the “breathing room” indicator.

## Project structure

```text
centsage/
├── src/main.c              # C HTTP server, calculations, validation, rendering
├── public/styles.css       # Responsive accessible styling
├── public/manus-routes.json# Webdev route manifest
├── Makefile                # Local build and clean commands
├── Dockerfile              # Production container build
├── TODO.md                 # Outcome-based delivery tracking
└── plan.md                 # This implementation and design plan
```

## Constraints and safeguards

The experience will clearly say it is educational and not licensed financial advice. It will not make investment, tax, insurance, lending, or guaranteed-return claims. Calculations use simple monthly arithmetic and show their assumptions in plain language. The server does not persist submitted financial values.
