# Art bibles, world bibles and consistent AI-generated game art

Research date: 2026-10-21. Scope: what professional art and world bibles contain; how people write them with LLMs and turn them into prompts; techniques that keep AI image generation consistent (style blocks, references, edit chains, seeds, tileable textures, sets from one master); failure modes; LLMs as art directors.

Source quality: many pages were reached only through search-engine summaries, because the direct fetch was blocked (403/404) for domusweb, techspot, polycount and tripo3d. Those findings are marked "(via search summary)". Vendor blogs (Scenario, Layer, Seeles, Tripo, Makko, Inkration) are marketing material, and their numbers are self-reported.

## 1. What a professional art bible / environment (world) bible contains, and at what granularity

### Takeaway
No industry standard exists. Practice converges on a short identity core: a one-line visual rule, principles with tests, mood, shape language and colour system. Production chapters follow: characters, environments, props/modular kits, UI, VFX, asset standards and texel density, references with "take / avoid", and prohibitions. Good environment bibles tie the look of each area to who lives there and to the history behind it. Cyberpunk 2077's four architectural styles map to social class and to a timeline. Ghost of Tsushima puts mood in sky and light, and makes wind a navigation tool.

### Cited Findings
**Typical section lists (practitioner sources)**
- Forum lists of art-bible sections:
  - an introduction with the visual intent;
  - characters;
  - environments ("description of the environmental intent and environment concepts");
  - storyboards and UI;
  - camera, which shows intended camera angles with environment and character mock-ups;
  - references.

  A prop-heavy game adds a props section, and a game with colour customisation gets its own section. Other posters ask for mood boards, a list of every required graphic with counts (2D objects, 3D models, environments), and approved reference sheets with cropped key assets. — [gamedev.net: sections for an art bible](https://comic.gamedev.net/forums/topic/671906-sections-for-an-art-bible); [gamedev.net: what is an art bible](https://gamedev.net/forums/topic/644435-what-is-an-art-bible-and-any-tips-to-make-one-better/) (via search summary; which thread said what is blended in the summary)
- Tom Sloper's definition: the style section tells artists whether the game is "cartoony or gothic or future shiny". The bible also carries the full graphics list and pipeline details: file formats and naming conventions. — [gamedev.net: creating art bible/style guide](https://gamedev.net/forums/topic/552212-creating-art-biblestyle-guide/4955332/) (via search summary)
- An environment production guide splits environment art into backgrounds, props and set dressing, architecture, terrain, modular kits, and textures and materials. Modular kits are reusable building parts, made to be combined in many ways: walls and structural parts, ground pieces, major surface elements and large set-dressing elements. — [RocketBrush](https://rocketbrush.com/blog/game-environment-art-types-workflow-and-production-guide) (via search summary)

**A worked, LLM-oriented template: the "art-bible" skill in Claude Code Game Studios**
- The skill writes `design/art/art-bible.md` in nine sections:
  1. **Visual Identity Statement:** "a one-line visual rule plus 2–3 principles, each with a design test".
  2. **Mood & Atmosphere:** emotional target, lighting character, 3–5 descriptors, energy level per game state.
  3. **Shape Language:** silhouette readability, environment geometry, UI shape grammar, hero versus supporting shapes.
  4. **Color System:** 5–7 primary colours with meanings, semantic usage, temperature per area, UI palette, colour-blind backup cues.
  5. **Character Design Direction:** archetypes, distinguishing features, expression and pose targets, LOD philosophy.
  6. **Environment Design Language:** architecture, texture approach, prop density, environmental storytelling.
  7. **UI/HUD Visual Direction.**
  8. **Asset Standards:** formats, naming, texture tiers, LOD, engine constraints.
  9. **Reference Direction:** 3–5 references, "each with a specific element to draw from and something to avoid". The references should not all point in the same direction.

  The document is framed as a constraint document that "narrows the solution space productively". — [SKILL.md (raw)](https://raw.githubusercontent.com/Donchitos/Claude-Code-Game-Studios/main/.claude/skills/art-bible/SKILL.md)
- The environment section asks for these points:
  - the architectural style and how it relates to the world's culture and history;
  - the texture philosophy: "painted vs. PBR vs. stylized — why this choice for this game?";
  - prop density rules: "sparse/dense — what drives the choice per area type?";
  - environmental storytelling: "what visual details should tell the story without text?".

  — [skillselion mirror of the skill](https://skillselion.com/skills/donchitos/claude-code-game-studios/art-bible)
- Conflicting versions: a registry listing of the same skill names other sections: Color Palette; Lighting & Atmosphere; Character Art Direction; Environment & Level Art; UI Visual Language; VFX & Particle Style; Asset Standards; **Style Prohibitions**. Its mood section asks for "lighting character (time of day, color temperature, contrast level)" per game state. This is probably an older or newer revision. — [tessl registry](https://tessl.io/registry/skills/github/Donchitos/Claude-Code-Game-Studios/art-bible) (via search summary), contrasted with the [raw SKILL.md](https://raw.githubusercontent.com/Donchitos/Claude-Code-Game-Studios/main/.claude/skills/art-bible/SKILL.md)

**District-level granularity: Cyberpunk 2077 (CD Projekt RED)**
- Night City has four visual styles arranged as a timeline, so that the city feels real. Concept artist Marthe Jonkers said the timeline was added to "overlap that 50-year gap from Cyberpunk 2020". The styles are mixed, so travellers see architecture from different eras. Kitsch is "really happy because at the time people were having a good time", with pink and yellow palettes and rounded forms. Neomilitarism is "very corporate". — [VGC](https://videogameschronicle.com/news/new-cyberpunk-2077-trailers-showcase-the-games-vehicles-and-night-citys-four-visual-styles)
- Each style maps to a social class and a district (via search summary):
  - **Entropism:** "Necessity over style". Poor districts use cheap, weathered materials such as old concrete. It grew out of war and financial collapse.
  - **Kitsch:** a reaction to Entropism. Bright, glossy surfaces, form before function, for a somewhat richer layer that shows off cyberware.
  - **Neomilitarism:** the megacorporations' style in the city centre. Monolithic and minimalist, with clean metal, smooth glass and darker tones; the closest to Blade Runner.
  - **Neokitsch:** the ultra-rich. Rare materials such as gold and leather, plus natural and animal-print elements, because animals are extinct in that future.

  — [domusweb](https://domusweb.it/en/architecture/gallery/2020/12/21/night-city-how-the-cyberpunk-2077s-megalopolis-was-built.html); [gamereactor](https://ipv6.gamereactor.eu/cities-of-the-future-art-design-in-cyberpunk-2077/); [Megascans blog](https://blog.megascans.se/?p=3671)

**Mood and light as bible content: Ghost of Tsushima (Sucker Punch)**
- Art director Jason Connell: "90% of the colour and mood and emotion comes from your upper hemisphere", meaning the sky and lighting. — [Red Bull interview](https://redbull.com/us-en/ghost-of-tsushima-art-director-interview-jason-connell) (via search summary)
- The palette was chosen to show how beautiful Japan is, not a war-torn look, so that the scenery contrasts with the combat. — [DualShockers](https://www.dualshockers.com/ghost-tsushima-art-director/) (via search summary)
- Wind was both a visual and a gameplay rule, "a huge gameplay affordance to guide you through the island". SIGGRAPH 2021 course notes describe a "Kurosawa Mode" for dramatic lighting, wind, clouds, haze and fog. — [Red Bull](https://redbull.com/us-en/ghost-of-tsushima-art-director-interview-jason-connell); [SIGGRAPH 2021 Advances, J. Patry](https://www.advances.realtimerendering.com/s2021/jpatry_advances2021/index.html) (via search summary)

**Asset standards inside the bible: texel density**
- Maxon gives example densities by camera: first person 10.24 px/cm (1024 px per metre), third person 5.12, top-down 2.56 or lower. The closer the camera can get, the higher the density must be. — [Maxon C4D docs](https://help.maxon.net/c4d/2025/en-us/Content/html/Texel_Density.html)
- Studios often set this guideline early, for example 5.12 px/cm for background props and 10.24 px/cm for hero assets. — [RebusFarm](https://rebusfarm.net/blog/texel-density-basics-every-artist-should-know) (via search summary)

### Inferences
- A per-district world bible entry can be assembled from these sources. Synthesis, not a quoted template:
  - **Who lives here and why it looks this way:** class, trade, history, and the "era layer" as in Cyberpunk's timeline.
  - **Architecture:** construction logic, typical silhouettes, the shape-language rule.
  - **Materials palette:** the 3–6 materials allowed, each with real-world scale and allowed wear.
  - **Wear and dirt logic:** where dirt sits and why (water, smoke, foot traffic).
  - **Colour:** hex anchors, temperature, value range.
  - **Light:** sky, time of day, where the mood comes from, as Connell puts it.
  - **Signature props and details:** 3–5 items found only here.
  - **Prop density.**
  - **Do / Don't list.**
  - **Reference set:** 3–5 references, each with "take / avoid".
- The "design test" attached to each principle, and "references with something to avoid", are the most transferable ideas for LLM use. They make the bible checkable instead of merely descriptive.

### Gaps
- I found no complete public AAA art bible PDF with district-level pages. Real bibles are under NDA. The district detail above is reconstructed from interviews, not from the document itself.
- I could not fetch GDC Vault talks (Ghost of Tsushima world building, Dishonored 2 Karnaca). Slides were not retrieved.
- No source gave a meters-per-tile convention for trim sheets.

## 2. Writing a bible with LLMs and using it as the source of truth for prompts; techniques that keep a large AI-generated set consistent

### Takeaway
Prompt craft alone does not hold a large set together. The working pattern has several layers:
1. A bible written once.
2. A fixed style block, or a machine-readable "style container", reused verbatim.
3. Each prompt anchored to bible sections.
4. Reference images with assigned roles. Edit chains that restate what must not change.
5. A trained style model (LoRA or vendor style training on 10–25 curated images) for big sets.
6. Human review gates.

Seeds help little. References and fine-tunes carry the consistency.

### Cited Findings
**Bible → per-asset spec → prompt (LLM workflow)**
- The Claude Code Game Studios `asset-spec` skill turns the bible into per-asset specs. Each spec has a table (Category, Dimensions, Format, Naming, Polycount, Texture Res). Texture resolution must match a bible §8 tier, and the technical-artist agent is told not to invent new sizes. Each spec also has:
  - a 2–3 sentence visual description;
  - an "Art Bible Anchors" list that cites sections, for example §3 Shape Language and §4 Color System;
  - a generation prompt with style keywords, composition notes, colour palette anchors, lighting direction and negative prompts;
  - a status.

  The rules:
  - A spec that contradicts the bible is wrong.
  - A description must be specific enough that two artists would produce consistent results.
  - Conflicts between the art director and the technical artist are surfaced to the user, not resolved silently.
  - Asset IDs stay stable across re-runs.
  - Shared assets are referenced, not duplicated.

  — [asset-spec SKILL.md](https://raw.githubusercontent.com/Donchitos/Claude-Code-Game-Studios/main/.claude/skills/asset-spec/SKILL.md)
- Adobe Design describes an LLM workflow for briefs:
  - Use the LLM as a drafting partner, not a finished-prompt generator.
  - Start from "one to three somewhat similar images".
  - Ask the LLM to "Translate the image into a JSON context profile" (lighting, mood, textures, composition).
  - Save it as a reusable "style container" that is not tied to one subject, and swap the subject.
  - Use hex colour codes.
  - Keep the core prompt stable and change one variable at a time.
  - Test the same prompt across models, because each "preserves the core elements of the prompt with a slightly different interpretation".

  — [Adobe Design: Creative direction](https://adobe.design/ideas/creative-direction-the-secret-to-great-ai-images)

**OpenAI GPT Image (official guide, current as of fetch)**
- **Prompt structure:** for complex requests, use labelled sections such as "Scene:", "Subject:", "Style:", "Mood:", "Constraints:". Suggested order:
  1. the result (subject, use, composition, aspect ratio);
  2. visible details (materials, lighting, medium);
  3. people and actions;
  4. exact text;
  5. constraints.

  Any format works (paragraphs, JSON-like, tags); "choose the one easiest to update". — [OpenAI image prompting guide](https://developers.openai.com/api/docs/guides/image-prompting.md)
- **Consistency:**
  - Pass generated outputs back in as inputs.
  - Restate constraints ("Do not redesign the character", plus the features to keep).
  - "Same style as before" can carry context, but critical constraints should be restated if results drift.

  — [OpenAI guide](https://developers.openai.com/api/docs/guides/image-prompting.md)
- **Multi-image input:** number each input and give it a role (subject, style, clothing, background), for example "Place the dog from the second image into the setting of image 1". Style-transfer example: "Use the same style from the input image and generate a man riding a motorcycle on a white background." — [OpenAI guide](https://developers.openai.com/api/docs/guides/image-prompting.md)
- **Preservation:**
  - Name what to keep: identity, geometry, layout, lighting, camera angle, surrounding objects.
  - Useful fragments: "Do not change anything else." / "Preserve camera angle, room lighting, floor shadows, and surrounding objects." / "Do not add new elements or text."
  - Repeated edits drift, so restate constraints and inspect each result.
  - If a region must stay pixel-identical, composite the approved edit into the original instead of relying on the prompt.
  - Test the whole edit sequence, not only single steps.

  — [OpenAI guide](https://developers.openai.com/api/docs/guides/image-prompting.md)
- **`input_fidelity`:** on gpt-image-1 and 1.5 it is `low` or `high`; high preserves input detail and costs more input tokens. On gpt-image-2 the parameter is omitted, because inputs are always processed at high fidelity. — [OpenAI guide](https://developers.openai.com/api/docs/guides/image-prompting.md)
- **Transparent cut-outs:**
  - Set `background="transparent"` with PNG or WebP.
  - Add "Do not add a solid backdrop, checkerboard, scenery, or shadow."
  - Verify that a real alpha channel exists ("a drawn checkerboard is not transparency").
  - Check edges: hair, glass, outlines.

  — [OpenAI guide](https://developers.openai.com/api/docs/guides/image-prompting.md)
- **Model lifecycle:** gpt-image-1 is deprecated with shutdown on **2026-10-23**, and gpt-image-1.5 with shutdown on 2026-12-01. Current models are gpt-image-2 and gpt-image-2.5 "flare" (fast) and "sunburst" (quality). Custom sizes go up to 3,840 px per edge, multiples of 16, aspect ratio at most 3:1. — [OpenAI guide](https://developers.openai.com/api/docs/guides/image-prompting.md)
- On GPT Image 2 inside Scenario: mention each reference image by number in the prompt, and re-upload the character reference for every generation to keep the character consistent. — [Scenario help (via search summary)](https://help.scenario.com/articles/8473821828-gpt-image-2-the-essentials)

**Google Gemini image models ("Nano Banana")**
- "Multi-turn conversation is the recommended way to iterate on images". Chain edits with `previous_interaction_id` instead of starting over.
- Example of naming what is fixed: "Update this infographic to be in Spanish. Do not change any other elements of the image."
- Reference limits: Nano Banana 2.1 and 3.1 Flash take up to 10 object images and 5 character images. Nano Banana Pro takes up to 6 object images and 3 style references. Nano Banana 2 Lite is "Not optimized for multiple reference inputs or multi-turn sequential editing."

— [Gemini API image generation docs](https://ai.google.dev/gemini-api/docs/image-generation)

**Midjourney (official docs)**
- `--sref` captures "the visual vibe of an existing image. They don't copy objects or people". Several references can be passed, with individual weights.
- `--sw` runs from 0 to 1000, default 100. `--sv` picks the sref model: in V7, `--sv 6` is the default and `--sv 4` is the old pre-2025-06-16 model.
- The Style Creator makes reusable style codes from images you pick and reject. Moodboards cannot be combined with `--sw` or `--sv`.

— [MJ Style Reference](https://docs.midjourney.com/hc/en-us/articles/32180011136653-Style-Reference); [MJ Style Creator](https://docs.midjourney.com/hc/en-us/articles/41308374558221-Style-Creator); [MJ Moodboards](https://docs.midjourney.com/hc/en-us/articles/39193335040013-Moodboards) (via search summary)
- Practitioner advice:
  - Seeds give little control and are easily overridden by other parameters.
  - Weighted multi-sref syntax: `--sref urlA::2 urlB::3`.
  - Keep the prompt simple and drop style words, so the reference drives the look.

  — [Ruzuku](https://www.ruzuku.com/learn/tools/ai/midjourney-style-consistency); [Atypical Co. substack](https://atypicalco.substack.com/p/midjourney-a-guide-to-style-references) (via search summary; which tip comes from which guide is not separated)

**Trained style models (Scenario, Layer, SD/FLUX)**
- Scenario style training uses a dataset of 10–25 images, at 1024 px or more. Curation is the biggest factor: review the images for traits that stay consistent (colour, composition, line work). Flux is described as best for consistent style across assets; SDXL needs accurate captions. The API "game asset pipeline" pattern uploads 10–30 reference images of the game's style and trains. — [Scenario: inventory-style training](https://www.scenario.gg/post/training-custom-inventory-style-ai-models-for-enhanced-game-development); [Scenario: style LoRA](https://help.scenario.com/en/articles/training-a-style-lora); [Scenario API docs](https://docs.scenario.com/get-started/documentation/integrating-with-applications-real-world-scenarios) (via search summary)
- Layer case studies:
  - Games United trained **separate styles per category** (bags, currency, objects, terrain).
  - From a small starting set, they generated more compatible assets and fed them back into training (bootstrapping).
  - LBC Studios reports 10 style-consistent characters plus vehicles, icons and props in a week (self-reported).

  — [Layer: Games United](https://www.layer.ai/case-study/games-united); [Layer: LBC Studios](https://www.layer.ai/case-study/lbc-studios) (via search summary)
- Layer's own guide calls prompt engineering "Unreliable at scale. Depends on individual skill". It says LoRA "Improves style anchoring, but does not solve workflow governance". It separates *style* consistency (matching the look) from *IP* consistency (world, lore, colour systems, rendering logic, production constraints), and recommends central libraries, team templates, fixed generation parameters, permissions and versioning. — [Layer blog](https://layer.ai/blog/ip-consistency-ai-generation-guide-game-studios)
- Division of labour in SD/FLUX pipelines:
  - **LoRA** fixes a style learned from your own images; suited to a tileset or an enemy roster.
  - **IP-Adapter** carries a look over from 1–2 references.
  - **ControlNet** locks pose, silhouette or layout.
  - Most consistent pipelines combine two of the three.

  — [Playgama blog](https://playgama.com/blog/?p=15181) (via search summary). Scenario: IP-Adapters can produce assets in a project's style "allowing for the use of a single token to add style-consistent variations". — [Scenario IP-Adapter](https://Scenario.gg/features/ip-adapter) (via search summary). Canny ControlNet plus a style LoRA keeps asset structure while the style changes. — [Runware guide](https://runware.ai/docs/guides/game-assets-canny) (via search summary)
- A six-step ComfyUI asset pipeline from a skill catalogue: style reference → ControlNet → IP-Adapter → batch → background removal → engine export. It advises captioning LoRA data with rare tokens and having 12 GB+ VRAM. — [skillselion: ai-game-art-generation](https://skillselion.com/skills/omer-metin/skills-for-antigravity/ai-game-art-generation) (via search summary)
- Consistency tip: generate the whole set in one grid first, then split it. — [Midjourney third-party guides](https://atypicalco.substack.com/p/midjourney-a-guide-to-style-references) (via search summary; attribution not certain)

### Inferences
- A practical prompt template, synthesised from the OpenAI section order, the asset-spec fields and Adobe's style container (not quoted from any one source):
  ```
  [ASSET] <id + name>, for <use: building facade / prop / tile>, <aspect, framing, camera>
  [WORLD] <district entry: who lives here, era, why it looks this way>  (from bible §Env, cited)
  [STYLE BLOCK — paste verbatim, never paraphrase] <one-line visual rule; medium; brushwork/realism level; value range; palette hex anchors; light>
  [MATERIALS] <only the district's allowed materials, with scale and wear>
  [REFERENCES] Image 1 = style/palette; Image 2 = construction/silhouette; Image 3 = previous approved asset (do not change its style)
  [CONSTRAINTS] Do not add text, logos, people. Do not change <list>. Avoid: <bible prohibitions>.
  ```
- For a set made from one master concept: approve a master, then derive every variant by an edit of that master (or of the previous approved image), with numbered roles and an explicit "keep" list. Restart the chain from the master when drift appears; the OpenAI guide warns that repeated edits drift. Composite when pixels must match.
- The most "source-of-truth" behaviour comes from two things: pasting the style block verbatim, and citing bible sections in every spec, as asset-spec does. Then a prompt that contradicts the bible can be detected mechanically.
- For gpt-image users: no seed is documented in OpenAI's guide (see Gaps), so reproducibility must come from references and fixed text blocks.

### Gaps
- I could not confirm from OpenAI docs whether GPT Image models accept a seed. Third-party pages claim there is none, but the attribution was unclear in the search summary.
- No primary studio source (GDC talk or postmortem) showed a full bible-to-prompt pipeline in production. The evidence is vendor case studies and open-source skill repos.
- I did not reach Google's official "how to prompt Gemini image" templates. A third-party page claims they exist ([pasqualepillitteri](https://pasqualepillitteri.it/en/news/856/nano-banana-prompt-official-google-guide)).
- Leonardo's consistency features (Elements, Style Reference) were not researched for lack of budget.

## 3. Textures: tileable/seamless, trim sheets, modular panels, positional dirt, real-world scale

### Takeaway
AI tools can now produce seamless top-down tiles and full PBR sets: Scenario Texture and PATINA, SD circular padding, Tiled Diffusion, and Ubisoft's CHORD. Studios still treat the results as prototype or placeholder quality. Classic techniques solve repetition and scale, not the generator:
- low-contrast base tiles without stand-out features;
- dirt and damage moved to decals, vertex-blend masks and macro layers;
- a fixed texel density (for example 10.24 px/cm for first person).

I found no AI-specific method for trim sheets.

### Cited Findings
**AI tile generation**
- **Scenario Texture:**
  - Adds tileable framing automatically and outputs flat, top-down repeatable materials.
  - An optional "Erase Seam" pass works on both axes, but it is a post-process and won't fix a poor base generation.
  - Outputs go up to 4K; 2048×2048 is suggested for hero assets.
  - A prompt like "a cobblestone street at night with a streetlamp" is a poor fit, because the model makes flat tiles, not perspective renders.
  - Add "seamless tileable texture" only if seams or drift appear.
  - Custom texture models train on 5–20 square textures with neutral lighting; Flux 2 is the recommended base.

  — [Scenario Texture essentials](https://help.scenario.com/articles/3312894507-scenario-texture-the-essentials); [Train a custom texture model](https://help.scenario.com/articles/2574453096-train-a-custom-texture-model) (via search summary)
- Scenario example prompts, verbatim from its basics page:
  - "An ancient stone wall, composed of large, timeworn limestone blocks arranged in a sturdy formation… displaying erosion with gray and sandy tones, with moss between the stones";
  - "A weathered fence texture featuring rough, splintered wood in faded tan and gray hues."

  Other guidance on the page:
  - "Keep prompts specific but concise".
  - A tiling control sets how often the texture repeats.
  - Test the result in a 3D viewer under different lighting environments.
  - Generate a full PBR set (height, normal, metallic, edge, AO); a height map may need inverting.
  - Flux defaults to 28 steps and guidance 3.5; SDXL guidance stays between 6 and 12.

  — [Scenario texture generation basics](https://help.scenario.com/articles/5486933912-texture-generation-basics)
- PATINA (via the Scenario API) makes seamlessly tiling PBR materials from text: base colour, normal, roughness, metalness, height. It supports img2img variations and inpainting, a tiling mode (both, horizontal or vertical) and up to 8K. — [Scenario docs: Patina](https://docs.scenario.com/get-started/generation/third-party-model-generation/third-party-model-generation-patina-ai/) (via search summary)
- SD "asymmetric tiling" replaces standard padding with circular padding, so outputs tile by design. Tiling can soften detail at large sizes, so upscaling follows. — [Aiarty SD tiling guide](https://aiarty.com/stable-diffusion-guide/stable-diffusion-tiling.htm); [How-To Geek](https://www.howtogeek.com/832164/this-stable-diffusion-fork-can-generate-tiled-images/) (via search summary)
- Tiled Diffusion (arXiv 2412.15185) generates a set of tiles that join in any order and quantity, with any latent diffusion model. This is useful for large surfaces and against obvious repetition. — [arXiv 2412.15185](https://arxiv.org/html/2412.15185v4) (via search summary)
- **Ubisoft La Forge CHORD** (SIGGRAPH Asia 2025): material estimation from *generated* texture images. Its ComfyUI workflow chains texture generation → PBR map estimation → upscaling. It is open-sourced under a research-only licence, and its use is mainly prototyping and placeholders. Ubisoft reports it beats other methods on most maps by PSNR and LPIPS, but its artists say the output is not production-ready. Ubisoft chose ComfyUI for ControlNets, image guidance and inpainting. — [Ubisoft La Forge](https://www.ubisoft.com/en-us/studio/laforge/news/1i3YOvQX2iArLlScBPqBZs/generative-base-material-an-opensource-prototype-for-pbr-material-estimation-debuting-at-siggraph-asia-2025); [Comfy blog](https://blog.comfy.org/p/ubisoft-open-sources-the-chord-model) (via search summary)
- Input for img-to-material workflows: a clean overhead shot of a flat surface, without strong shadows or perspective. Make it seamless first, then estimate the maps. The base colour should carry colour with no lighting baked in. — [Floyo workflow](https://floyo.ai/workflows/seamless-pbr-texture-workflow--bd5zcgc81o0f); [UEFN texture generator](https://uefncentral.com/uefn-textures) (via search summary)

**Repetition, positional dirt and scale (classic practice)**
- Polycount advice for big walls:
  - a low-frequency macro layer blended add/subtract;
  - decals;
  - two materials mixed by a vertex-colour mask;
  - shifted or mirrored UVs;
  - non-square texel sizes.

  The bigger problem is "a dark, repeating feature that stands out", so balance a tile's visually strong areas across its quadrants. Another thread adds small scattered geometry, UV shifting, flips and rotations, and macro derivative maps. For roads: a plain tiling base plus decals for cracks and potholes. — [Polycount: avoiding repetition on big walls](https://polycount.com/discussion/comment/2689608/); [Polycount threads](https://polycount.com/discussion/comment/2689496) (via search summary; fetch was 403)
- On scale: author a floor-tile pattern so that tiling it N times lands on the intended real tile count, and adjust texel density to a real-world pattern. Slight density differences across environment materials are rarely noticed. One practitioner sets a wall tile so that 1 m of wall gets 10.24 px/cm. — [Polycount texel density discussion](https://polycount.com/discussion/comment/2787699) (via search summary)
- First-person target 10.24 px/cm, third-person 5.12, top-down 2.56. — [Maxon docs](https://help.maxon.net/c4d/2025/en-us/Content/html/Texel_Density.html)

### Inferences
- A texture prompt should fix three things the generator otherwise invents: (a) a flat, orthographic, shadow-free surface (Scenario's flat-tile framing; floyo/UEFN "no baked lighting"); (b) the real size the tile covers, for example "one tile = 2 × 2 m, blocks ~40 cm"; (c) low feature contrast, with no single stain, crack or stone that stands out. Dirt, leaks and moss belong in decals or vertex-blend layers placed by world logic (the district's wear rules), not in the tile. Positional dirt baked into a tile repeats on every copy.
- Size the tile from texel density. At 10.24 px/cm, a 2048 px tile covers 2 m. Choosing the tile's real size first makes the prompt's "block size" concrete and checkable.
- Trim sheets and modular panels: no AI tool was found that understands strip layouts. A plausible approach is to generate each strip (for example a plank edge, a stone course or a moulding) as its own seamless horizontal tile (PATINA's horizontal-only tiling mode fits), then pack the strips into a trim atlas by hand or script. This is untested.

### Gaps
- No source found on AI-generated trim sheets, or on modular wall-panel kits generated as a coherent set.
- No measured comparison of seam quality or style drift across texture generators. Vendor pages claim seamlessness without evidence.
- I did not verify texture prompt fragments such as "orthographic, no horizon" on the Summer Engine pages. The fetched page held no prompt.

## 4. Failure modes of AI art pipelines in games and how studios mitigate them

### Takeaway
Recurring failures:
- style drift as the set grows, toward "cartoonish" or "semi-real";
- the "fine but basic" generic look;
- inconsistency between assets made with different models or references;
- drift along edit chains;
- AI placeholders that ship by accident.

Public backlash in 2025–26 made this a reputational risk on top of the quality risk: Expedition 33, Black Ops 7, Anno 117 and Larian. Mitigations are mostly process: approved references, explicit rules with prohibitions, trained style models, small review batches with art-director gates, checklists, and QA for placeholders.

### Cited Findings
**Drift and the generic look**
- Even with precise prompts, outputs can drift toward too much cartoonishness or toward semi-realism. — [QuData](https://qudata.com/en/blog/casual-art-without-illusions/) (via search summary)
- Reliable consistency comes from a production system: approved reference assets, explicit visual rules, reference-conditioned generation, small review batches and a shared quality checklist. Without them, drift grows with asset count. — [Seeles practical test](https://www.seeles.ai/resources/blogs/ai-game-visual-consistency-across-assets) (via search summary, vendor)
- Causes of drift: vague prompts, inconsistent reference images, switching models, no negative examples, weak review criteria. — [Virtuall](https://virtuall.pro/blog/how-to-create-ai-game-assets-that-match-your-art-style) (via search summary)
- Without explicit craft cues (composition, lighting, materials, mood, technical details, imperfections such as grain, dust and scratches), images "often look fine but basic". "The model doesn't know what a good image looks like. You do." — [Adobe Design](https://adobe.design/ideas/creative-direction-the-secret-to-great-ai-images)
- One vendor treats the style guide as a machine-readable "living schema" for tools to ingest first: palettes, value bands, brushwork, **texture-noise ceilings**, biome and material rules, camera language. Human gates sit at key art, hero props, biome masters and faces. Automated topology, PBR, texel-density and scene-lint checks run, plus render diffs. — search-result set for "AI generated game art style drift consistency" ([Ixie](https://www.ixiegaming.com/blog/expanding-creativity-with-ai/), [skillselion art-consistency](https://skillselion.com/skills/omer-metin/skills-for-antigravity/art-consistency)); attribution between these two is unclear in the summary
- Mandated stages at one 3D studio: AI draft → art-director review against "Style Guards and Rejection Criteria" → artist refinement. Off-style parts are regenerated in isolation. — [Tripo blog](https://www.tripo3d.ai/blog/explore/ai-3d-model-generator-enforcing-studio-art-direction-rules) (via search summary; fetch 403)
- A 2026 roundup says Midjourney has weak consistency between generations, and most studios run several systems together. — [Inkration](https://inkration.com/ai-art-tools-for-game-studios-in-2026-what-actually-works-for-production/) (via search summary)
- Repeated edits drift, and pixel-exact regions should be composited. — [OpenAI guide](https://developers.openai.com/api/docs/guides/image-prompting.md)
- In an academic LLM+SDXL game-asset pipeline, the semantic themes came through but intended visual styles were "less clear". — [CrawLLM paper (Liapis et al.)](https://www.antoniosliapis.com/papers/crawllm_an_llm-based_pipeline_for_game_asset_generation.pdf) (via search summary)

**Shipped incidents and studio statements (2025–2026)**
- **Clair Obscur: Expedition 33 (Sandfall):**
  - The Indie Game Awards rescinded its Game of the Year and Debut Game awards.
  - Sandfall described temporary AI placeholder textures that QA missed and that were removed within five days.
  - Sandfall had told the organisers that no gen AI was used.

  — [CogConnected, Dec 2025](https://cogconnected.com/2025/12/clair-obscur-expedition-33-loses-indie-game-of-the-year-for-using-generative-ai/) (via search summary)
- **Call of Duty: Black Ops 7 (Activision):**
  - Players flagged many of its 680+ calling cards as generative AI, some resembling Ghibli style.
  - The Steam page says: "Our team uses generative AI tools to help develop some in game assets".
  - Players claimed refunds.

  **Anno 117 (Ubisoft)** carries a similar Steam AI disclaimer and drew negative reviews for it. — [Hitmarker](https://hitmarker.net/news/call-of-duty-black-ops-7-faces-backlash-over-ai-generated-calling-card-artwork-1576825); [TechSpot](https://www.techspot.com/news/110274-developers-respond-black-ops-7-anno-117-arc.html) (via search summary; I could not confirm details of the Anno 117 placeholder image)
- **Larian:**
  - December 2025: after the Divinity reveal, Swen Vincke told Bloomberg the studio used AI for exploring ideas, presentations, concept art and placeholder text, with no AI content shipping. He later noted the studio has 72 artists, 23 of them concept artists.
  - January 2026 Reddit AMA: "There is not going to be any generative AI art in Divinity", and no genAI in concept-art development.

  — [VGC](https://www.videogameschronicle.com/news/holy-f-guys-were-not-replacing-artists-larian-boss-responds-to-ai-backlash); [CGMagazine](https://www.cgmagonline.com/news/larian-generative-ai-divinity-reddit) (via search summary)
- **Ubisoft CHORD:** the artists' own verdict is "not production-ready". It is positioned for prototyping and placeholders. — [Ubisoft La Forge](https://www.ubisoft.com/en-us/studio/laforge/news/1i3YOvQX2iArLlScBPqBZs/generative-base-material-an-opensource-prototype-for-pbr-material-estimation-debuting-at-siggraph-asia-2025)

### Inferences
- Over-clean or over-uniform results and the generic look are the same root failure: the model fills unspecified dimensions with its averages. The fixes are an explicit wear and imperfection vocabulary in the bible, value-band and noise ceilings, and references that show the intended amount of grime.
- Placeholder hygiene belongs in the bible as asset standards: a naming or flag convention for AI placeholders plus a pre-release scan. This is the direct lesson of Expedition 33.
- Gate the masters (key art, district or biome masters, hero buildings) and let variants derive from them. That keeps the review surface small, matching the gates the "living schema" vendor proposes.

### Gaps
- No studio postmortem quantifies drift (for example, the percentage of AI drafts rejected for being off-style).
- I found nothing primary from Embark on AI art. Its known AI use is voice, which is outside this scope.

## 5. Using Claude or other LLMs as an art director that writes and enforces the bible

### Takeaway
The best public example is an open-source Claude Code "game studio" with an art-director agent. It authors the bible section by section through Question → Options → Decision → Draft → Approval, and turns it into per-asset specs and prompts. Designers also use LLMs to turn reference images into reusable JSON style profiles. I found no published evidence of LLM or VLM art directors *judging* outputs against a bible reliably. That remains a human gate, and research suggests LLM pipelines keep themes better than style.

### Cited Findings
- **Claude Code Game Studios:**
  - It has an `art-director` agent and the skills `/art-bible`, `/asset-spec` and `/asset-audit`.
  - The art-bible skill delegates content to specialists: "Never draft a section yourself — every section's content comes from the relevant specialist".
  - Each section goes through Question → Options → Decision → Draft → Approval → Write, with "[A] Lock this in and write it to design/art/art-bible.md".
  - Disagreements between agents go to the user, and an "AD-ART-BIBLE" sign-off gate can be skipped only in solo or lean modes, with the skip recorded.
  - The bible itself produces no image prompts; `/asset-spec` produces "per-asset visual specs and AI generation prompts from approved GDDs".

  — [Repo README](https://github.com/Donchitos/Claude-Code-Game-Studios); [art-bible SKILL.md](https://raw.githubusercontent.com/Donchitos/Claude-Code-Game-Studios/main/.claude/skills/art-bible/SKILL.md); [asset-spec SKILL.md](https://raw.githubusercontent.com/Donchitos/Claude-Code-Game-Studios/main/.claude/skills/asset-spec/SKILL.md)
- Without a bible, `asset-spec` falls back to the brief's one-line art direction and labels its rules as "proposals". — [asset-spec SKILL.md](https://raw.githubusercontent.com/Donchitos/Claude-Code-Game-Studios/main/.claude/skills/asset-spec/SKILL.md)
- Adobe: use the LLM as a briefing partner in art-direction vocabulary and push back on its first draft ("Make it feel more editorial"). Have it convert reference images into a JSON context profile that is reused across subjects. — [Adobe Design](https://adobe.design/ideas/creative-direction-the-secret-to-great-ai-images)
- Research: the CrawLLM LLM-driven game-asset pipeline (SDXL) conveyed themes but made intended visual styles "less clear". A study of LLM-generated SVG game assets reports that an aesthetic-quality gap remains. — [CrawLLM](https://www.antoniosliapis.com/papers/crawllm_an_llm-based_pipeline_for_game_asset_generation.pdf); [SBGames paper](https://sol.sbc.org.br/index.php/sbgames/article/download/45382/45143/) (via search summary)
- Vendors position "art director" features as generation-side controls (team prompt libraries, model selection, style training on 10–25 curated images), not as automated judges. — [Layer for art directors](https://www.layer.ai/for/art-directors) (via search summary)

### Inferences
- Claude is useful as an art director in three roles:
  - **Author:** run the question-options-decision loop with the user and write the bible.
  - **Compiler:** turn the bible into specs and prompts that cite sections, with a verbatim style block.
  - **Checker of text:** flag prompts or specs that contradict the bible's prohibitions, palette or materials.

  Judging images against the bible should stay paired with human review. No source showed reliable automated visual judgement.
- Rules worth adopting from the Donchitos design: one section at a time; explicit user approval before writing; surface disagreements, never resolve them silently; stable asset IDs; prompts carry "Art Bible Anchors".

### Gaps
- I found no Anthropic-specific guidance or case study about Claude as an art director.
- No benchmark measures how well vision LLMs score style consistency of game art against a written guide.
