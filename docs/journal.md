# Diário (bullet journal)

The Diário page turns Followup into a bullet journal: tasks, notes and events filed
under a **year**, **month**, **week** or **day**, pulled down level by level until the
day they get done. Heavy planning happens in the web app
([followup-web](https://github.com/Caliton/followup-web)); the device captures and
executes the day and the week. Both sides share one Firestore database.

## Components

| Piece | Role |
| --- | --- |
| `components/journal_service` | Item store: `journal/items.jsonl` on the SD card (one JSON object per line, rewritten atomically) mirrored in a PSRAM cache. Also `journal_period` (period keys, ISO weeks, pt-BR labels), pure C++ with host tests. |
| `components/journal_view` | View model: builds the timeline groups of one level and the per-item actions. Pure C++, so `scripts/preview_screens.py` renders the real page. |
| `main/journal_page_*` | Page coordinator (focus: level switcher → group chips → items → footer) and runtime (store sync, item-actions modal, release on leave). |
| `components/journal_sync_service` | Firebase sync: anonymous Auth, pairing code, pull/push over the Firestore REST API. |
| `main/app_link_runtime` | Settings → "Conectar app" button: code modal, linked modal, toasts. |

## Model

An item has a `type` (task / note / event), a `status` (open / done / cancelled) and a
`period` key:

| Level | Key | Example |
| --- | --- | --- |
| year | `YYYY` | `2026` |
| month | `YYYY-MM` | `2026-10` |
| week | `GGGG-Www` (ISO 8601, Monday first) | `2026-W40` |
| day | `YYYY-MM-DD` | `2026-10-03` |

"Pulling" an item into a narrower period only changes `period`. `planned` keeps the
widest period it was first filed under, so the month still lists a task pulled into a
week, marked `» S40` (the e-paper fonts are Latin-1, so `»` stands in for the BuJo `>`).

An open item whose period already ended is **pending**. The day view opens on a
"Pendentes" group so they get migrated one by one (today / this week / this month /
postpone / cancel) — the BuJo review.

## On the device

- Dashboard menu: **Diário** (badge = pending count), **Ideias** (notes + ideas, with the
  Checar vibe and Resumir buttons), Acompanhar, Livros. The progress bar follows today's
  journal tasks. There is no separate task list: the old `todos/` folder is moved once to
  `tarefas_antigas/` on the SD card at boot (out of the app; delete it over OTG if unwanted).
- OK on the switcher enters it; UP/DOWN flips Ano / Mês / Semana / Dia live; OK or
  hold DOWN leaves it. OK on a group enters its items; OK on an item opens its actions.
- **Voice**: Gemini classifies each take as task / note / idea / event and extracts a
  spoken "when" (`today`, `tomorrow`, `this_week`, `next_week`, `this_month`,
  `next_month` or a date). Tasks and events always become journal items (spoken period,
  else the Diário screen's period, else today); notes do when recorded on the Diário screen
  or when they name a time; ideas always stay in Ideias. A take tagged "Tarefa" by hand
  (no Gemini) becomes an audio-only journal task. The recording is linked to the item
  (`journal_item_id` in its sidecar) and hidden from Ideias.
- **Resumir → Semana** summarizes this week's journal items plus the pending ones.
- Only this year's items plus the pending ones are materialized on the page, as
  one-line excerpts capped at 20 rows per group: every row string lives in internal RAM.

## Sync with the web app

The device signs in to Firebase Auth anonymously (`accounts:signUp`), keeps the refresh
token in NVS (`jsync` namespace) and pairs through a 6-character code:

1. Settings → **Conectar app**: the device creates `pairings/{code}` and shows the code.
2. The user types it in the web app (Aparelhos), which links the device's uid to their
   account (`users/{uid}/devices/{deviceUid}`) and fills `ownerUid` in the pairing.
3. The device polls the pairing, stores the owner uid and starts syncing.

A sync round (on Wi-Fi up, 8 s after a local change, every 15 min, or on demand) pulls
`users/{owner}/items` ordered by `serverUpdatedAt` after the stored cursor, merges them
last-writer-wins on `updatedAt`, then commits the dirty items with a `REQUEST_TIME`
transform on `serverUpdatedAt`. Deletes are soft (`deleted: true`) so every side learns
about them; synced tombstones are pruned from the SD after 30 days.

Firestore document fields (`users/{uid}/items/{id}`): `type`, `text` (≤ 500 chars),
`period`, `planned`, `status`, `recordingId`, `createdAt`, `updatedAt` (unix seconds),
`deleted`, `origin` (`device` / `web`), `serverUpdatedAt` (server timestamp). The
security rules live in the web repo (`firestore.rules`).

The worker is a short-lived task with an 8 KB internal-RAM stack (TLS); it only starts
while no transcription is in flight and the internal heap has ≥ 20 KB free. cJSON
allocations prefer PSRAM app-wide (`main/main.cpp`), so a sync page cannot exhaust the
internal heap. The Firebase project and public web API key are Kconfig options
(`FOLLOWUP_FIREBASE_PROJECT_ID`, `FOLLOWUP_FIREBASE_API_KEY`).

## Checking changes on the PC

```bash
python scripts/test_journal_host.py            # period math (ISO weeks, labels, ...)
python scripts/preview_screens.py diario       # the Diário scenes as PNG
```
