# Photo-scanned / CC0 material libraries vs AI generation as the base for realistic game textures (2023-2026)

Research date: 2026-10-23. Context: a free open-source Gothic remaster published on GitHub, where texture files would sit in a public repo or content pack.

Method notes: licence pages were fetched directly where possible. The Fab EULA page (fab.com/eula) sits behind a Cloudflare challenge and could not be fetched, so the Fab clauses below are verbatim quotes as posted on the Epic forum and in a licence wiki. The Textures.com site renders in JavaScript, so its terms come from search snippets of its own FAQ and Terms of Service PDFs. Asset lists for Poly Haven and ambientCG come from their public JSON APIs (api.polyhaven.com/assets, ambientcg.com/api/v2/full_json), queried on 2026-10-23.

## Which libraries are truly CC0 and allow redistribution inside an open-source repo / game content pack?

### Takeaway
Poly Haven, ambientCG, 3DTextures.me and (per its own statement and secondary sources) cgbookcase are plain CC0: you may commit their files, or edits of them, to a public GitHub repo and ship them in a content pack. ShareTextures calls itself "Custom CC0 ... with specific restrictions": it forbids redistribution "as part of collections" without permission, so it is risky for a repo or pack. Textures.com explicitly forbids open-source release even of modified textures. Fab/Megascans (Standard License) allows shipping assets only inside a Project in object code, with end users restricted from extracting them, so raw or derived Megascans files cannot go in a public repo.

### Cited Findings

**Poly Haven: CC0, no conditions**
- "Our assets are all licensed as CC0" (HDRIs, textures, models). The page describes CC0 as effectively public domain. "You can use our assets for any purpose", including commercial work; "You can redistribute them"; "You do not need to give credit." Only the site's own content is excluded (logos, avatars, example and user renders, text). The site terms also forbid "Web scraping or data mining without express permission" (this covers the website, not the assets). — [Poly Haven licence](https://polyhaven.com/license)
- Poly Haven's FAQ says its CC0 assets may be used to train AI models. A blog post says the team has committed not to make assets with generative AI, preferring careful photoscanning, though it uses AI-adjacent tools such as ArtEngine to make scanned textures seamless. — [Poly Haven FAQ](https://docs.polyhaven.com/en/faq); [AI and Poly Haven (blog)](https://blog.polyhaven.com/?p=2377)

**ambientCG: CC0, no conditions**
- "All ambientCG assets are provided under the Creative Commons CC0 1.0 Universal License." This "applies to the downloadable asset files and the material preview renders." "You can copy, modify, distribute and perform the assets, even for commercial purposes, all without asking permission." "You can include the raw files in your project, for example a video game." Credit is not required. Suggested credit line: "Created using <asset name> from ambientCG.com, licensed under the Creative Commons CC0 1.0 Universal License." — [ambientCG licence](https://docs.ambientcg.com/license/)

**3DTextures.me: CC0**
- "All textures on this site are licensed as CC0." "You can redistribute them", alone or inside your own project. "The only thing you can't do is claim to be the author of the textures." Credit is not required. 4K textures and Substance Designer sources (SBS/SBSAR) are Patreon-only, and bulk download requires Ko-fi support. — [3DTextures.me about](https://3dtextures.me/about/)

**cgbookcase: CC0 (not verified on a current licence page)**
- cgbookcase's /license page returned 404 on 2026-10-23. Secondary sources say the textures are CC0. A 2019 CG Channel article describes tileable PBR sets "available free under a CC0 licence", with albedo, normal, height, roughness, AO and metalness maps, mostly up to 4K. — [CG Channel 2019](https://www.cgchannel.com/?p=102394); [BlenderNation 2018](https://www.blendernation.com/2018/09/14/250-free-cc0-high-quality-pbr-textures-from-cgbookcase-com/)

**ShareTextures: "Custom CC0" with restrictions**
- Content is "licensed under the Custom CC0 (Creative Commons Zero) license with specific restrictions." "No asset redistribution on other websites, in plugins, or as part of collections without our written permission." "No automated downloads, hotlinking, or embedding direct downloads in third-party apps (like Blender addons)." "CC0 only applies to assets downloaded directly from our site." "No attribution required (but appreciated!)." — [ShareTextures licence](https://www.sharetextures.com/p/license)

**Textures.com: forbids open-source release**
- License FAQ: "it is not allowed to release our images under Open Source licenses (even when the materials are modified)." If the game is released under an Open Source license, "you need to release content that has been created using our materials under a closed source license." The stated reason is that almost all open-source licences allow unlimited redistribution. Using the materials in a free or commercial video game is otherwise allowed, without attribution. — [Textures.com License FAQ](https://www.textures.com/support/faq-license)
- The Terms of Service list releasing the Content, or derivative products containing it, under Open Source licences as a prohibited use (Rev3-8 from 2020, kept in Rev3-21). — [ToS Rev3-21 (PDF)](https://www.textures.com/static/terms/TexturesCom%20-%20Terms%20of%20Service%20Rev3-21.pdf); [ToS Rev3-8 (PDF)](https://www.textures.com/system/TexturesCom%20-%20Terms%20of%20Service%20Rev3-8.pdf)
- Its account terms say images may not be redistributed or resold as competing products (textures, texture packs, material packs) "even when the images are modified or if you give them away for free". This is from a search snippet of the account page. — [Textures.com account page](https://www.textures.com/accountActivated)

**Fab / Quixel Megascans: Standard License, no standalone or public redistribution**
- EULA clauses as quoted on the Epic forum:
  - Section 4c: "you may only authorize end users to make use of Content solely as incorporated in the Project in object code" and "you must restrict end users from extracting or otherwise using Content outside of the Project."
  - Section 5a: "you may not Distribute Content on a standalone basis to third parties except to your collaborators."
  - Section 6 bars attempts to "reverse engineer, decompile, translate, disassemble, or derive source code or data from the Content" and bars letting "any third party to incorporate Content into their own products, services, or other projects."
  - The Personal tier depends on the "$100,000 USD in gross revenue" threshold, assessed "at the time of the Transaction."
  — [Epic forum thread quoting the Fab EULA](https://forums.unrealengine.com/t/all-free-assets-including-ue-ones-are-now-standard-license-on-fab-meaning-no-commercial-use/2080626)
- Fab's own summary says you may share an asset "directly, via a private repository or in the Project" with collaborators, and may not "resell or redistribute the asset for free on a standalone basis." — [Fab Standard License summary (search snippet; page blocked to fetch)](https://www.fab.com/eula)
- A licence wiki summarising the EULA:
  - Standard License content "may not be combined with GPL, LGPL (except dynamic linking) or Creative Commons ShareAlike material."
  - Collaborators "must delete it when no longer needed."
  - Megascans rights "depend on the plan the account was enrolled in when the content was acquired." Under the Personal plan, content is "not distributed in source format."
  — [Licenseware wiki: Fab and Epic content licensing](https://wiki.licenseware.io/wiki/fab-and-epic-content-licensing/)
- Timeline:
  - Megascans was free to all on Fab until the end of 2024, under the Standard License, for "all engines and tools", but only if each asset was added to your Fab library.
  - Content downloaded via Bridge falls under the older Epic Content License, which allows free use only in Unreal Engine.
  - From 2025 assets are priced individually: 2D/3D assets from $0.99, procedural kits from $4.99, packs from $24.99.
  — [CG Channel, Oct 2024](https://www.cgchannel.com/2024/10/epic-games-has-made-megascans-free-to-all-but-only-until-the-end-of-2024/); [Digital Production, Sep 2024](https://digitalproduction.com/2024/09/19/quixels-megascans-no-longer-free-after-2024/)
- Epic said the 2025 pricing also applies to Unreal Engine plan users, while some Megascans stay free. Content acquired via Quixel.com/Bridge stays under the licence in force when it was acquired. — [Epic forum: upcoming changes to Quixel](https://forums.unrealengine.com/t/upcoming-changes-to-quixel-related-to-fabs-launch-in-october/2024362); [Epic forum: legacy Megascans access](https://forums.unrealengine.com/t/access-legacy-megascans-on-bridge-and-quixel-com/2142664)
- Anecdotal: an itch.io thread says Quixel objected to open-source projects redistributing Megascans. I could not verify the original Quixel statement. — [itch.io thread](https://itch.io/post/5928491/view-in-topic)

**Other paid libraries met in passing:** TextureMax's terms exclude open-source games from its video-game allowance and require that image files not be directly accessible in the game. TextureCan uses CC0. — [TextureMax licensing](https://www.texturemax.com/licensing); [TextureCan terms](https://texturecan.com/terms/)

### Inferences
- In a public GitHub repo of a game remaster, every texture file is readable by anyone. That conflicts directly with Fab §4c/§5a (restrict extraction, no standalone distribution) and with the Textures.com open-source ban. Only the plain-CC0 sources are safe as a base: Poly Haven, ambientCG, 3DTextures.me, and cgbookcase (after checking its page). Derived textures from them can be relicensed or put under any licence.
- ShareTextures' "no redistribution as part of collections" probably catches a content pack of many textures. Use it only with written permission, or not at all.
- Megascans would also clash with the Fab "no GPL / CC-BY-SA" clause if the repo or its other content uses copyleft. That is worth checking against the repo's own licence, which I did not check.
- A CC0 base also keeps provenance clean if AI tools are later run on the textures: Poly Haven explicitly permits AI training on its assets.

### Gaps
- I could not read the exact current Fab EULA text (Cloudflare). The quotes are second-hand (Epic forum and Licenseware), and the 2025 EULA update thread was not read: [Fab Document Updates](https://forums.unrealengine.com/t/fab-document-updates-eula-fab-distribution-agreement-etc/2540618).
- cgbookcase's licence page URL has moved or gone. CC0 status is confirmed only by secondary and older sources.
- I found no statement from ambientCG on AI generation or AI training.

## Do they have weathered lime plaster, whitewash over render, mouldy plaster, timber-frame infill? Specific assets, resolutions, real-world scale

### Takeaway
Poly Haven is the richest CC0 source: about 40 photoscanned plaster walls, many weathered, mossy or stained, mostly 8K-16K, with stated real-world sizes of typically 1-4 m per tile. ambientCG has about 30 plaster materials, but most are procedural (Substance-made) from 2018-2020. Only four are photogrammetry scans: PaintedPlaster006, PaintedPlaster017, PaintedPlaster018 and Plaster007. ShareTextures has 58 plaster textures up to 4K. No library has a dedicated "limewash over wattle-and-daub between timbers" scan. Timber-frame infill has to be built from a plaster tile plus a separate wood material, a mask or a trim.

### Cited Findings

**Poly Haven, from its API (`max_resolution` px; real-world tile size from `dimensions`, given in mm, converted here to m).** Base URL https://polyhaven.com/a/<id>:
- Closest to old European lime plaster or whitewash:
  - [medieval_wall_01](https://polyhaven.com/a/medieval_wall_01): 8K, 2 m, tags: wall, rough, uneven, plastered.
  - [worn_mossy_plasterwall](https://polyhaven.com/a/worn_mossy_plasterwall): 16K, 1.8 m, moss, aged, exterior, peeling.
  - [painted_plaster_wall](https://polyhaven.com/a/painted_plaster_wall): 16K, 2 m, weathered, stained, dirty, exterior, timeworn.
  - [whitewashed_brick](https://polyhaven.com/a/whitewashed_brick): 16K, 2 m, whitewashed, weathered, aged.
  - [clay_plaster](https://polyhaven.com/a/clay_plaster): 16K, 2 m, cracked exterior clay plaster.
  - [worn_cracked_plaster](https://polyhaven.com/a/worn_cracked_plaster): 8K, 1.8 m.
  - [worn_plaster_wall](https://polyhaven.com/a/worn_plaster_wall): 8K, 1.8 m.
  - [damaged_plaster](https://polyhaven.com/a/damaged_plaster): 16K, 1.85 m, plaster over brick.
  - [red_plaster_weathered](https://polyhaven.com/a/red_plaster_weathered): 8K, 2 m.
  - [blue_plaster_weathered](https://polyhaven.com/a/blue_plaster_weathered): 16K, 2 m, peeling.
  - [yellow_plaster_02](https://polyhaven.com/a/yellow_plaster_02): 8K, 2 m, tags include wood, planks, mud, cabin.
  - [rough_plaster_broken](https://polyhaven.com/a/rough_plaster_broken): 8K, 1.5 m, peeling patches.
  - [white_plaster_rough_01](https://polyhaven.com/a/white_plaster_rough_01) and [white_plaster_rough_02](https://polyhaven.com/a/white_plaster_rough_02): 8K, 1 m, moss.
  - [grey_plaster](https://polyhaven.com/a/grey_plaster): 8K, 1 m, moss.
  - [plaster_stone_wall_01](https://polyhaven.com/a/plaster_stone_wall_01): 16K, 2.3 m, and [plastered_stone_wall](https://polyhaven.com/a/plastered_stone_wall): 16K, 2.5 m. Both are plaster over rubble stone.
  - [wall_bricks_plaster](https://polyhaven.com/a/wall_bricks_plaster) and the rough_plaster_brick 01-05 series: 8K, 1-2 m.
  - [white_stucco](https://polyhaven.com/a/white_stucco): 8K, 2 m.
  - [concrete_wall_003](https://polyhaven.com/a/concrete_wall_003): 16K, 3 m, painted, dirty, white or yellow.
  — [Poly Haven API](https://api.polyhaven.com/assets?t=textures)
- Poly Haven says it uses careful photoscanning and does not make assets with generative AI. — [AI and Poly Haven](https://blog.polyhaven.com/?p=2377)

**ambientCG, from its API (`creationMethod`, release date).** Base URL https://ambientcg.com/view?id=<id>:
- Photogrammetry:
  - [PaintedPlaster006](https://ambientcg.com/view?id=PaintedPlaster006): 2019, broken, facade, patches.
  - [PaintedPlaster017](https://ambientcg.com/view?id=PaintedPlaster017): 2021, white.
  - [PaintedPlaster018](https://ambientcg.com/view?id=PaintedPlaster018): 2021, damaged yellow.
  - [Plaster007](https://ambientcg.com/view?id=Plaster007): 2025-04-23, "broken, old, paint, plaster, wall, white".
- Procedural (Substance), 2018-2020:
  - Plaster001-006.
  - PaintedPlaster001-005 and 007-016. 009-016 are tagged "bricks, broken, old, paint".
- "PBRApproximated" (photo converted to PBR): Concrete012 ("church, old, plaster, sandstone"), Concrete034-036.
- Downloads go up to 8K (JPG/PNG). The API did not return real-world dimensions for most plaster assets (dimensionX/Y = 0).
- Only one "stucco" result (Plaster001, procedural) and zero "whitewash" results.
— [ambientCG API](https://ambientcg.com/api/v2/full_json?type=Material&q=plaster)

**ShareTextures:**
- The sidebar shows a "Plaster" category with 58 items. — [ShareTextures wall category](https://www.sharetextures.com/textures/wall)
- Examples:
  - [Plaster 47](https://www.sharetextures.com/textures/plaster/plaster-47): CC0, damaged old plaster over brick, 4K.
  - [Plaster 48](https://www.sharetextures.com/textures/plaster/plaster-48): worn plaster over brick, CC0.
  - [Plaster 41](https://www.sharetextures.com/textures/plaster/plaster-41): worn old white wall, CC0.
  - [Plaster 38](https://www.sharetextures.com/textures/plaster/plaster-38): weathered grey plaster, up to 4K.
  - [Plaster 46](https://www.sharetextures.com/textures/plaster/plaster-46): aged worn plaster, up to 4K.
  - [Plaster 37](https://www.sharetextures.com/textures/plaster/plaster-37): photogrammetry scan, 4K, 16-bit.
  These descriptions come from search snippets.

**Historical reference for what the material should look like:**
- Limewash on exposed timber framing was typically renewed each spring and filled hairline cracks from seasonal movement. — [Building Conservation: Wattle and Daub](https://www.buildingconservation.com/articles/wattleanddaub/wattleanddaub.htm)
- Wattle-and-daub infill cracked readily, which is why it was coated in lime plaster or whitewash. — [Building Science BSI-033](https://buildingscience.com/documents/insights/bsi-033-evolution)
- Limewash took on the tint of the local earth, giving reddish, yellow or white tones (a popular source, lower reliability). — [Lost Kingdom](http://www.lostkingdom.net/medieval-architecture-building-materials/)

### Inferences
- Real-world scale matters for a game wall: a Poly Haven tile of 1.8-2 m at 8K is about 4 px/mm. A Gothic house panel of about 1.5-2 m between timbers maps to roughly one tile, so a single scan covers a panel without visible repetition. Downsampled to 1-2K it keeps a plausible grain.
- Poly Haven scans like medieval_wall_01, worn_mossy_plasterwall and painted_plaster_wall look like the best CC0 candidates for "dirty old lime plaster". The infill-between-timbers look (plaster edge thickening against the wood, dirt streaks under beams) is not in any tile and must come from geometry-driven masks or a painted layer.
- Most of ambientCG's plaster set is procedural, not scanned. For "real" grain, prefer Poly Haven and ambientCG's few photogrammetry items.

### Gaps
- Megascans plaster asset names and links could not be collected: Fab is behind Cloudflare. Megascans is unusable for this repo anyway (see above).
- I did not check whether Textures.com or Sharetextures have a scan of genuine half-timber infill. Searches for "half timbered" infill scans returned nothing in any library.
- Poly Haven's capture method per asset (full photogrammetry or photo-based) was not checked individually.

## How do artists build "realistic" weathering: scan base + grunge masks + edge wear from geometry (curvature/AO) vs AI images? Pros/cons in quality and art-direction control

### Takeaway
Recent breakdowns of medieval European buildings (2020-2026) all follow the same recipe:
- a tiling base, either a scan or a Substance Designer material;
- variation driven by masks: AO/curvature generators, RGBA masks, vertex-paint blends, noise colour ramps;
- decals for story: leaks, dirt;
- a detail normal for close range.

Weathering is placed by geometry-derived data, so it follows the actual timbers and edges. That placement is the art-direction control an unconstrained AI image lacks. AI tools today mostly enter the pipeline as converters, as in Adobe Sampler's AI Image-to-Material and AI delighting, not as the base material.

### Cited Findings
- **Brian Pentz, 80.lv, 30 Mar 2026, medieval European town (UE + Substance 3D):**
  - He perfects one module "where I develop the core look for the wood, stone, and plaster", then turns it into a Smart Material for all buildings.
  - Layers use masks "driven by Ambient Occlusion and Curvature data" to place moss, edge wear and dust in crevices. Mask opacity is tuned by hand to avoid a procedural look.
  - Custom height detail feeds the generators, giving "the same rich visual information as a baked high-poly model."
  - Modules are textured uniquely, so texel density needs care.
  - In UE, a Master Material with a Detail Normal map keeps surfaces sharp up close.
  - The article does not mention Megascans.
  — [80.lv](https://80.lv/articles/how-to-design-a-medieval-european-environment-using-unreal-engine-and-substance-3d)
- **Dany Arfan, 80.lv, Dec 2020, medieval building (Blender + Megascans):**
  - The main surface is one damaged white plaster Megascans texture plus one or two leak decals.
  - Variation comes from duplicating the albedo and driving saturation and brightness changes with a noise-fed colour ramp, plus the same trick on roughness for wetness.
  - Megascans decals add "more story and realism."
  — [80.lv](https://80.lv/articles/working-on-a-medieval-building-with-blender-and-quixel-tools)
- **Finn Bogaert, Games Artist, 23 Nov 2025, Medieval Forest House:**
  - "I wanted to refrain from using any Megascans for the main house"; "Everything in this scene is textured with tiling materials made in Substance Designer."
  - Shared grunge and blend effects "keep them looking uniform and the same style."
  - Uses vertex-paint material blending and an RGBA mask workflow for unique detail on tilers.
  - Mesh decals "played a big role."
  — [Games Artist](https://gamesartist.co.uk/medieval-forest-house/)
- **Adobe Substance 3D Sampler (AI as converter):**
  - "Image to Material" generates base colour, roughness, normal, displacement and metallic from one image.
  - Sampler 4.2 (Sep 2023) brought a model "trained on all material types", with a material-type selector.
  - The original AI mode used machine learning "to recognize shapes and objects" to produce height and normal, with a delighter "to remove any shadows and highlights". That model was trained mainly on exterior surfaces such as grounds, pebbles, bark and stones under natural light.
  — [Sampler 4.2 release notes](https://helpx.adobe.com/substance-3d-sampler/release-notes/version-4-2.html); [Image to Material docs](https://helpx.adobe.com/substance-3d-sampler/filters/tools/image-to-material.html); [Delight (AI Powered)](https://helpx.adobe.com/substance-3d-sampler/filters/tools/delight-ai-powered.html); [Sampler 2020.2.2 notes](https://helpx.adobe.com/substance-3d-sampler/release-notes/old-versions/version-2020-2-2-2.html)
- **Poly Haven uses AI-adjacent tools only for seam removal** (ArtEngine) on its scans. — [AI and Poly Haven](https://blog.polyhaven.com/?p=2377)

### Inferences
- **Scan base + geometry masks: pros**
  - Real micro-structure at a known physical scale, so the grain reads "real" at any distance.
  - Normal, height and roughness are physically consistent, because they were measured rather than guessed.
  - Weathering follows the model: AO and curvature put dirt where water and hands actually go.
  - Everything is editable layer by layer.
- **Scan base + geometry masks: cons**
  - Scans come with the lighting and "design" of someone else's wall.
  - A library scan rarely matches a specific art bible (e.g. a Gothic palette).
  - Needs delighting and colour work.
  - A modern scanned wall, such as concrete or render, can read as the wrong era.
- **AI image as base: pros**
  - Exact art direction of colour, story and era from a prompt.
  - Unique picture per panel.
- **AI image as base: cons**
  - The maps are guessed from the picture rather than measured, so relief, roughness and real scale are unreliable.
  - The dirt is painted into the picture rather than tied to the geometry.
  - Repeatability and consistency across a set are hard to keep.
  - Tiling and mip artifacts (see next question).
- The breakdowns' common pattern, a shared master or smart material across all assets, is the main tool for set consistency. A per-panel AI picture works against it unless a shared layer stack is applied on top.

### Gaps
- No 2023-2026 breakdown found by a professional environment artist using generative AI as the main base for realistic plaster. The examples found use scans or Substance Designer.
- Vertex painting is described only in passing (Bogaert). No numbers on mask settings.

## Practical comparisons reported by artists: AI texture vs scan texture realism; and is it common to use scans as a detail layer under AI or hand-painted colour?

### Takeaway
I found no rigorous, independent artist comparison of AI and scan realism. Almost all sources on AI textures are vendor blogs. Their recurring caveats, even from vendors:
- AI gives a plausible albedo, but heuristic or absent normal, roughness and height;
- tiling and mip artifacts;
- weaker results for art-directed hero assets.

The "scan or measured detail under directed colour" pattern does exist in practice as the detail normal / detail map in master materials (Pentz). I found no source documenting a "scan grain under AI colour" workflow as common practice.

### Cited Findings
- **Vendor and guide claims:**
  - A guide says most text-to-texture systems produce a plausible albedo and "sometimes heuristic normal or roughness maps", concluding that AI mainly supplies appearance, not physical accuracy.
  - Its recommended fix: generate a clean albedo, run a bitmap-to-PBR tool for normal, roughness, height and AO, then tune normal intensity under engine lighting at grazing and head-on angles.
  - It also warns that artifacts invisible at 4K can appear at 256-512 px mips as moiré or shimmer.
  Vendor source: Hailuo AI. — [Hailuo AI guide](https://hailuoai.video/pages/knowledge/generative-ai-tileable-textures-game-art)
- A tool-review blog notes that many AI texture generators output only colour images, with "no normal maps, roughness maps, or displacement maps". It says human texture artists remain preferred for art-directed hero assets, because AI only approximates real-world references while humans can match them exactly. — [Lovart AI texture generators compared, 2026](https://www.lovart.ai/blog/ai-texture-generator-tools-compared); [aimagicx 2026 guide](https://www.aimagicx.com/blog/ai-texture-generator-game-development-2026)
- A 2026 guide concludes that AI output is shippable for prototypes, jams, indie titles and most archviz, while AAA hero assets still need a texture artist in Substance Painter (vendor-adjacent opinion). — [aimagicx 2026 guide](https://www.aimagicx.com/blog/ai-texture-generator-game-development-2026)
- **Hands-on reports:**
  - A free AI texture pack's creator generated several variations and spliced them by hand in GIMP into one tileable 1024² texture, tweaking normals in TextureLab.
  - Another pack recommends a normal strength of about 0.4 to avoid visible seams.
  These are creator notes, not benchmarks. — [weightedcreepy AI Texture Pack 1](https://weightedcreepy.gumroad.com/l/YHgBl); [TerraBasics Ground Texture Pack](https://reptilianalien.itch.io/tbgp)
- Casey Primozic made 4K PBR textures with SDXL. Tiling mode was needed for seamless output, and the other maps came from a separate tool, not from the generator (about 2023). — [cprimozic.net notes](https://cprimozic.net/notes/posts/generating-textures-for-3d-using-stable-diffusion/)
- A Polycount thread on AI for hand-painted textures (about 2023): AI images are projected onto the model from several angles at low denoise and 640-960 px, after baking AO, normal and an ID map first so the AI output follows the geometry. — [Polycount](https://polycount.com/discussion/233570/using-ai-to-create-hand-painted-textures)
- On scans: a phototexture is "a good starting point for an albedo" but may need shadows or highlights removed. In Substance Sampler, photogrammetry-derived input reportedly gives only a diffuse image, and AO generated from height can break. — [Franki T, 3D creation playbook, Sep 2025](https://www.francescatabor.com/articles/2025/9/9/the-3d-creation-playbook-how-ai-scanning-amp-photogrammetry-collide); [Adobe community thread](https://community.adobe.com/t5/substance-3d-sampler-discussions/custom-meshes-for-pbr-texture-generation-of-scaned-objects/m-p/14842835); [Adobe bug report](https://community.adobe.com/bug-reports-68/it-gets-such-a-mistake-1210447)
- **Detail layer in practice:** Pentz uses a Detail Normal map in the UE master material to keep uniquely painted modules sharp up close. Arfan layers procedural colour and roughness variation over a Megascans plaster. — [80.lv Pentz 2026](https://80.lv/articles/how-to-design-a-medieval-european-environment-using-unreal-engine-and-substance-3d); [80.lv Arfan 2020](https://80.lv/articles/working-on-a-medieval-building-with-blender-and-quixel-tools)

### Inferences
- A hybrid that matches the reported practice:
  - Take micro relief and roughness (normal, height, roughness) from a CC0 scan at known scale, used as a tiling detail layer.
  - Take the macro picture (colour, staining story, era-specific look) from a directed source: an AI picture, a hand-painted image, or a mask stack.
  - Place dirt with AO and curvature masks from the actual model.
- This avoids the two main AI weaknesses: guessed relief and dirt not tied to geometry. It also avoids the main scan weakness: someone else's colour and design.
- The Polycount trick of baking AO, normal and ID first so AI output follows the geometry is the AI-side version of the same idea: constrain the generator with the model's own data.
- If an AI base is used, check it at game distance and at low mips, since the reported failure modes (moiré, seams, over-strong normals) show up there.

### Gaps
- I found no blind or side-by-side test of AI vs scan textures by working artists in 2023-2026, and no Reddit or Polycount thread with substantive evidence came up in searches. Statements about realism are mostly vendor marketing.
- I found no source quantifying how common "scan as detail layer under AI colour" is. The detail-normal practice is documented, but the specific AI-colour-plus-scan-detail combination is not.
