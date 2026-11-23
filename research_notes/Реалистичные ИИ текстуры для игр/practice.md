# Real practice in game texture remasters and HD texture packs, with and without AI (2019–2026)

Scope note: "official" = studio or publisher product; "community" = fan mod. Several primary pages (World of Players archive, Nexus Gothic pages, ModDB Gold Remaster, Daggerfall Workshop forum, Steam Workshop) returned 403/429 or bot-protection to the fetcher; for those, claims below rest on search-engine snippets of the page and are marked "(snippet)".

## 1. Which remasters/mods used AI upscaling or generation, with which tools, and what were the results and criticism?

### Takeaway
Official studios split into two camps: those who batch-upscaled originals (Grove Street Games' GTA Trilogy DE, 2021 — publicly panned for waxy faces and garbled text; Feral's Total War: Rome Remastered, 2021 — documented an ESRGAN chain for modders) and those who rebuilt assets from scratch and say so (Virtuos' Oblivion Remastered 2025, Hangar 13's Mafia DE 2020, Alkimia's Gothic 1 Remake). Community mods use AI heavily (ESRGAN, Topaz Gigapixel, waifu2x, Stable Diffusion img2img, PBRFusion/PBRify), and the better-received ones always add a manual pass.

### Cited Findings
**Official / studio practice**
- GTA: The Trilogy – Definitive Edition (Grove Street Games / Rockstar, Nov 2021): AI upscaling was used to remaster more than 100,000 textures across the three games; result criticised for "uncanny, wax-like faces" and vehicles that looked unfinished; AI upscalers "often struggle to interpret text or other precise details" — [Yahoo Tech](https://tech.yahoo.com/gaming/articles/ai-upscaling-future-game-remasters-133015632.html)
- GTA DE concrete AI errors at launch: billboard text garbled (a hot-dog stand ad reading "heat" instead of "meat", a guitar shop sign) — [Multiplayer.it](https://multiplayer.it/notizie/gta-the-trilogy-definitive-edition-errori-texture-allargate-dallia.html); Digital Foundry's re-check after patches: some signage spelling fixed but "still a lot of work left" — [Kotaku](https://kotaku.com/grand-theft-auto-the-trilogy-definitive-edition-patches-1848920282); DF disappointed by "upscaling and AI texture and model reconstruction problems" — [Everyeye.it summary of DF](https://www.everyeye.it/notizie/gta-trilogy-problemi-remaster-vice-city-san-andreas-test-df-554056.html)
- Total War: Rome Remastered (Feral Interactive, 2021) — official modding docs on GitHub describe upscaling original RTW textures with Cupscale + ESRGAN (PyTorch), chaining `1x_BCGone-DetailedV2_40-60` ("Cleans DXT1 compression artefacts") then `4xUniversalUpscaler-Detailed`, settings borrowed from the Chivalry and RIS fan mods; output DDS BC3 with mipmaps. The guide calls itself a "quick primer" and mentions no manual cleanup or style guidance — [Feral GitHub guide](https://github.com/FeralInteractive/romeremastered/blob/main/documentation/techart_guides/Upscaling_RTW_Textures_to_Remastered.md)
- Oblivion Remastered (Bethesda + Virtuos, Apr 2025, Unreal Engine 5): coverage reports Virtuos's claim that every asset was recreated from scratch — [RPG Codex](https://rpgcodex.net/article.php?id=12554), [Heise](https://heise.de/-10358618). No developer statement about AI in the texture pipeline found; a Steam forum allegation that Virtuos "used AI" is unsourced and was disputed in-thread — [Steam discussion](https://steamcommunity.com/app/2623190/discussions/0/604154603520481141)
- Oblivion Remastered — community AI on top of the official game: Canarious's "Unchained Textures" aims to upscale every texture to at least 4K with AI; Bobobass84 and Kuzja80 also released AI upscales — [Wccftech](https://wccftech.com/oblivion-remastered-4k-texture-packs/)
- Mafia: Definitive Edition (Hangar 13, 2020): rebuilt from the ground up; art director Petr Motejzik: "it was a priority for us to keep the maximum number of elements from the original game, and to improve and develop them further"; work started with new concept art and mood pieces before in-game assets — [mafiagame.com art post](https://mafiagame.com/news/mafia-definitive-edition-art), [USgamer interview](https://www.usgamer.net/articles/mafia-definitive-edition-remake-hangar-13-interview). No source found on AI or photogrammetry use.
- Gothic 1 Remake (Alkimia Interactive / THQ Nordic): director Reinhard Pollice — "We also decided very early on, we are not making an HD version of the art style of the original, that's not our goal" (Wccftech, 8 Jul 2025) — [Wccftech](https://wccftech.com/alkimia-interactive-delves-into-its-approach-to-art-in-the-gothic-remake/). See Q5.

**Semi-official (NVIDIA-backed) — RTX Remix**
- Half-Life 2 RTX (Orbifold Studios, community studio backed by NVIDIA): refused Remix's built-in AI upscaling; founder Chris Workman: the team is "essentially scrutinizing every single pixel to make sure we're hitting the exact same notes" the original designers intended; "a backlog of several thousand assets" — [PCGamesN (updated 14 Mar 2024)](https://www.pcgamesn.com/half-life-2/rtx-ai-textures). NVIDIA: materials rebuilt with PBR, extra geometry via Hammer, average world textures have 8x the pixels of the originals — [NVIDIA announcement](https://www.nvidia.com/en-us/geforce/news/half-life-2-rtx-remix-in-development/)
- RTX Remix Mod Contest winners (NVIDIA blog, 28 Aug 2025): Painkiller RTX (Merry Pencil Studios) used PBRFusion for 4x upscaling + normal/roughness/height, "It set the baseline look, while we focused our manual efforts on the assets players notice most"; Unreal RTX (mstewart401) "wouldn't have been able to create PBR textures without AI"; NFS Underground RTX generated 500+ textures in ComfyUI and refined in Photoshop: "The original look was preserved by using the original textures as input for AI"; Portal 2 RTX moved from AI upscalers to a mix of AI and hand-painting — [NVIDIA blog](https://blogs.nvidia.com/blog/rtx-ai-garage-rtx-remix-mod-contest-gen-ai)

**Community mods using AI**
- Final Fantasy IX Moguri Mod: "aided by AI (ESRGAN and Stable Diffusion)", with manual redraw of all ~11k layer edges and area names, backgrounds merged and polished by hand; needed because Square Enix lacked the original background art, so work started from low-res dithered PS1 output — [Moguri Mod site](https://sites.google.com/view/moguri-mod/home), [Wikipedia](https://en.wikipedia.org/wiki/Moguri_Mod); a guide notes the older 2020 background set is "more pixel-perfect, paint-like, but noisier" — [Steam guide](https://steamcommunity.com/sharedfiles/filedetails/?id=3068861646) (snippet)
- Silent Hill 2 (Enhanced Edition ecosystem): a fan texture project aims to raise every texture at least 4x "using various AI scaling tools and manual editing" — [VGTimes](https://vgtimes.com/games/silent-hill-2-2001/files/50859-enhanced-textures.html)
- Morrowind: protoBelisarius's 32 GB 4K pack (Apr 2023) = GAN upscaler + second img2img ("sd") pass with "very slight variations" that "preserves the original pattern" and removes most upscaling artefacts — [DSOGaming](https://www.dsogaming.com/mods/the-elder-scrolls-iii-morrowind-gets-a-32gb-4k-texture-pack/), [Nexus 52578](https://www.nexusmods.com/morrowind/mods/52578). Morrowind Enhanced Textures: multiple ML passes, 4x — [PC Gamer](https://www.pcgamer.com/morrowind-looks-better-than-ever-with-this-ai-enhanced-texture-mod/); HD Vanilla Textures used waifu2x ("non-blurry double sized vanilla textures") — [Nexus 45247](https://www.nexusmods.com/morrowind/mods/45247) (snippet)
- PC Gamer's AI upscale roundup: ESRGAN results praised on stylised games (Hexen), but higher-detail faces on low-poly models look "eerie" — [PC Gamer](https://www.pcgamer.com/AI-upscale-mod-list/)
- GTAForums ESRGAN character pack: author mixes outputs from several models and hand-corrects before finalising — [GTAForums](https://gtaforums.com/topic/956290-iiivcsa-esrgan-characters-textures-upscaled/) (snippet)
- Kingdom Hearts HD 1.5+2.5 (Steam forum): users say some AI-upscaled textures "scaled well, some botched badly", 2D world graphics "look smeared"; one would "rather keep the original hand painted textures (low resolution or not)" — [Steam discussion](https://steamcommunity.com/app/2552430/discussions/0/6364230198203217113/) (snippet)
- Daggerfall Workshop "AI Upscaled Textures" thread: an "intangible quality seems to disappear" in upscales; ESRGAN distorts faces/small features on low-res sprites; a purist argues any scaling other than nearest-neighbour changes pixel art — [DF Workshop forum](https://forums.dfworkshop.net/viewtopic.php?t=1642&start=280) (snippet; page 403)
- Duke Nukem 3D upscale pack: world textures "are going to need manual work" (unlike sprites); concern that hand edits on thousands of images to make them tile must be redone whenever the model improves — [Duke4.net](https://forums.duke4.net/topic/10473-release-duke-nukem-3d-upscale-pack/page__st__30__p__316094) (snippet)

### Inferences
- The publicly failed official case (GTA DE) is the one that shipped a batch AI pass without per-asset art review; the praised fan cases (Moguri, Painkiller RTX) are AI-baseline + heavy manual pass on what players notice.
- Studios doing "true" remasters/remakes (Oblivion R, Mafia DE, Gothic Remake) talk about rebuilding from concept/intent, not about upscaling; AI use, if any, is undisclosed.

### Gaps
- No Digital Foundry primary article on GTA DE fetched (only secondary summaries).
- No developer disclosure on AI use in Oblivion Remastered or Gothic 1 Remake texture pipelines.
- Silent Hill 2 Enhanced texture project author/workflow details not found.

## 2. How do the better HD packs keep the "character" of the original? Specific modder statements.

### Takeaway
Recurring strategies: (a) use the original texture itself as the AI input and keep its pattern/colour (NFS U RTX, protoBelisarius, Morrowind VanillaPlus); (b) match colour and saturation to vanilla exactly and colour-correct mixed sources (VanillaPlus, Gothic II Gold Remaster); (c) hand-redraw edges, text, faces and other "precise" content AI fails on (Moguri); (d) check each asset against designer intent in context, in-engine (HL2 RTX, Painkiller RTX); (e) don't upscale small/distant textures, put manual effort on hero assets (Painkiller RTX).

### Cited Findings
- Morrowind VanillaPlus: aims to keep vanilla style while matching "the exact same color and saturation"; author describes the work as retouching rather than filters — [Nexus 45813](https://www.nexusmods.com/morrowind/mods/45813) (snippet)
- NFS Underground RTX: "The original look was preserved by using the original textures as input for AI", then Photoshop refinement for consistency — [NVIDIA blog, Aug 2025](https://blogs.nvidia.com/blog/rtx-ai-garage-rtx-remix-mod-contest-gen-ai)
- Painkiller RTX (NVIDIA Technical Blog, 5 Feb 2026): constraints defined early, AI output treated as a shared baseline, materials judged in context from in-engine captures; small/distant textures not upscaled; height maps only on large flat surfaces ("don't benefit from height maps at all" for many); drama from "correct interactions with light rather than painted-in shadows", grime and surface variation used to preserve the look; all lighting hand-tuned per scene — [NVIDIA dev blog](https://developer.nvidia.com/blog/how-painkiller-rtx-uses-generative-ai-to-modernize-game-assets-at-scale)
- Half-Life 2 RTX: every asset checked against original designers' intent, "Each and every asset we're taking the time to make sure we get right and that is time consuming" — [PCGamesN](https://www.pcgamesn.com/half-life-2/rtx-ai-textures)
- Moguri Mod: manual redraw of ~11k layer edges and all area names; hand merge/polish — [Moguri site](https://sites.google.com/view/moguri-mod/home)
- Mafia DE (official): keep "the maximum number of elements from the original" and develop them; new concept art and mood pieces first — [mafiagame.com](https://mafiagame.com/news/mafia-definitive-edition-art)
- Gothic II Gold Remaster (Steam Workshop): best and "most authentic and suitable" textures chosen from several existing packs, exposure and colour adjusted so they match; many AI-upscaled and individually sharpened — [Steam Workshop 3372979990](https://steamcommunity.com/sharedfiles/filedetails/?id=3372979990) (snippet; page 429)
- Counter-example in Gothic: Vurt's G2 Graphics Overhaul says its textures are "resynthesized, thus they are entirely new" and "not a simple upscale" — [Nexus gothic2/91](https://www.nexusmods.com/gothic2/mods/91) (snippet; page 403)
- Oblivion Remastered (official) shows the risk of departing from the original palette: Steam users call it "hyper realistic generic UE5" with "zero artistic direction", palette leaning "way too much into the brown"; Gamereactor says characters look worse due to realistic wrinkles, "Unreal has made everything too realistic", yet mood of the original preserved; Push Square calls it "a cohesive direction" — [Steam thread](https://steamcommunity.com/app/2623190/discussions/0/604154444533329549), [Gamereactor](https://www.gamereactor.eu/the-elder-scrolls-iv-oblivion-remastered-1534583/), [Push Square](https://www.pushsquare.com/previews/hands-on-oblivion-remastered-is-still-oblivion-in-all-its-glory-crap-frame-rate-aside) (forum points anecdotal)

### Inferences
- "Keep the character" in practice means: same palette/saturation as vanilla, same pattern layout, same scale of motifs, and judging in the game at play distance — not resolution.
- Photosourced/resynthesized packs (Vurt) and full rebuilds trade fidelity for realism; the realism direction is what draws the "generic" criticism (Oblivion R).

### Gaps
- Could not find interviews with Skyrim retexture authors (Pfuscher/Skyrim 202X, Noble Skyrim, aMidianBorn) about photosourcing vs hand-painting; searches returned only user reviews — e.g. [DSOGaming on Pfuscher terrain](https://www.dsogaming.com/news/4k-highly-detailed-and-realistic-terrain-landscape-textures-released-for-skyrim/).
- No modder statement found specifically on "detail at the right scale" (texel density matching the original motif size).

## 3. Does RTX Remix's texture AI produce soft results; what do Remix modders do to fix it?

### Takeaway
NVIDIA's docs describe a Speed mode that downsizes input to 256x256 and always outputs 1K — an obvious source of softness — and a Quality mode (tiled, 4x of original) recommended for finals. No public NVIDIA or forum write-up that explicitly says "Remix AI is soft" was found; the documented community response is to replace or bypass the built-in model (PBRFusion, PBRify chaiNNer/ComfyUI chains), and to hand-fix roughness, metals, glass, skin and baked lighting.

### Cited Findings
- Remix AI Texture Tools: 4x upscale and generation of albedo/normal/roughness; Speed mode "Resizes the input to 256x256 and always outputs 1k" ("quick iterations"); Quality mode tiles at original resolution into 256x256 tiles, outputs 4x, "recommended for final textures"; inputs ≤512 recommended; ≥12 GB VRAM. No mention of hand touch-up — [NVIDIA Remix docs 1.2.4](https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/1.2.4/docs/howto/learning-aitexturetools.html)
- PBRify_Remix (Kim2091): custom models trained only on CC0 ambientCG content ("ethical"); upscaler trained to reduce noise, DXT1 compression, dithering and "oversharpening/halos"; default displacement too strong — lower per texture to ~0.1 or 0.05; runs as chaiNNer chain or through NVIDIA's ComfyUI-RTX-Remix bridge — [GitHub](https://github.com/Kim2091/PBRify_Remix); release notes claim the V3 model (4x-PBRify_RPLKSRd_V3) gives "more natural detail and cleaner edges", clearer PBR, and earlier releases added colour-matching so upscales stay true to the original — [releases](https://github.com/Kim2091/PBRify_Remix/releases) (snippet)
- Painkiller RTX (Feb 2026): PBRFusion as "backbone", ~80% of repetitive work automated; NightRaven: model "isn't perfectly accurate, especially for roughness generation, it will get things wrong"; atlases confuse it ("a single texture contains multiple unrelated surfaces"); metals mostly hand-crafted; glass, transparency, skin custom; roughness checked against real-world PBR references; hero materials blend CC0 PBR, AI and procedural (InstaMAT) — [NVIDIA dev blog](https://developer.nvidia.com/blog/how-painkiller-rtx-uses-generative-ai-to-modernize-game-assets-at-scale)
- Baked lighting: Remix docs note old games used baked lighting and low-res textures "which may not appear optimal" and recommend material replacement — [Remix materials docs](https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/latest/docs/howto/learning-materials.html); Painkiller team stripped baked shadows from albedo; "incorrect lighting responses couldn't be fixed by simply adjusting texture brightness"; lost contrast rebuilt with roughness variation, stronger normals and controlled self-shadowing; Quinn Baddams saw "a gap in tools" for de-lighting legacy textures — [NVIDIA dev blog](https://developer.nvidia.com/blog/how-painkiller-rtx-uses-generative-ai-to-modernize-game-assets-at-scale)
- Non-Remix example of manual de-lighting: Bloodborne PC Remaster mod hand-removes baked shadows; even its "original-resolution" option keeps the de-lit lightmaps — [Nexus Bloodborne 45](https://www.nexusmods.com/bloodborne/mods/45?tab=files) (snippet)
- Half-Life 2 RTX simply does not use the Remix AI tools — [PCGamesN](https://www.pcgamesn.com/half-life-2/rtx-ai-textures)

### Inferences
- "Soft" Remix output is most plausibly Speed-mode downsampling plus an upscaler trained to denoise; modders' fix is model choice, Quality mode, and hand-authored roughness/normal contrast. (Inference — not stated by NVIDIA.)
- Once baked light/AO is removed from albedo, the texture loses its painted contrast, and this must be put back via normals/roughness/self-shadowing or lighting; otherwise PBR versions look flat.

### Gaps
- No Reddit r/RTXRemix or Remix Discord thread surfaced (search engine returned none); direct community complaints about softness of the built-in tool are undocumented here.

## 4. Write-ups of "original low-res looks more detailed than the AI HD version" and why

### Takeaway
No dedicated game-industry write-up was found. Converging evidence: AI upscalers trained on photos treat hand-painted strokes/grain as noise and smooth it (waxy/plastic look); denoising flattens micro-contrast; text and faces get reinterpreted; and once painted-in light/AO is removed for PBR, the image loses the contrast that read as "detail".

### Cited Findings
- GTA DE: upscalers "prone to over-correcting and mistakenly removing visual details", giving an "unnaturally smooth appearance"; wax-like faces — [Yahoo Tech](https://tech.yahoo.com/gaming/articles/ai-upscaling-future-game-remasters-133015632.html)
- Hand-drawn art: upscalers "trained predominantly on photographic datasets" interpret hand-drawn features as noise; GAN upscalers "over-smooth fine linework, invent uniform textures, suppress grain" — [Alibaba product-insights article](https://www.alibaba.com/product-insights/ai-powered-anime-upscalers-are-they-preserving-artistic-intent-or-flattening-hand-drawn-texture.html) (low-authority source, snippet)
- Generic upscaler failure modes: with compressed/noisy input the model fills gaps with learned patterns; micro-texture replaced by smoothness ("plastic or waxy"); sharpening priority causes halos; plain interpolation "introduces no new data, maintains original contrast and edge behavior" — [Let's Enhance blog](https://letsenhance.io/blog/all/ai-upscaling-makes-images-worse/) (vendor source, snippet)
- Players: "rather keep the original hand painted textures (low resolution or not)" (Kingdom Hearts) — [Steam](https://steamcommunity.com/app/2552430/discussions/0/6364230198203217113/); "intangible quality seems to disappear" (Daggerfall) — [DF Workshop](https://forums.dfworkshop.net/viewtopic.php?t=1642&start=280) (snippets)
- Painted-in light as part of the look: Painkiller RTX found removing baked shadows dropped contrast, rebuilt via roughness variation, stronger normals and self-shadowing — [NVIDIA dev blog](https://developer.nvidia.com/blog/how-painkiller-rtx-uses-generative-ai-to-modernize-game-assets-at-scale)
- Moguri's old (2020) backgrounds described as "more pixel-perfect, paint-like, but noisier" vs the newer cleaner set — [Steam guide](https://steamcommunity.com/sharedfiles/filedetails/?id=3068861646) (snippet)
- Denoise trade-off: stronger noise reduction after upscaling = blurrier result — [Nexus Fallout 4 mod posts](https://www.nexusmods.com/fallout4/mods/54341?tab=posts) (snippet)

### Inferences
- The "low-res looks more detailed" effect is best explained as loss of local contrast and stroke structure (high-frequency painted marks, painted AO/light) at the scale the eye reads in game, not as loss of pixels; an upscale that is smooth at 4x shows *fewer* readable marks per screen area than the crisp original at native size.
- Practical rule seen across good packs: keep or re-inject original contrast/AO (either in albedo or via normals/roughness/lighting) and judge at play distance.

### Gaps
- No Digital Foundry, GDC or 80.lv article specifically analysing this effect was found.

## 5. Gothic-specific: texture remasters for Gothic 1/2 and how they were made

### Takeaway
Community Gothic work is mostly AI upscales of vanilla (ESRGAN, Topaz Gigapixel), curated compilations (Gold Remaster), and a few rebuilt/resynthesized packs (Vurt, Artemiano) used with the GD3D11 (DX11) renderer for normal maps and tessellation. The official Gothic 1 Remake explicitly is not an HD version of the original art; no description of its texture pipeline was found.

### Cited Findings
- ESRGAN-UpScaleProject (Gothic 1, Nexus): "x4 Original Texture resized with AI based ESRGAN software", installed as a single VDF — [Nexus gothic/5](https://www.nexusmods.com/gothic/mods/5) (snippet)
- Gothic 2 Textures Upscaled 4X (Nexus): made with Topaz Gigapixel AI, all from the original textures — [Nexus gothic2/117](https://www.nexusmods.com/gothic2/mods/117) (snippet)
- Gothic II Gold Remaster (Steam Workshop / ModDB): many textures "optimized with a modern A.I. image upscaler and individually sharpened"; the most authentic textures selected from many existing packs; exposure and colour adjusted to match — [Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3372979990), [ModDB](https://www.moddb.com/mods/gothic-2-gold-remaster) (snippets; pages 429/403)
- Vurt's G2 and CoM Graphics Overhaul: textures "resynthesized, thus they are entirely new", "not a simple upscale" — [Nexus gothic2/91](https://www.nexusmods.com/gothic2/mods/91) (snippet); older Vurt's Gothic II Texture Pack thread — [RPGWatch](https://rpgwatch.com/forum/threads/mod-vurts-gothic-ii-texture-pack.11732/) (403)
- Artemiano's texture packs (Gothic 1/2, World of Players): higher-resolution textures and improved meshes, staying close to the original style; Freddy's Texturepatch for G1 world textures also cited — [WoP archive t-1558094](https://forum.worldofplayers.de/forum/archive/index.php/t-1558094.html) (snippet; page behind Anubis bot protection)
- Riisis Texture Mix (ModDB) and Gothic II Graphics Overhaul (Nexus gothic2/4) exist as further compilations — [ModDB Riisis](https://www.moddb.com/mods/riisistexturepack), [Nexus gothic2/4](https://www.nexusmods.com/gothic2/mods/4) (no workflow details retrieved)
- GD3D11 / DX11 renderer: dynamic shadows, normal mapping, hardware tessellation; large mod + HD textures + renderer can crash — [WoP D3D11 thread](https://forum.worldofplayers.de/forum/archive/index.php/t-1421732-p-2.html), [Steam guide DX11 + HQ textures + tessellation](https://steamcommunity.com/sharedfiles/filedetails/?id=636014226) (snippets)
- Gothic 1 Remake (Alkimia): "we are not making an HD version of the art style of the original" (Pollice, Jul 2025); goal is a believable world that still sells the magical elements — [Wccftech](https://wccftech.com/alkimia-interactive-delves-into-its-approach-to-art-in-the-gothic-remake/); art director Daniel Candil at Gamelab described the original's mood and light as the target, dark fantasy, "Baroque" approach — [Gamereactor video](https://www.gamereactor.eu/video/710333/An+exclusive+insight+into+the+Gothic+Remake+and+its+Baroque+artistic+approach+at+Gamelab+Nexus/) (auto-transcript; wording imprecise); making-of video on rebuilding the Valley of Mines — [THQ Nordic](https://thqnordic.com/news/beyond-an-hd-remaster-how-alkimia-is-reimagining-gothic-1-new-making-of-video); a review notes some surfaces look rough up close — [Niche Gamer](https://nichegamer.com/reviews/gothic-remake-review/)

### Inferences
- The Gothic community's strongest "faithful" packs are curations with colour/exposure matching plus sharpening, not raw ESRGAN; that mirrors the general finding that colour/saturation fidelity to vanilla matters more than the upscaler.
- The official remake is a reinterpretation, so it is not a reference for "same Gothic, better made" texture fidelity; at most for mood/light intent.

### Gaps
- World of Players threads (German) could not be read (bot protection); no community verdict on AI vs original Gothic textures was retrieved.
- L'Hiver (Gothic 2 total conversion) texture workflow: nothing found.
- No r/worldofgothic / r/GothicGame comparison threads surfaced.
- Gothic 1 Remake texture/material pipeline (photogrammetry, Substance, AI) undisclosed in sources found.
