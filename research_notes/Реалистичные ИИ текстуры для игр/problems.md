# Known quality problems of AI-generated game textures (2024-2026)

Scope note: researched 2026-10-23. Forum evidence is anecdotal; many "comparison" sources are vendor blogs (Meshy, Tripo, 3D AI Studio, Lovart, Hailuo), which is flagged per item. Practitioner threads specifically about plaster/stone/wood "soapy" softness on Reddit/Polycount were NOT surfaced by search (see Gaps).

## 1. Do practitioners report the soft / plastic / painterly / mushy look, and what causes it?

### Takeaway
Yes, but mostly in adjacent words: "soft", "blotchy mess of globs", "a fake texture all over", "between that and an image upscaler", "hurts my eyes", exaggerated highlights/shadows. The best-documented technical cause is the latent autoencoder (VAE) acting as a low-pass filter that under-reconstructs high-frequency texture; for OpenAI models users additionally report noise/compression-like artifacts on "complex natural surface structures", degradation over chained edits and with reference images, and over-sharpening.

### Cited Findings
- gpt-image-2 era (Apr 2026) ChatGPT users: "details and small objects in the image, as well as complex natural surface structures trigger the effect"; noise compared to "64 kb/s MP3 artifacts in sound or 20% compressed JPEG"; artifacts "especially on structured surfaces or clouds"; user _j: "The images are a bit soft ... High resolution on API to be then downsized might mitigate that a bit." — [OpenAI Community, GPT-image 2.0 issues thread](https://community.openai.com/t/collection-of-gpt-image-generator-2-0-issues-bugs-and-work-around-tips-check-first-post/1379535)
- Same thread: artifacts appear "even on the very first generation, whenever I have used a reference image" (scohissto), while without reference images the model "can do very high quality without any artifacts" (summerstay); diagonal grid artifacts linked to "in the style of" prompts. — [OpenAI Community](https://community.openai.com/t/collection-of-gpt-image-generator-2-0-issues-bugs-and-work-around-tips-check-first-post/1379535)
- gpt-image-1.5 (Jan 2026) edit endpoint: "significant increase in artifacts"; degradation described as "the more the image devolves into a blotchy mess of globs" and "a fake texture all over"; fix reported by the poster: setting quality and `input_fidelity` to "high". — [OpenAI Community, gpt-image-1.5 decreased quality](https://community.openai.com/t/gpt-image-1-5-decreased-image-render-quality/1371885)
- gpt-image-1: high-quality API requests reported to look like low-quality output in details ("faces, background etc."); chained high-fidelity edits produce grain/distortion that worsens each generation and darkens backgrounds (lossy re-saving suspected, but one user saw it with PNG too). — [OpenAI Community: quality issues over API](https://community.openai.com/t/gpt-image-1-quality-issues-over-api-low-quality-result-for-high-quality-request/1264174); [OpenAI Community: grainy after edits](https://community.openai.com/t/multiple-gpt-image-1-high-fidelity-edits-lead-to-grainy-result/1320474)
- Heise Online (Apr 2025) on GPT Image 1: "technical weaknesses such as over-sharpening artifacts", a warm colour bias, errors in poses/overlaps. — [Wikipedia: GPT Image](https://en.wikipedia.org/wiki/GPT_Image)
- Vintage Story modders (Aug 2025) on an AI texture pack: "Somewhere between that and an image upscaler" (Facethief); "It hurts my eyes" (Krougal); on cobble "some of the highlights and shadows are exaggerated, but by different amounts" (Diff); "AI is going to fall short of the level of handcrafted textures" (Diff); "rocks can be a little off so they need some edits" while brick/cobble were "perfect" (Nabbs1). — [Vintage Story forum](https://www.vintagestory.at/forums/topic/16347-experience-with-using-ai-to-generate-texture-packs/?page=5)
- Cause (latent models SD/SDXL/Flux-class): "conventional objectives inherently prioritize low-frequency reconstruction, often at the expense of high-frequency fidelity", leading to "over-smoothed outputs and visual artifacts", particularly in textured regions. SD-VAE (f16c16) high-frequency error 0.0100 vs 0.0020 for their frequency-aware VAE (~5x). — [Missing Fine Details in Images: Last Seen in High Frequencies, arXiv 2509.05441](https://arxiv.org/html/2509.05441v2)
- Real-world super-resolution work reports the 8x VAE causes poor reconstruction of fine structures "such as small characters and textures" and moves to a 4x VAE to fix it. — [PolyU: fine-structure-preserved real-world SR](https://research.polyu.edu.hk/en/publications/fine-structure-preserved-real-world-image-super-resolution-via-tr/)
- Texture-specific SD fine-tune is described as producing "flat textures with very little visible lighting/shadows" (flatness traded for de-lighting). — [dream-textures/texture-diffusion README](https://huggingface.co/dream-textures/texture-diffusion/blob/refs%2Fpr%2F5/README.md)
- Older (2022-23) Blender-AI-texture commenter: early tools are "first, and rough generation", baked shadows a problem without good setup. — [GameDev.tv community](https://community.gamedev.tv/t/creating-textures-with-ai-is-easy-in-blender-heres-how/211430)

### Inferences
- The "soapy/camouflage blotch" symptom fits two mechanisms at once: (a) VAE/decoder low-pass loss of pixel-scale grain, and (b) the model's tendency to invent mid-frequency "texture-like" blobs (the "fake texture all over" / "blotchy globs" reports) instead of physically structured micro-detail (pores, cracks, grain fibres).
- For gpt-image models, reference-image/edit workflows (which we use in Codex image_gen edits) appear to be the most artifact-prone path per user reports; fresh generations without references were reported cleaner.
- OpenAI's image models are autoregressive/multimodal, not public SD-style latent diffusion; the VAE explanation is proven for SD-family models and only plausibly analogous for gpt-image (OpenAI has not published its decoder).

### Gaps
- No OpenAI statement explaining internal processing, decoder, or any post-sharpening/compression in ChatGPT/gpt-image output.
- No Reddit/Polycount thread found that uses the words "soapy"/"mushy" for plaster/stone/wood specifically; search engine did not surface Reddit at all.

## 2. gpt-image-1 / ChatGPT image: specific known limits and official notes

### Takeaway
gpt-image-1/1.5 output is limited to 1024x1024, 1536x1024, 1024x1536 with quality low/medium/high; newer gpt-image-2.5 (Sep-Oct 2026) allows custom sizes up to 3840 px edge (above 2560x1440 "experimental") and quality up to xhigh/max. Official limitations list consistency and precise composition; no official claim of seamless tiling. Known community issues: warm/yellow cast (fixed-ish in 1.5), over-sharpening, edit-chain degradation, near-identical results across a session.

### Cited Findings
- Timeline: GPT-4o image gen 2025-03-25; `gpt-image-1` API 2025-04-23; `gpt-image-1-mini` 2025-10-06 (~80% cheaper); `gpt-image-1.5` 2025-12-16 (up to 4x faster, 20% cheaper); `gpt-image-2` Apr 2026 (reasoning in generation); GPT Image 2.5 Sep 2026 with Flare (faster, some quality trade-off) and Sunburst (more detailed, slower). Three sizes 1024x1024, 1536x1024, 1024x1536. GPT Image 1.5 "addressed ... the earlier warm color bias". — [Wikipedia: GPT Image](https://en.wikipedia.org/wiki/GPT_Image)
- Current official guide: models `gpt-image-2.5-sunburst` (editing precision) and `gpt-image-2.5-flare` (fast); recommended sizes 1024x1024, 1536x1024, 1024x1536; custom sizes multiples of 16, aspect 1:3-3:1, max edge 3840 px, 655,360-8,294,400 total pixels, resolutions above 2560x1440 "experimental"; quality `low|medium|high|xhigh|max|auto` (xhigh/max only on 2.5; "Earlier models top out at high"); formats png/jpeg/webp with `output_compression` 0-100 for jpeg/webp. — [OpenAI image generation guide](https://developers.openai.com/api/docs/guides/image-generation)
- Official limitations quoted: "the model may occasionally struggle to maintain visual consistency for recurring characters or brand elements"; "may have difficulty placing elements precisely in structured or layout-sensitive compositions"; "Masking with GPT Image is entirely prompt-based" (mask shape not followed exactly). — [OpenAI image generation guide](https://developers.openai.com/api/docs/guides/image-generation)
- Warm/yellow cast ("piss filter") widely reported since Mar 2025: whites drift to cream, shadows muddy brown; most visible on neutral surfaces like "a white wall"; no official cause; fixes are post-processing (RGB scaling). — [Popular AI](https://www.popularai.org/p/chatgpts-piss-filter-why-ai-images); [Freepik blog (403 on fetch, title only)](https://www.freepik.com/blog/fixing-openais-color-bias-with-simple-rgb-scaling)
- Consistency the other way: "No unique pictures anymore in the same session" — repeated prompts produce near-identical images (Apr 2026). — [OpenAI Community](https://community.openai.com/t/collection-of-gpt-image-generator-2-0-issues-bugs-and-work-around-tips-check-first-post/1379535)
- Scenario's tileable "Scenario Texture" pipeline routes reference images through a "GPT Image 2 edit pipeline"; it notes that going below 2048 "may lose surface detail". — [Scenario help: Texture essentials](https://help.scenario.com/articles/3312894507-scenario-texture-the-essentials)

### Inferences
- A whitewash/plaster wall is exactly the "neutral surface" where the warm cast shows; colour must be corrected against the original's measured albedo rather than trusted.
- With gpt-image-1/1.5 at 1024-1536 px covering a whole wall/facade, per-texel detail is necessarily low; with 2.5 at up to 3840 px the cap is relaxed but above 2560x1440 is officially experimental.

### Gaps
- No OpenAI doc on native seamless tiling for any gpt-image model; no official note on sharpening or JPEG-like internal compression in ChatGPT-delivered images.
- Whether Codex `image_gen` uses gpt-image-1.5, 2 or 2.5 and which quality/size it requests was not found.

## 3. Seamless tiling and scale: how tools handle it and what fails

### Takeaway
Base diffusion models are not trained to tile; tiling is added by circular padding, mixture/tiled diffusion, or post-hoc seam inpainting (Scenario "Erase Seam", Substance Sampler "Make it Tile"). Seam-free is not repetition-free: low-frequency value variation creates visible bands at 4x4+, and models do not obey requests for a smaller pattern scale; upscaling breaks seams.

### Cited Findings
- "Stable Diffusion is not trained to generate seamless textures"; a community script changes convolution padding to wrap edges (SD 1.5); mixture-tiling pipelines for SD1.5/SDXL blend region boundaries. — [diffusers community scripts README](https://huggingface.co/spaces/seawolf2357/eawolf2357-git/blob/main/diffusers/examples/community/README_community_scripts.md)
- Scenario: prompt auto-wrapped with a "tileable, top-down hint"; Erase Seam inpaints horizontal+vertical seams (Overlap default 128 px, up to 1024; Feather default 64); raise Overlap to 192-256 for cobblestones/planks; "Erase Seam can't fix a weak base generation"; default 1024², 2048² suggested for hero assets, cap ~8.3 MP; no real-world scale given. — [Scenario help](https://help.scenario.com/articles/3312894507-scenario-texture-the-essentials)
- Adobe Substance 3D Sampler Text-to-Texture (Firefly, introduced in 4.4 beta): output described as "square and tileable, with proper perspective", four variations per generation; generative features in beta. — [Adobe Sampler generative workflows](https://helpx.adobe.com/substance-3d-sampler/features-and-workflows/generative-workflows.html); [Sampler 4.4 beta notes](https://helpx.adobe.com/substance-3d-sampler/release-notes/beta-version.html)
- Indie texture-pack author (itch.io, ~2024-25): "AI-models mostly trained for 1024 resolution" so they generate 1024 then upscale to 2048, and "upscaling messing with the seams"; asking for smaller-scale pattern: "nothing happens. It doesn't understand"; "AI just bad at that resolution". — [itch.io post](https://itch.io/post/12362763)
- Vintage Story modders: "While the tiling is not good, it would be way worse if they weren't fed an existing tileable texture"; "echo" hiccups where AI draws past texture bounds, visible along the seam; "Tiles are difficult but there are utilities but they still leave some seams". — [Vintage Story forum](https://www.vintagestory.at/forums/topic/16347-experience-with-using-ai-to-generate-texture-packs/?page=5)
- Vendor explainer (Hailuo, treat with caution): naive tiling shows "obvious repetition at 4×4 and beyond"; Tiled Diffusion reports "low-frequency repetition bands"; low-frequency variation "usually breaks tiling at a distance" while high-frequency detail tiles well; target densities "1600 px/m" hero, "512–1024 px/m" background (4096 px at 1600 px/m covers ~2.5 m); artifacts at "256–512 px mips as moiré or shimmer"; recommends generating at "2–4×" target density and downscaling, and a 50% offset wrap test. (Its own 10.24 cm/px example is internally inconsistent.) — [Hailuo: tileable textures with generative AI](https://hailuoai.video/knowledge/generative-ai-tileable-textures-game-art)
- Meshy (vendor) tiling test: same texture on a large plane, check for repeat; un-trained-to-tile materials "will show a visible repeat once applied across a large surface"; a texture "can look great in isolation and still clash with everything else in a scene". — [Meshy blog](https://www.meshy.ai/blog/best-ai-texture-generators)
- Academic: TexTile (CVPR 2024) shows SSIM/LPIPS/DISTS/BRISQUE disagree on tiling artifacts; offers a differentiable tileability metric usable to guide diffusion synthesis. — [TexTile paper](https://openaccess.thecvf.com/content/CVPR2024/papers/Rodriguez-Pardo_TexTile_A_Differentiable_Metric_for_Texture_Tileability_CVPR_2024_paper.pdf); [arXiv 2403.12961](https://arxiv.org/abs/2403.12961v1)

### Inferences
- "Camouflage blotches" in a tiled wall are the low-frequency repetition problem: big value patches that are fine in one image become a visible grid once repeated; they should be flattened (high-pass/large-radius equalisation) before tiling, with large-scale variation supplied separately (decals, vertex colour, second layer).
- Image models have no notion of physical scale; scale has to be enforced by framing (e.g. telling it exactly how many bricks/boards fit) or by cropping/resampling afterwards — and the itch.io report suggests prompting alone fails.

### Gaps
- No vendor states cm-per-image for its outputs; no independent test of Firefly/Sampler tiling quality or resolution found.
- Midjourney `--tile` behaviour and quality was not covered by fetched sources.

## 4. Do lower-res photo/hand-made textures read more real than higher-res AI ones, and why?

### Takeaway
No rigorous study found. Indirect evidence: VAE-based generation loses high-frequency energy; scan libraries are valued for evenly lit, physically captured micro-detail; practitioners judge AI output against hand-made/Substance results and find it falls short on some materials (metal, rocks, grass). The mechanism (high-frequency contrast, physically coherent micro-structure, no invented blobs) is an inference.

### Cited Findings
- Latent autoencoders systematically under-reconstruct high frequencies (textures, fine structures), producing over-smoothed outputs. — [arXiv 2509.05441](https://arxiv.org/html/2509.05441v2)
- Megascans were "excellent quality assets for the time, since they were scanned in an environment with equalized lighting conditions"; downside: "If one rock looks hyper realistic, everything has to be hyper realistic" (2022, older). — [NONODE opinion](http://www.nonode.dev/2022/11/13/dont-use-megascans/)
- WithPoly user (Maxwell Render forum): 15 weathered-wood variants in ~8 minutes after ~2 hours learning prompts; its metal looked weak next to a Substance Designer version; suggested a photo-based approach might have matched the wood. — summarised via search of [Maxwell Render forum](https://forum.maxwellrender.com/viewtopic.php?p=401604) (not fetched directly)
- Meshy (vendor) attributes Polycam's photorealism and "reliable tiling" to scan training; ArmorLab's photorealism "generally lags behind cloud tools trained on larger datasets". — [Meshy blog](https://www.meshy.ai/blog/best-ai-texture-generators)
- Hand-made comparison: "AI is going to fall short of the level of handcrafted textures"; highlights/shadows "exaggerated, but by different amounts" (inconsistent lighting baked across a set). — [Vintage Story forum](https://www.vintagestory.at/forums/topic/16347-experience-with-using-ai-to-generate-texture-packs/?page=5)
- Upscaling "smooths harsh AI noise"; generate 1024 → upscale 4096 → downsample 2048 claimed to give cleaner mip chains (vendor explainer). — [Hailuo](https://hailuoai.video/knowledge/generative-ai-tileable-textures-game-art)

### Inferences
- A photo at 512 px still carries real sensor-scale contrast and physically consistent cracks/grain, so after downscaling the remaining detail is coherent; an AI image's apparent detail at 1024-1536 px is partly invented mid-frequency mottling plus a smoothed high band, so on downscale it collapses into soft blotches. This matches our symptom but is not proven by a cited study.
- Baked, exaggerated and inconsistent light/shadow in AI albedo reads as "painted" once engine lighting is added on top.

### Gaps
- No perceptual study or A/B test of "photo 512 vs AI 1024" realism found; no frequency-spectrum measurement of gpt-image outputs vs photos found.

## 5. Which tools/models are judged best for realistic tileable PBR textures, and why?

### Takeaway
No independent head-to-head benchmark exists; rankings come from vendors. Recurrent opinions: Substance 3D (Sampler + Firefly) is the controlled "starting point" for artists; scan-trained tools (Polycam) are credited with photorealism and tiling; Scenario has a dedicated tileable pipeline with seam inpainting; WithPoly is fast and free but unverified at 8K; general image models need post-processing for tiling, de-lighting and PBR maps.

### Cited Findings
- Meshy's 7-tool comparison (vendor, "typical, not best-case" output, no scores): Substance 3D generative features "positioned as a starting point rather than a finished PBR set", tiling "via Painter (manual/assisted)"; Polycam tiling "Yes, strong"; Tripo "solid for a first pass", tiling "Partial", PBR channels "less consistent"; Scenario PBR maps "a secondary capability"; Rodin "up to 4K PBR"; Meshy claims "native 8K base color" and "cleaner de-lighting". — [Meshy blog](https://www.meshy.ai/blog/best-ai-texture-generators)
- Substance generative features need an Adobe account and metered Firefly credits; the Steam Indie licence excludes them. — via search summary of [Meshy blog](https://www.meshy.ai/blog/best-ai-texture-generators)
- WithPoly: claimed tileable up to 8K with 32-bit PBR maps (colour, normal, displacement, AO, roughness, metalness); free tier; no independent verification. — [CGPress](https://cgpress.org/archives/withpoly-ai-powered-texture-generation.html); [80.lv](https://80.lv/articles/withpoly-free-ai-powered-texture-generator)
- Competitor Lovart claims most AI texture tools still show visible grid lines when tiled (biased source). — [Lovart comparison](https://www.lovart.ai/blog/ai-texture-generator-tools-compared)
- CDM (2024) presenter "generally disappointed" by AI texture generation; Substance Sampler's approach is "more conservative and tuned for consistent results". — [CDM: AI textures](https://cdm.link/2024/07/ai-textures/)
- Free/offline options mentioned: ArmorLab, Materialize (PBR from photos), Dream Textures (Blender add-on). — [3D AI Studio blog (vendor)](https://www.3daistudio.com/blog/best-ai-texture-and-pbr-generators-2026)
- Vendor note: baked lighting in AI textures "subtle enough to be acceptable for most stylized games", but dynamic lighting reveals baked-in shadows. — [3D AI Studio blog](https://www.3daistudio.com/blog/best-way-to-generate-3d-model-from-image-for-game-development)

### Inferences
- For realism the consistent thread is "trained on or derived from real captures" (scan-trained models, photo-to-PBR) beats pure text-to-image; general image models (gpt-image, Midjourney, Flux) are best used for intent/colour/layout, with micro-detail coming from photos, scans or procedural/baked detail.

### Gaps
- No independent benchmark of tileable PBR realism across tools; no data found for Stable Projectorz, Leonardo texture, or Midjourney texture quality; academic texture-diffusion methods (Material Palette, ControlMat, MatFuse, TexGen) limitations were not retrievable in this pass beyond MatFuse's abstract ([arXiv 2308.11408](https://arxiv.org/abs/2308.11408v2)).
