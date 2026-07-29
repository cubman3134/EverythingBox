# EverythingBox Discord Community Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the EverythingBox Discord server from a declarative config, and make it discoverable from the repo and from inside the app.

**Architecture:** Two repositories. A new **private** `cubman3134/everythingbox-discord` holds a zero-dependency Node script that reconciles a live Discord server against a committed `server.json`, plus the rules and bot-macro copy. The existing `cubman3134/EverythingBox` gets only the discoverability surface: four text edits and one code change adding a Community row to both halves of `MainWindow::openGeneralSettings()`.

**Tech Stack:** Node ≥ 18 (built-in `fetch`, built-in `node:test`), the Discord REST API v10, and Qt 6 / C++17 for the in-app link.

Design spec: [`docs/superpowers/specs/2026-07-29-discord-community-design.md`](../specs/2026-07-29-discord-community-design.md)

## Global Constraints

- **Zero runtime dependencies** in the Discord repo. No `npm install`, no `package-lock.json`, no `node_modules`. Only Node built-ins.
- **The script never deletes.** Reconciliation emits `create` and `update` operations only. A channel or role present on the server but absent from `server.json` is left alone.
- **`--dry-run` prints the plan and exits 0 without issuing a single mutating request.**
- **The bot token is never committed, logged, or echoed.** Read from `DISCORD_BOT_TOKEN`, falling back to a gitignored `.token` beside the script.
- **The piracy domain blocklist stays private** — it lives only in the private repo. Do not copy it into the EverythingBox repo, the spec, or a commit message.
- **Discord API base:** `https://discord.com/api/v10`. Auth header: `Authorization: Bot <token>`.
- **EverythingBox repo only:** any user-facing setting must be added to **both** builders in `MainWindow::openGeneralSettings()` (see `CONTRIBUTING.md`). The branch gate is `BUILD_DIR=build bash native/tools/run-headless-probes.sh` ending in `ALL HEADLESS PROBES PASSED`.
- **Never run a target-less `cmake --build build`** — 43 probe harnesses build by default. Name the target.

---

## Prerequisites (manual — the user does these, not the implementer)

These require account access and cannot be scripted. Tasks 1–4 can be written before they are done, but Task 5 (apply) blocks on all of them.

- [ ] **P1.** Create an empty Discord server.
- [ ] **P2.** Server Settings → **Enable Community**. Forum channels, Onboarding, Announcement channels and the full AutoMod ruleset all depend on it.
- [ ] **P3.** Confirm **2FA** is enabled on the owning account — Discord requires it for admin actions on community servers.
- [ ] **P4.** Create a bot application at <https://discord.com/developers/applications>, add a bot, and invite it to the server with the **Administrator** permission. Copy the bot token.
- [ ] **P5.** Copy the **server (guild) ID**: enable Developer Mode in Discord settings, right-click the server → Copy Server ID.
- [ ] **P6.** Create the **private** repo `cubman3134/everythingbox-discord` on GitHub.

---

## File Structure

**`everythingbox-discord`** (new, private):

| File | Responsibility |
|---|---|
| `plan.js` | Pure reconciliation: given current server state and desired config, compute the operation list. No I/O. This is where the never-delete invariant lives, and the only part with unit tests. |
| `api.js` | Thin Discord REST wrapper: auth, rate-limit retry, the handful of endpoints used. All I/O lives here. |
| `apply.js` | CLI entry. Reads config + token, fetches state, calls `plan()`, prints or executes. |
| `server.json` | The declarative server definition. Source of truth. |
| `blocklist.json` | Piracy domains and phrases for the AutoMod rules. Private. |
| `test/plan.test.js` | `node --test` unit tests for `plan.js`. |
| `package.json` | `{"type":"module"}` and nothing else — a module marker, not a dependency manifest. Node 22.7+ sniffs ESM syntax and would load these files without it, but on the Node 18/20 floor this plan supports, `.js` is CommonJS and `export` / top-level `await` are syntax errors. Pinning the type makes behaviour identical across the supported range. It declares no dependencies, so no lockfile and no `node_modules` follow from it. |
| `content/rules.md` | The `#welcome-and-rules` copy. |
| `content/tags.md` | Carl-bot tag macro content. |
| `.gitignore` | Ignores `.token`. |
| `README.md` | How to run it. |

**`EverythingBox`** (existing):

| File | Change |
|---|---|
| `README.md` | Community section + badge |
| `.github/ISSUE_TEMPLATE/config.yml` | New — `contact_links` |
| `.github/SUPPORT.md` | New |
| `CONTRIBUTING.md` | Dev-channel pointer |
| `native/src/ui/MainWindow.cpp` | Community row in both builders |

Splitting `plan.js` (pure) from `api.js` (I/O) is the load-bearing decision: it makes the never-delete invariant testable without a Discord server, a token, or a network.

---

## Task 1: Reconciliation planner

The pure core, built test-first. No network, no token, no Discord.

**Files:**
- Create: `everythingbox-discord/plan.js`
- Create: `everythingbox-discord/test/plan.test.js`
- Create: `everythingbox-discord/package.json`
- Create: `everythingbox-discord/.gitignore`

**Interfaces:**
- Consumes: nothing (first task).
- Produces: `plan(current, desired) -> Op[]` where
  `current = { roles: [{id, name}], channels: [{id, name, type, topic, parent_id}] }`,
  `desired = { roles: [{name, ...}], categories: [{name, channels: [...]}] }`,
  and each `Op` is `{ action: 'create'|'update', kind: 'role'|'category'|'channel', name: string, parent?: string, payload: object, id?: string }`.
  Task 3 consumes `plan`; Task 2 supplies the `desired` shape.

- [ ] **Step 1: Initialise the repo**

```bash
mkdir everythingbox-discord && cd everythingbox-discord
git init
git remote add origin git@github.com:cubman3134/everythingbox-discord.git
printf '.token\n' > .gitignore
mkdir -p test content
```

Then create `package.json`. This is a **module marker, not a dependency
manifest**. Node 22.7+ detects ESM syntax on its own and will load `plan.js`
without it — but on the Node 18/20 floor this plan supports, `.js` is CommonJS
and every `export` in `plan.js` and the top-level `await` in `apply.js` is a
syntax error. Pinning the type makes the behaviour the same everywhere. It
declares no dependencies, so it produces no lockfile and no `node_modules`:

```json
{
  "name": "everythingbox-discord",
  "private": true,
  "type": "module"
}
```

- [ ] **Step 2: Write the failing tests**

Create `test/plan.test.js`:

```js
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { plan } from '../plan.js';

const desired = {
  roles: [{ name: 'Maintainer', color: 0xE67E22, hoist: true, permissions: '8' }],
  categories: [
    { name: 'Support', channels: [{ name: 'support', type: 15, topic: 'Ask here' }] },
  ],
};

test('empty server yields a create for every role, category and channel', () => {
  const ops = plan({ roles: [], channels: [] }, desired);
  assert.deepEqual(ops.map(o => `${o.action}:${o.kind}:${o.name}`), [
    'create:role:Maintainer',
    'create:category:Support',
    'create:channel:support',
  ]);
});

test('a fully matching server yields no operations', () => {
  const current = {
    roles: [{ id: 'r1', name: 'Maintainer' }],
    channels: [
      { id: 'c1', name: 'Support', type: 4, parent_id: null },
      { id: 'c2', name: 'support', type: 15, topic: 'Ask here', parent_id: 'c1' },
    ],
  };
  assert.deepEqual(plan(current, desired), []);
});

test('a drifted topic yields an update carrying the existing id', () => {
  const current = {
    roles: [{ id: 'r1', name: 'Maintainer' }],
    channels: [
      { id: 'c1', name: 'Support', type: 4, parent_id: null },
      { id: 'c2', name: 'support', type: 15, topic: 'stale', parent_id: 'c1' },
    ],
  };
  const ops = plan(current, desired);
  assert.equal(ops.length, 1);
  assert.equal(ops[0].action, 'update');
  assert.equal(ops[0].id, 'c2');
  assert.equal(ops[0].payload.topic, 'Ask here');
});

test('NEVER deletes: server extras absent from the config produce no operation', () => {
  const current = {
    roles: [{ id: 'r1', name: 'Maintainer' }, { id: 'r9', name: 'Some Bot Role' }],
    channels: [
      { id: 'c1', name: 'Support', type: 4, parent_id: null },
      { id: 'c2', name: 'support', type: 15, topic: 'Ask here', parent_id: 'c1' },
      { id: 'c9', name: 'random-channel', type: 0, parent_id: null },
    ],
  };
  assert.deepEqual(plan(current, desired), []);
  assert.equal(plan(current, desired).some(o => o.action === 'delete'), false);
});

test('a channel matches by name only within its own category', () => {
  const current = {
    roles: [{ id: 'r1', name: 'Maintainer' }],
    channels: [
      { id: 'c1', name: 'Support', type: 4, parent_id: null },
      { id: 'c8', name: 'support', type: 0, parent_id: null },
    ],
  };
  const ops = plan(current, desired);
  assert.deepEqual(ops.map(o => `${o.action}:${o.name}`), ['create:support']);
});
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `node --test`
Expected: FAIL — `Cannot find module '.../plan.js'`

- [ ] **Step 4: Implement `plan.js`**

```js
// Pure reconciliation. No I/O, no token, no network — which is what makes the
// never-delete invariant testable. Everything that talks to Discord is in api.js.
//
// Matching is by NAME, not id: the config has no ids (a fresh server has not
// issued any yet), and a name is the only stable handle across runs. Channels
// match within their category only, so a #support in Support and a stray
// #support at the root are different channels — renaming a category therefore
// reads as "create the new one", never "move the old one".

const CATEGORY = 4;

// Fields we own. Anything else on the live channel (position, permission
// overwrites set by hand, bot-managed state) is deliberately not compared —
// we would otherwise fight the server owner every run.
const CHANNEL_FIELDS = ['type', 'topic'];

function drifted(existing, want, fields) {
    return fields.some(f => want[f] !== undefined && existing[f] !== want[f]);
}

function patch(existing, want, fields) {
    const p = {};
    for (const f of fields) if (want[f] !== undefined && want[f] !== existing[f]) p[f] = want[f];
    return p;
}

export function plan(current, desired) {
    const ops = [];

    for (const role of desired.roles ?? []) {
        const found = (current.roles ?? []).find(r => r.name === role.name);
        if (!found) {
            ops.push({ action: 'create', kind: 'role', name: role.name, payload: role });
        } else {
            // Roles carry no comparable body in `current` (the guild endpoint returns
            // more than we set), so an existing role by name is treated as satisfied.
            // Recolouring a role by hand is a legitimate act we must not stomp.
        }
    }

    for (const cat of desired.categories ?? []) {
        const existingCat = (current.channels ?? [])
            .find(c => c.type === CATEGORY && c.name === cat.name);

        if (!existingCat) {
            ops.push({ action: 'create', kind: 'category', name: cat.name, payload: { name: cat.name, type: CATEGORY } });
        }

        for (const ch of cat.channels ?? []) {
            // Only ever look inside THIS category. A create is emitted when the
            // category itself is new, because nothing can be parented to it yet.
            const existingCh = existingCat
                ? (current.channels ?? []).find(c => c.parent_id === existingCat.id && c.name === ch.name)
                : undefined;

            if (!existingCh) {
                ops.push({ action: 'create', kind: 'channel', name: ch.name, parent: cat.name, payload: ch });
            } else if (drifted(existingCh, ch, CHANNEL_FIELDS)) {
                ops.push({
                    action: 'update', kind: 'channel', name: ch.name, parent: cat.name,
                    id: existingCh.id, payload: patch(existingCh, ch, CHANNEL_FIELDS),
                });
            }
        }
    }

    return ops;   // never a 'delete' — see the test that asserts it
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `node --test`
Expected: PASS — `# pass 5`, `# fail 0`

- [ ] **Step 6: Commit**

```bash
git add .gitignore package.json plan.js test/plan.test.js
git commit -m "feat: pure reconciliation planner, never-delete by construction"
```

---

## Task 2: The server definition

**Files:**
- Create: `everythingbox-discord/server.json`
- Create: `everythingbox-discord/blocklist.json`

**Interfaces:**
- Consumes: the `desired` shape from Task 1 (`{roles, categories:[{name, channels:[{name,type,topic}]}]}`).
- Produces: `server.json`, read by Task 3's `apply.js`. Channel `type` integers: `0` text, `2` voice, `4` category, `5` announcement, `15` forum.

- [ ] **Step 1: Write `server.json`**

Transcribe the spec's structure table exactly. Roles first, then categories in display order:

```json
{
  "roles": [
    { "name": "Maintainer",     "color": 15105570, "hoist": true,  "permissions": "8" },
    { "name": "Moderator",      "color": 3447003,  "hoist": true,  "permissions": "268445702" },
    { "name": "Contributor",    "color": 5763719,  "hoist": true,  "permissions": "0" },
    { "name": "Addon/Theme Dev","color": 10181046, "hoist": false, "permissions": "0" },
    { "name": "Tester",         "color": 15844367, "hoist": false, "permissions": "0" },
    { "name": "Windows",        "color": 0, "hoist": false, "permissions": "0" },
    { "name": "macOS",          "color": 0, "hoist": false, "permissions": "0" },
    { "name": "Linux",          "color": 0, "hoist": false, "permissions": "0" },
    { "name": "Android TV",     "color": 0, "hoist": false, "permissions": "0" },
    { "name": "iOS",            "color": 0, "hoist": false, "permissions": "0" },
    { "name": "Bots",           "color": 9807270, "hoist": false, "permissions": "0" }
  ],
  "categories": [
    { "name": "📌 Start here", "channels": [
      { "name": "welcome-and-rules", "type": 0, "topic": "Read this first. Rules, what this server is, and where to find the app.", "readOnly": true },
      { "name": "announcements",     "type": 5, "topic": "Releases and breaking changes. Follow this channel to get them in your own server.", "readOnly": true },
      { "name": "releases",          "type": 0, "topic": "Automated release feed from GitHub.", "readOnly": true }
    ]},
    { "name": "💬 Community", "channels": [
      { "name": "general",   "type": 0, "topic": "General EverythingBox chat." },
      { "name": "showcase",  "type": 0, "topic": "Show off your library, your TV setup, your themes and add-ons." },
      { "name": "off-topic", "type": 0, "topic": "Everything else." },
      { "name": "Voice Chat", "type": 2 }
    ]},
    { "name": "🛠 Support", "channels": [
      { "name": "support", "type": 15, "topic": "Ask for help here. Tag your platform and the area, and mark your thread Solved when it is.",
        "tags": ["Windows","macOS","Linux","Android TV","iOS","Video","Audio","Emulation","Addons","Themes","Readers","Solved"] },
      { "name": "faq", "type": 0, "topic": "Answers to the questions that come up most.", "readOnly": true }
    ]},
    { "name": "🧩 Addons & themes", "channels": [
      { "name": "addon-development", "type": 0, "topic": "Building media-source add-ons against the sandboxed JS addon protocol." },
      { "name": "theme-development", "type": 0, "topic": "Building themes against the QML theme surface." }
    ]},
    { "name": "🔧 Dev", "gated": "Contributor", "channels": [
      { "name": "dev-general", "type": 0, "topic": "Working on EverythingBox itself." },
      { "name": "github",      "type": 0, "topic": "Automated issue, PR and commit feed.", "readOnly": true },
      { "name": "testing",     "type": 0, "topic": "Release-candidate testing. Platform roles get pinged here." }
    ]},
    { "name": "🔒 Staff", "gated": "Moderator", "channels": [
      { "name": "mod-log",  "type": 0, "topic": "AutoMod alerts and bot logging." },
      { "name": "mod-chat", "type": 0, "topic": "Moderator coordination." }
    ]}
  ]
}
```

- [ ] **Step 2: Write `blocklist.json`**

Two lists, used by Task 5's AutoMod configuration. `domains` are hard-blocked; `phrases` only raise an alert, per the spec's asymmetry. Populate `domains` with the ROM/stream sites you actually see; seed `phrases` narrowly enough that ordinary emulation talk never trips it.

```json
{
  "_comment": "PRIVATE. Publishing this list is a roadmap for evading it. Domains are blocked outright; phrases only alert #mod-log, because 'I dumped my own ROM' and 'which core loads this ISO' are legitimate on-topic messages.",
  "domains": [],
  "phrases": [
    "where can i download * rom",
    "where do i get * iso",
    "send me the rom",
    "dm me the iso",
    "link to * rom pack"
  ]
}
```

- [ ] **Step 3: Verify both files are valid JSON**

Run: `node --input-type=module -e "import fs from 'node:fs'; JSON.parse(fs.readFileSync('server.json','utf8')); JSON.parse(fs.readFileSync('blocklist.json','utf8')); console.log('JSON OK')"`
Expected: `JSON OK`

- [ ] **Step 4: Verify the config satisfies the planner**

Run: `node --input-type=module -e "import fs from 'node:fs'; import {plan} from './plan.js'; const d=JSON.parse(fs.readFileSync('server.json','utf8')); const ops=plan({roles:[],channels:[]},d); console.log(ops.length+' ops, deletes: '+ops.filter(o=>o.action==='delete').length);"`
Expected: `33 ops, deletes: 0` (11 roles + 6 categories + 16 channels)

- [ ] **Step 5: Commit**

```bash
git add server.json blocklist.json
git commit -m "feat: declarative server definition and the private AutoMod blocklist"
```

---

## Task 3: REST client and the apply CLI

**Files:**
- Create: `everythingbox-discord/api.js`
- Create: `everythingbox-discord/apply.js`
- Create: `everythingbox-discord/README.md`

**Interfaces:**
- Consumes: `plan(current, desired)` from Task 1; `server.json` from Task 2.
- Produces: the `node apply.js [--dry-run]` CLI. Nothing downstream consumes it in code.

- [ ] **Step 1: Write `api.js`**

```js
// Every network call lives here. Two things matter: the token never appears in
// any thrown message or log line, and 429s are honoured — Discord rate-limits
// channel creation aggressively and a bulk apply WILL hit it.

const BASE = 'https://discord.com/api/v10';

export function readToken(fs, path) {
    const env = process.env.DISCORD_BOT_TOKEN;
    if (env && env.trim()) return env.trim();
    try {
        const t = fs.readFileSync(path, 'utf8').trim();
        if (t) return t;
    } catch { /* fall through to the error below */ }
    throw new Error('No bot token. Set DISCORD_BOT_TOKEN or create a .token file beside apply.js.');
}

export function makeClient(token) {
    async function request(method, route, body) {
        for (let attempt = 0; ; attempt++) {
            const res = await fetch(BASE + route, {
                method,
                headers: {
                    'Authorization': `Bot ${token}`,
                    'Content-Type': 'application/json',
                    'User-Agent': 'EverythingBoxSetup (https://github.com/cubman3134/EverythingBox, 1.0)',
                },
                body: body ? JSON.stringify(body) : undefined,
            });

            if (res.status === 429) {
                const retry = Number(res.headers.get('retry-after') ?? 1);
                if (attempt >= 5) throw new Error(`Rate limited on ${method} ${route} after 5 retries.`);
                await new Promise(r => setTimeout(r, (retry + 0.5) * 1000));
                continue;
            }
            if (!res.ok) {
                // res.statusText only — a Discord error body can echo the request,
                // and the token must never reach a log.
                throw new Error(`${method} ${route} failed: ${res.status} ${res.statusText}`);
            }
            return res.status === 204 ? null : res.json();
        }
    }

    return {
        getRoles:    guild => request('GET',  `/guilds/${guild}/roles`),
        getChannels: guild => request('GET',  `/guilds/${guild}/channels`),
        createRole:  (guild, body) => request('POST',  `/guilds/${guild}/roles`, body),
        createChannel: (guild, body) => request('POST', `/guilds/${guild}/channels`, body),
        patchChannel:  (id, body) => request('PATCH', `/channels/${id}`, body),
    };
}
```

- [ ] **Step 2: Write `apply.js`**

```js
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { plan } from './plan.js';
import { readToken, makeClient } from './api.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const dryRun = process.argv.includes('--dry-run');
const guild = process.env.DISCORD_GUILD_ID;
if (!guild) { console.error('Set DISCORD_GUILD_ID to the server id.'); process.exit(1); }

const desired = JSON.parse(fs.readFileSync(path.join(here, 'server.json'), 'utf8'));
const client = makeClient(readToken(fs, path.join(here, '.token')));

const current = { roles: await client.getRoles(guild), channels: await client.getChannels(guild) };
const ops = plan(current, desired);

if (!ops.length) { console.log('Server already matches server.json. Nothing to do.'); process.exit(0); }

console.log(`${ops.length} operation(s):`);
for (const op of ops) console.log(`  ${op.action} ${op.kind} ${op.parent ? op.parent + ' / ' : ''}${op.name}`);

if (dryRun) { console.log('\n--dry-run: nothing was changed.'); process.exit(0); }

// Categories must exist before their channels can be parented, so creates run
// in plan order and the created ids are threaded forward as we go.
const catIds = new Map();
for (const c of current.channels) if (c.type === 4) catIds.set(c.name, c.id);

for (const op of ops) {
    if (op.kind === 'role' && op.action === 'create') {
        await client.createRole(guild, op.payload);
    } else if (op.kind === 'category' && op.action === 'create') {
        const made = await client.createChannel(guild, { name: op.name, type: 4 });
        catIds.set(op.name, made.id);
    } else if (op.kind === 'channel' && op.action === 'create') {
        const { tags, readOnly, ...body } = op.payload;   // not channel-create fields
        await client.createChannel(guild, { ...body, parent_id: catIds.get(op.parent) });
    } else if (op.action === 'update') {
        await client.patchChannel(op.id, op.payload);
    }
    console.log(`  done: ${op.action} ${op.name}`);
}
console.log('\nApplied. Forum tags, read-only overwrites and category gating are set in the client — see README.');
```

- [ ] **Step 3: Verify the CLI refuses to run without a token**

Run: `DISCORD_GUILD_ID=1 node apply.js --dry-run`
Expected: exits non-zero with `No bot token. Set DISCORD_BOT_TOKEN or create a .token file beside apply.js.` — and **no token value anywhere in the output**.

- [ ] **Step 4: Write `README.md`**

```markdown
# everythingbox-discord

Private. Builds the EverythingBox Discord server from `server.json`.

## Run

    export DISCORD_GUILD_ID=<server id>
    export DISCORD_BOT_TOKEN=<bot token>     # or put it in .token (gitignored)

    node apply.js --dry-run                  # always do this first
    node apply.js

Idempotent: matches by name, creates what is missing, updates what has drifted,
and **never deletes**. Safe to re-run after editing `server.json`.

## Set by hand in the client

The script builds structure. These are one-time client-side steps it does not do:
forum tags on `#support`, read-only permission overwrites, `🔧 Dev` and `🔒 Staff`
category gating, Onboarding questions, the GitHub webhooks, and the AutoMod rules
from `blocklist.json`.

## Tests

    node --test
```

- [ ] **Step 5: Commit**

```bash
git add api.js apply.js README.md
git commit -m "feat: Discord REST client and the idempotent apply CLI"
git push -u origin main
```

---

## Task 4: Rules and macro copy

**Files:**
- Create: `everythingbox-discord/content/rules.md`
- Create: `everythingbox-discord/content/tags.md`

**Interfaces:**
- Consumes: nothing.
- Produces: copy pasted into `#welcome-and-rules` and into Carl-bot tags during Task 5.

- [ ] **Step 1: Write `content/rules.md`**

Eight rules. Rule 4 carries its reasoning, per the spec — a prohibition with a rationale is followed, a bare one is argued with.

```markdown
**Welcome to EverythingBox.**

EverythingBox is a free, open-source media hub — video, music, emulation, comics
and books, all in one place, on desktop, Android TV and iOS.
GitHub: <https://github.com/cubman3134/EverythingBox>

**1.** Be decent to each other. Disagree about software all you like; don't make it personal.
**2.** English in the public channels, so everyone can follow along and search finds answers.
**3.** Use the right room. Questions go in the #support forum with your platform tagged — not #general.
**4.** **No piracy.** No links to, requests for, or sourcing of copyrighted ROMs, ISOs, or media streams — not in public, not in DMs arranged here. This is not us being precious: Discord's Terms of Service prohibit it, and servers that tolerate it get removed, which would take this one with it. Emulation, core setup, dumping media you own, and legal sources are all completely fine and actively welcome.
**5.** No piracy-site links in your profile, status, or username either.
**6.** Don't advertise. No unsolicited server invites or self-promo without asking first.
**7.** Keep it work-safe. No NSFW content, harassment, or hate speech.
**8.** Bug reports belong on GitHub — the issue templates get them fixed faster than a chat message will.

Moderator decisions are final here; take an appeal to DM rather than to the channel.
```

- [ ] **Step 2: Write `content/tags.md`**

Six macros covering the questions the README's own caveats predict.

```markdown
# Carl-bot tags

Create with `!tag add <name> <content>`.

**logs** — The log is `stream_debug.log` in the app data directory. In-app: Settings ▸ Debug shows its tail and can open the folder. Attach it to your #support thread; it's usually the fastest route to a diagnosis.

**macos** — The macOS build is unsigned, so Gatekeeper blocks a normal double-click. First launch only: right-click the app ▸ Open, then confirm. After that it opens normally.

**androidtv** — One APK covers phones, tablets and Android TV; sideload it. The media hub and in-process libretro cores work. The standalone console emulators (Dolphin, PCSX2, RPCS3…) are desktop-only — Android can't launch downloaded desktop executables — so they're gated off the Android build.

**ios** — The .ipa is unsigned. Sideload with AltStore or Sideloadly, which re-sign it with your own Apple ID. The media hub works; emulation is unavailable on iOS.

**build** — Configure with `-DEVERYTHINGBOX_BUILD_APP=ON` plus your Qt/libmpv paths, then build **named targets only** — a bare `cmake --build build` compiles all 43 probe harnesses. Full instructions: <https://github.com/cubman3134/EverythingBox/blob/main/CONTRIBUTING.md>

**bug** — Please open an issue: <https://github.com/cubman3134/EverythingBox/issues/new/choose>. Include your platform, the version from Settings ▸ General, and `stream_debug.log` (see `!logs`).
```

- [ ] **Step 3: Commit**

```bash
git add content/
git commit -m "docs: rules and bot macro copy"
git push
```

---

## Task 5: Apply and configure the live server

Blocks on prerequisites P1–P6 and Tasks 1–4. This task is mostly client-side clicking; it produces the invite URL that Tasks 6 and 7 embed.

**Files:** none changed.

**Interfaces:**
- Consumes: `apply.js`, `server.json`, `blocklist.json`, `content/*`.
- Produces: **the permanent invite URL**, which Tasks 6 and 7 both hard-code.

- [ ] **Step 1: Dry-run and read the plan**

```bash
export DISCORD_GUILD_ID=<server id>
export DISCORD_BOT_TOKEN=<bot token>
node apply.js --dry-run
```

Expected: 33 operations, all `create`, no `delete`. If anything says `delete`, stop — that is a bug in `plan.js`, not a config problem.

- [ ] **Step 2: Apply**

Run: `node apply.js`
Expected: each operation echoed as `done:`, ending with `Applied.`

- [ ] **Step 3: Verify in the client**

Six categories, sixteen channels, eleven roles. `#support` is a Forum, `#announcements` is an Announcement channel.

- [ ] **Step 4: Set what the script deliberately leaves alone**

- Forum tags on `#support`: the twelve from `server.json`.
- Read-only channels (`#welcome-and-rules`, `#announcements`, `#releases`, `#faq`, `#github`): deny **Send Messages** for `@everyone`.
- `🔧 Dev`: deny **View Channel** for `@everyone`, allow for Contributor. `🔒 Staff`: deny for `@everyone`, allow for Moderator and Maintainer.

- [ ] **Step 5: Post the rules and configure Onboarding**

Paste `content/rules.md` into `#welcome-and-rules`. Then Server Settings ▸ Onboarding: enable rules screening; add a platform question granting the five platform roles (multi-select), and a "What brings you here?" question whose answers grant Tester, Addon/Theme Dev and Contributor. Default channels: `#welcome-and-rules`, `#general`, `#support`, `#announcements`.

- [ ] **Step 6: Configure AutoMod**

Five rules, per the spec:

| Rule | Trigger | Action |
|---|---|---|
| Piracy domains | keyword list from `blocklist.json` `domains` | Block message + alert `#mod-log` |
| Piracy phrases | keyword list from `blocklist.json` `phrases` | **Alert `#mod-log` only — do NOT block** |
| Invite links | `discord.gg`, `discord.com/invite` | Block, exempt Maintainer/Moderator |
| Mention spam | native, threshold 5 | Block |
| Suspected spam | native | Block |

Rule 2 must be alert-only. Blocking it makes the server hostile to its own subject matter.

- [ ] **Step 7: Wire the GitHub webhooks**

In `#releases`: Integrations ▸ Webhooks ▸ New Webhook, copy the URL, and add it to the EverythingBox repo (Settings ▸ Webhooks) with the `/github` suffix, **Releases only**. Repeat for `#github` with issues, pull requests and pushes.

- [ ] **Step 8: Add Carl-bot, create the tags, and seed `#faq`**

Invite Carl-bot, set logging to `#mod-log`, and create the six tags from `content/tags.md`. Then post the same six answers into `#faq` as one message — the spec has `#faq` mirroring the macros, so someone scrolling finds them without knowing the tag names exist.

- [ ] **Step 9: Create the permanent invite**

Right-click `#welcome-and-rules` ▸ Invite People ▸ Edit invite link: **Expire After: Never**, **Max Number of Uses: No Limit**. Record the URL — Tasks 6 and 7 both need it.

---

## Task 6: EverythingBox repo wiring

**Files:**
- Modify: `README.md`
- Create: `.github/ISSUE_TEMPLATE/config.yml`
- Create: `.github/SUPPORT.md`
- Modify: `CONTRIBUTING.md:154-161` (the "Reporting bugs and proposing features" section)

**Interfaces:**
- Consumes: the invite URL from Task 5 Step 9. Written below as `<INVITE>` — substitute the real URL.
- Produces: nothing consumed by code.

- [ ] **Step 1: Branch**

```bash
cd "C:/Users/cubma/Project Goliath"
git checkout main && git pull && git checkout -b feat/discord-community
```

- [ ] **Step 2: Add the README badge and Community section**

Insert the badge immediately below the `<p align="center">` logo block at `README.md:1-3`:

```markdown
<p align="center">
  <a href="<INVITE>"><img src="https://img.shields.io/discord/<SERVER_ID>?label=Discord&logo=discord&logoColor=white&color=5865F2" alt="Discord"></a>
</p>
```

Then add a Community section immediately above `## Licence` (`README.md:93`):

```markdown
## Community

Questions, setup help, and everything else: **[join the Discord](<INVITE>)**.

Ask in `#support` — tag your platform and the area and you'll get a faster,
more specific answer. Bugs still belong in the
[issue tracker](https://github.com/cubman3134/EverythingBox/issues), and
`#announcements` carries every release.
```

- [ ] **Step 3: Add the issue-template contact link**

Create `.github/ISSUE_TEMPLATE/config.yml`. This puts the invite on GitHub's New Issue chooser, catching people at the moment they need help — before they file a support question as a bug:

```yaml
blank_issues_enabled: false
contact_links:
  - name: Questions and setup help
    url: <INVITE>
    about: Not sure it's a bug? Ask in the Discord — #support is faster than an issue for install, emulator and add-on problems.
  - name: Add-ons and themes
    url: https://github.com/cubman3134/everythingbox-addons
    about: Browse the community add-on registry, or contribute your own.
```

- [ ] **Step 4: Add `SUPPORT.md`**

```markdown
# Getting help with EverythingBox

**Questions, setup problems, "is this supposed to happen?"** → the Discord: <INVITE>

Post in `#support` and tag your platform (Windows / macOS / Linux / Android TV /
iOS) and the area (Video / Audio / Emulation / Addons / Themes / Readers). Attach
`stream_debug.log` — Settings ▸ Debug shows its tail and opens its folder. Mark
the thread **Solved** when it is, so the next person with your problem finds it.

**A reproducible bug, or a feature idea** → the
[issue tracker](https://github.com/cubman3134/EverythingBox/issues/new/choose).

**Contributing code** → [CONTRIBUTING.md](CONTRIBUTING.md), then `#dev-general`.
```

- [ ] **Step 5: Point CONTRIBUTING at the dev channels**

In `CONTRIBUTING.md`, append to the "Reporting bugs and proposing features" section, immediately before the Code of Conduct line at `CONTRIBUTING.md:160`:

```markdown
For design discussion before you write code — protocol changes, anything that
touches the nav kit — `#dev-general` on the [Discord](<INVITE>) is lower latency
than issue comments. Add-on and theme authors have `#addon-development` and
`#theme-development`.
```

- [ ] **Step 6: Verify no placeholder survived**

Run: `grep -rn "<INVITE>\|<SERVER_ID>" README.md CONTRIBUTING.md .github/`
Expected: **no output**. Any hit is an unsubstituted placeholder.

- [ ] **Step 7: Commit**

```bash
git add README.md CONTRIBUTING.md .github/ISSUE_TEMPLATE/config.yml .github/SUPPORT.md
git commit -m "docs: point users at the Discord from the repo surfaces"
```

---

## Task 7: In-app Community link

The one code change. Both builders, per `CONTRIBUTING.md` — the themed surface is the default-reachable one, so a row added only to the classic builder ships invisible to most users.

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` — constant near the top; themed rows at `:9805`; themed handler at `:9818`; classic builder at `:10573`.

**Interfaces:**
- Consumes: the invite URL from Task 5 Step 9.
- Produces: nothing consumed by other tasks.

- [ ] **Step 1: Add the invite constant**

Next to the other file-scope constants in `MainWindow.cpp`, mirroring `AppUpdater`'s `kReleasesPage`:

```cpp
// The community server. Permanent, non-expiring invite — see the Discord design spec.
static constexpr const char* kDiscordInvite = "<INVITE>";
```

- [ ] **Step 2: Add the themed row**

In `openGeneralSettings()`, immediately after the Debrid `textf(...)` at `MainWindow.cpp:9804-9805` and before the `setInfo` helper at `:9808`:

```cpp
        // --- Community ---
        sep(tr("Community"));
        action(QStringLiteral("community.discord"), tr("Join the Discord"));
```

- [ ] **Step 3: Add the themed handler branch**

In the `present()` handler lambda, immediately after the `disp.fullscreen` branch at `MainWindow.cpp:9818-9821`:

```cpp
                else if (id == QStringLiteral("community.discord")) {
                    // Outward navigation to the browser — same idiom as Appearance's theme-gallery row.
                    QDesktopServices::openUrl(QUrl(QString::fromLatin1(kDiscordInvite)));
                }
```

- [ ] **Step 4: Add the classic twin**

At the end of the classic builder, immediately after `v->addWidget(dNote);` at `MainWindow.cpp:10573` and before the closing `}, [this] { openSettingsHub(); });` at `:10574`:

```cpp
        // --- Community: the classic twin of the themed builder's community.discord row. ---
        v->addSpacing(10);
        auto* cHeading = new QLabel(tr("Community"));
        cHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(cHeading);
        auto* cNote = new QLabel(tr("Questions, setup help, and release news. Ask in #support and tag your "
            "platform — it gets you a faster, more specific answer than the issue tracker will."));
        cNote->setWordWrap(true);
        cNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(cNote);
        auto* cJoin = panelRow(tr("Join the Discord"));
        connect(cJoin, &QPushButton::clicked, this, [this] {
            QDesktopServices::openUrl(QUrl(QString::fromLatin1(kDiscordInvite)));
        });
        v->addWidget(cJoin);
```

- [ ] **Step 5: Build**

```bash
cmake --build build --config Release --target everythingbox
```

Expected: builds clean. **Do not** run a target-less build.

- [ ] **Step 6: Run the gate**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

Expected: ends with `ALL HEADLESS PROBES PASSED`. Anything else is a failing branch regardless of how unrelated it looks.

- [ ] **Step 7: Verify both surfaces show the row**

The probe suite gates invariants, not this row's presence, so confirm it by driving the app. Launch with `EB_UITEST=1` and drive via `native/tools/uitest.py` — no window focus needed:

- Themed surface (default): Settings ▸ General, scroll to the bottom. **Community** separator, **Join the Discord** action row, reachable with the D-pad.
- Classic surface: same, with the heading, the grey note, and the button.

Activate the row on the themed surface and confirm the browser opens the invite.

- [ ] **Step 8: Commit**

```bash
git add native/src/ui/MainWindow.cpp
git commit -m "feat: Community row opening the Discord, in both settings builders"
```

Note: the pre-commit hook bumps the patch version and `git add`s `native/CMakeLists.txt` and `native/src/main.cpp` wholesale. Make sure neither carries unrelated unstaged work first, or it gets swept into this commit. `EB_NO_VERSION_BUMP=1` skips the hook.

- [ ] **Step 9: Open the PR**

```bash
git push -u origin feat/discord-community
gh pr create --title "Discord community server" --body "Points users at the new Discord from the README, the issue chooser, SUPPORT.md and Settings ▸ General. Server structure and setup script live in the private everythingbox-discord repo. Spec: docs/superpowers/specs/2026-07-29-discord-community-design.md"
```

---

## Verification

The whole thing is done when:

- `node --test` passes in `everythingbox-discord`, including the never-delete test.
- `node apply.js --dry-run` against the live server reports **0 operations** — the config and the server agree.
- A fresh account joining hits rules screening, picks a platform, and lands in `#general` seeing four channels.
- Posting a blocklisted domain is blocked and alerts `#mod-log`; posting a blocklisted *phrase* is **not** blocked but does alert.
- A GitHub release posts to `#releases`.
- `BUILD_DIR=build bash native/tools/run-headless-probes.sh` ends in `ALL HEADLESS PROBES PASSED`.
- Settings ▸ General ▸ **Join the Discord** opens the invite on the themed surface *and* the classic one.
