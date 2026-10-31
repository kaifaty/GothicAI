# Claude as a game designer / creative design partner: prompts, project instructions and workflows that steer design work

Scope note. Research done 2026-10-21. Sources are tagged [Claude] when the claim is about Claude specifically, [LLM-general] when it is about LLMs in general, and [older] when published before 2025. Prompt fragments are paraphrased unless a short phrase is in quotation marks. Anthropic's sample prompts are paraphrased closely enough to rebuild them, and the URLs lead to the full text.

## 1. Which concrete prompts, system prompts and CLAUDE.md patterns do game developers publish for using Claude as a design partner? What works and what fails?

### Takeaway
The most-copied public pattern is a Claude Code "studio" template: role subagents (creative director, game designer, and so on), a consultant-not-executor protocol (ask, give 2–4 options, the user decides, draft, approve before writing), and fixed GDD sections that start from "Player Fantasy". Smaller indie repos use a shorter pattern: a project CLAUDE.md that points to one vision file holding the design pillars as "non-negotiables" with a decision owner. Practitioners say the structure helps, but that same-model "independent" reviewers, context bloat and "nobody owns fun" are the main ways these setups fail.

### Cited Findings
- **Claude Code Game Studios (Donchitos), the largest public template** [Claude]. It is MIT-licensed. The README shows 49 agents, 74 skills, 15 hooks and 14 rules, and about 27k stars when fetched; the About blurb says 72 skills, so the counts disagree. Agents come in three tiers: Directors (creative-director, technical-director, producer) on Opus, Department Leads (game-designer and others) and Specialists. The README notes that tiers mean seniority of role, not a billing plan. — [GitHub README](https://github.com/Donchitos/Claude-Code-Game-Studios)
- The template's collaboration protocol has five steps: Ask → Present options → You decide → Draft → Approve. Agents ask before proposing, show 2–4 options with pros and cons, and nothing is written without sign-off. A `modes.rigor` switch (minimal/standard/full) sets how heavy the process is, with minimal as the default. — [GitHub README](https://github.com/Donchitos/Claude-Code-Game-Studios)
- The `game-designer` agent definition [Claude]:
  - It calls itself a "collaborative consultant, not an autonomous executor", and the user makes all creative decisions.
  - Bash is disallowed. `maxTurns` is 20.
  - It drafts one section at a time and uses no Write/Edit before approval.
  - When `AskUserQuestion` is not available, it marks its own pick as "(Recommended)".
  - Hard "must not" rules: no implementation code, no art or audio direction, no final narrative text, no architecture choices, no scope changes without the producer. — [game-designer.md](https://raw.githubusercontent.com/Donchitos/Claude-Code-Game-Studios/main/.claude/agents/game-designer.md)
- The frameworks named in that agent prompt:
  - MDA, worked backward from target aesthetics;
  - Self-Determination Theory (autonomy, competence, relatedness);
  - Flow, with a sawtooth difficulty curve;
  - Bartle and Quantic Foundry player types;
  - nested core loops (30-second micro, 5–15-minute meso, session-level macro);
  - TTK/TTC as balance anchors;
  - sink/faucet economy;
  - Sirlin's "Playing to Win", to tell mastery from degenerate play. — [game-designer.md](https://raw.githubusercontent.com/Donchitos/Claude-Code-Game-Studios/main/.claude/agents/game-designer.md)
- **GDD standard put in `design/CLAUDE.md`** [Claude]. Eight sections, in order: Overview, Player Fantasy (intended feeling), Detailed Rules, Formulas (all math with variables), Edge Cases, Dependencies, Tuning Knobs, Acceptance Criteria (testable). — [design/CLAUDE.md](https://github.com/Donchitos/Claude-Code-Game-Studios/blob/05ad7c8a5b8aa2aa86bbd3439ae7cf7295cac418/design/CLAUDE.md)
- AlterLab GameForge's starter CLAUDE.md [Claude]:
  - It uses the same 8-section spec, with Acceptance Criteria written as "Given X, when Y, then Z".
  - Tuning knobs list default, valid range and effect.
  - A routing table sends "creative vision or pillars" to a `/game-creative-director` skill and mechanics or balance to `/game-designer`. — [AlterLab CLAUDE.md](https://github.com/AlterLab-IEU/AlterLab_GameForge/blob/5f5148d61986b32299070e87fcd4a1ab3718eacf/starters/claude-config/CLAUDE.md)
- Indie pattern, "pillars as non-negotiables with a decision owner" [Claude]:
  - BlackCommission (Unity 6). CLAUDE.md declares `AGENTS.md` authoritative for game vision, design pillars, scope and long-term constraints, names the human PM as final decision maker, and requires reading it before any design or implementation work. — [BlackCommission CLAUDE.md](https://github.com/DarkGameHub/BlackCommission/blob/ff08076d965e57390b23f67d601019f2d69ebc5e/CLAUDE.md)
  - vpet-game. Three pillars (permadeath, opaque evolution, social battle) are headed "never remove without an explicit recorded decision". Another rule forbids changing ENGINE logic for a visual or content change. — [vpet-game CLAUDE.md](https://github.com/Dutchthenomad/vpet-game/blob/bb5b4f32c5422e58f2eb09235cd05942bcccc8f1/CLAUDE.md)
  - spacehole_rogue. Five one-line pillars sit directly in CLAUDE.md, for example that jump fuel is the critical gate and "one more system" is the addiction. Design detail lives in `planning/DESIGN.md`. — [spacehole_rogue CLAUDE.md](https://github.com/burtbyproxy/spacehole_rogue/blob/d5080769a13fab3dcdde6e20a4f273ff2f1fcd71/CLAUDE.md)
  - Mythic Mayor (city-simulator). CLAUDE.md says to read `prompts/00-vision.md` before any work. That file holds:
    - a "core diagnosis" of why the overhaul exists ("doom spiral", "invisible agency");
    - a visual north star of reference images, with what to take from them and what not to copy;
    - six pillars, framed as "every feature decision is tested against these; if a mechanic violates one, don't build it";
    - phase specs in build order, each with its reason. — [CLAUDE.md](https://github.com/bitwessel/city-simulator/blob/7e4c2c6c4e77598e4d6d18663220e5ef72e1b299/CLAUDE.md), [00-vision.md](https://github.com/bitwessel/city-simulator/blob/master/prompts/00-vision.md)
- **Practitioner critique of the studio template** (Rob Sandberg, 19 May 2026) [Claude]:
  - What works: tiering models by role (Opus for directors, Sonnet for leads, Haiku for read-only checks), named gates that return parseable verdicts, review-intensity modes, path-scoped rules, and human approval.
  - What fails, first: the context window. All agents share one session, the workflows load large documents, and compaction degrades what the agents know.
  - What fails, second: the "independent" Creative and Technical Directors are the same model with different prompts reading the same docs.
  - What fails, third: institutional memory sits in markdown files someone has to maintain, and "the planning tool becomes the project". — [Production Alchemist](https://www.productionalchemist.com/p/claude-code-game-studios-49-agents)
- **One-person studio on Claude Code** (rudy_candy, Oct 2026) [Claude]:
  - Setup: a "producer" session runs 15 subagents.
  - What helped: splitting departments by question (what the original does, what fits this game, how to build it, is it fun) rather than by output; keeping reviewers separate from authors, because the author session writes tests that pass "because they were written to pass".
  - What failed: at first nobody asked whether the game was fun, so a playtest-critic department was added.
  - Result: 7 games and about $1 of ad revenue. — [DEV Community](https://dev.to/rudycandy/i-run-a-one-person-game-studio-on-claude-code-two-months-7-games-about-1-in-ad-revenue-4j37)
- **5-agent studio on Claude Code Teams** (Yurukusa, 2026; Opus 4.6) [Claude]:
  - Each role prompt states the role, allowed actions and explicit prohibitions.
  - `blockedBy` task dependencies stopped agents from acting before their inputs existed.
  - In the comments, the author says they would not yet fully trust an AI designer for game feel. — [DEV Community](https://dev.to/yurukusa/i-ran-a-5-agent-game-studio-with-claude-code-teams-2lpk)
- Game-jam repo ProjectJamJam [Claude]. Claude Code sessions kept a handoff doc (state, architecture, tuning knobs, decision log, known issues) and a CLAUDE.md of conventions, and the design was locked in a versioned commit. This is a log, not a retrospective. — [PR #1](https://github.com/unSAME-Studio/ProjectJamJam/pull/1) (as summarised by search; not opened in full)
- The cc-godot plugin [Claude]. A `/gd:init-game` command asks about game type and mechanics, then writes a GDD and scaffolds folders and scenes. It needs a Godot MCP server. — [cc-godot README](https://cdn.jsdelivr.net/gh/zate/cc-godot@main/README.md) (search summary)
- Prompt-marketplace "Claude prompts for game designers" pages offer fill-in templates (goal, audience, tone, format, must-include points). I found no evidence of testing. — [gptprompts.ai](https://gptprompts.ai/claude-prompts-for-game-designer), [Pixeldex](https://pixeldex.dev/prompts/)

### Inferences
- The patterns that recur in independent repos are:
  1. one authoritative vision/pillars file that CLAUDE.md points to, read before any design work;
  2. pillars as tests ("if it violates one, don't build it") rather than slogans;
  3. a named human decision owner;
  4. explicit "must not" lists by role;
  5. approval before writing design files.
  They fit Anthropic's advice to keep CLAUDE.md short and move domain detail into imported files or skills (Q4).
- Practitioners name the same failures again and again: role-play reviewers that are not independent, context bloat from loading every design doc, process overhead that outgrows the game, and missing ownership of "is it fun / does it feel right". The remedies that appear: fresh-context reviewers, per-question departments, and human playtest as the final gate.

### Gaps
- I found no r/gamedesign, r/gamedev or r/ClaudeAI threads, and no Hacker News threads, with concrete reports on Claude as a design partner (search did not surface them). The evidence here is mostly GitHub templates and blog posts, all anecdotal.
- I found no published CLAUDE.md from a commercial studio, and no Anthropic customer story about game design work.
- A Medium survey of Claude Code for game development (chierhu) returned 403, so its case list (an RTS in Godot that was scrapped, poor grounding in Godot quirks) is known only from search snippets. — [Medium](https://chierhu.medium.com/claude-code-for-game-development-7a88fcd19992)

## 2. How do people make Claude critique a design rather than agree with it?

### Takeaway
Anthropic's own position: Claude should be "diplomatically honest rather than dishonestly diplomatic". Earlier claude.ai system prompts told it not to open with praise and to critically evaluate ideas instead of agreeing. Practitioners add devil's-advocate or red-team turns, a separate fresh-context critic, and checks scored against pillars. Two cautions recur: forced adversarial prompts tend toward vague or contrarian objections, and a reviewer told to find gaps will always find some. Anthropic's fix is to state what counts as a finding.

### Cited Findings
- **Anthropic's constitution (Jan 2026)** [Claude]. Claude should avoid being sycophantic or fostering excessive reliance. Honesty can need courage: disagree with experts when there is good reason, point out unwelcome things, and engage critically with speculative ideas instead of giving empty validation. "Epistemic cowardice" (deliberately vague answers given to placate) is named as a breach. — [anthropic.com/constitution](https://www.anthropic.com/constitution)
- **claude.ai system prompt for Opus 4.1 (5 Aug 2025)** [Claude, mid-2025]:
  - Claude never opens by calling a question or idea good, great, fascinating and so on.
  - It critically evaluates theories and ideas rather than agreeing or praising, and points out flaws or lack of evidence.
  - It gives honest feedback even when that is not what the person hopes to hear.
  - When corrected, it first thinks the issue through, since users make errors too. — [System prompt, Opus 4.1](https://platform.claude.com/docs/en/release-notes/system-prompts/claude-opus-4-1)
- Later claude.ai prompts (Sonnet 4.5, Nov 2025/Jan 2026; Opus 5.5, 22 Sep 2026) [Claude]:
  - The explicit no-flattery lines are gone. A softer line remains: Claude is willing to push back and be honest, constructively.
  - The Opus 5.5 prompt adds: no "self-abasement… or surrender" when criticised, and steady, honest helpfulness.
  - Removing the flattery rules suggests the behaviour is now trained in rather than prompted (my inference, not stated by Anthropic). — [Sonnet 4.5 prompts](https://platform.claude.com/docs/en/release-notes/system-prompts/claude-sonnet-4-5), [Opus 5.5 prompt](https://platform.claude.com/docs/en/release-notes/system-prompts/claude-opus-5-5)
- Anthropic on model tone [Claude]. Opus 4.8 tends toward a "direct, opinionated style with minimal validation-forward phrasing". If you want warmth you must add it back. — [Prompting Opus 4.8](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-opus-4-8)
- **Anthropic: reviewers follow severity filters literally** [Claude]:
  - Phrases like "only report high-severity issues", "be conservative" or "don't nitpick" make Opus 4.8 and Opus 5 drop findings below the stated bar.
  - Recommended instead: report everything, each finding with confidence and severity, and filter in a separate step.
  - If you must filter in one pass, define the bar concretely rather than with "important". — [Prompting Opus 4.8](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-opus-4-8), [Prompting Opus 5](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-opus-5)
- **Anthropic: adversarial review in a fresh context** [Claude]:
  - Claude Code best practices recommend a subagent that sees only the diff and the criteria, not the reasoning that produced the change.
  - Example prompt (paraphrase): review X against PLAN.md, check each requirement is met and nothing outside scope changed, and report gaps, not style preferences.
  - Warning: a reviewer asked for gaps will usually report some even when the work is sound, and chasing every one leads to over-engineering. Tell it to flag only gaps that affect correctness or the stated requirements.
  - The Writer/Reviewer two-session pattern exists because a fresh context is not biased toward its own work. — [Claude Code best practices](https://code.claude.com/docs/en/best-practices)
- Anthropic: the self-correction chain (draft → review against criteria → refine) as separate calls, so each step can be inspected. — [Prompting best practices](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/claude-prompting-best-practices)
- Practitioner devil's-advocate prompts (tipsforclaude, Mar 2026) [Claude]. Paraphrased:
  - after an answer: argue against it, give the strongest case for the opposite, and say where the reasoning is weakest;
  - steelman: give the strongest argument for the option you advised against;
  - poke holes: list your own assumptions and what could go wrong;
  - red team: as a skeptical reviewer, give the top 3 reasons the plan fails. — [tipsforclaude](https://tipsforclaude.com/tips/devils-advocate/)
- "I reprogrammed my AI to disagree with me" (Wyndo, Jul 2025) [Claude/ChatGPT]:
  - The custom instruction made Claude a "constructive challenger" with challenge-first rules and no immediate validation.
  - It killed a weak product idea that default Claude had approved.
  - Downsides: the challenges were often vague and needed redirecting, the rigid challenge-first order invited reflexive contrarianism, and the author softened the prompt a day later. — [The AI Maker](https://aimaker.substack.com/p/i-reprogrammed-my-ai-chatgpt-claude-to-disagree-with-me-devil-advocate)
- The studio template's creative-director agent [Claude]:
  - It is told to explain trade-offs honestly, saying what each option sacrifices; to defer to the user's context; and to set success metrics ("we'll know this was right if…").
  - It adjudicates pillar conflicts by which choice best serves the target player experience in the MDA aesthetics ranking.
  - For scope it uses a "pillar proximity test": features furthest from the pillars are cut first. — [creative-director.md](https://github.com/Donchitos/Claude-Code-Game-Studios/blob/main/.claude/agents/creative-director.md)
- The same template's gates return APPROVE / CONCERNS / REJECT / NOT ASSESSED. NOT ASSESSED never counts as approval; it is treated as CONCERNS. — [brainstorm full-concept.md](https://github.com/Donchitos/Claude-Code-Game-Studios/blob/main/.claude/skills/brainstorm/references/full-concept.md)
- Pillar-based rubric scoring (SPINE, FDG 2026) [LLM-general; Gemini 2.0 Flash, not Claude]:
  - A "feature validation" prompt rates 1–5 how well a feature idea fits the given pillars. Experts called it particularly valuable.
  - Experts also flagged that the LLM treated deliberate tension between pillars as a contradiction: it said the game should not have a twist instead of evaluating the twist. — [arXiv 2605.09767v2](https://arxiv.org/html/2605.09767v2)
- Third-party Claude skills such as "Devil's Advocate" (Claude as blue team vs Codex as red team, round-based, severity-rated verdict) exist, but their instructions could not be read from the listing. — [skills.rest](https://skills.rest/skill/devil-s-advocate)

### Inferences
- For design critique, the most robust combination fits both Anthropic's guidance and the practitioner reports:
  1. a separate fresh-context critic that gets only the design doc, pillars and anti-pillars, not the chat that produced the design;
  2. a rubric with a concrete bar (for example, "a finding = the feature serves no pillar, violates an anti-pillar, breaks the player fantasy, or has an unhandled edge case");
  3. a request for all findings with confidence and severity, triaged by the human.
- Avoid blanket "be harsh" or "challenge first" persona prompts: they lead to vague or contrarian objections.
- Same-model critics catch structural inconsistency (pillar drift, contradictions) better than taste problems. Taste and feel stay with human playtest. Sandberg, Yurukusa and rudy_candy all point this way.
- Opus 4.8 and later already lean direct. The bigger risk on current models may be the reviewer over-reporting when asked to find gaps, rather than flattery.

### Gaps
- I found no controlled comparison of anti-sycophancy prompts on design-critique tasks, for Claude or any LLM. All evidence is anecdotal or comes from general docs.
- I did not find Anthropic's current published sycophancy evaluation numbers for Opus 5.5.

## 3. How are design frameworks (MDA, pillars, player experience goals, design-intent one-pagers) used inside prompts to keep the model aligned with a vision?

### Takeaway
Frameworks work best as checkable structure, not as background theory. That means pillars with a one-sentence definition and a "design test", explicit anti-pillars, a Player Fantasy section required in every system spec, MDA used backward from the target aesthetics, and cross-document checks for "pillar drift" and conflicting player fantasies. Academic work with pillar-checking prompts shows the LLM is useful for spotting gaps and scoring alignment. Left to rewrite, it waters pillars down.

### Cited Findings
- **Pillars with design tests and anti-pillars** (Claude Code Game Studios brainstorm skill) [Claude]:
  - Draft 3–5 pillars, each with a name, a one-sentence definition and a design test ("debating X or Y, this pillar picks __").
  - Pillars should pull against each other a little.
  - Add 3 or more anti-pillars in the form "We will NOT do [thing] because it would compromise [pillar]".
  - Ask at each phase and never generate the whole concept silently; the facilitator facilitates and the user decides.
  - Concepts are generated three ways: verb-first, mashup, or experience-first (MDA backward from one of 8 aesthetics).
  - Each concept card carries: elevator pitch (10-second test), core verb, core fantasy, hook ("like X, and also Y"), primary MDA aesthetic, scope, why it could work, biggest risk. — [full-concept.md](https://github.com/Donchitos/Claude-Code-Game-Studios/blob/main/.claude/skills/brainstorm/references/full-concept.md)
- **Cross-document checks** (`/review-all-gdds`, "Design Holism" phase) [Claude]:
  - Each system's Player Fantasy is checked against the pillars. A system that serves no pillar is flagged "Pillar Drift": add a pillar, redesign the system, or cut it.
  - Any system that does what an anti-pillar forbids is flagged as a violation.
  - Player fantasies across systems must describe one consistent player identity. Example of a flagged conflict: "ruthless warrior" combat, "violence is always avoidable" dialogue and "reckless adventurer" exploration. — [3-design-holism.md](https://github.com/Donchitos/Claude-Code-Game-Studios/blob/main/.claude/skills/review-all-gdds/references/3-design-holism.md)
- Creative-director framework [Claude]:
  - Tone is defined through "experience targets": concrete moments the player should have, not abstract adjectives.
  - The hook must pass the "and also" test.
  - Target MDA aesthetics are ranked in priority order.
  - A positioning map places the game against comparable titles on 2–3 axes. — [creative-director.md](https://github.com/Donchitos/Claude-Code-Game-Studios/blob/main/.claude/agents/creative-director.md)
- Vision file as a prompt (Mythic Mayor) [Claude]:
  - It opens with a diagnosis: what is wrong with the current game and why the overhaul exists.
  - The pillars are phrased as testable constraints, for example "no fail-pressure", "if a feature only moves numbers, find its visual expression", "determinism is sacred".
  - The visual north star lists both what to take from the references and what not to copy.
  - All of this is read before every session. — [00-vision.md](https://github.com/bitwessel/city-simulator/blob/master/prompts/00-vision.md)
- **SPINE (Geheeb, Schwarz, Dyrda, Groh; FDG '26, arXiv May/Aug 2026)** [LLM-general; Gemini 2.0 Flash and GPT-4o-mini, not Claude]:
  - Formal definition: a pillar is a normative high-level principle that directs and constrains decisions, made of a title and a statement of an experiential or structural property.
  - Quality criteria for one pillar: clarity, unicity (one principle per pillar), conciseness, actionability.
  - Set-level criteria: mutual non-contradiction, completeness, bounded size of 3–5.
  - Prompts: structural validation, structural repair, coverage against the core idea, contradiction check, adding missing pillars, and feature-to-pillar alignment scored 1–5. — [arXiv 2605.09767v2](https://arxiv.org/html/2605.09767v2)
- SPINE findings [LLM-general]:
  - Neither model flagged redundancy between similar pillars; the authors blame the prompt design.
  - Repeated "repairs" made pillars "super watered down and abstract"; one expert called each fix "adding more water".
  - The tool often just reworded what the designer had already written.
  - Experts preferred being nudged ("hey this is missing") over being handed rewritten pillars.
  - They wanted the core design idea included as context during rewrites.
  - Feature validation was considered the most valuable feature.
  - Small samples: one game jam and 4 expert interviews. — [arXiv 2605.09767v2](https://arxiv.org/html/2605.09767v2)
- GamePlot (Microsoft/UBC, Nov 2024) [older; LLM-general]. In a study with 14 designers of an LLM plot assistant, designers reported high satisfaction and ownership. The study also confirmed that LLMs struggle to produce "complex and truly innovative content". — [arXiv 2411.02714](https://arxiv.org/abs/2411.02714)
- Background on pillar practice [LLM-general, not Claude-specific]. Use 3–5 pillars, specific and actionable enough to decide mechanics, level design, art, audio and progression. — [gamedesignskills.com: pillars](https://gamedesignskills.com/game-design/design-pillars/), [Strudo concept template](https://strudoapp.com/articles/game-concept-document-template)
- Anthropic's analogous finding for creative output [Claude]:
  - Models drift toward "distributional convergence", the safe high-probability centre of their training data.
  - What breaks it: name the specific defaults to avoid, offer concrete alternatives, set guidance at the right altitude (neither exact hex codes nor vague shared-context wording), give high-level context first, and package it as a skill loaded on demand. — [Claude blog, 12 Nov 2025](https://www.claude.com/blog/improving-frontend-design-through-skills)
- The same pattern on current models [Claude]:
  - On Opus 5.5, a general "avoid a generic AI look" mostly swaps one default for another. What works is naming the specific patterns to avoid and extending the list on each iteration.
  - On Opus 4.8, having the model propose 4 distinct directions before building, and letting the user pick, reliably breaks its default. — [Prompting Opus 5.5](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-opus-5-5), [Prompting Opus 4.8](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-opus-4-8)

### Inferences
- Frameworks keep Claude aligned when they become pass/fail checks:
  - pillar plus design test;
  - anti-pillars as explicit "will not do" lines;
  - a Player Fantasy required in every spec;
  - acceptance criteria in "Given/When/Then" form.
  A paragraph of MDA theory in the prompt does not do this.
- The SPINE "watered down" result and Anthropic's "distributional convergence" describe the same effect. Repeated LLM rewriting pulls distinctive intent toward the generic. So let the model critique and score against the designer's wording, and have the designer keep authorship of the pillars and intent statements.
- Anti-pillars and deliberate tensions should be written down explicitly, with their reason. Otherwise LLM critics treat intended tension as a contradiction (the SPINE "twist" example).
- "Ask for 3–4 distinct directions, the user picks, then build only that one" is documented both by Anthropic (frontend) and by the studio template (concepts and pillars). It is the closest thing to a cross-source practice for creative divergence.

### Gaps
- I found no source that maps MDA layers onto prompt structure and measures the effect.
- I found no study testing pillar-guided prompting on Claude specifically.
- The GDC talk "The Four One-Page Design Docs You Need" turned up in search but was not reviewed; it may be relevant to design-intent one-pagers. — [GDC Vault](https://gdcvault.com/play/1035120/The-Four-One-Page-Design)

## 4. What does Anthropic's official prompt-engineering guidance for Claude 4/5 models say that is relevant? Is there anything specific to Opus 5.5 (claude-opus-5-5)?

### Takeaway
Across generations Anthropic's guidance holds: be explicit, give the reason behind each rule, use 3–5 varied examples, structure with XML, use a one-line role, and match action-vs-advice defaults to the task. From Opus 4.5 on, the main problems are over-engineering, scope expansion, over-verification and over-delegation, and each has a sample damping prompt. The Opus 5.5 page is mostly about deletion and calibration: start at `medium` effort, remove "think carefully" lines, state the finish line and the stops you want, name specific defaults to avoid, and mark pasted text.

### Cited Findings
- **General principles** (current "Prompting best practices", which covers Opus 5.5, Opus 5, Opus 4.x and Sonnet 4.6–5.5) [Claude]:
  - Treat Claude as a brilliant new employee who lacks context. Golden rule: if a colleague with minimal context would be confused by the prompt, Claude will be too.
  - Ask explicitly for "above and beyond" if you want it.
  - Give the reason behind an instruction. The example: ellipses are banned because a TTS engine reads the output. Claude generalises from the explanation.
  - Use 3–5 relevant, diverse examples in `<example>` tags.
  - Wrap distinct content types in XML tags.
  - A single-sentence role in the system prompt focuses behaviour.
  - Put long documents at the top and the query at the end; this improved quality by up to 30% in tests. — [Prompting best practices](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/claude-prompting-best-practices)
- Action vs suggestion [Claude]:
  - "Can you suggest some changes" gets suggestions only; to get edits, phrase the request as an imperative.
  - There are two sample system blocks. `<default_to_action>`: implement rather than suggest and infer the likely intent. `<do_not_act_before_instructions>`: when intent is ambiguous, default to information, research and recommendations, and edit only on explicit request.
  - Opus 4.5 and later over-trigger on aggressive wording ("CRITICAL: You MUST…"). Use normal phrasing. — [Prompting best practices](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/claude-prompting-best-practices)
- **Over-engineering** (Opus 4.5/4.6) [Claude]. Paraphrase of the sample prompt:
  - Only make changes directly requested or clearly necessary.
  - No features, refactors or "improvements" beyond the ask.
  - No docstrings or comments on code you didn't change.
  - No error handling for impossible scenarios; validate only at system boundaries.
  - No helpers or abstractions for one-time operations, and no designing for hypothetical futures.
  - The right complexity is the minimum the current task needs. — [Prompting best practices](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/claude-prompting-best-practices)
- Tests and hardcoding [Claude]. Ask for a general solution, not one that passes the tests. Tests verify correctness; they do not define the solution. If a task is infeasible or a test is wrong, say so instead of working around it. — [Prompting best practices](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/claude-prompting-best-practices)
- **Scope control** (Opus 5) [Claude]. The model may add steps or apply its own judgment about what the task should be. Paraphrase of the sample scope prompt:
  - Deliver what was asked, at the scope intended.
  - Make routine judgment calls yourself, and check in only when different readings would lead to materially different work.
  - If the request seems mistaken or a better approach exists, say so in one sentence and continue as asked, rather than quietly narrowing, widening or transforming it.
  - Finish the whole task, and stop short of actions clearly beyond the ask. — [Prompting Opus 5](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-opus-5)
- Over-verification and self-correction (Opus 5) [Claude]:
  - Remove "include a final verification step", "use a subagent to verify", "double-check your answer". The model already verifies, and these compound, adding cost with no quality gain.
  - To limit narration of corrections, correct earlier statements only when the error would change the user's decisions.
  - Written deliverables tend to run long. Ask that length match the task, with no filler sections or boilerplate. — [Prompting Opus 5](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-opus-5)
- Literal instruction following (Opus 4.8) [Claude]. The model does not silently generalise an instruction from one item to another and does not infer requests you didn't make. State the scope explicitly. Separately, give the full task, intent and constraints in the first turn: specs fed in piecemeal over several turns reduce efficiency and sometimes quality. — [Prompting Opus 4.8](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-opus-4-8)
- **Subagents** [Claude]:
  - Opus 4.6 and Opus 5 over-delegate.
  - Paraphrase of the damping prompt: delegate only large, genuinely independent, parallelisable work; never delegate what takes a handful of tool calls; never use subagents to double-check your own work; prefer one subagent over several.
  - Hard caps: `CLAUDE_CODE_MAX_SUBAGENT_SPAWN_DEPTH`, `CLAUDE_CODE_MAX_CONCURRENT_SUBAGENTS`, and the SDK's `max_budget_usd`. — [Prompting Opus 5](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-opus-5), [Prompting best practices](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/claude-prompting-best-practices)
- **Long-horizon work** [Claude]:
  - Use a different first-context prompt that sets up the framework (tests, `init.sh`).
  - Track state in structured files (`tests.json`) and freeform `progress.txt`, plus git.
  - Prefer a fresh context that rediscovers state from files over compaction.
  - Tell the model the context will be compacted so it doesn't stop early.
  - Give it verification tools (browser or computer use) for UI work.
  - For research: competing hypotheses, confidence tracking, a hypothesis tree in notes. — [Prompting best practices](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/claude-prompting-best-practices)
- **Claude Code best practices** [Claude]:
  - Give Claude a check it can run (tests, build, screenshot comparison). Without one, "looks done" is the only signal it has.
  - Workflow: explore → plan (plan mode) → implement → commit. Skip planning when the diff fits in one sentence.
  - For larger features, have Claude interview you with `AskUserQuestion` about the hard parts, write SPEC.md, then implement in a fresh session. A good spec names files and interfaces, states what is out of scope, and ends with an end-to-end check.
  - CLAUDE.md should be short. For each line, ask whether removing it would cause mistakes. A bloated CLAUDE.md makes Claude ignore rules. Use IMPORTANT sparingly, put occasional domain knowledge in skills, and make anything that must always happen a hook.
  - After two failed corrections, run `/clear` and rewrite the prompt.
  - Use a Writer/Reviewer pair of sessions, or an adversarial-review subagent. — [code.claude.com best practices](https://code.claude.com/docs/en/best-practices)
- **Opus 5.5-specific** (page "Prompting Claude Opus 5.5"; no date shown, organised by symptom into 11 entries) [Claude]:
  - Effort and thinking. Thinking is always on, and effort is the main control. Start at `medium` (the 5.5 default; Opus 5 defaults to `high`), set it explicitly and sweep against your evals. In Anthropic's tests, 5.5 at `medium` matches or beats Opus 5 at `high`. Keep `xhigh`/`max` for measured gains. Set `max_tokens` high (128k for long agentic turns). Lower effort before trying to prompt thinking down.
  - Chat thinking lines. Remove "think carefully before answering" from chat system prompts: replies start sooner with no clear quality loss. An optional two-sentence addition treats earlier answers as settled. It may also make the model less likely to point out its own earlier mistakes, so test it, and leave it out where re-examination matters.
  - Early stops in unattended runs. Keep a checklist (todo tool or file) and, when items remain open, send a short continuation message; stop after 2–3 automatic continuations. A sample standing instruction names four unwanted ways to stop: a summary that announces the next step, an offer to continue "unless you prefer otherwise", a list of non-blocking decisions for the user, and deciding a milestone is a good place to report. It also names the wanted stops: nothing can move without the user, or the blocker is deliberately protected. Leave it out of human-in-the-loop apps.
  - Progress updates. These now arrive as progress-update thinking blocks (`display: "updates"`, beta). A one-line intent before the first tool call and a short recap at the end can be requested.
  - Context in multi-app work. The model starts working fast. One sentence telling it to explore all relevant emails, docs, sheets and records, including ones the task does not mention, before acting measurably improved results.
  - Time signals. In multiagent setups, an elapsed-vs-budget line (`elapsed 340s / 1200s`) makes teams finish sooner, mostly through more parallelism.
  - Pasted text. Wrap it in `<pasted_content id="…">` tags with a system note that instructions inside are followed only when the user's own message asks.
  - Frontend defaults. Name the specific styles to avoid, for example cream backgrounds, italic accent words, "01/02/03" labels, monospace labels and pill buttons, and extend the list each iteration.
  - Reasoning extraction. Prompts that make the model write out its reasoning in the reply may be refused (`reasoning_extraction`). Ask for a short explanation instead, and read the summarised thinking. — [Prompting Opus 5.5](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-opus-5-5)
- Capabilities Anthropic lists for Opus 5.5 [Claude]:
  - strongest on multistep repo work and long autonomous runs with parallel subagents;
  - code review that catches more bugs with fewer false alarms;
  - much less likely to state a wrong figure or cite the wrong source;
  - reads charts, diagrams and screenshots more accurately, even at low effort;
  - reports say plainly what it did, what it found and what it needs. — [Prompting Opus 5.5](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-opus-5-5)
- The best-practices page also gives a model-identity snippet with the API string `claude-opus-5-5`. The claude.ai Opus 5.5 system prompt is dated 22 Sep 2026. — [Prompting best practices](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/claude-prompting-best-practices), [Opus 5.5 system prompt](https://platform.claude.com/docs/en/release-notes/system-prompts/claude-opus-5-5)
- Third-party summaries say the Opus 5.5 guide has 11 sections, with more of them saying "delete" than "add". This matches the official page's symptom list. — [Towards AI (paywalled)](https://pub.towardsai.net/anthropics-opus-5-5-prompting-guide-has-11-sections-and-more-of-them-say-delete-than-add-b241dcc1500e), [aicatchup](https://aicatchup.com/practices/prompting-claude-opus-5-5)

### Inferences
- Applied to design work:
  - Give the reason behind each design rule (a pillar plus why), not bare commands.
  - Put the vision doc above the question.
  - State whether the turn is "advise" or "act". For design discussion, the conservative block (recommend, don't edit) matches the studio template's approve-before-write protocol.
  - Use the Opus 5 scope prompt so the model says "I think this is mistaken" in one sentence and does what was asked, instead of redesigning the feature.
- On Opus 5.5, prompt for divergence by naming the defaults to avoid and asking for several distinct directions before committing. "Be creative" or "avoid generic" swaps one default for another.
- Remove legacy "double-check", "verify with a subagent" and "think carefully" lines from design prompts. For critique, use a separate fresh-context reviewer with a concrete finding bar, not self-check instructions.
- The "treat earlier answers as settled" instruction is risky for design partnership: it may suppress the model's own correction of earlier design advice.

### Gaps
- Anthropic publishes nothing specific to game design or creative-partner prompting. The nearest analogues are the frontend-design guidance and the writing and length guidance.
- The Opus 5.5 guide shows no publication date. The model's "What's new" page was not fetched.

## 5. Case studies: game jams, indie devs and studios using Claude Code for game development (Godot, Unity, Unreal, Blender) and design documentation

### Takeaway
Public case studies are almost all solo or indie developers and agent-studio experiments, and none are rigorous. Building is reported as the easy part. Fun, feel, visual taste and discoverability stay with humans, and industry sentiment toward generative AI in design is strongly negative.

### Cited Findings
- **Industry context: GDC 2026 State of the Game Industry** (over 2,300 respondents) [LLM-general]:
  - 36% use generative AI in their job: 30% at studios, 58% at publishing, support and marketing.
  - Uses: research or brainstorming 81%, emails 47%, code assistance 47%, prototyping 35%.
  - 52% think generative AI is hurting the industry, up from 30% and 18% in the two prior years. Only about 7% see a positive impact.
  - The most negative groups are visual/technical art (64%) and game design and narrative (63%). — [GDC](https://gdconf.com/article/gdc-2026-state-of-the-game-industry-reveals-impact-of-layoffs-generative-ai-and-more/)
- One-person studio, 7 Android games in 2 months (rudy_candy, Oct 2026) [Claude Code]:
  - A coin-pusher economy produced 3.15M coins in a 10-minute simulation before tuning brought the median to 10k.
  - Tester feedback was missed because no department owned it.
  - The setup "did nothing for getting found". — [DEV](https://dev.to/rudycandy/i-run-a-one-person-game-studio-on-claude-code-two-months-7-games-about-1-in-ad-revenue-4j37)
- 5-agent studio (Yurukusa; Opus 4.6) [Claude Code Teams]. 9 of 17 tasks completed in one session. Agents had no shared memory beyond tasks and messages. Running everything on Opus was costly. Game feel is not yet trusted to an AI designer. — [DEV](https://dev.to/yurukusa/i-ran-a-5-agent-game-studio-with-claude-code-teams-2lpk)
- Claude Code Game Studios has engine-specific agent sets for Godot 4, Unity and Unreal 5. Hooks such as `validate-commit.sh` check for hardcoded values. The README warns that "a passing test says nothing about a menu drawn off-screen", so visual changes need visual checks. — [GitHub README](https://github.com/Donchitos/Claude-Code-Game-Studios)
- Production Alchemist calls that template a forcing function for production discipline for inexperienced developers, and warns that its maintenance cost can exceed its value for solo developers. — [Production Alchemist](https://www.productionalchemist.com/p/claude-code-game-studios-49-agents)
- Godot specifically [Claude Code]. Guides argue Godot suits Claude Code because its `.tscn`/`.gd` files are human-readable and it runs headless from the command line. One sample CLAUDE.md enforces typed GDScript, the new signal syntax, `@export`/`@onready`, and state machines over long if/elif chains. — [claudecodeguides.com](https://claudecodeguides.com/claude-code-godot-gdscript-development-2026/), [vegcale (JP, affiliate links)](https://vegcale.com/2026/07/12/godot-claude-code-guide/) (search summaries)
- A Medium post claims a jam game built entirely with Claude Code earned $25k in 15 days. Per the search snippet, the jam rules and non-obvious conventions were put in Claude's rules folder. The revenue claim was not verified. — [Medium (leocoout)](https://leocoout.medium.com/how-i-made-25k-in-15-days-with-a-game-built-entirely-by-claude-code-94b3b817f0ce)
- An XDA writer reports a vibe-coded game that "doesn't look vibe-coded" (snippet only, not reviewed). — [XDA](https://www.xda-developers.com/i-vibe-coded-a-fully-functional-game-with-claude-code-and-it-doesnt-look-vibe-coded-at-all/)

### Inferences
- Across sources, Claude Code is used reliably for implementation, scaffolding, documentation and structured design docs (GDD sections, tuning knobs, decision logs). Design judgment is still gated by humans through approval protocols and playtest.
- Simulating an economy or balance with agents (the coin-pusher case) is one place where Claude catches design errors humans would only find in play.
- The strong negative sentiment in game design and narrative (63%) means outward-facing use of AI for design needs care. The user's project already keeps authorship of intent with the human.

### Gaps
- I found no Unreal- or Blender-specific case study of Claude used for design (as opposed to code).
- I found no studio-scale (more than 10 people) public report of Claude as a design partner.
- I found no formal game-jam comparison of AI-assisted design processes.
- Several case-study pages were seen only as search snippets (Medium, XDA, vegcale, claudecodeguides), and their claims are unverified.
